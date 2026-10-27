/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_chat_folders.h"

#include "data/data_peer.h"
#include "main/main_session.h"
#include "storage/local_chat_folders_format.h"
#include "storage/local_data_store.h"
#include "storage/storage_domain.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtCore/QSet>

#include <algorithm>
#include <map>
#include <set>

namespace Data {
namespace {

using Account = Storage::LocalData::Account;
using Result = Storage::LocalData::Result;
namespace Format = Storage::LocalData::ChatFolders;

[[nodiscard]] Account SessionAccount(not_null<Main::Session*> session) {
	return {
		session->isTestMode() ? u"test"_q : u"production"_q,
		QString::number(session->userId().bare),
	};
}

[[nodiscard]] PeerId ReadPeer(const QJsonObject &peer) {
	const auto type = peer.value(u"peer_type"_q).toString();
	const auto id = peer.value(u"peer_id"_q).toString().toULongLong();
	return (type == u"user"_q)
		? peerFromUser(UserId(id))
		: (type == u"chat"_q)
		? peerFromChat(ChatId(id))
		: (type == u"channel"_q)
		? peerFromChannel(ChannelId(id))
		: PeerId();
}

[[nodiscard]] QJsonObject WritePeer(PeerId peer) {
	const auto type = peerIsUser(peer)
		? u"user"_q
		: peerIsChat(peer)
		? u"chat"_q
		: peerIsChannel(peer)
		? u"channel"_q
		: QString();
	return {
		{ u"peer_type"_q, type },
		{ u"peer_id"_q,
			QString::number(peer.value & PeerId::kChatTypeMask) },
	};
}

[[nodiscard]] bool Active(const QJsonObject &record) {
	return !record.value(u"deleted"_q).toBool();
}

[[nodiscard]] ChatFilter::Flag RuleFlag(const QString &op) {
	using Flag = ChatFilter::Flag;
	if (op == u"contacts"_q) return Flag::Contacts;
	if (op == u"non_contacts"_q) return Flag::NonContacts;
	if (op == u"groups"_q) return Flag::Groups;
	if (op == u"channels"_q) return Flag::Channels;
	if (op == u"bots"_q) return Flag::Bots;
	if (op == u"no_muted"_q) return Flag::NoMuted;
	if (op == u"no_read"_q) return Flag::NoRead;
	if (op == u"no_archived"_q) return Flag::NoArchived;
	return Flag(0);
}

[[nodiscard]] QJsonArray Rules(ChatFilter::Flags flags) {
	using Flag = ChatFilter::Flag;
	const auto names = std::array<std::pair<Flag, QStringView>, 8>{
		std::pair{ Flag::Contacts, u"contacts"_q },
		std::pair{ Flag::NonContacts, u"non_contacts"_q },
		std::pair{ Flag::Groups, u"groups"_q },
		std::pair{ Flag::Channels, u"channels"_q },
		std::pair{ Flag::Bots, u"bots"_q },
		std::pair{ Flag::NoMuted, u"no_muted"_q },
		std::pair{ Flag::NoRead, u"no_read"_q },
		std::pair{ Flag::NoArchived, u"no_archived"_q },
	};
	auto result = QJsonArray();
	for (const auto &[flag, name] : names) {
		if (flags & flag) {
			result.push_back(QJsonObject{
				{ u"op"_q, QString(name) },
				{ u"enabled"_q, true },
			});
		}
	}
	return result;
}

[[nodiscard]] QJsonArray RulesFor(const LocalChatFolder &folder) {
	if (folder.rules.isEmpty()) {
		return Rules(folder.flags);
	}
	using Flag = ChatFilter::Flag;
	const auto names = std::array<std::pair<Flag, QString>, 8>{
		std::pair{ Flag::Contacts, u"contacts"_q },
		std::pair{ Flag::NonContacts, u"non_contacts"_q },
		std::pair{ Flag::Groups, u"groups"_q },
		std::pair{ Flag::Channels, u"channels"_q },
		std::pair{ Flag::Bots, u"bots"_q },
		std::pair{ Flag::NoMuted, u"no_muted"_q },
		std::pair{ Flag::NoRead, u"no_read"_q },
		std::pair{ Flag::NoArchived, u"no_archived"_q },
	};
	auto result = folder.rules;
	auto seen = base::flat_set<QString>();
	for (auto i = result.begin(); i != result.end(); ++i) {
		auto object = i->toObject();
		const auto op = object.value(u"op"_q).toString();
		const auto flag = RuleFlag(op);
		if (!flag) {
			continue;
		}
		seen.insert(op);
		object.insert(u"enabled"_q, bool(folder.flags & flag));
		*i = std::move(object);
	}
	for (const auto &[flag, name] : names) {
		if ((folder.flags & flag) && !seen.contains(name)) {
			result.push_back(QJsonObject{
				{ u"op"_q, name },
				{ u"enabled"_q, true },
			});
		}
	}
	return result;
}

[[nodiscard]] QJsonArray Peers(const std::vector<PeerId> &peers) {
	auto result = QJsonArray();
	for (const auto peer : peers) {
		result.push_back(WritePeer(peer));
	}
	return result;
}

[[nodiscard]] int FindRecord(
		const QJsonArray &records,
		const QString &id) {
	for (auto i = 0; i != records.size(); ++i) {
		if (records.at(i).toObject().value(u"id"_q).toString() == id) {
			return i;
		}
	}
	return -1;
}

void PutRecord(QJsonArray &records, int index, QJsonObject value) {
	if (index < 0) {
		records.push_back(std::move(value));
	} else {
		records[index] = std::move(value);
	}
}

[[nodiscard]] QString EntityKey(const QJsonObject &entity) {
	return entity.value(u"type"_q).toString()
		+ u":"_q + QString::number(entity.value(u"offset"_q).toInt())
		+ u":"_q + QString::number(entity.value(u"length"_q).toInt())
		+ u":"_q + entity.value(u"data"_q).toString();
}

void PutRecordPreserving(
		QJsonArray &records,
		int index,
		QJsonObject value) {
	if (index >= 0) {
		auto merged = records.at(index).toObject();
		for (auto i = value.begin(); i != value.end(); ++i) {
			if (i.key() == u"peer"_q
				&& i.value().isObject()
				&& merged.value(i.key()).isObject()) {
				auto peer = merged.value(i.key()).toObject();
				const auto newPeer = i.value().toObject();
				for (auto j = newPeer.begin(); j != newPeer.end(); ++j) {
					peer.insert(j.key(), j.value());
				}
				merged.insert(i.key(), std::move(peer));
			} else if (i.key() == u"title_entities"_q
				&& i.value().isArray()
				&& merged.value(i.key()).isArray()) {
				auto previous = QMap<QString, QJsonObject>();
				for (const auto &entity : merged.value(i.key()).toArray()) {
					if (entity.isObject()) {
						previous.insert(EntityKey(entity.toObject()), entity.toObject());
					}
				}
				auto entities = QJsonArray();
				for (const auto &entity : i.value().toArray()) {
					if (!entity.isObject()) {
						entities.push_back(entity);
						continue;
					}
					auto current = entity.toObject();
					const auto old = previous.constFind(EntityKey(current));
					if (old != previous.cend()) {
						auto combined = old.value();
						for (auto field = current.begin();
							field != current.end();
							++field) {
							combined.insert(field.key(), field.value());
						}
						current = std::move(combined);
					}
					entities.push_back(std::move(current));
				}
				merged.insert(i.key(), std::move(entities));
			} else {
				merged.insert(i.key(), i.value());
			}
		}
		if (value.value(u"kind"_q).toString() == u"folder"_q
			&& !value.contains(u"color"_q)) {
			merged.remove(u"color"_q);
		}
		value = std::move(merged);
	}
	PutRecord(records, index, std::move(value));
}

[[nodiscard]] QJsonObject FolderRecord(
		const Account &account,
		const LocalChatFolder &folder) {
	const auto id = Format::FolderId(account, folder.id);
	auto result = QJsonObject{
		{ u"id"_q, id },
		{ u"kind"_q, u"folder"_q },
		{ u"folder_id"_q, folder.id },
		{ u"title"_q, folder.title },
		{ u"title_entities"_q, folder.titleEntities },
		{ u"static_title"_q, folder.staticTitle },
		{ u"icon"_q, folder.iconEmoji },
		{ u"position"_q, folder.position },
		{ u"rules"_q, RulesFor(folder) },
		{ u"deleted"_q, false },
	};
	if (folder.colorIndex) {
		result.insert(u"color"_q, *folder.colorIndex);
	}
	return result;
}

[[nodiscard]] QJsonObject MemberRecord(
		const Account &account,
		const QString &folder,
		PeerId peer,
		QString mode) {
	const auto peerObject = WritePeer(peer);
	const auto id = Format::MemberId(account, folder, peerObject, mode);
	return {
		{ u"id"_q, id },
		{ u"kind"_q, u"member"_q },
		{ u"folder_id"_q, folder },
		{ u"peer"_q, peerObject },
		{ u"mode"_q, std::move(mode) },
		{ u"deleted"_q, false },
	};
}

[[nodiscard]] QJsonObject SettingsRecord(
		const Account &account,
		bool tagsEnabled) {
	return {
		{ u"id"_q, Format::SettingsId(account) },
		{ u"kind"_q, u"settings"_q },
		{ u"scope"_q, u"account"_q },
		{ u"tags_enabled"_q, tagsEnabled },
		{ u"deleted"_q, false },
	};
}

[[nodiscard]] bool SamePeer(PeerId a, PeerId b) {
	return a == b;
}

} // namespace

LocalChatFolders::LocalChatFolders(
		QObject *parent,
		not_null<Main::Session*> session)
: QObject(parent)
, _session(session)
, _store(session->domainLocal().localData()) {
	_store->changes() | rpl::on_next([=] { reload(); }, _lifetime);
	crl::on_main(this, [=] { reload(); });
}

LocalChatFolders::~LocalChatFolders() = default;

bool LocalChatFolders::loaded() const {
	return _loaded;
}

bool LocalChatFolders::busy() const {
	return _busy || !_mutations.empty();
}

const QString &LocalChatFolders::error() const {
	return _error;
}

const std::vector<LocalChatFolder> &LocalChatFolders::folders() const {
	return _folders;
}

std::optional<bool> LocalChatFolders::tagsEnabled() const {
	return _tagsEnabled;
}

std::optional<std::optional<uint8>> LocalChatFolders::cloudColor(
		const QString &scope) const {
	const auto i = _cloudColors.find(scope);
	return (i != end(_cloudColors))
		? std::make_optional(i->second)
		: std::nullopt;
}

rpl::producer<> LocalChatFolders::changes() const {
	return _changes.events();
}

LocalChatFolders::CloudColors LocalChatFolders::ParseCloudColors(
		const QJsonObject &module) {
	auto result = CloudColors();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		if (!Active(record)
			|| record.value(u"kind"_q).toString() != u"cloud_color"_q) {
			continue;
		}
		const auto color = record.value(u"color_index"_q);
		if (!color.isNull() && color.toInt() >= 7) {
			continue;
		}
		result.emplace(
			record.value(u"scope"_q).toString(),
			color.isNull() ? std::nullopt : std::make_optional(uint8(color.toInt())));
	}
	return result;
}

