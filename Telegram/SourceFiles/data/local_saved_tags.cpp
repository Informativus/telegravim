/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_saved_tags.h"

#include "data/data_session.h"
#include "data/data_peer.h"
#include "data/data_saved_sublist.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/local_data_store.h"
#include "storage/storage_domain.h"
#include "ui/emoji_config.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtCore/QUuid>

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace Data {
namespace {

using Account = Storage::LocalData::Account;
using Result = Storage::LocalData::Result;

constexpr auto kMaximumTitle = 100;
constexpr auto kMaximumEmoji = 32;
constexpr auto kMaximumText = 32768;
constexpr auto kMaximumQuery = 4096;

struct ParsedModule {
	std::vector<LocalSavedTag> tags;
	std::vector<LocalTaggedMessage> messages;
	LocalSavedTagsFilter filter;
};

struct ParsedMessage {
	LocalTaggedMessage value;
	std::map<QString, int> positions;
	bool sourcePresent = false;
	bool sourceHashVerified = false;
};

[[nodiscard]] Account SessionAccount(not_null<Main::Session*> session) {
	return {
		session->isTestMode() ? u"test"_q : u"production"_q,
		QString::number(session->userId().bare),
	};
}

[[nodiscard]] QString StableId(const Account &account, const QString &key) {
	const auto input = account.environment
		+ u":"_q + account.userId + u":"_q + key;
	return QUuid::createUuidV5(
		QUuid(u"{f48d2b40-25f4-4c32-b568-af6a63b4add7}"_q),
		input.toUtf8()).toString(QUuid::WithoutBraces);
}

[[nodiscard]] bool Active(const QJsonObject &record, QStringView kind) {
	return !record.value(u"deleted"_q).toBool()
		&& record.value(u"kind"_q).toString() == kind;
}

[[nodiscard]] Result Invalid(const QString &message) {
	return { Storage::LocalData::Error::Invalid, message };
}

[[nodiscard]] QString TextHash(const QString &text) {
	return QString::fromLatin1(QCryptographicHash::hash(
		text.toUtf8(),
		QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] ReactionId ReadReaction(const QJsonObject &record) {
	const auto native = record.value(u"native_reaction"_q).toObject();
	if (native.value(u"kind"_q).toString() == u"custom"_q) {
		const auto id = native.value(u"document_id"_q).toString().toULongLong();
		return id ? ReactionId{ DocumentId(id) } : ReactionId();
	}
	return { native.value(u"emoji"_q).toString() };
}

[[nodiscard]] QJsonObject WriteReaction(const ReactionId &id) {
	return id.custom()
		? QJsonObject{
			{ u"kind"_q, u"custom"_q },
			{ u"document_id"_q, QString::number(id.custom()) },
		}
		: QJsonObject{
			{ u"kind"_q, u"emoji"_q },
			{ u"emoji"_q, id.emoji() },
		};
}

[[nodiscard]] bool ValidReaction(const ReactionId &id) {
	if (id.custom()) {
		return true;
	}
	const auto emoji = id.emoji();
	auto length = 0;
	return !emoji.isEmpty()
		&& emoji.size() <= kMaximumEmoji
		&& Ui::Emoji::Find(emoji, &length)
		&& length == emoji.size();
}

[[nodiscard]] QString ReactionKey(const ReactionId &id) {
	return id.custom()
		? u"reaction:custom:"_q + QString::number(id.custom())
		: u"reaction:emoji:"_q + id.emoji();
}

[[nodiscard]] Result NormalizeNativeTags(QJsonObject &module) {
	const auto version = module.value(u"schema_version"_q).toInt();
	if (version != 1 && version != 2) {
		return { Storage::LocalData::Error::Unsupported,
			u"Unsupported saved tags version."_q };
	}
	auto records = module.value(u"records"_q).toArray();
	auto order = std::vector<int>();
	for (auto i = 0; i != records.size(); ++i) {
		if (Active(records.at(i).toObject(), u"tag")) {
			order.push_back(i);
		}
	}
	ranges::sort(order, [&](int a, int b) {
		const auto left = records.at(a).toObject();
		const auto right = records.at(b).toObject();
		return std::tuple(left.value(u"position"_q).toInt(),
			left.value(u"id"_q).toString())
			< std::tuple(right.value(u"position"_q).toInt(),
				right.value(u"id"_q).toString());
	});
	auto used = std::set<ReactionId>();
	auto missing = std::vector<int>();
	for (const auto index : order) {
		const auto id = ReadReaction(records.at(index).toObject());
		if (!ValidReaction(id) || !used.insert(id).second) {
			missing.push_back(index);
		}
	}
	auto palette = QStringList{
		u"❤"_q, u"👍"_q, u"👎"_q, u"🔥"_q, u"🥰"_q,
		u"👏"_q, u"😁"_q, u"🤔"_q, u"🤯"_q, u"😱"_q,
		u"🤬"_q, u"😢"_q, u"🎉"_q, u"🤩"_q, u"🤮"_q,
		u"💩"_q, u"🙏"_q, u"👌"_q, u"🕊"_q, u"🤡"_q,
		u"🥱"_q, u"🥴"_q, u"😍"_q, u"🐳"_q, u"💯"_q,
	};
	if (!missing.empty()) {
		using Section = Ui::Emoji::Section;
		for (auto i = int(Section::People); i <= int(Section::Symbols); ++i) {
			for (const auto emoji : Ui::Emoji::GetSection(Section(i))) {
				palette.push_back(emoji->id());
			}
		}
	}
	auto next = 0;
	auto reserved = used;
	for (const auto index : missing) {
		const auto emoji = records.at(index).toObject().value(u"emoji"_q).toString();
		if (ValidReaction({ emoji })) {
			reserved.insert({ Ui::Emoji::Find(emoji)->id() });
		}
	}
	for (const auto index : missing) {
		auto record = records.at(index).toObject();
		auto reaction = ReactionId{ record.value(u"emoji"_q).toString() };
		if (ValidReaction(reaction)) {
			reaction = { Ui::Emoji::Find(reaction.emoji())->id() };
		}
		if (!ValidReaction(reaction) || used.contains(reaction)) {
			while (next < palette.size()
				&& reserved.contains(ReactionId{ palette[next] })) {
				++next;
			}
			if (next == palette.size()) {
				return Invalid(u"No unused emoji is available for legacy tags."_q);
			}
			reaction = { palette[next++] };
		}
		used.insert(reaction);
		reserved.insert(reaction);
		if (const auto old = record.value(u"native_reaction"_q); old.isObject()) {
			auto previous = record.value(u"previous_native_reactions"_q).toArray();
			if (!previous.contains(old)) {
				previous.push_back(old);
			}
			record.insert(u"previous_native_reactions"_q, previous);
		}
		record.insert(u"native_reaction"_q, WriteReaction(reaction));
		records[index] = record;
	}
	module.insert(u"records"_q, records);
	module.insert(u"schema_version"_q, 2);
	return {};
}

[[nodiscard]] std::optional<PeerId> ReadSavedPeer(const QJsonObject &record) {
	const auto peer = record.value(u"saved_peer"_q).toObject();
	const auto id = peer.value(u"peer_id"_q).toString().toULongLong();
	if (!id) {
		return std::nullopt;
	}
	const auto type = peer.value(u"peer_type"_q).toString();
	return type == u"user" ? peerFromUser(UserId(id))
		: type == u"chat" ? peerFromChat(ChatId(id))
		: type == u"channel" ? std::optional(peerFromChannel(ChannelId(id)))
		: std::nullopt;
}

[[nodiscard]] QJsonObject WriteSavedPeer(PeerId peer, QJsonObject result) {
	result.insert(u"peer_type"_q, peerIsUser(peer) ? u"user"_q
		: peerIsChat(peer) ? u"chat"_q : u"channel"_q);
	result.insert(u"peer_id"_q, QString::number(peerIsUser(peer)
		? peerToUser(peer).bare : peerIsChat(peer)
		? peerToChat(peer).bare : peerToChannel(peer).bare));
	return result;
}

[[nodiscard]] QString EnsureReactionTag(
		QJsonArray &records,
		const Account &account,
		const ReactionId &reaction) {
	auto position = 0;
	for (const auto &value : records) {
		const auto record = value.toObject();
		if (Active(record, u"tag")) {
			if (ReadReaction(record) == reaction) {
				return record.value(u"id"_q).toString();
			}
			position = std::max(position,
				std::min(record.value(u"position"_q).toInt(), 1000000) + 1);
		}
	}
	const auto id = StableId(account, ReactionKey(reaction));
	auto index = -1;
	for (auto i = 0; i != records.size(); ++i) {
		if (records.at(i).toObject().value(u"id"_q).toString() == id) {
			index = i;
			break;
		}
	}
	auto record = index >= 0 ? records.at(index).toObject() : QJsonObject();
	record.insert(u"id"_q, id);
	record.insert(u"kind"_q, u"tag"_q);
	record.insert(u"deleted"_q, false);
	if (!record.contains(u"title"_q)) {
		record.insert(u"title"_q, QString());
	}
	record.insert(u"emoji"_q, reaction.emoji());
	auto native = record.value(u"native_reaction"_q).toObject();
	const auto fields = WriteReaction(reaction);
	for (auto i = fields.begin(); i != fields.end(); ++i) {
		native.insert(i.key(), i.value());
	}
	record.insert(u"native_reaction"_q, native);
	record.insert(u"position"_q, position);
	if (index >= 0) {
		records[index] = record;
	} else {
		records.push_back(record);
	}
	return id;
}

[[nodiscard]] ParsedModule ParseModule(
		const QJsonObject &module,
		const Account &account) {
	auto result = ParsedModule();
	if (module.value(u"schema_version"_q).toInt() != 2) {
		return result;
	}
	auto known = std::map<QString, int>();
	const auto records = module.value(u"records"_q).toArray();
	for (const auto &value : records) {
		const auto record = value.toObject();
		if (Active(record, u"tag")) {
			const auto id = record.value(u"id"_q).toString();
			known.emplace(id, record.value(u"position"_q).toInt());
			result.tags.push_back({
				.id = id,
				.title = record.value(u"title"_q).toString(),
				.emoji = record.value(u"emoji"_q).toString(),
				.reaction = ReadReaction(record),
				.position = record.value(u"position"_q).toInt(),
			});
		}
	}
	auto grouped = std::map<MsgId, ParsedMessage>();
	for (const auto &value : records) {
		const auto record = value.toObject();
		if (Active(record, u"binding")) {
			const auto tag = record.value(u"tag_id"_q).toString();
			const auto message = record.value(u"message"_q).toObject();
			const auto number = message.value(u"message_id"_q).toString();
			auto valid = false;
			const auto id = MsgId(number.toInt(&valid));
			if (!known.contains(tag)
				|| !valid
				|| id.bare <= 0
				|| message.value(u"peer_type"_q).toString() != u"user"
				|| message.value(u"peer_id"_q).toString() != account.userId) {
				continue;
			}
			auto &parsed = grouped[id];
			auto &entry = parsed.value;
			entry.id = FullMsgId(peerFromUser(UserId(account.userId.toULongLong())), id);
			if (const auto savedPeer = ReadSavedPeer(record)) {
				entry.savedPeer = savedPeer;
			}
			if (!entry.tags.contains(tag)) {
				entry.tags.push_back(tag);
			}
			parsed.positions[tag] = record.value(u"reaction_position"_q)
				.toInt(known.at(tag));
			const auto source = record.value(u"source_text"_q);
			if (source.isString()) {
				auto text = source.toString();
				const auto hash = record.value(u"source_hash"_q).toString();
				const auto verified = !hash.isEmpty() && hash == TextHash(text);
				if (!parsed.sourcePresent
					|| (entry.text.isEmpty() && !text.isEmpty())
					|| (entry.text.isEmpty() == text.isEmpty()
						&& !parsed.sourceHashVerified && verified)) {
					entry.text = std::move(text);
					parsed.sourcePresent = true;
					parsed.sourceHashVerified = verified;
				}
			}
		} else if (Active(record, u"filter")) {
			const auto id = record.value(u"id"_q).toString();
			if (id != StableId(account, u"filter"_q)) {
				continue;
			}
			for (const auto &tag : record.value(u"tags"_q).toArray()) {
				const auto text = tag.toString();
				if (known.contains(text) && !result.filter.tags.contains(text)) {
					result.filter.tags.push_back(text);
				}
			}
			result.filter.text = record.value(u"text"_q).toString();
		}
	}
	auto counts = std::map<QString, int>();
	for (auto i = grouped.rbegin(); i != grouped.rend(); ++i) {
		const auto &positions = i->second.positions;
		ranges::sort(i->second.value.tags, [&](const auto &a, const auto &b) {
			return std::tie(positions.at(a), a) < std::tie(positions.at(b), b);
		});
		for (const auto &id : i->second.value.tags) {
			++counts[id];
		}
		result.messages.push_back(std::move(i->second.value));
	}
	for (auto &tag : result.tags) {
		tag.count = counts[tag.id];
	}
	ranges::sort(result.tags, [](const auto &a, const auto &b) {
		return std::tie(a.position, a.id) < std::tie(b.position, b.id);
	});
	return result;
}

} // namespace

LocalSavedTags::LocalSavedTags(
	QObject *parent,
	not_null<Main::Session*> session)
: QObject(parent)
, _session(session)
, _store(session->domainLocal().localData())
, _messages(std::make_shared<const std::vector<LocalTaggedMessage>>()) {
	_store->changes() | rpl::on_next([=] { reload(); }, _lifetime);
}

LocalSavedTags::~LocalSavedTags() = default;

void LocalSavedTags::reload() {
	if (_busy || !_mutations.empty()) {
		_reloadPending = true;
		return;
	}
	_busy = true;
	const auto weak = QPointer(this);
	const auto store = _store;
	const auto account = SessionAccount(_session);
	crl::async([=] {
		auto snapshot = Storage::LocalData::Snapshot();
		auto result = store->snapshot(snapshot);
		auto module = result
			? Storage::LocalData::ReadModule(snapshot, account, u"saved_tags"_q)
			: QJsonObject();
		if (result) {
			auto projected = module;
			result = NormalizeNativeTags(projected);
			if (result && projected != module) {
				result = store->mutate(account, u"saved_tags"_q,
					[](QJsonObject &value) { return NormalizeNativeTags(value); });
				if (result) {
					result = store->snapshot(snapshot);
					module = Storage::LocalData::ReadModule(
						snapshot, account, u"saved_tags"_q);
				}
			}
		}
		auto parsed = result ? ParseModule(module, account) : ParsedModule();
		crl::on_main([=, parsed = std::move(parsed)]() mutable {
			if (!weak) {
				return;
			}
			weak->_busy = false;
			weak->_error = result.message;
			if (result) {
				weak->apply(std::move(parsed.tags),
					std::move(parsed.messages), std::move(parsed.filter));
			} else {
				auto pending = std::move(weak->_mutations);
				weak->_mutations.clear();
				weak->rebuildVisible();
				for (auto &entry : pending) {
					if (entry.done) {
						entry.done(false);
					}
				}
			}
			if (!weak) {
				return;
			}
			weak->_changes.fire({});
			if (weak) {
				if (!result) {
					weak->_reloadPending = false;
				}
				weak->runNextMutation();
			}
		});
	});
}

bool LocalSavedTags::busy() const {
	return _busy
		|| !_mutations.empty()
		|| !_observedMessages.empty()
		|| (_loaded && _pendingFilter.has_value());
}

bool LocalSavedTags::loaded() const {
	return _loaded;
}

const QString &LocalSavedTags::error() const {
	return _error;
}

const std::vector<LocalSavedTag> &LocalSavedTags::tags() const {
	return _tags;
}

QString LocalSavedTags::tagId(const ReactionId &reaction) const {
	const auto i = _tagIds.find(reaction);
	return i == end(_tagIds) ? QString() : i->second;
}

const LocalSavedTagsFilter &LocalSavedTags::filter() const {
	return _filter;
}

std::vector<ReactionId> LocalSavedTags::filterReactions() const {
	auto result = std::vector<ReactionId>();
	for (const auto &id : _filter.tags) {
		const auto i = ranges::find(_tags, id, &LocalSavedTag::id);
		if (i != end(_tags) && !i->reaction.empty()) {
			result.push_back(i->reaction);
		}
	}
	return result;
}

void LocalSavedTags::rememberFilter(std::vector<ReactionId> tags, QString text) {
	_pendingFilter = LocalSavedTagsFilter{ .text = text.left(kMaximumQuery) };
	_pendingFilterReactions = std::move(tags);
	flushFilter();
}

void LocalSavedTags::flushFilter() {
	if (_busy || !_loaded || !_mutations.empty() || !_pendingFilter) {
		return;
	}
	auto filter = std::move(*_pendingFilter);
	_pendingFilter = std::nullopt;
	for (const auto &reaction : base::take(_pendingFilterReactions)) {
		const auto id = tagId(reaction);
		if (!id.isEmpty() && !filter.tags.contains(id)) {
			filter.tags.push_back(id);
		}
	}
	if (filter != _filter) {
		saveFilter(std::move(filter), nullptr);
	}
}

auto LocalSavedTags::messagesSnapshot() const
-> std::shared_ptr<const std::vector<LocalTaggedMessage>> {
	return _messages;
}

uint64 LocalSavedTags::revision() const {
	return _revision;
}

uint64 LocalSavedTags::viewRevision() const {
	return _viewRevision;
}

auto LocalSavedTags::nativeReactions() const
-> const std::map<FullMsgId, std::vector<MessageReaction>> & {
	return _reactions;
}

std::optional<PeerId> LocalSavedTags::savedPeer(FullMsgId id) const {
	const auto i = _savedPeers.find(id);
	return i == end(_savedPeers) ? std::nullopt : std::optional(i->second);
}

const std::vector<MessageReaction> &LocalSavedTags::reactions(FullMsgId id) const {
	static const auto kEmpty = std::vector<MessageReaction>();
	const auto i = _reactions.find(id);
	return i != end(_reactions) ? i->second : kEmpty;
}

std::vector<ReactionId> LocalSavedTags::chosenReactions(FullMsgId id) const {
	auto result = std::vector<ReactionId>();
	for (const auto &reaction : reactions(id)) {
		result.push_back(reaction.id);
	}
	return result;
}

void LocalSavedTags::applyReaction(
		const std::vector<FullMsgId> &messages,
		const ReactionId &reaction,
		bool assigned) {
	auto tag = ranges::find(_tags, reaction, &LocalSavedTag::reaction);
	if (tag == end(_tags) && assigned) {
		_tags.push_back({
			.id = StableId(SessionAccount(_session), ReactionKey(reaction)),
			.emoji = reaction.emoji(),
			.reaction = reaction,
			.position = int(_tags.size()),
		});
		tag = end(_tags) - 1;
		_tagIds[reaction] = tag->id;
	}
	for (const auto message : messages) {
		auto &list = _reactions[message];
		const auto i = ranges::find(list, reaction, &MessageReaction::id);
		if (assigned && i == end(list)) {
			list.push_back({ .id = reaction, .count = 1, .my = true });
			if (tag != end(_tags)) {
				++tag->count;
			}
		} else if (!assigned && i != end(list)) {
			list.erase(i);
			if (tag != end(_tags)) {
				tag->count = std::max(0, tag->count - 1);
			}
		}
	}
}

void LocalSavedTags::toggleReaction(
		FullMsgId message,
		ReactionId reaction,
		Fn<void(bool)> done) {
	const auto assigned = !ranges::contains(
		reactions(message), reaction, &MessageReaction::id);
	setReaction({ message }, std::move(reaction), assigned, std::move(done));
}

void LocalSavedTags::setReaction(
		std::vector<FullMsgId> messages,
		ReactionId reaction,
		bool assigned,
		Fn<void(bool)> done) {
	const auto valid = ValidReaction(reaction)
		&& ranges::all_of(messages, [&](FullMsgId id) {
			return id.peer == _session->userPeerId()
				&& id.msg.bare > 0
				&& id.msg.bare <= std::numeric_limits<int>::max();
		});
	if (!valid) {
		_error = tr::lng_local_saved_tags_invalid_selection(tr::now);
		const auto weak = QPointer(this);
		_changes.fire({});
		if (weak && done) {
			done(false);
		}
		return;
	}
	ranges::sort(messages);
	messages.erase(std::unique(begin(messages), end(messages)), end(messages));
	auto metadata = std::map<FullMsgId, LocalTaggedMessage>();
	for (const auto id : messages) {
		if (const auto item = _session->data().message(id)) {
			auto &entry = metadata[id];
			entry.id = id;
			entry.text = item->originalText().text.left(kMaximumText);
			if (const auto sublist = item->savedSublist()) {
				entry.savedPeer = sublist->sublistPeer()->id;
			}
		}
	}
	const auto account = SessionAccount(_session);
	mutate([=](QJsonObject &module) {
		auto records = module.value(u"records"_q).toArray();
		auto tag = QString();
		if (assigned) {
			tag = EnsureReactionTag(records, account, reaction);
		} else {
			for (const auto &value : records) {
				const auto record = value.toObject();
				if (Active(record, u"tag") && ReadReaction(record) == reaction) {
					tag = record.value(u"id"_q).toString();
					break;
				}
			}
			if (tag.isEmpty()) {
				return Result();
			}
		}
		auto tagPositions = QMap<QString, int>();
		for (const auto &value : records) {
			const auto record = value.toObject();
			if (Active(record, u"tag")) {
				tagPositions.insert(record.value(u"id"_q).toString(),
					record.value(u"position"_q).toInt());
			}
		}
		for (const auto id : messages) {
			const auto number = QString::number(id.msg.bare);
			auto index = -1;
			for (auto i = 0; i != records.size(); ++i) {
				auto record = records.at(i).toObject();
				if (record.value(u"kind"_q).toString() != u"binding"
					|| record.value(u"tag_id"_q).toString() != tag
					|| record.value(u"message"_q).toObject()
						.value(u"message_id"_q).toString() != number) {
					continue;
				}
				if (index < 0) {
					index = i;
				} else {
					record.insert(u"deleted"_q, true);
					records[i] = record;
				}
			}
			if (index < 0 && !assigned) {
				continue;
			}
			auto record = index >= 0 ? records.at(index).toObject() : QJsonObject();
			if (assigned && (index < 0 || !Active(record, u"binding"))) {
				auto order = std::vector<int>();
				for (auto i = 0; i != records.size(); ++i) {
					const auto other = records.at(i).toObject();
					if (i != index && Active(other, u"binding")
						&& other.value(u"message"_q).toObject()
							.value(u"message_id"_q).toString() == number) {
						order.push_back(i);
					}
				}
				const auto key = [&](int index) {
					const auto other = records.at(index).toObject();
					const auto tag = other.value(u"tag_id"_q).toString();
					return std::tuple(other.value(u"reaction_position"_q)
						.toInt(tagPositions.value(tag)), tag,
						other.value(u"id"_q).toString());
				};
				ranges::sort(order, [&](int a, int b) { return key(a) < key(b); });
				for (auto i = 0; i != int(order.size()); ++i) {
					auto other = records.at(order[i]).toObject();
					other.insert(u"reaction_position"_q, i);
					records[order[i]] = other;
				}
				record.insert(u"reaction_position"_q, int(order.size()));
			}
			if (index < 0) {
				record.insert(u"id"_q, StableId(account, tag + u":"_q + number));
			}
			record.insert(u"kind"_q, u"binding"_q);
			record.insert(u"tag_id"_q, tag);
			record.insert(u"deleted"_q, !assigned);
			auto message = record.value(u"message"_q).toObject();
			message.insert(u"peer_type"_q, u"user"_q);
			message.insert(u"peer_id"_q, account.userId);
			message.insert(u"message_id"_q, number);
			record.insert(u"message"_q, message);
			if (const auto i = metadata.find(id); i != end(metadata)) {
				record.insert(u"source_text"_q, i->second.text);
				record.insert(u"source_hash"_q, TextHash(i->second.text));
				if (i->second.savedPeer) {
					record.insert(u"saved_peer"_q, WriteSavedPeer(
						*i->second.savedPeer,
						record.value(u"saved_peer"_q).toObject()));
				}
			}
			if (index < 0) {
				records.push_back(record);
			} else {
				records[index] = record;
			}
		}
		module.insert(u"records"_q, records);
		return Result();
	}, std::move(done), [=] {
		applyReaction(messages, reaction, assigned);
		for (const auto &[id, entry] : metadata) {
			if (entry.savedPeer) {
				_savedPeers[id] = *entry.savedPeer;
			}
		}
	});
}

void LocalSavedTags::renameReaction(
		ReactionId reaction,
		QString title,
		Fn<void(bool)> done) {
	title = title.trimmed();
	if (!ValidReaction(reaction) || title.size() > kMaximumTitle) {
		_error = tr::lng_local_saved_tags_invalid_tag(tr::now);
		const auto weak = QPointer(this);
		_changes.fire({});
		if (weak && done) {
			done(false);
		}
		return;
	}
	const auto account = SessionAccount(_session);
	mutate([=](QJsonObject &module) {
		auto records = module.value(u"records"_q).toArray();
		const auto id = EnsureReactionTag(records, account, reaction);
		for (auto i = 0; i != records.size(); ++i) {
			auto record = records.at(i).toObject();
			if (record.value(u"id"_q).toString() == id) {
				record.insert(u"title"_q, title);
				records[i] = record;
				break;
			}
		}
		module.insert(u"records"_q, records);
		return Result();
	}, std::move(done), [=] {
		const auto i = ranges::find(_tags, reaction, &LocalSavedTag::reaction);
		if (i != end(_tags)) {
			i->title = title;
		}
	});
}

void LocalSavedTags::removeReaction(ReactionId reaction, Fn<void(bool)> done) {
	mutate([=](QJsonObject &module) {
		auto records = module.value(u"records"_q).toArray();
		auto id = QString();
		for (const auto &value : records) {
			const auto record = value.toObject();
			if (Active(record, u"tag") && ReadReaction(record) == reaction) {
				id = record.value(u"id"_q).toString();
				break;
			}
		}
		for (auto i = 0; i != records.size(); ++i) {
			auto record = records.at(i).toObject();
			if (!id.isEmpty() && (record.value(u"id"_q).toString() == id
				|| (Active(record, u"binding")
					&& record.value(u"tag_id"_q).toString() == id))) {
				record.insert(u"deleted"_q, true);
				records[i] = record;
			}
		}
		module.insert(u"records"_q, records);
		return Result();
	}, std::move(done), [=] {
		_tags.erase(ranges::remove(_tags, reaction, &LocalSavedTag::reaction), end(_tags));
		_tagIds.erase(reaction);
		for (auto &[id, list] : _reactions) {
			list.erase(ranges::remove(list, reaction, &MessageReaction::id), end(list));
		}
	});
}

void LocalSavedTags::observeMessage(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	if (!_loaded || id.peer != _session->userPeerId()
		|| reactions(id).empty()) {
		return;
	}
	auto entry = LocalTaggedMessage{
		.id = id,
		.text = item->originalText().text.left(kMaximumText),
	};
	if (const auto sublist = item->savedSublist()) {
		entry.savedPeer = sublist->sublistPeer()->id;
	}
	const auto i = ranges::find(*_messages, id, &LocalTaggedMessage::id);
	if (i != end(*_messages) && i->text == entry.text
		&& i->savedPeer == entry.savedPeer) {
		return;
	}
	_observedMessages[id] = std::move(entry);
	if (_observationScheduled) {
		return;
	}
	_observationScheduled = true;
	crl::on_main(this, [=] { flushObservedMessages(); });
}

void LocalSavedTags::flushObservedMessages() {
	_observationScheduled = false;
	if (_observedMessages.empty()) {
		return;
	}
	auto messages = std::vector<LocalTaggedMessage>();
	for (auto &[id, message] : _observedMessages) {
		messages.push_back(std::move(message));
	}
	_observedMessages.clear();
	updateTexts(std::move(messages), nullptr);
}

std::vector<LocalTaggedMessage> LocalSavedTags::messages(
		const QStringList &tags) const {
	auto result = std::vector<LocalTaggedMessage>();
	for (const auto &message : *_messages) {
		if (ranges::all_of(tags, [&](const auto &tag) {
			return message.tags.contains(tag);
		})) {
			result.push_back(message);
		}
	}
	return result;
}

int LocalSavedTags::assignedCount(
		const QString &tag,
		const std::vector<FullMsgId> &messages) const {
	return ranges::count_if(messages, [&](const auto id) {
		const auto i = ranges::find(*_messages, id, &LocalTaggedMessage::id);
		return i != end(*_messages) && i->tags.contains(tag);
	});
}

rpl::producer<> LocalSavedTags::changes() const {
	return _changes.events();
}

void LocalSavedTags::apply(
		std::vector<LocalSavedTag> tags,
		std::vector<LocalTaggedMessage> messages,
		LocalSavedTagsFilter filter) {
	if (!_loaded || _committedTags != tags || *_messages != messages) {
		++_revision;
	}
	_committedTags = std::move(tags);
	_messages = std::make_shared<const std::vector<LocalTaggedMessage>>(
		std::move(messages));
	_filter = std::move(filter);
	_loaded = true;
	rebuildVisible();
}

void LocalSavedTags::rebuildVisible() {
	_tags = _committedTags;
	_tagIds.clear();
	_savedPeers.clear();
	for (auto &[id, reactions] : _reactions) {
		reactions.clear();
	}
	auto tags = std::map<QString, ReactionId>();
	for (const auto &tag : _tags) {
		tags.emplace(tag.id, tag.reaction);
		_tagIds.emplace(tag.reaction, tag.id);
	}
	for (const auto &message : *_messages) {
		if (message.savedPeer) {
			_savedPeers[message.id] = *message.savedPeer;
		}
		auto &list = _reactions[message.id];
		for (const auto &tag : message.tags) {
			const auto i = tags.find(tag);
			if (i != end(tags) && !i->second.empty()) {
				list.push_back({ .id = i->second, .count = 1, .my = true });
			}
		}
	}
	for (const auto &mutation : _mutations) {
		if (mutation.optimistic) {
			mutation.optimistic();
		}
	}
	++_viewRevision;
}

void LocalSavedTags::mutate(
		Fn<Result(QJsonObject &)> change,
		Fn<void(bool)> done,
		Fn<void()> optimistic) {
	_mutations.push_back({ std::move(change), std::move(done),
		std::move(optimistic) });
	_error.clear();
	const auto weak = QPointer(this);
	if (_mutations.back().optimistic) {
		_mutations.back().optimistic();
		++_viewRevision;
	}
	_changes.fire({});
	if (weak) {
		runNextMutation();
	}
}

void LocalSavedTags::runNextMutation() {
	if (_busy) {
		return;
	} else if (!_loaded && !_mutations.empty()) {
		auto pending = std::move(_mutations);
		_mutations.clear();
		reload();
		_mutations = std::move(pending);
		return;
	} else if (_mutations.empty()) {
		if (base::take(_reloadPending)) {
			reload();
		} else {
			flushFilter();
		}
		return;
	}
	_busy = true;
	const auto weak = QPointer(this);
	const auto store = _store;
	const auto account = SessionAccount(_session);
	const auto change = _mutations.front().change;
	crl::async([=] {
		auto result = store->mutate(account, u"saved_tags"_q, [=](auto &module) {
			if (auto normalized = NormalizeNativeTags(module); !normalized) {
				return normalized;
			}
			if (auto changed = change(module); !changed) {
				return changed;
			}
			return NormalizeNativeTags(module);
		});
		auto snapshot = Storage::LocalData::Snapshot();
		auto read = store->snapshot(snapshot);
		const auto module = read
			? Storage::LocalData::ReadModule(snapshot, account, u"saved_tags"_q)
			: QJsonObject();
		if (read && module.value(u"schema_version"_q).toInt() != 2) {
			read = { Storage::LocalData::Error::Unsupported,
				u"Unsupported saved tags version."_q };
		}
		auto parsed = read
			? ParseModule(module, account)
			: ParsedModule();
		if (result && !read) {
			result = read;
		}
		crl::on_main([=, parsed = std::move(parsed)]() mutable {
			if (!weak) {
				return;
			}
			auto done = std::move(weak->_mutations.front().done);
			weak->_mutations.pop_front();
			weak->_busy = false;
			weak->_error = result.message;
			if (read) {
				weak->apply(std::move(parsed.tags),
					std::move(parsed.messages), std::move(parsed.filter));
			} else {
				weak->rebuildVisible();
			}
			weak->_changes.fire({});
			if (done) {
				done(result.ok());
			}
			if (weak) {
				weak->runNextMutation();
			}
		});
	});
}

void LocalSavedTags::saveTag(
		QString id,
		QString title,
		QString emoji,
		Fn<void(bool)> done) {
	title = title.trimmed();
	emoji = emoji.trimmed();
	auto emojiLength = 0;
	const auto validEmoji = emoji.isEmpty()
		|| (Ui::Emoji::Find(emoji, &emojiLength) && emojiLength == emoji.size());
	if (title.isEmpty()
		|| title.size() > kMaximumTitle
		|| emoji.size() > kMaximumEmoji
		|| !validEmoji) {
		_error = tr::lng_local_saved_tags_invalid_tag(tr::now);
		const auto weak = QPointer(this);
		_changes.fire({});
		if (weak && done) {
			done(false);
		}
		return;
	}
	const auto created = id.isEmpty();
	if (created) {
		id = Storage::LocalData::NewId();
	}
	const auto missing = tr::lng_local_saved_tags_missing_tag(tr::now);
	mutate([=](QJsonObject &module) {
		auto records = module.value(u"records"_q).toArray();
		auto index = -1;
		auto position = 0;
		for (auto i = 0; i != records.size(); ++i) {
			const auto record = records.at(i).toObject();
			if (Active(record, u"tag")) {
				position = std::max(position,
					std::min(record.value(u"position"_q).toInt(), 1000000) + 1);
				if (record.value(u"id"_q).toString() == id) {
					index = i;
				}
			}
		}
		if (!created && index < 0) {
			return Invalid(missing);
		}
		auto record = index >= 0 ? records.at(index).toObject() : QJsonObject();
		record.insert(u"id"_q, id);
		record.insert(u"kind"_q, u"tag"_q);
		record.insert(u"title"_q, title);
		record.insert(u"emoji"_q, emoji);
		if (index < 0) {
			record.insert(u"position"_q, position);
			records.push_back(record);
		} else {
			records[index] = record;
		}
		module.insert(u"records"_q, records);
		return Result();
	}, std::move(done));
}

void LocalSavedTags::removeTag(QString id, Fn<void(bool)> done) {
	mutate([=](QJsonObject &module) {
		auto records = module.value(u"records"_q).toArray();
		for (auto i = 0; i != records.size(); ++i) {
			auto record = records.at(i).toObject();
			if ((Active(record, u"tag") && record.value(u"id"_q).toString() == id)
				|| (Active(record, u"binding")
					&& record.value(u"tag_id"_q).toString() == id)) {
				record.insert(u"deleted"_q, true);
				records[i] = record;
			}
		}
		module.insert(u"records"_q, records);
		return Result();
	}, std::move(done));
}

void LocalSavedTags::moveTag(QString id, int delta, Fn<void(bool)> done) {
	const auto missing = tr::lng_local_saved_tags_missing_tag(tr::now);
	mutate([=](QJsonObject &module) {
		auto records = module.value(u"records"_q).toArray();
		auto order = std::vector<int>();
		for (auto i = 0; i != records.size(); ++i) {
			if (Active(records.at(i).toObject(), u"tag")) {
				order.push_back(i);
			}
		}
		ranges::sort(order, [&](int a, int b) {
			const auto left = records.at(a).toObject();
			const auto right = records.at(b).toObject();
			const auto x = left.value(u"position"_q).toInt();
			const auto y = right.value(u"position"_q).toInt();
			return (x != y) ? x < y : left.value(u"id"_q).toString()
				< right.value(u"id"_q).toString();
		});
		const auto found = ranges::find_if(order, [&](int index) {
			return records.at(index).toObject().value(u"id"_q).toString() == id;
		});
		if (found == end(order)) {
			return Invalid(missing);
		}
		const auto current = int(found - begin(order));
		const auto target = std::clamp(current + delta, 0, int(order.size()) - 1);
		std::swap(order[current], order[target]);
		for (auto i = 0; i != int(order.size()); ++i) {
			auto record = records.at(order[i]).toObject();
			record.insert(u"position"_q, i);
			records[order[i]] = record;
		}
		module.insert(u"records"_q, records);
		return Result();
	}, std::move(done));
}

void LocalSavedTags::setAssigned(
		QString tag,
		std::vector<FullMsgId> messages,
		bool assigned,
		Fn<void(bool)> done) {
	const auto account = SessionAccount(_session);
	auto texts = QMap<qint64, QString>();
	for (const auto id : messages) {
		if (id.peer != _session->userPeerId()
			|| id.msg.bare <= 0
			|| id.msg.bare > std::numeric_limits<int>::max()) {
			_error = tr::lng_local_saved_tags_invalid_selection(tr::now);
			const auto weak = QPointer(this);
			_changes.fire({});
			if (weak) {
				done(false);
			}
			return;
		}
		if (const auto item = _session->data().message(id)) {
			texts.insert(id.msg.bare, item->originalText().text.left(kMaximumText));
		}
	}
	const auto missing = tr::lng_local_saved_tags_missing_tag(tr::now);
	mutate([=](QJsonObject &module) {
		auto records = module.value(u"records"_q).toArray();
		const auto exists = ranges::any_of(records, [&](const auto &value) {
			const auto record = value.toObject();
			return Active(record, u"tag") && record.value(u"id"_q).toString() == tag;
		});
		if (!exists) {
			return Invalid(missing);
		}
		for (const auto id : messages) {
			const auto number = QString::number(id.msg.bare);
			const auto key = StableId(account, tag + u":"_q + number);
			auto index = -1;
			for (auto i = 0; i != records.size(); ++i) {
				const auto record = records.at(i).toObject();
				const auto message = record.value(u"message"_q).toObject();
				if (record.value(u"id"_q).toString() == key
					|| (record.value(u"kind"_q).toString() == u"binding"
						&& record.value(u"tag_id"_q).toString() == tag
						&& message.value(u"message_id"_q).toString() == number)) {
					if (index < 0) {
						index = i;
					} else {
						auto duplicate = record;
						duplicate.insert(u"deleted"_q, true);
						records[i] = duplicate;
					}
				}
			}
			if (index < 0 && !assigned) {
				continue;
			}
			auto record = index >= 0 ? records.at(index).toObject() : QJsonObject();
			if (index < 0) {
				record.insert(u"id"_q, key);
			}
			record.insert(u"kind"_q, u"binding"_q);
			record.insert(u"tag_id"_q, tag);
			auto message = record.value(u"message"_q).toObject();
			message.insert(u"peer_type"_q, u"user"_q);
			message.insert(u"peer_id"_q, account.userId);
			message.insert(u"message_id"_q, number);
			record.insert(u"message"_q, message);
			record.insert(u"deleted"_q, !assigned);
			if (texts.contains(id.msg.bare)) {
				const auto text = texts.value(id.msg.bare);
				record.insert(u"source_text"_q, text);
				record.insert(u"source_hash"_q, TextHash(text));
			}
			if (index < 0) {
				records.push_back(record);
			} else {
				records[index] = record;
			}
		}
		module.insert(u"records"_q, records);
		return Result();
	}, std::move(done));
}

void LocalSavedTags::saveFilter(
		LocalSavedTagsFilter filter,
		Fn<void(bool)> done) {
	const auto id = StableId(SessionAccount(_session), u"filter"_q);
	mutate([=](QJsonObject &module) {
		auto records = module.value(u"records"_q).toArray();
		auto index = -1;
		for (auto i = 0; i != records.size(); ++i) {
			if (records.at(i).toObject().value(u"id"_q).toString() == id) {
				index = i;
				break;
			}
		}
		auto record = index >= 0 ? records.at(index).toObject() : QJsonObject();
		record.insert(u"id"_q, id);
		record.insert(u"kind"_q, u"filter"_q);
		record.insert(u"deleted"_q, false);
		record.insert(u"tags"_q, QJsonArray::fromStringList(filter.tags));
		record.insert(u"text"_q, filter.text.left(kMaximumQuery));
		if (index < 0) {
			records.push_back(record);
		} else {
			records[index] = record;
		}
		module.insert(u"records"_q, records);
		return Result();
	}, std::move(done));
}

void LocalSavedTags::updateTexts(
		std::vector<LocalTaggedMessage> messages,
		Fn<void(bool)> done) {
	mutate([=](QJsonObject &module) {
		auto texts = QMap<QString, LocalTaggedMessage>();
		for (auto entry : messages) {
			entry.text = entry.text.left(kMaximumText);
			texts.insert(QString::number(entry.id.msg.bare), std::move(entry));
		}
		auto records = module.value(u"records"_q).toArray();
		for (auto i = 0; i != records.size(); ++i) {
			auto record = records.at(i).toObject();
			if (!Active(record, u"binding")) {
				continue;
			}
			const auto message = record.value(u"message"_q).toObject();
			const auto id = message.value(u"message_id"_q).toString();
			const auto text = texts.constFind(id);
			if (text == texts.cend()) {
				continue;
			}
			record.insert(u"source_text"_q, text->text);
			record.insert(u"source_hash"_q, TextHash(text->text));
			if (text->savedPeer) {
				record.insert(u"saved_peer"_q, WriteSavedPeer(
					*text->savedPeer,
					record.value(u"saved_peer"_q).toObject()));
			}
			records[i] = record;
		}
		module.insert(u"records"_q, records);
		return Result();
	}, std::move(done));
}

} // namespace Data
