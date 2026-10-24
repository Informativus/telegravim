/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_pinned_dialogs.h"

#include "apiwrap.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_filters.h"
#include "data/data_community.h"
#include "data/data_folder.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_saved_messages.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "data/local_pinned_items.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_pinned_list.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mtproto/sender.h"

#include <crl/crl_on_main.h>
#include <QtCore/QPointer>

namespace Data {
namespace {

constexpr auto kConcurrentHydration = 4;
constexpr auto kHydrationAttempts = 5;
constexpr auto kHydrationBackoff = crl::time(30000);

[[nodiscard]] PeerId KeyPeer(Dialogs::Key key) {
	if (const auto history = key.history()) {
		return history->peer->id;
	} else if (const auto sublist = key.sublist()) {
		return sublist->sublistPeer()->id;
	}
	return PeerId();
}

[[nodiscard]] std::vector<PeerId> Peers(const std::vector<Dialogs::Key> &keys) {
	auto result = std::vector<PeerId>();
	result.reserve(keys.size());
	for (const auto key : keys) {
		if (const auto peer = KeyPeer(key)) {
			result.push_back(peer);
		}
	}
	return result;
}

[[nodiscard]] bool Eligible(not_null<History*> history, bool confirmed) {
	const auto peer = history->peer;
	if (!peer->isLoaded()
		|| peer->migrateTo()
		|| !history->folderKnown()) {
		return false;
	} else if (const auto channel = peer->asChannel()) {
		if ((!channel->amIn() && !history->isTopPromoted())
			|| (channel->flags() & ChannelDataFlag::Forbidden)) {
			return false;
		}
	}
	const auto community = history->communityListInfo();
	if (community
		&& community->collapsedInChatLists()
		&& community->channel() != peer) {
		return false;
	}
	const auto chat = peer->asChat();
	const auto channel = peer->asChannel();
	return confirmed
		|| history->inChatList()
		|| history->isPinnedDialog(FilterId())
		|| history->chatListMessage()
		|| (chat && chat->amIn())
		|| (channel && channel->amIn());
}

} // namespace

LocalPinnedDialogs::LocalPinnedDialogs(not_null<Session*> owner)
: _owner(owner)
, _items(std::make_unique<LocalPinnedItems>(this, &owner->session()))
, _api(std::make_unique<MTP::Sender>(&owner->session().mtp()))
, _hydrateTimer([=] { scheduleRefresh(); }) {
	_items->changes() | rpl::on_next([=] {
		_owner->chatsFilters().refreshLocalColors();
		const auto error = _items->error();
		if (error != _error) {
			_error = error;
			if (!error.isEmpty()) {
				_errors.fire_copy(error);
			}
		}
		scheduleRefresh();
		if (!_items->busy() && Core::Quitting()) {
			Core::App().quitPreventFinished();
		}
	}, _lifetime);

	owner->chatsFilters().changed() | rpl::on_next([=] {
		syncFilters();
		scheduleRefresh();
	}, _lifetime);
	owner->chatsListChanges() | rpl::on_next([=] {
		scheduleRefresh();
	}, _lifetime);
	owner->chatListEntryRefreshes() | rpl::on_next([=](const auto &update) {
		if (_watched.contains(KeyPeer(update.key))) {
			scheduleRefresh();
		}
	}, _lifetime);
	owner->savedMessages().chatsListChanges() | rpl::on_next([=] {
		scheduleRefresh();
	}, _lifetime);
	owner->session().changes().historyUpdates(
		HistoryUpdate::Flag::Folder
	) | rpl::on_next([=](const HistoryUpdate &update) {
		if (_watched.contains(update.history->peer->id)) {
			scheduleRefresh();
		}
	}, _lifetime);
	owner->session().changes().messageUpdates(
		MessageUpdate::Flag::DialogRowRefresh | MessageUpdate::Flag::Destroyed
	) | rpl::on_next([=](const MessageUpdate &update) {
		const auto peer = update.item->history()->peer->id;
		if (_watched.contains(peer) || peer == owner->session().userPeerId()) {
			scheduleRefresh();
		}
	}, _lifetime);
	owner->session().changes().peerUpdates(
		PeerUpdate::Flag::Name
		| PeerUpdate::Flag::Migration
		| PeerUpdate::Flag::ChannelAmIn
		| PeerUpdate::Flag::UnavailableReason
	) | rpl::on_next([=](const PeerUpdate &update) {
		if (_watched.contains(update.peer->id)) {
			scheduleRefresh();
		}
	}, _lifetime);
	crl::on_main(this, [=] {
		if (_stopped) {
			return;
		}
		_ready = true;
		_items->reload();
		refresh();
	});
}

LocalPinnedDialogs::~LocalPinnedDialogs() = default;

bool LocalPinnedDialogs::supports(Dialogs::Key key, FilterId filterId) const {
	if (_stopped || !key) {
		return false;
	} else if (const auto history = key.history()) {
		const auto community = history->communityListInfo();
		return filterId
			|| !community
			|| !community->collapsedInChatLists()
			|| community->channel() == history->peer;
	} else if (const auto sublist = key.sublist()) {
		return !filterId && !sublist->parentChat();
	}
	return false;
}

QString LocalPinnedDialogs::filterScope(FilterId filterId) const {
	const auto id = _owner->chatsFilters().localId(filterId);
	return !id.isEmpty()
		? u"local:"_q + id
		: (filterId > 0 ? _items->filterScope(filterId) : QString());
}

QString LocalPinnedDialogs::scope(Dialogs::Key key, FilterId filterId) const {
	if (!supports(key, filterId)) {
		return QString();
	} else if (filterId) {
		return filterScope(filterId);
	} else if (key.sublist()) {
		return u"saved"_q;
	}
	return key.entry()->folder() ? u"archive"_q : u"main"_q;
}

bool LocalPinnedDialogs::includes(
		FilterId filterId,
		not_null<History*> history) const {
	if (!_items->loaded()
		|| _removed.contains(history->peer->id)
		|| !Eligible(history, _confirmed.contains(history->peer->id))) {
		return false;
	}
	return isPinned(filterId, history->peer->id);
}

bool LocalPinnedDialogs::isPinned(FilterId filterId, PeerId peer) const {
	const auto scope = filterScope(filterId);
	return !scope.isEmpty() && _items->overridden(scope, peer).value_or(false);
}

bool LocalPinnedDialogs::busy() const {
	return _items->busy();
}

rpl::producer<QString> LocalPinnedDialogs::errors() const {
	return _errors.events();
}

void LocalPinnedDialogs::setPinned(
		Dialogs::Key key,
		FilterId filterId,
		bool pinned,
		Fn<void(bool)> done) {
	if (!supports(key, filterId)) {
		if (done) {
			done(false);
		}
		return;
	}
	if (pinned && key.history()) {
		_removed.erase(KeyPeer(key));
		_confirmed.insert(KeyPeer(key));
	}
	const auto list = filterId
		? _owner->chatsFilters().chatsList(filterId)
		: _owner->chatsListFor(key.entry());
	_items->setPinned(
		scope(key, filterId),
		KeyPeer(key),
		pinned,
		Peers(list->pinned()->order()),
		crl::guard(this, [=](bool success) {
			refresh();
			if (done) {
				done(success);
			}
		}));
	refresh();
}

void LocalPinnedDialogs::remove(PeerId peer) {
	if (_stopped) {
		return;
	}
	_removed.insert(peer);
	_confirmed.erase(peer);
	if (const auto i = _hydration.find({ false, peer })
		; i != end(_hydration) && i->second.requestId) {
		_owner->histories().cancelRequest(i->second.requestId);
		i->second.requestId = 0;
		i->second.pending = false;
		--_hydrating;
	}
	syncFilters();
	_items->removePeer(peer, crl::guard(this, [=](bool success) {
		if (success) {
			_removed.erase(peer);
		}
		refresh();
	}));
	refresh();
}

void LocalPinnedDialogs::storeOrder(
		const QString &scope,
		not_null<Dialogs::PinnedList*> list) {
	_items->saveOrder(scope, Peers(list->order()), crl::guard(this, [=](bool) {
		refresh();
	}));
}

void LocalPinnedDialogs::saveOrder(Folder *folder) {
	storeOrder(folder ? u"archive"_q : u"main"_q,
		_owner->chatsList(folder)->pinned());
}

void LocalPinnedDialogs::saveOrder(FilterId filterId) {
	storeOrder(filterScope(filterId),
		_owner->chatsFilters().chatsList(filterId)->pinned());
}

void LocalPinnedDialogs::saveOrder(not_null<SavedMessages*> saved) {
	if (!saved->parentChat()) {
		storeOrder(u"saved"_q, saved->chatsList()->pinned());
	}
}

Dialogs::Key LocalPinnedDialogs::resolve(
		const QString &scope,
		PeerId peer) const {
	if (scope == u"saved") {
		const auto loaded = _owner->peerLoaded(peer);
		const auto sublist = loaded
			? _owner->savedMessages().sublistLoaded(loaded)
			: nullptr;
		return (sublist && sublist->chatListMessage())
			? Dialogs::Key(sublist)
			: Dialogs::Key();
	}
	const auto history = _owner->historyLoaded(peer);
	if (!history
		|| _removed.contains(peer)
		|| !Eligible(history, _confirmed.contains(peer))) {
		return {};
	} else if (scope == u"main" && history->folder()) {
		return {};
	} else if (scope == u"archive" && !history->folder()) {
		return {};
	}
	return history;
}

std::vector<Dialogs::Key> LocalPinnedDialogs::project(
		const QString &scope,
		const std::vector<Dialogs::Key> &cloud) const {
	if (_stopped || !_items->loaded() || scope.isEmpty()) {
		return cloud;
	}
	auto result = std::vector<Dialogs::Key>();
	const auto order = _items->overlay(scope, Peers(cloud));
	result.reserve(order.size() + cloud.size());
	auto existing = std::map<PeerId, Dialogs::Key>();
	auto included = std::set<Dialogs::Key>();
	for (const auto key : cloud) {
		if (const auto peer = KeyPeer(key)) {
			existing.emplace(peer, key);
		} else if (included.emplace(key).second) {
			result.push_back(key);
		}
	}
	for (const auto peer : order) {
		const auto i = existing.find(peer);
		const auto key = (i != end(existing)) ? i->second : resolve(scope, peer);
		if (const auto history = key.history()
			; history
			&& (_removed.contains(peer)
				|| (scope == u"main" && history->folder())
				|| (scope == u"archive" && !history->folder()))) {
			continue;
		}
		if (key && included.emplace(key).second) {
			result.push_back(key);
		}
	}
	return result;
}

void LocalPinnedDialogs::attach(
		not_null<Dialogs::PinnedList*> list,
		Folder *folder) {
	if (!_ready || _stopped || list->hasProjection()) {
		return;
	}
	const auto scope = folder ? u"archive"_q : u"main"_q;
	const auto weak = QPointer<LocalPinnedDialogs>(this);
	list->setProjection([=](const std::vector<Dialogs::Key> &cloud) {
		return weak ? weak->project(scope, cloud) : cloud;
	}, [=](Dialogs::Key key) {
		return key.entry()->folderKnown() && key.entry()->folder() == folder;
	});
}

void LocalPinnedDialogs::attach(
		not_null<Dialogs::PinnedList*> list,
		FilterId filterId) {
	if (!_ready || _stopped || list->hasProjection()) {
		return;
	}
	const auto weak = QPointer<LocalPinnedDialogs>(this);
	list->setProjection([=](const std::vector<Dialogs::Key> &cloud) {
		if (!weak || weak->_stopped) {
			return cloud;
		}
		const auto &filters = weak->_owner->chatsFilters().list();
		if (!ranges::contains(filters, filterId, &ChatFilter::id)) {
			return cloud;
		}
		auto result = weak->project(weak->filterScope(filterId), cloud);
		const auto i = ranges::find(filters, filterId, &ChatFilter::id);
		if (i != end(filters) && i->isLocal()) {
			result.erase(ranges::remove_if(result, [&](Dialogs::Key key) {
				const auto history = key.history();
				return !history || i->never().contains(history);
			}), end(result));
		}
		return result;
	});
}

void LocalPinnedDialogs::attach(
		not_null<Dialogs::PinnedList*> list,
		not_null<SavedMessages*> saved) {
	if (!_ready || _stopped || saved->parentChat() || list->hasProjection()) {
		return;
	}
	const auto weak = QPointer<LocalPinnedDialogs>(this);
	list->setProjection([=](const std::vector<Dialogs::Key> &cloud) {
		return weak ? weak->project(u"saved"_q, cloud) : cloud;
	});
}

void LocalPinnedDialogs::scheduleRefresh() {
	if (_stopped || !_ready || _scheduled || _refreshing) {
		return;
	}
	_scheduled = true;
	crl::on_main(this, [=] {
		_scheduled = false;
		refresh();
	});
}

void LocalPinnedDialogs::syncFilters() {
	if (!_ready || _stopped || !_items->loaded() || _filtersSyncing
		|| !_owner->chatsFilters().loaded()) {
		return;
	}
	auto ids = std::vector<FilterId>();
	for (const auto &filter : _owner->chatsFilters().list()) {
		if (filter.id() && !filter.isLocal()) {
			ids.push_back(filter.id());
		}
	}
	ranges::sort(ids);
	auto mapped = std::vector<FilterId>();
	auto signature = QString();
	for (const auto id : ids) {
		signature += QString::number(id) + u","_q;
	}
	signature += u";"_q;
	for (const auto &[id, scope] : _items->filterScopes()) {
		mapped.push_back(id);
		signature += scope + u","_q;
	}
	ranges::sort(mapped);
	if (mapped == ids || signature == _filterSyncAttempt) {
		return;
	}
	_filtersSyncing = true;
	_filterSyncAttempt = signature;
	_items->syncFilters(std::move(ids), crl::guard(this, [=](bool success) {
		_filtersSyncing = false;
		if (success) {
			_filterSyncAttempt.clear();
		}
		scheduleRefresh();
	}));
}

void LocalPinnedDialogs::refresh() {
	if (_stopped || !_ready || _refreshing) {
		return;
	}
	_refreshing = true;
	const auto finished = gsl::finally([&] { _refreshing = false; });
	syncFilters();
	_watched.clear();
	for (const auto &scope : _items->scopes()) {
		for (const auto peer : _items->localPins(scope)) {
			_watched.insert(peer);
		}
	}
	_owner->chatsList()->pinned()->refreshProjection();
	if (const auto folder = _owner->folderLoaded(Folder::kId)) {
		folder->chatsList()->pinned()->refreshProjection();
	}
	_owner->savedMessages().chatsList()->pinned()->refreshProjection();
	for (const auto &filter : _owner->chatsFilters().list()) {
		if (const auto id = filter.id()) {
			_owner->chatsFilters().chatsList(id)->pinned()->refreshProjection();
		}
	}
	_owner->notifyPinnedDialogsOrderUpdated();
	hydrate();
}

void LocalPinnedDialogs::hydrate() {
	if (!_items->loaded() || _stopped) {
		return;
	}
	const auto now = crl::now();
	auto next = crl::time(0);
	const auto retryAt = [&](crl::time when) {
		if (!next || when < next) {
			next = when;
		}
	};
	const auto page = [&](const QString &scope) {
		const auto saved = (scope == u"saved");
		const auto folder = (scope == u"archive")
			? _owner->folder(Folder::kId).get()
			: nullptr;
		const auto list = saved
			? _owner->savedMessages().chatsList()
			: _owner->chatsList(folder);
		if (list->loaded()) {
			return;
		}
		auto &state = _pages[scope];
		const auto count = list->indexed()->size();
		if (state.count != count) {
			state = Hydration{ .count = count };
		}
		if (state.attempts >= kHydrationAttempts) {
			return;
		} else if (state.next > now) {
			retryAt(state.next);
			return;
		}
		state.next = now + kHydrationBackoff * ++state.attempts;
		retryAt(state.next);
		if (saved) {
			_owner->savedMessages().loadMore();
		} else {
			_owner->session().api().requestDialogs(folder);
		}
	};
	for (const auto &scope : _items->scopes()) {
		if ((scope.startsWith(u"local:") || scope.startsWith(u"filter:"))
			&& !ranges::any_of(
				_owner->chatsFilters().list(), [&](const ChatFilter &filter) {
					return filterScope(filter.id()) == scope;
				})) {
			continue;
		}
		for (const auto id : _items->localPins(scope)) {
			if (resolve(scope, id)) {
				continue;
			}
			const auto saved = (scope == u"saved");
			const auto peer = _owner->peerLoaded(id);
			if (!peer || !peer->isLoaded()
				|| peer->input().type() == mtpc_inputPeerEmpty) {
				if (saved) {
					page(u"saved"_q);
				} else if (scope == u"main" || scope == u"archive") {
					page(scope);
				} else {
					page(u"main"_q);
					page(u"archive"_q);
				}
				continue;
			}
			if (!saved) {
				const auto channel = peer->asChannel();
				if (_removed.contains(id)
					|| (channel && channel->isCommunity())) {
					continue;
				}
			} else if (const auto sublist = _owner->savedMessages().sublistLoaded(peer)
				; sublist && sublist->chatListMessageKnown()) {
				continue;
			}
			const auto key = std::pair(saved, id);
			auto &state = _hydration[key];
			if (state.pending || state.attempts >= kHydrationAttempts) {
				continue;
			} else if (state.next > now) {
				retryAt(state.next);
				continue;
			} else if (_hydrating >= kConcurrentHydration) {
				continue;
			}
			state.pending = true;
			++state.attempts;
			++_hydrating;
			const auto done = crl::guard(this, [=] {
				auto &state = _hydration[key];
				if (!state.pending) {
					return;
				}
				state.pending = false;
				state.requestId = 0;
				state.next = crl::now() + kHydrationBackoff * state.attempts;
				--_hydrating;
				scheduleRefresh();
			});
			if (saved) {
				_owner->savedMessages().requestSublist(peer, done);
			} else {
				state.requestId = _owner->histories().sendRequest(
					_owner->history(peer),
					Histories::RequestType::History,
					[=](Fn<void()> finish) {
						return _api->request(MTPmessages_GetPeerDialogs(
							MTP_vector<MTPInputDialogPeer>(1,
								MTP_inputDialogPeer(peer->input()))
						)).done([=](const MTPmessages_PeerDialogs &result) {
							for (const auto &dialog : result.data().vdialogs().v) {
								if (dialog.type() == mtpc_dialog
									&& peerFromMTP(dialog.c_dialog().vpeer()) == id) {
									_confirmed.insert(id);
								}
							}
							_owner->histories().applyPeerDialogs(result);
							done();
							finish();
						}).fail([=] {
							done();
							finish();
						}).send();
					});
			}
		}
	}
	_hydrateTimer.cancel();
	if (next) {
		_hydrateTimer.callOnce(std::max(crl::time(1), next - now));
	}
}

void LocalPinnedDialogs::stop() {
	if (_stopped) {
		return;
	}
	_stopped = true;
	for (const auto &[key, state] : _hydration) {
		if (state.requestId) {
			_owner->histories().cancelRequest(state.requestId);
		}
	}
	_api.reset();
	_hydrateTimer.cancel();
	_lifetime.destroy();
	_owner->chatsList()->pinned()->clear();
	if (const auto folder = _owner->folderLoaded(Folder::kId)) {
		folder->chatsList()->pinned()->clear();
	}
	_owner->savedMessages().chatsList()->pinned()->clear();
	for (const auto &filter : _owner->chatsFilters().list()) {
		if (filter.id()) {
			_owner->chatsFilters().chatsList(filter.id())->pinned()->clear();
		}
	}
}

} // namespace Data