std::optional<bool> LocalChatFolders::ParseTagsEnabled(
		const QJsonObject &module,
		const Account &account) {
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		if (Active(record)
			&& record.value(u"kind"_q).toString() == u"settings"_q
			&& record.value(u"id"_q).toString()
				== Format::SettingsId(account)) {
			return record.value(u"tags_enabled"_q).toBool();
		}
	}
	return std::nullopt;
}

std::vector<LocalChatFolder> LocalChatFolders::Parse(
		const QJsonObject &module) {
	auto folders = std::map<QString, LocalChatFolder>();
	auto members = std::map<QString, std::vector<std::pair<PeerId, bool>>>();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		if (!Active(record)) {
			continue;
		}
		if (record.value(u"kind"_q).toString() == u"folder"_q) {
			auto folder = LocalChatFolder();
			folder.id = record.value(u"folder_id"_q).toString();
			folder.title = record.value(u"title"_q).toString();
			folder.titleEntities = record.value(u"title_entities"_q).toArray();
			folder.staticTitle = record.value(u"static_title"_q).toBool();
			folder.iconEmoji = record.value(u"icon"_q).toString();
			folder.rules = record.value(u"rules"_q).toArray();
			if (record.value(u"color"_q).isDouble()) {
				folder.colorIndex = uint8(std::clamp(
					record.value(u"color"_q).toInt(), 0, 255));
			}
			folder.position = record.value(u"position"_q).toInt();
			for (const auto &rule : record.value(u"rules"_q).toArray()) {
				const auto object = rule.toObject();
				if (object.value(u"enabled"_q).toBool(true)) {
					folder.flags |= RuleFlag(object.value(u"op"_q).toString());
				}
			}
			if (!folder.id.isEmpty()) {
				folders[folder.id] = std::move(folder);
			}
		} else if (record.value(u"kind"_q).toString() == u"member"_q) {
			const auto peer = ReadPeer(record.value(u"peer"_q).toObject());
			const auto folder = record.value(u"folder_id"_q).toString();
			if (peer && !folder.isEmpty()) {
				members[folder].emplace_back(
					peer,
					record.value(u"mode"_q).toString() == u"exclude"_q);
			}
		}
	}
	auto result = std::vector<LocalChatFolder>();
	for (auto &[id, folder] : folders) {
		for (const auto &[peer, excluded] : members[id]) {
			auto &target = excluded ? folder.exclude : folder.include;
			if (ranges::find(target, peer) == target.end()) {
				target.push_back(peer);
			}
		}
		for (const auto peer : folder.exclude) {
			std::erase(folder.include, peer);
		}
		result.push_back(std::move(folder));
	}
	ranges::sort(result, {}, &LocalChatFolder::position);
	return result;
}

