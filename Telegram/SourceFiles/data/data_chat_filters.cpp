/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_chat_filters.h"

#include "data/local_chat_folders.h"
#include "data/local_pinned_dialogs.h"
#include "api/api_text_entities.h"
#include "history/history.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_premium_limits.h"
#include "data/data_folder.h"
#include "data/data_histories.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "history/history_unread_things.h"
#include "ui/ui_utility.h"
#include "ui/chat/more_chats_bar.h"
#include "main/main_session.h"
#include "main/main_app_config.h"
#include "apiwrap.h"
#include "storage/local_data_store.h"
#include "core/application.h"
#include "lang/lang_keys.h"

namespace Data {
namespace {

constexpr auto kRefreshSuggestedTimeout = 7200 * crl::time(1000);
constexpr auto kLoadExceptionsAfter = 100;
constexpr auto kLoadExceptionsPerRequest = 100;
constexpr auto kLocalFolderColorsCount = 7;

[[nodiscard]] crl::time RequestUpdatesEach(not_null<Session*> owner) {
	const auto appConfig = &owner->session().appConfig();
	return appConfig->get<int>(u"chatlist_update_period"_q, 3600)
		* crl::time(1000);
}

} // namespace

TextWithEntities ForceCustomEmojiStatic(TextWithEntities text) {
	for (auto &entity : text.entities) {
		if (entity.type() == EntityType::CustomEmoji) {
			entity = EntityInText(
				EntityType::CustomEmoji,
				entity.offset(),
				entity.length(),
				u"force-static:"_q + entity.data());
		}
	}
	return text;
}

ChatFilter::ChatFilter(
	FilterId id,
	ChatFilterTitle title,
	QString iconEmoji,
	std::optional<uint8> colorIndex,
	Flags flags,
	base::flat_set<not_null<History*>> always,
	std::vector<not_null<History*>> pinned,
	base::flat_set<not_null<History*>> never)
: _id(id)
, _title(std::move(title.text))
, _iconEmoji(std::move(iconEmoji))
, _colorIndex(colorIndex)
, _always(std::move(always))
, _pinned(std::move(pinned))
, _never(std::move(never))
, _flags(title.isStatic
	? (flags | Flag::StaticTitle)
	: (flags & ~Flag::StaticTitle)) {
}

ChatFilter ChatFilter::FromTL(
		const MTPDialogFilter &data,
		not_null<Session*> owner) {
	return data.match([&](const MTPDdialogFilter &data) {
		const auto flags = (data.is_contacts() ? Flag::Contacts : Flag(0))
			| (data.is_non_contacts() ? Flag::NonContacts : Flag(0))
			| (data.is_groups() ? Flag::Groups : Flag(0))
			| (data.is_broadcasts() ? Flag::Channels : Flag(0))
			| (data.is_bots() ? Flag::Bots : Flag(0))
			| (data.is_exclude_muted() ? Flag::NoMuted : Flag(0))
			| (data.is_exclude_read() ? Flag::NoRead : Flag(0))
			| (data.is_exclude_archived() ? Flag::NoArchived : Flag(0))
			| (data.is_title_noanimate() ? Flag::StaticTitle : Flag(0));
		auto &&to_histories = ranges::views::transform([&](
				const MTPInputPeer &input) {
			const auto peer = Data::PeerFromInputMTP(owner, input);
			return peer ? owner->history(peer).get() : nullptr;
		}) | ranges::views::filter([](History *history) {
			return history != nullptr;
		}) | ranges::views::transform([](History *history) {
			return not_null<History*>(history);
		});
		auto &&always = ranges::views::concat(
			data.vinclude_peers().v
		) | to_histories;
		auto pinned = ranges::views::all(
			data.vpinned_peers().v
		) | to_histories | ranges::to_vector;
		auto &&never = ranges::views::all(
			data.vexclude_peers().v
		) | to_histories;
		auto &&all = ranges::views::concat(always, pinned);
		auto list = base::flat_set<not_null<History*>>{
			all.begin(),
			all.end()
		};
		return ChatFilter(
			data.vid().v,
			{
				Api::ParseTextWithEntities(&owner->session(), data.vtitle()),
				data.is_title_noanimate(),
			},
			qs(data.vemoticon().value_or_empty()),
			data.vcolor()
				? std::make_optional(data.vcolor()->v)
				: std::nullopt,
			flags,
			std::move(list),
			std::move(pinned),
			{ never.begin(), never.end() });
	}, [](const MTPDdialogFilterDefault &) {
		return ChatFilter();
	}, [&](const MTPDdialogFilterChatlist &data) {
		auto &&to_histories = ranges::views::transform([&](
				const MTPInputPeer &data) {
			const auto peer = data.match([&](const MTPDinputPeerUser &data) {
				const auto user = owner->user(data.vuser_id().v);
				user->setAccessHash(data.vaccess_hash().v);
				return (PeerData*)user;
			}, [&](const MTPDinputPeerChat &data) {
				return (PeerData*)owner->chat(data.vchat_id().v);
			}, [&](const MTPDinputPeerChannel &data) {
				const auto channel = owner->channel(data.vchannel_id().v);
				channel->setAccessHash(data.vaccess_hash().v);
				return (PeerData*)channel;
			}, [&](const MTPDinputPeerSelf &data) {
				return (PeerData*)owner->session().user();
			}, [&](const auto &data) {
				return (PeerData*)nullptr;
			});
			return peer ? owner->history(peer).get() : nullptr;
		}) | ranges::views::filter([](History *history) {
			return history != nullptr;
		}) | ranges::views::transform([](History *history) {
			return not_null<History*>(history);
		});
		auto &&always = ranges::views::concat(
			data.vinclude_peers().v
		) | to_histories;
		auto pinned = ranges::views::all(
			data.vpinned_peers().v
		) | to_histories | ranges::to_vector;
		auto &&all = ranges::views::concat(always, pinned);
		auto list = base::flat_set<not_null<History*>>{
			all.begin(),
			all.end()
		};
		return ChatFilter(
			data.vid().v,
			{
				Api::ParseTextWithEntities(&owner->session(), data.vtitle()),
				data.is_title_noanimate(),
			},
			qs(data.vemoticon().value_or_empty()),
			data.vcolor()
				? std::make_optional(data.vcolor()->v)
				: std::nullopt,
			(Flag::Chatlist
				| (data.is_has_my_invites() ? Flag::HasMyLinks : Flag())
				| (data.is_title_noanimate() ? Flag::StaticTitle : Flag(0))),
			std::move(list),
			std::move(pinned),
			{});
	});
}

ChatFilter ChatFilter::withId(FilterId id) const {
	auto result = *this;
	result._id = id;
	return result;
}

ChatFilter ChatFilter::withTitle(ChatFilterTitle title) const {
	auto result = *this;
	result._title = std::move(title.text);
	if (title.isStatic) {
		result._flags |= Flag::StaticTitle;
	} else {
		result._flags &= ~Flag::StaticTitle;
	}
	return result;
}

ChatFilter ChatFilter::withColorIndex(std::optional<uint8> c) const {
	auto result = *this;
	result._colorIndex = c;
	result._localColorIndex.reset();
	return result;
}

ChatFilter ChatFilter::withLocalColorIndex(
		std::optional<std::optional<uint8>> color) const {
	auto result = *this;
	result._localColorIndex = color;
	return result;
}

ChatFilter ChatFilter::withColorFrom(const ChatFilter &other) const {
	auto result = *this;
	result._colorIndex = other._colorIndex;
	result._localColorIndex = other._localColorIndex;
	return result;
}

ChatFilter ChatFilter::withChatlist(bool chatlist, bool hasMyLinks) const {
	auto result = *this;
	result._flags &= Flag::RulesMask;
	if (chatlist) {
		result._flags |= Flag::Chatlist;
		if (hasMyLinks) {
			result._flags |= Flag::HasMyLinks;
		} else {
			result._flags &= ~Flag::HasMyLinks;
		}
	}
	return result;
}

ChatFilter ChatFilter::withLocalId(QString id) const {
	auto result = *this;
	result._localId = std::move(id);
	return result;
}

ChatFilter ChatFilter::withoutAlways(not_null<History*> history) const {
	auto result = *this;
	if (CanRemoveFromChatFilter(result, history)) {
		result._always.remove(history);
	}
	return result;
}

MTPDialogFilter ChatFilter::tl(FilterId replaceId) const {
	auto always = _always;
	auto pinned = QVector<MTPInputPeer>();
	pinned.reserve(_pinned.size());
	for (const auto &history : _pinned) {
		pinned.push_back(history->peer->input());
		always.remove(history);
	}
	auto include = QVector<MTPInputPeer>();
	include.reserve(always.size());
	for (const auto &history : always) {
		include.push_back(history->peer->input());
	}
	auto title = MTP_textWithEntities(
		MTP_string(_title.text),
		Api::EntitiesToMTP(
			nullptr,
			_title.entities,
			Api::ConvertOption::SkipLocal));
	if (_flags & Flag::Chatlist) {
		using TLFlag = MTPDdialogFilterChatlist::Flag;
		const auto flags = TLFlag::f_emoticon
			| (_colorIndex ? TLFlag::f_color : TLFlag(0))
			| (staticTitle() ? TLFlag::f_title_noanimate : TLFlag(0));
		return MTP_dialogFilterChatlist(
			MTP_flags(flags),
			MTP_int(replaceId ? replaceId : _id),
			std::move(title),
			MTP_string(_iconEmoji),
			MTP_int(_colorIndex.value_or(0)),
			MTP_vector<MTPInputPeer>(pinned),
			MTP_vector<MTPInputPeer>(include));
	}
	using TLFlag = MTPDdialogFilter::Flag;
	const auto flags = TLFlag::f_emoticon
		| (_colorIndex ? TLFlag::f_color : TLFlag(0))
		| (staticTitle() ? TLFlag::f_title_noanimate : TLFlag(0))
		| ((_flags & Flag::Contacts) ? TLFlag::f_contacts : TLFlag(0))
		| ((_flags & Flag::NonContacts) ? TLFlag::f_non_contacts : TLFlag(0))
		| ((_flags & Flag::Groups) ? TLFlag::f_groups : TLFlag(0))
		| ((_flags & Flag::Channels) ? TLFlag::f_broadcasts : TLFlag(0))
		| ((_flags & Flag::Bots) ? TLFlag::f_bots : TLFlag(0))
		| ((_flags & Flag::NoMuted) ? TLFlag::f_exclude_muted : TLFlag(0))
		| ((_flags & Flag::NoRead) ? TLFlag::f_exclude_read : TLFlag(0))
		| ((_flags & Flag::NoArchived)
			? TLFlag::f_exclude_archived
			: TLFlag(0));
	auto never = QVector<MTPInputPeer>();
	never.reserve(_never.size());
	for (const auto &history : _never) {
		never.push_back(history->peer->input());
	}
	return MTP_dialogFilter(
		MTP_flags(flags),
		MTP_int(replaceId ? replaceId : _id),
		std::move(title),
		MTP_string(_iconEmoji),
		MTP_int(_colorIndex.value_or(0)),
		MTP_vector<MTPInputPeer>(pinned),
		MTP_vector<MTPInputPeer>(include),
		MTP_vector<MTPInputPeer>(never));
}

FilterId ChatFilter::id() const {
	return _id;
}

const TextWithEntities &ChatFilter::titleText() const {
	return _title;
}

ChatFilterTitle ChatFilter::title() const {
	return { _title, !!(_flags & Flag::StaticTitle) };
}

QString ChatFilter::iconEmoji() const {
	return _iconEmoji;
}

std::optional<uint8> ChatFilter::colorIndex() const {
	return _localColorIndex.value_or(_colorIndex);
}

ChatFilter::Flags ChatFilter::flags() const {
	return _flags;
}

bool ChatFilter::staticTitle() const {
	return _flags & Flag::StaticTitle;
}

bool ChatFilter::chatlist() const {
	return _flags & Flag::Chatlist;
}

bool ChatFilter::hasMyLinks() const {
	return _flags & Flag::HasMyLinks;
}

bool ChatFilter::isLocal() const {
	return !_localId.isEmpty();
}

const QString &ChatFilter::localId() const {
	return _localId;
}

const base::flat_set<not_null<History*>> &ChatFilter::always() const {
	return _always;
}

const std::vector<not_null<History*>> &ChatFilter::pinned() const {
	return _pinned;
}

const base::flat_set<not_null<History*>> &ChatFilter::never() const {
	return _never;
}

bool ChatFilter::contains(
		not_null<History*> history,
		bool ignoreFakeUnread) const {
	if (_never.contains(history)) {
		return false;
	}
	if (const auto local = history->owner().localPinnedDialogsLoaded()
		; local && local->includes(_id, history)) {
		return true;
	}
	const auto flag = [&] {
		const auto peer = history->peer;
		if (const auto user = peer->asUser()) {
			return user->isBot()
				? Flag::Bots
				: user->isContact()
				? Flag::Contacts
				: Flag::NonContacts;
		} else if (peer->isChat()) {
			return Flag::Groups;
		} else if (const auto channel = peer->asChannel()) {
			if (channel->isBroadcast()) {
				return Flag::Channels;
			} else {
				return Flag::Groups;
			}
		} else {
			Unexpected("Peer type in ChatFilter::contains.");
		}
	}();
	const auto channel = history->peer->asChannel();
	if (channel && channel->isCommunity()) {
		// A community never matches a filter by chat type (it is neither a
		// group nor a channel); it can only be included explicitly by id.
		return _always.contains(history);
	}
	const auto state = (_flags & (Flag::NoMuted | Flag::NoRead))
		? history->chatListBadgesState()
		: Dialogs::BadgesState();
	return false
		|| ((_flags & flag)
			&& (!(_flags & Flag::NoMuted)
				|| !history->muted()
				|| (state.mention
					&& history->folderKnown()
					&& !history->folder()))
			&& (!(_flags & Flag::NoRead)
				|| state.unread
				|| state.mention
				|| (!ignoreFakeUnread && history->fakeUnreadWhileOpened()))
			&& (!(_flags & Flag::NoArchived)
				|| (history->folderKnown() && !history->folder())))
		|| _always.contains(history);
}

ChatFilters::ChatFilters(not_null<Session*> owner)
: _owner(owner)
, _localFolders(std::make_unique<LocalChatFolders>(nullptr, &owner->session()))
, _moreChatsTimer([=] { checkLoadMoreChatsLists(); }) {
	_list.emplace_back();
	_localFolders->changes() | rpl::on_next([=] {
		_tagsEnabled = _localFolders->tagsEnabled().value_or(_cloudTagsEnabled);
		rebuildLocal();
		if (_lastLocalError != localError()) {
			_lastLocalError = localError();
			if (!_lastLocalError.isEmpty()) {
				_localErrors.fire_copy(_lastLocalError);
			}
		}
		crl::on_main(&owner->session(), [=] {
			if (!localBusy()) {
				Core::App().quitPreventFinished();
			}
		});
	}, _localFoldersLifetime);
	crl::on_main(&owner->session(), [=] {
		load();
	});
}

ChatFilters::~ChatFilters() = default;

not_null<Dialogs::MainList*> ChatFilters::chatsList(FilterId filterId) {
	auto &pointer = _chatsLists[filterId];
	if (!pointer) {
		auto limit = rpl::single(rpl::empty_value()) | rpl::then(
			_owner->session().appConfig().refreshed()
		) | rpl::map([=] {
			return _owner->pinnedChatsLimit(filterId);
		});
		pointer = std::make_unique<Dialogs::MainList>(
			&_owner->session(),
			filterId,
			_owner->maxPinnedChatsLimitValue(filterId));
	}
	if (const auto local = _owner->localPinnedDialogsLoaded()) {
		local->attach(pointer->pinned(), filterId);
	}
	return pointer.get();
}

FilterId ChatFilters::localFilterId(const QString &id) {
	if (const auto i = _localFilterIds.find(id); i != end(_localFilterIds)) {
		return i->second;
	}
	auto result = -1000 - int(qHash(id) & 0x3FFFFFFF);
	const auto collision = [&](FilterId value) {
		return ranges::find(_localFilterIds, value,
			[](const auto &entry) { return entry.second; })
			!= end(_localFilterIds)
			|| ranges::contains(_list, value, &ChatFilter::id);
	};
	while (result == 0 || collision(result)) {
		--result;
	}
	_localFilterIds.emplace(id, result);
	return result;
}

bool ChatFilters::rebuildLocal(bool notify) {
	if (!_localFolders->loaded()) {
		return false;
	}
	auto changed = false;
	for (auto &filter : _list) {
		if (filter.id() > 0) {
			changed = applyChange(filter, applyLocalColor(filter)) || changed;
		}
	}
	auto active = base::flat_set<QString>();
	for (const auto &folder : _localFolders->folders()) {
		active.insert(folder.id);
		const auto id = localFilterId(folder.id);
		auto always = base::flat_set<not_null<History*>>();
		for (const auto peer : folder.include) {
			always.emplace(_owner->history(peer));
		}
		auto never = base::flat_set<not_null<History*>>();
		for (const auto peer : folder.exclude) {
			never.emplace(_owner->history(peer));
		}
		auto title = tr::marked(folder.title);
		for (const auto &value : folder.titleEntities) {
			const auto entity = value.toObject();
			const auto offset = entity.value(u"offset"_q).toInt();
			const auto length = entity.value(u"length"_q).toInt();
			if (entity.value(u"type"_q).toString() == u"custom_emoji"_q
				&& offset >= 0
				&& length > 0
				&& offset <= title.text.size()
				&& length <= title.text.size() - offset) {
				title.entities.emplace_back(
					EntityType::CustomEmoji,
					offset,
					length,
					entity.value(u"data"_q).toString());
			}
		}
		auto filter = ChatFilter(
			id,
			{ std::move(title), folder.staticTitle },
			folder.iconEmoji,
			(folder.colorIndex && *folder.colorIndex < kLocalFolderColorsCount)
				? folder.colorIndex
				: std::nullopt,
			folder.flags,
			std::move(always),
			{},
			std::move(never)).withLocalId(folder.id);
		const auto i = ranges::find(_list, id, &ChatFilter::id);
		if (i == end(_list)) {
			applyInsert(std::move(filter), _list.size());
			changed = true;
		} else {
			changed = applyChange(*i, std::move(filter)) || changed;
		}
	}
	for (auto i = _list.size(); i-- > 0;) {
		if (_list[i].isLocal() && !active.contains(_list[i].localId())) {
			applyRemove(i);
			changed = true;
		}
	}
	auto order = _list | ranges::views::filter([](const ChatFilter &filter) {
		return !filter.isLocal();
	}) | ranges::views::transform(&ChatFilter::id) | ranges::to_vector;
	auto nextPosition = 0;
	for (const auto &folder : _localFolders->folders()) {
		const auto position = std::clamp(
			folder.position,
			nextPosition,
			int(order.size()));
		order.insert(begin(order) + position, localFilterId(folder.id));
		nextPosition = position + 1;
	}
	for (auto position = 0; position != order.size(); ++position) {
		const auto i = ranges::find(_list, order[position], &ChatFilter::id);
		Assert(i != end(_list));
		if (i != begin(_list) + position) {
			std::iter_swap(i, begin(_list) + position);
			changed = true;
		}
	}
	if (changed && notify) {
		_listChanged.fire({});
	}
	return changed;
}

void ChatFilters::clear() {
	_localFoldersLifetime.destroy();
	_localFolders->stop();
	_chatsLists.clear();
	_list.clear();
}

void ChatFilters::setPreloaded(
		const QVector<MTPDialogFilter> &result,
		bool tagsEnabled) {
	_loadRequestId = -1;
	_cloudTagsEnabled = tagsEnabled;
	_tagsEnabled = _localFolders->tagsEnabled().value_or(_cloudTagsEnabled);
	received(result);
	crl::on_main(&_owner->session(), [=] {
		if (_loadRequestId == -1) {
			_loadRequestId = 0;
		}
	});
}

void ChatFilters::load() {
	load(false);
}

void ChatFilters::reload() {
	_reloading = true;
	load();
}

void ChatFilters::load(bool force) {
	if (_loadRequestId && !force) {
		return;
	}
	auto &api = _owner->session().api();
	api.request(_loadRequestId).cancel();
	_loadRequestId = api.request(MTPmessages_GetDialogFilters(
	)).done([=](const MTPmessages_DialogFilters &result) {
		_cloudTagsEnabled = result.data().is_tags_enabled();
		_tagsEnabled = _localFolders->tagsEnabled().value_or(_cloudTagsEnabled);
		received(result.data().vfilters().v);
		_loadRequestId = 0;
	}).fail([=] {
		_loadRequestId = 0;
		if (_reloading) {
			_reloading = false;
			_listChanged.fire({});
		}
	}).send();
}

bool ChatFilters::tagsEnabled() const {
	return _tagsEnabled.current();
}

rpl::producer<bool> ChatFilters::tagsEnabledValue() const {
	return _tagsEnabled.value();
}

rpl::producer<bool> ChatFilters::tagsEnabledChanges() const {
	return _tagsEnabled.changes();
}

void ChatFilters::requestToggleTags(bool value, Fn<void()> fail) {
	_localFolders->setTagsEnabled(value, [fail = std::move(fail)](bool success) {
		if (!success && fail) {
			fail();
		}
	});
}

void ChatFilters::received(const QVector<MTPDialogFilter> &list) {
	auto order = QVector<MTPint>();
	auto changed = false;
	for (const auto &filter : list) {
		auto parsed = ChatFilter::FromTL(filter, _owner);
		order.push_back(MTP_int(parsed.id()));
		const auto i = ranges::find(_list, parsed.id(), &ChatFilter::id);
		if (i == end(_list)) {
			applyInsert(std::move(parsed), _list.size());
			changed = true;
		} else {
			changed = applyChange(*i, std::move(parsed)) || changed;
		}
	}
	if (!ranges::contains(order, 0, &MTPint::v)) {
		order.push_front(MTP_int(0));
	}
	for (auto i = _list.size(); i-- > 0;) {
		if (!_list[i].isLocal()
			&& !ranges::contains(order, _list[i].id(), &MTPint::v)) {
			applyRemove(i);
			changed = true;
		}
	}
	if (!ranges::contains(_list, 0, &ChatFilter::id)) {
		_list.insert(begin(_list), ChatFilter());
		changed = true;
	}
	auto before = _list | ranges::views::transform(&ChatFilter::id)
		| ranges::to_vector;
	applyOrder(order);
	changed = rebuildLocal(false) || changed;
	const auto after = _list | ranges::views::transform(&ChatFilter::id)
		| ranges::to_vector;
	if (changed || before != after || !_loaded || _reloading) {
		_loaded = true;
		_reloading = false;
		_listChanged.fire({});
	}
}

void ChatFilters::apply(const MTPUpdate &update) {
	update.match([&](const MTPDupdateDialogFilter &data) {
		if (const auto filter = data.vfilter()) {
			set(ChatFilter::FromTL(*filter, _owner));
		} else {
			remove(data.vid().v);
		}
	}, [&](const MTPDupdateDialogFilters &data) {
		load(true);
	}, [&](const MTPDupdateDialogFilterOrder &data) {
		if (applyOrder(data.vorder().v)) {
			_listChanged.fire({});
		} else {
			load(true);
		}
	}, [](auto&&) {
		Unexpected("Update in ChatFilters::apply.");
	});
}

ChatFilterLink ChatFilters::add(
		FilterId id,
		const MTPExportedChatlistInvite &update) {
	const auto i = ranges::find(_list, id, &ChatFilter::id);
	if (i == end(_list) || !i->chatlist()) {
		LOG(("Api Error: "
			"Attempt to add chatlist link to a non-chatlist filter: %1"
			).arg(id));
		return {};
	}
	auto &links = _chatlistLinks[id];
	const auto &data = update.data();
	const auto url = qs(data.vurl());
	const auto title = qs(data.vtitle());
	auto chats = data.vpeers().v | ranges::views::transform([&](
			const MTPPeer &peer) {
		return _owner->history(peerFromMTP(peer));
	}) | ranges::to_vector;
	const auto j = ranges::find(links, url, &ChatFilterLink::url);
	if (j != end(links)) {
		if (j->title != title || j->chats != chats) {
			j->title = title;
			j->chats = std::move(chats);
			_chatlistLinksUpdated.fire_copy(id);
		}
		return *j;
	}
	links.push_back({
		.id = id,
		.url = url,
		.title = title,
		.chats = std::move(chats),
	});
	_chatlistLinksUpdated.fire_copy(id);
	return links.back();
}

void ChatFilters::edit(
		FilterId id,
		const QString &url,
		const QString &title) {
	auto &links = _chatlistLinks[id];
	const auto i = ranges::find(links, url, &ChatFilterLink::url);
	if (i != end(links)) {
		i->title = title;
		_chatlistLinksUpdated.fire_copy(id);

		_owner->session().api().request(MTPchatlists_EditExportedInvite(
			MTP_flags(MTPchatlists_EditExportedInvite::Flag::f_title),
			MTP_inputChatlistDialogFilter(MTP_int(id)),
			MTP_string(url),
			MTP_string(title),
			MTPVector<MTPInputPeer>() // peers
		)).done([=](const MTPExportedChatlistInvite &result) {
			//const auto &data = result.data();
			//const auto link = _owner->chatsFilters().add(id, result);
			//done(link);
		}).fail([=](const MTP::Error &error) {
			//done({ .id = id });
		}).send();
	}
}

void ChatFilters::destroy(FilterId id, const QString &url) {
	auto &links = _chatlistLinks[id];
	const auto i = ranges::find(links, url, &ChatFilterLink::url);
	if (i != end(links)) {
		links.erase(i);
		_chatlistLinksUpdated.fire_copy(id);

		const auto api = &_owner->session().api();
		api->request(_linksRequestId).cancel();
		_linksRequestId = api->request(MTPchatlists_DeleteExportedInvite(
			MTP_inputChatlistDialogFilter(MTP_int(id)),
			MTP_string(url)
		)).send();
	}
}

rpl::producer<std::vector<ChatFilterLink>> ChatFilters::chatlistLinks(
		FilterId id) const {
	return _chatlistLinksUpdated.events_starting_with_copy(
		id
	) | rpl::filter(rpl::mappers::_1 == id) | rpl::map([=] {
		const auto i = _chatlistLinks.find(id);
		return (i != end(_chatlistLinks))
			? i->second
			: std::vector<ChatFilterLink>();
	});
}

void ChatFilters::reloadChatlistLinks(FilterId id) {
	const auto api = &_owner->session().api();
	api->request(_linksRequestId).cancel();
	_linksRequestId = api->request(MTPchatlists_GetExportedInvites(
		MTP_inputChatlistDialogFilter(MTP_int(id))
	)).done([=](const MTPchatlists_ExportedInvites &result) {
		const auto &data = result.data();
		_owner->processUsers(data.vusers());
		_owner->processChats(data.vchats());
		_chatlistLinks[id].clear();
		for (const auto &link : data.vinvites().v) {
			add(id, link);
		}
		_chatlistLinksUpdated.fire_copy(id);
	}).send();
}

void ChatFilters::set(ChatFilter filter) {
	if (!filter.id()) {
		return;
	}
	const auto i = ranges::find(_list, filter.id(), &ChatFilter::id);
	if (i == end(_list)) {
		applyInsert(std::move(filter), _list.size());
		rebuildLocal(false);
		_listChanged.fire({});
	} else if (applyChange(*i, std::move(filter))) {
		_listChanged.fire({});
	}
}

void ChatFilters::applyInsert(ChatFilter filter, int position) {
	Expects(position >= 0 && position <= _list.size());

	_list.insert(
		begin(_list) + position,
		ChatFilter(filter.id(), {}, {}, {}, {}, {}, {}, {}));
	applyChange(*(begin(_list) + position), std::move(filter));
}

void ChatFilters::remove(FilterId id) {
	const auto i = ranges::find(_list, id, &ChatFilter::id);
	if (i == end(_list)) {
		return;
	}
	applyRemove(i - begin(_list));
	rebuildLocal(false);
	_listChanged.fire({});
}

void ChatFilters::moveAllToFront() {
	const auto i = ranges::find(_list, FilterId(), &ChatFilter::id);
	if (!_list.empty() && i == begin(_list)) {
		return;
	} else if (i != end(_list)) {
		_list.erase(i);
	}
	_list.insert(begin(_list), ChatFilter());
}

void ChatFilters::applyRemove(int position) {
	Expects(position >= 0 && position < _list.size());

	// Remove the filter from the list before applyChange() tears down its
	// chats. Unpinning a chat re-caches its siblings' pinned indices, which
	// runs Session::refreshChatListEntry(); while the filter is still listed
	// but emptied that re-enters PinnedList::setPinned() on the same pinned
	// list mid-iteration and crashes (see also PinnedList::setPinned).
	const auto i = begin(_list) + position;
	auto filter = std::move(*i);
	_list.erase(i);
	applyChange(filter, ChatFilter(filter.id(), {}, {}, {}, {}, {}, {}, {}));
}

bool ChatFilters::applyChange(ChatFilter &filter, ChatFilter &&updated) {
	Expects(filter.id() == updated.id());

	using Flag = ChatFilter::Flag;

	updated = applyLocalColor(std::move(updated));
	const auto id = filter.id();
	const auto exceptionsChanged = filter.always() != updated.always();
	const auto rulesMask = Flag() | Flag::RulesMask;
	const auto rulesChanged = exceptionsChanged
		|| ((filter.flags() & rulesMask) != (updated.flags() & rulesMask))
		|| (filter.never() != updated.never());
	const auto pinnedChanged = (filter.pinned() != updated.pinned());
	const auto chatlistChanged = (filter.chatlist() != updated.chatlist())
		|| (filter.hasMyLinks() != updated.hasMyLinks());
	const auto listUpdated = rulesChanged
		|| pinnedChanged
		|| (filter.titleText() != updated.titleText())
		|| (filter.staticTitle() != updated.staticTitle())
		|| (filter.iconEmoji() != updated.iconEmoji());
	const auto colorChanged = filter.colorIndex() != updated.colorIndex();
	const auto colorExistenceChanged = (!filter.colorIndex())
		!= (!updated.colorIndex());
	if (!listUpdated && !chatlistChanged && !colorChanged) {
		filter = std::move(updated);
		return false;
	}
	const auto wasFilter = std::move(filter);
	filter = std::move(updated);
	auto entryToRefreshHeight = (Dialogs::Entry*)(nullptr);
	if (rulesChanged) {
		const auto filterList = _owner->chatsFilters().chatsList(id);
		const auto areTagsEnabled = tagsEnabled();
		const auto tagsExistence = [&](not_null<Dialogs::Row*> row) {
			return (!areTagsEnabled || entryToRefreshHeight)
				? false
				: row->entry()->hasChatsFilterTags(0);
		};
		const auto feedHistory = [&](not_null<History*> history) {
			const auto now = filter.contains(history);
			const auto was = wasFilter.contains(history);
			if (now != was) {
				if (now) {
					history->addToChatList(id, filterList);
				} else {
					history->removeFromChatList(id, filterList);
				}
			}
		};
		const auto feedList = [&](not_null<const Dialogs::MainList*> list) {
			for (const auto &entry : *list->indexed()) {
				if (const auto history = entry->history()) {
					const auto wasTags = tagsExistence(entry);
					feedHistory(history);
					if (wasTags != tagsExistence(entry)) {
						entryToRefreshHeight = entry->entry();
					}
				}
			}
		};
		feedList(_owner->chatsList());
		if (const auto folder = _owner->folderLoaded(Data::Folder::kId)) {
			feedList(folder->chatsList());
		}
		if (exceptionsChanged && !filter.always().empty()) {
			_exceptionsToLoad.push_back(id);
			Ui::PostponeCall(&_owner->session(), [=] {
				_owner->session().api().requestMoreDialogsIfNeeded();
			});
		}
	}
	if (pinnedChanged) {
		const auto filterList = _owner->chatsFilters().chatsList(id);
		filterList->pinned()->applyList(filter.pinned());
	}
	if (chatlistChanged) {
		_isChatlistChanged.fire_copy(id);
	}
	if (colorChanged) {
		_tagColorChanged.fire_copy(TagColorChanged{
			.filterId = id,
			.colorExistenceChanged = colorExistenceChanged,
		});
	}
	if (entryToRefreshHeight) {
		// Trigger a full refresh of height for the main list.
		entryToRefreshHeight->updateChatListEntryHeight();
	}
	return listUpdated || colorChanged;
}

bool ChatFilters::applyOrder(const QVector<MTPint> &order) {
	auto cloud = _list | ranges::views::filter([](const ChatFilter &filter) {
		return !filter.isLocal();
	}) | ranges::to_vector;
	if (order.size() != cloud.size()) {
		return false;
	}
	for (auto position = 0; position != order.size(); ++position) {
		const auto i = ranges::find(
			begin(cloud) + position,
			end(cloud),
			order[position].v,
			&ChatFilter::id);
		if (i == end(cloud)) {
			return false;
		}
		std::iter_swap(i, begin(cloud) + position);
	}
	auto i = begin(cloud);
	for (auto &filter : _list) {
		if (!filter.isLocal()) {
			filter = std::move(*i++);
		}
	}
	return true;
}

const ChatFilter &ChatFilters::applyUpdatedPinned(
		FilterId id,
		const std::vector<Dialogs::Key> &dialogs) {
	const auto i = ranges::find(_list, id, &ChatFilter::id);
	Assert(i != end(_list));

	const auto limit = _owner->pinnedChatsLimit(id);
	auto always = i->always();
	auto pinned = std::vector<not_null<History*>>();
	pinned.reserve(dialogs.size());
	for (const auto &row : dialogs) {
		if (const auto history = row.history()) {
			if (always.contains(history)) {
				pinned.push_back(history);
			} else if (always.size() < limit) {
				always.insert(history);
				pinned.push_back(history);
			}
		}
	}
	set(ChatFilter(
		id,
		i->title(),
		i->iconEmoji(),
		i->colorIndex(),
		i->flags(),
		std::move(always),
		std::move(pinned),
		i->never()).withLocalId(i->localId()).withColorFrom(*i));
	return *i;
}

void ChatFilters::saveOrder(
		const std::vector<FilterId> &order,
		mtpRequestId after) {
	const auto hasLocal = ranges::any_of(order, [=](FilterId id) {
		return isLocal(id);
	});
	auto cloudOrder = order | ranges::views::filter([](FilterId id) {
		return id >= 0;
	}) | ranges::to_vector;
	auto currentCloudOrder = _list
		| ranges::views::filter([](const ChatFilter &filter) {
			return !filter.isLocal();
		}) | ranges::views::transform(&ChatFilter::id) | ranges::to_vector;
	if (hasLocal) {
		reorderLocal(order);
		crl::on_main(&_owner->session(), [=] {
			_listChanged.fire({});
		});
	}
	if (!after && cloudOrder == currentCloudOrder) {
		return;
	}
	if (after) {
		_saveOrderAfterId = after;
	}
	const auto api = &_owner->session().api();
	api->request(_saveOrderRequestId).cancel();

	auto ids = QVector<MTPint>();
	ids.reserve(cloudOrder.size());
	for (const auto id : cloudOrder) {
		ids.push_back(MTP_int(id));
	}
	const auto wrapped = MTP_vector<MTPint>(ids);

	apply(MTP_updateDialogFilterOrder(wrapped));
	_saveOrderRequestId = api->request(MTPmessages_UpdateDialogFiltersOrder(
		wrapped
	)).afterRequest(_saveOrderAfterId).send();
}

bool ChatFilters::archiveNeeded() const {
	for (const auto &filter : _list) {
		if (!(filter.flags() & ChatFilter::Flag::NoArchived)) {
			return true;
		}
	}
	return false;
}

const std::vector<ChatFilter> &ChatFilters::list() const {
	return _list;
}

FilterId ChatFilters::defaultId() const {
	return lookupId(0);
}

FilterId ChatFilters::lookupId(int index) const {
	Expects(index >= 0 && index < _list.size());

	if (_owner->session().user()->isPremium() || !_list.front().id()) {
		return _list[index].id();
	}
	const auto i = ranges::find(_list, FilterId(0), &ChatFilter::id);
	return !index
		? FilterId()
		: (index <= int(i - begin(_list)))
		? _list[index - 1].id()
		: _list[index].id();
}

bool ChatFilters::loaded() const {
	return _loaded;
}

bool ChatFilters::has() const {
	return _list.size() > 1;
}

bool ChatFilters::isLocal(FilterId id) const {
	const auto i = ranges::find(_list, id, &ChatFilter::id);
	return i != end(_list) && i->isLocal();
}

QString ChatFilters::localId(FilterId id) const {
	const auto i = ranges::find(_list, id, &ChatFilter::id);
	return i != end(_list) ? i->localId() : QString();
}

bool ChatFilters::isLocked(FilterId id) const {
	if (!id || isLocal(id)) {
		return false;
	}
	auto cloud = 0;
	for (const auto &filter : _list) {
		if (!filter.id() || filter.isLocal()) {
			continue;
		}
		if (filter.id() == id) {
			return cloud >= Data::PremiumLimits(&_owner->session())
				.dialogFiltersCurrent();
		}
		++cloud;
	}
	return false;
}

bool ChatFilters::localBusy() const {
	return _localFolders->busy();
}

const QString &ChatFilters::localError() const {
	return _localFolders->error();
}

rpl::producer<QString> ChatFilters::localErrors() const {
	return _localErrors.events();
}

ChatFilter ChatFilters::makeLocal(ChatFilter filter) {
	if (filter.isLocal()) {
		return filter;
	}
	const auto id = Storage::LocalData::NewId();
	return filter.withColorIndex(filter.colorIndex())
		.withId(localFilterId(id)).withLocalId(id);
}

ChatFilter ChatFilters::applyLocalColor(ChatFilter filter) const {
	if (filter.id() <= 0) {
		return filter;
	}
	const auto pins = _owner->localPinnedDialogsLoaded();
	const auto scope = pins ? pins->filterScope(filter.id()) : QString();
	return filter.withLocalColorIndex(_localFolders->cloudColor(scope));
}

void ChatFilters::refreshLocalColors() {
	if (!_localFolders->loaded()) {
		return;
	}
	auto changed = false;
	for (auto &filter : _list) {
		if (filter.id() > 0) {
			changed = applyChange(filter, applyLocalColor(filter)) || changed;
		}
	}
	if (changed) {
		_listChanged.fire({});
	}
}

void ChatFilters::saveCloudColor(FilterId id, std::optional<uint8> color) {
	if (id <= 0 || !ranges::contains(_list, id, &ChatFilter::id)) {
		return;
	}
	_localFolders->setCloudColor(
		_owner->localPinnedDialogs().filterScope(id),
		color);
}

void ChatFilters::saveLocal(ChatFilter filter, Fn<void(bool)> done) {
	if (!filter.isLocal() || filter.id() >= 0) {
		if (done) {
			done(false);
		}
		return;
	}
	const auto i = ranges::find(
		_localFolders->folders(),
		filter.localId(),
		&LocalChatFolder::id);
	auto folder = (i != end(_localFolders->folders()))
		? *i
		: LocalChatFolder();
	if (i == end(_localFolders->folders())) {
		folder.position = _list.size();
	}
	folder.id = filter.localId();
	folder.title = filter.titleText().text;
	folder.staticTitle = filter.staticTitle();
	auto entities = QJsonArray();
	for (const auto &entity : folder.titleEntities) {
		if (entity.toObject().value(u"type"_q).toString()
			!= u"custom_emoji"_q) {
			entities.push_back(entity);
		}
	}
	for (const auto &entity : filter.titleText().entities) {
		if (entity.type() == EntityType::CustomEmoji) {
			entities.push_back(QJsonObject{
				{ u"type"_q, u"custom_emoji"_q },
				{ u"offset"_q, entity.offset() },
				{ u"length"_q, entity.length() },
				{ u"data"_q, entity.data() },
			});
		}
	}
	folder.titleEntities = std::move(entities);
	folder.iconEmoji = filter.iconEmoji();
	if (filter.colorIndex() || !folder.colorIndex
		|| *folder.colorIndex < kLocalFolderColorsCount) {
		folder.colorIndex = filter.colorIndex();
	}
	folder.flags = filter.flags() & ChatFilter::Flag::RulesMask;
	folder.include.clear();
	folder.exclude.clear();
	for (const auto history : filter.always()) {
		folder.include.push_back(history->peer->id);
	}
	auto unpin = std::vector<PeerId>();
	if (i != end(_localFolders->folders())) {
		for (const auto peer : i->include) {
			if (!ranges::contains(folder.include, peer)) {
				unpin.push_back(peer);
			}
		}
	}
	for (const auto history : filter.never()) {
		folder.exclude.push_back(history->peer->id);
		if (i != end(_localFolders->folders())
			&& !ranges::contains(i->exclude, history->peer->id)) {
			unpin.push_back(history->peer->id);
		}
	}
	_localFolders->save(std::move(folder), crl::guard(
		&_owner->session(),
		[=, done = std::move(done)](bool success) {
			if (success && isLocal(filter.id())) {
				for (const auto peer : unpin) {
					const auto pins = &_owner->localPinnedDialogs();
					if (pins->isPinned(filter.id(), peer)) {
						pins->setPinned(_owner->history(peer), filter.id(), false);
					}
				}
			}
			if (done) {
				done(success);
			}
		}));
}

void ChatFilters::removeLocal(FilterId id, Fn<void(bool)> done) {
	const auto uuid = localId(id);
	if (uuid.isEmpty()) {
		if (done) done(false);
		return;
	}
	_localFolders->remove(uuid, std::move(done));
}

void ChatFilters::reorderLocal(
		std::vector<FilterId> order,
		Fn<void(bool)> done) {
	std::vector<QString> ids;
	ids.reserve(order.size());
	for (const auto id : order) {
		if (const auto uuid = localId(id); !uuid.isEmpty()) {
			ids.push_back(uuid);
		} else {
			ids.push_back(QString());
		}
	}
	_localFolders->reorder(std::move(ids), std::move(done));
}

rpl::producer<> ChatFilters::changed() const {
	return _listChanged.events();
}

rpl::producer<FilterId> ChatFilters::isChatlistChanged() const {
	return _isChatlistChanged.events();
}

rpl::producer<TagColorChanged> ChatFilters::tagColorChanged() const {
	return _tagColorChanged.events();
}

bool ChatFilters::loadNextExceptions(bool chatsListLoaded) {
	if (_exceptionsLoadRequestId) {
		return true;
	} else if (!chatsListLoaded
		&& (_owner->chatsList()->fullSize().current()
			< kLoadExceptionsAfter)) {
		return false;
	}
	auto queued = base::flat_set<PeerId>(
		begin(_exceptionPeersToLoad),
		end(_exceptionPeersToLoad));
	while (!_exceptionsToLoad.empty()) {
		const auto id = _exceptionsToLoad.front();
		_exceptionsToLoad.pop_front();
		const auto i = ranges::find(_list, id, &ChatFilter::id);
		if (i == end(_list)) {
			continue;
		}
		for (const auto history : i->always()) {
			if (!history->folderKnown()
				&& queued.emplace(history->peer->id).second) {
				_exceptionPeersToLoad.push_back(history->peer->id);
			}
		}
	}
	auto inputs = QVector<MTPInputDialogPeer>();
	while (!_exceptionPeersToLoad.empty()
		&& inputs.size() < kLoadExceptionsPerRequest) {
		const auto peer = _exceptionPeersToLoad.front();
		_exceptionPeersToLoad.pop_front();
		const auto history = _owner->history(peer);
		const auto input = history->peer->input();
		if (!history->folderKnown() && input.type() != mtpc_inputPeerEmpty) {
			inputs.push_back(MTP_inputDialogPeer(input));
		}
	}
	if (inputs.isEmpty()) {
		return false;
	}
	const auto api = &_owner->session().api();
	_exceptionsLoadRequestId = api->request(MTPmessages_GetPeerDialogs(
		MTP_vector(inputs)
	)).done([=](const MTPmessages_PeerDialogs &result) {
		_exceptionsLoadRequestId = 0;
		_owner->session().data().histories().applyPeerDialogs(result);
		_owner->session().api().requestMoreDialogsIfNeeded();
	}).fail([=] {
		_exceptionsLoadRequestId = 0;
		_owner->session().api().requestMoreDialogsIfNeeded();
	}).send();
	return true;
}

void ChatFilters::refreshHistory(not_null<History*> history) {
	if (history->inChatList() && !list().empty()) {
		_owner->refreshChatListEntry(history);
	}
}

void ChatFilters::requestSuggested() {
	if (_suggestedRequestId) {
		return;
	}
	if (_suggestedLastReceived > 0
		&& crl::now() - _suggestedLastReceived < kRefreshSuggestedTimeout) {
		return;
	}
	const auto api = &_owner->session().api();
	_suggestedRequestId = api->request(MTPmessages_GetSuggestedDialogFilters(
	)).done([=](const MTPVector<MTPDialogFilterSuggested> &data) {
		_suggestedRequestId = 0;
		_suggestedLastReceived = crl::now();

		_suggested = ranges::views::all(
			data.v
		) | ranges::views::transform([&](const MTPDialogFilterSuggested &f) {
			return f.match([&](const MTPDdialogFilterSuggested &data) {
				return SuggestedFilter{
					Data::ChatFilter::FromTL(data.vfilter(), _owner),
					qs(data.vdescription())
				};
			});
		}) | ranges::to_vector;

		_suggestedUpdated.fire({});
	}).fail([=] {
		_suggestedRequestId = 0;
		_suggestedLastReceived = crl::now() + kRefreshSuggestedTimeout / 2;

		_suggestedUpdated.fire({});
	}).send();
}

bool ChatFilters::suggestedLoaded() const {
	return (_suggestedLastReceived > 0);
}

const std::vector<SuggestedFilter> &ChatFilters::suggestedFilters() const {
	return _suggested;
}

rpl::producer<> ChatFilters::suggestedUpdated() const {
	return _suggestedUpdated.events();
}

rpl::producer<Ui::MoreChatsBarContent> ChatFilters::moreChatsContent(
		FilterId id) {
	if (!id) {
		return rpl::single(Ui::MoreChatsBarContent{ .count = 0 });
	}
	return [=](auto consumer) {
		auto result = rpl::lifetime();

		auto &entry = _moreChatsData[id];
		auto watching = entry.watching.lock();
		if (!watching) {
			watching = std::make_shared<bool>(true);
			entry.watching = watching;
		}
		result.add([watching] {});

		_moreChatsUpdated.events_starting_with_copy(
			id
		) | rpl::on_next([=] {
			consumer.put_next(Ui::MoreChatsBarContent{
				.count = int(moreChats(id).size()),
			});
		}, result);
		loadMoreChatsList(id);

		return result;
	};
}

const std::vector<not_null<PeerData*>> &ChatFilters::moreChats(
		FilterId id) const {
	static const auto kEmpty = std::vector<not_null<PeerData*>>();
	if (!id) {
		return kEmpty;
	}
	const auto i = _moreChatsData.find(id);
	return (i != end(_moreChatsData)) ? i->second.missing : kEmpty;
}

void ChatFilters::moreChatsHide(FilterId id, bool localOnly) {
	if (!localOnly) {
		const auto api = &_owner->session().api();
		api->request(MTPchatlists_HideChatlistUpdates(
			MTP_inputChatlistDialogFilter(MTP_int(id))
		)).send();
	}

	const auto i = _moreChatsData.find(id);
	if (i != end(_moreChatsData)) {
		if (const auto requestId = base::take(i->second.requestId)) {
			_owner->session().api().request(requestId).cancel();
		}
		i->second.missing = {};
		i->second.lastUpdate = crl::now();
		_moreChatsUpdated.fire_copy(id);
	}
}

void ChatFilters::loadMoreChatsList(FilterId id) {
	Expects(id != 0);

	const auto i = ranges::find(_list, id, &ChatFilter::id);
	if (i == end(_list) || !i->chatlist()) {
		return;
	}

	auto &entry = _moreChatsData[id];
	const auto now = crl::now();
	if (!entry.watching.lock() || entry.requestId) {
		return;
	}
	const auto last = entry.lastUpdate;
	const auto next = last ? (last + RequestUpdatesEach(_owner)) : 0;
	if (next > now) {
		if (!_moreChatsTimer.isActive()) {
			_moreChatsTimer.callOnce(next - now);
		}
		return;
	}
	auto &api = _owner->session().api();
	entry.requestId = api.request(MTPchatlists_GetChatlistUpdates(
		MTP_inputChatlistDialogFilter(MTP_int(id))
	)).done([=](const MTPchatlists_ChatlistUpdates &result) {
		const auto &data = result.data();
		_owner->processUsers(data.vusers());
		_owner->processChats(data.vchats());
		auto list = ranges::views::all(
			data.vmissing_peers().v
		) | ranges::views::transform([&](const MTPPeer &peer) {
			return _owner->peer(peerFromMTP(peer));
		}) | ranges::to_vector;

		auto &entry = _moreChatsData[id];
		entry.requestId = 0;
		entry.lastUpdate = crl::now();
		if (!_moreChatsTimer.isActive()) {
			_moreChatsTimer.callOnce(RequestUpdatesEach(_owner));
		}
		if (entry.missing != list) {
			entry.missing = std::move(list);
			_moreChatsUpdated.fire_copy(id);
		}
	}).fail([=] {
		auto &entry = _moreChatsData[id];
		entry.requestId = 0;
		entry.lastUpdate = crl::now();
	}).send();
}

void ChatFilters::checkLoadMoreChatsLists() {
	for (const auto &[id, entry] : _moreChatsData) {
		loadMoreChatsList(id);
	}
}

bool CanRemoveFromChatFilter(
		const ChatFilter &filter,
		not_null<History*> history) {
	using Flag = ChatFilter::Flag;
	const auto flagsWithoutNoReadNoArchivedNoMuted = filter.flags()
		& ~(Flag::NoRead | Flag::NoArchived | Flag::NoMuted);
	return (filter.always().size() > 1 || flagsWithoutNoReadNoArchivedNoMuted)
		&& filter.contains(history);
}

} // namespace Data