void LocalChatFolders::reload() {
	if (_stopped) {
		return;
	}
	if (_busy || !_mutations.empty()) {
		_reloadPending = true;
		return;
	}
	loadSnapshot();
}

void LocalChatFolders::stop() {
	_stopped = true;
	_busy = false;
	_reloadPending = false;
	_mutations.clear();
	_lifetime.destroy();
}

void LocalChatFolders::loadSnapshot() {
	_busy = true;
	const auto weak = QPointer(this);
	const auto store = _store;
	const auto account = SessionAccount(_session);
	crl::async([=] {
		auto snapshot = Storage::LocalData::Snapshot();
		auto result = store->snapshot(snapshot);
		const auto module = result
			? Storage::LocalData::ReadModule(snapshot, account, u"chat_folders"_q)
			: QJsonObject();
		if (result) {
			result = Format::Validate(module, account);
		}
		auto parsed = result
			? Parse(module)
			: std::vector<LocalChatFolder>();
		crl::on_main([=, parsed = std::move(parsed)]() mutable {
			if (!weak || weak->_stopped) {
				return;
			}
			weak->_busy = false;
			weak->_error = result.message;
			if (result) {
				weak->_committed = std::move(parsed);
				weak->_committedTagsEnabled = ParseTagsEnabled(module, account);
				weak->_committedCloudColors = ParseCloudColors(module);
				weak->_loaded = true;
				weak->rebuildVisible();
			} else {
				auto pending = std::move(weak->_mutations);
				weak->_mutations.clear();
				weak->_reloadPending = false;
				weak->rebuildVisible();
				for (auto &mutation : pending) {
					if (mutation.done) mutation.done(false);
					if (!weak) return;
				}
			}
			weak->_changes.fire({});
			if (!weak || weak->_stopped) return;
			weak->runNextMutation();
		});
	});
}

void LocalChatFolders::rebuildVisible() {
	_folders = _committed;
	_tagsEnabled = _committedTagsEnabled;
	_cloudColors = _committedCloudColors;
	for (const auto &mutation : _mutations) {
		if (mutation.optimistic) mutation.optimistic(_folders);
		if (mutation.optimisticTagsEnabled) {
			mutation.optimisticTagsEnabled(_tagsEnabled);
		}
		if (mutation.optimisticCloudColors) {
			mutation.optimisticCloudColors(_cloudColors);
		}
	}
	ranges::sort(_folders, {}, &LocalChatFolder::position);
}

void LocalChatFolders::fail(QString message, Fn<void(bool)> done) {
	_error = std::move(message);
	_changes.fire({});
	if (done) done(false);
}

void LocalChatFolders::mutate(Mutation mutation) {
	const auto weak = QPointer(this);
	_error.clear();
	_mutations.push_back(std::move(mutation));
	rebuildVisible();
	_changes.fire({});
	if (weak && !weak->_stopped) {
		weak->runNextMutation();
	}
}

void LocalChatFolders::runNextMutation() {
	if (_busy) return;
	if (!_loaded && !_mutations.empty()) {
		loadSnapshot();
		return;
	}
	if (_mutations.empty()) {
		if (std::exchange(_reloadPending, false)) loadSnapshot();
		return;
	}
	_busy = true;
	const auto weak = QPointer(this);
	const auto store = _store;
	const auto account = SessionAccount(_session);
	const auto change = _mutations.front().change;
	crl::async([=] {
		auto result = store->mutate(account, u"chat_folders"_q,
			[=](QJsonObject &module) { return change(module); });
		auto snapshot = Storage::LocalData::Snapshot();
		auto read = store->snapshot(snapshot);
		const auto module = read
			? Storage::LocalData::ReadModule(snapshot, account, u"chat_folders"_q)
			: QJsonObject();
		if (read) {
			read = Format::Validate(module, account);
		}
		auto parsed = read ? Parse(module) : std::vector<LocalChatFolder>();
		auto parsedTagsEnabled = read
			? ParseTagsEnabled(module, account)
			: std::optional<bool>();
		if (result && !read) result = read;
		crl::on_main([=, parsed = std::move(parsed)]() mutable {
			if (!weak || weak->_stopped) return;
			auto done = std::move(weak->_mutations.front().done);
			weak->_mutations.pop_front();
			weak->_busy = false;
			weak->_error = result.message;
			if (read) {
				weak->_committed = std::move(parsed);
				weak->_committedTagsEnabled = parsedTagsEnabled;
				weak->_committedCloudColors = ParseCloudColors(module);
			}
			weak->rebuildVisible();
			weak->_changes.fire({});
			if (!weak || weak->_stopped) return;
			if (done) done(result.ok());
			if (!weak || weak->_stopped) return;
			weak->runNextMutation();
		});
	});
}

void LocalChatFolders::save(LocalChatFolder folder, Fn<void(bool)> done) {
	folder.id = folder.id.isEmpty() ? Storage::LocalData::NewId() : folder.id;
	const auto validPeers = [](const std::vector<PeerId> &peers) {
		auto seen = std::set<PeerId>();
		for (const auto peer : peers) {
			if (Format::PeerKey(WritePeer(peer)).isEmpty()
				|| !seen.insert(peer).second) {
				return false;
			}
		}
		return true;
	};
	if (!Format::ValidUuid(folder.id) || folder.title.trimmed().isEmpty()
		|| folder.title.size() > 100
		|| !validPeers(folder.include)
		|| !validPeers(folder.exclude)
		|| folder.include.size() > Format::kMaximumPeersPerFolder
		|| folder.exclude.size() > Format::kMaximumPeersPerFolder) {
		return fail(u"Invalid local chat folder."_q, std::move(done));
	}
	const auto account = SessionAccount(_session);
	mutate({
		.change = [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			const auto folderId = Format::FolderId(account, folder.id);
			auto desiredMembers = QSet<QString>();
			for (const auto peer : folder.include) {
				desiredMembers.insert(Format::MemberId(
					account,
					folder.id,
					WritePeer(peer),
					u"include"_q));
			}
			for (const auto peer : folder.exclude) {
				desiredMembers.insert(Format::MemberId(
					account,
					folder.id,
					WritePeer(peer),
					u"exclude"_q));
			}
			for (auto i = records.size(); i-- > 0;) {
				const auto record = records.at(i).toObject();
				if (record.value(u"kind"_q).toString() == u"member"_q
					&& record.value(u"folder_id"_q).toString() == folder.id
					&& !desiredMembers.contains(
						record.value(u"id"_q).toString())) {
					records.removeAt(i);
				}
			}
			PutRecordPreserving(
				records,
				FindRecord(records, folderId),
				FolderRecord(account, folder));
			for (const auto peer : folder.include) {
				const auto record = MemberRecord(
					account,
					folder.id,
					peer,
					u"include"_q);
				PutRecordPreserving(
					records,
					FindRecord(records, record.value(u"id"_q).toString()),
					record);
			}
			for (const auto peer : folder.exclude) {
				const auto record = MemberRecord(
					account,
					folder.id,
					peer,
					u"exclude"_q);
				PutRecordPreserving(
					records,
					FindRecord(records, record.value(u"id"_q).toString()),
					record);
			}
			module.insert(u"records"_q, records);
			return Result();
		},
		.optimistic = [folder = std::move(folder)](auto &folders) {
			const auto i = ranges::find(folders, folder.id, &LocalChatFolder::id);
			if (i == folders.end()) folders.push_back(folder);
			else *i = folder;
		},
		.done = std::move(done),
	});
}

void LocalChatFolders::setTagsEnabled(bool value, Fn<void(bool)> done) {
	const auto account = SessionAccount(_session);
	mutate({
		.change = [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			const auto record = SettingsRecord(account, value);
			PutRecordPreserving(
				records,
				FindRecord(records, record.value(u"id"_q).toString()),
				record);
			module.insert(u"records"_q, records);
			return Result();
		},
		.optimisticTagsEnabled = [value](auto &tagsEnabled) {
			tagsEnabled = value;
		},
		.done = std::move(done),
	});
}

void LocalChatFolders::setCloudColor(
		QString scope,
		std::optional<uint8> color) {
	if (!Format::ValidCloudColorScope(scope) || (color && *color >= 7)) {
		return fail(
			u"Folder color could not be saved. Please reopen the folder."_q,
			nullptr);
	}
	const auto account = SessionAccount(_session);
	mutate({
		.change = [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			const auto id = Format::CloudColorId(account, scope);
			PutRecordPreserving(records, FindRecord(records, id), {
				{ u"id"_q, id },
				{ u"kind"_q, u"cloud_color"_q },
				{ u"scope"_q, scope },
				{ u"color_index"_q, color ? QJsonValue(*color) : QJsonValue() },
				{ u"deleted"_q, false },
			});
			module.insert(u"records"_q, records);
			return Result();
		},
		.optimisticCloudColors = [=](CloudColors &colors) {
			colors[scope] = color;
		},
	});
}

void LocalChatFolders::remove(QString id, Fn<void(bool)> done) {
	if (!Format::ValidUuid(id)) {
		return fail(u"Invalid local chat folder identity."_q, std::move(done));
	}
	const auto account = SessionAccount(_session);
	mutate({
		.change = [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			for (auto i = records.size(); i-- > 0;) {
				const auto record = records.at(i).toObject();
				const auto kind = record.value(u"kind"_q).toString();
				if ((kind == u"folder"_q || kind == u"member"_q)
					&& record.value(u"folder_id"_q).toString() == id) {
					records.removeAt(i);
				}
			}
			module.insert(u"records"_q, records);
			return Result();
		},
		.optimistic = [=](auto &folders) {
			std::erase_if(folders, [&](const auto &folder) { return folder.id == id; });
		},
		.done = std::move(done),
	});
}

void LocalChatFolders::reorder(
		std::vector<QString> ids,
		Fn<void(bool)> done) {
	mutate({
		.change = [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			for (auto i = 0; i != ids.size(); ++i) {
				for (auto j = 0; j != records.size(); ++j) {
					auto record = records.at(j).toObject();
					if (record.value(u"kind"_q).toString() == u"folder"_q
						&& record.value(u"folder_id"_q).toString() == ids[i]) {
						record.insert(u"position"_q, i);
						records[j] = record;
					}
				}
			}
			module.insert(u"records"_q, records);
			return Result();
		},
		.optimistic = [ids = std::move(ids)](auto &folders) {
			for (auto i = 0; i != ids.size(); ++i) {
				const auto it = ranges::find(folders, ids[i], &LocalChatFolder::id);
				if (it != folders.end()) it->position = i;
			}
		},
		.done = std::move(done),
	});
}

} // namespace Data
