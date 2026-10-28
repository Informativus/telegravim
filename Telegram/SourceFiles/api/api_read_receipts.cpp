/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_read_receipts.h"

#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_forum_topic.h"
#include "data/data_histories.h"
#include "data/data_replies_list.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/storage_account.h"

namespace Api {
namespace {

constexpr auto kPreference = "telegravim-read-after-send";
constexpr auto kScopesPreference = "telegravim-read-after-send-scopes";
constexpr auto kAllScopes = 7;

} // namespace

ReadReceipts::ReadReceipts(not_null<Main::Session*> session)
: _session(session) {
}

bool ReadReceipts::enabled() const {
	if (!_enabled.has_value()) {
		_enabled = _session->local().readPref<bool>(kPreference, false);
	}
	return *_enabled;
}

bool ReadReceipts::enabled(not_null<PeerData*> peer) const {
	const auto scope = peer->isUser()
		? Scope::PrivateChats
		: peer->isBroadcast()
		? Scope::Channels
		: Scope::Groups;
	return enabled() && scopeEnabled(scope);
}

bool ReadReceipts::enabled(PeerId peer) const {
	return enabled(_session->data().peer(peer));
}

rpl::producer<bool> ReadReceipts::enabledValue() const {
	return rpl::single(enabled()) | rpl::then(_changes.events());
}

void ReadReceipts::setEnabled(bool enabled) {
	if (this->enabled() == enabled) {
		return;
	}
	_enabled = enabled;
	invalidatePending();
	_session->local().writePref<bool>(kPreference, enabled);
	_changes.fire_copy(enabled);
}

bool ReadReceipts::scopeEnabled(Scope scope) const {
	if (!_scopes.has_value()) {
		const auto stored = _session->local().readPref<QByteArray>(
			kScopesPreference,
			QByteArray::number(kAllScopes));
		auto valid = false;
		const auto scopes = stored.toInt(&valid);
		_scopes = (valid && scopes >= 0 && scopes <= kAllScopes)
			? scopes
			: kAllScopes;
	}
	return (*_scopes & int(scope)) != 0;
}

rpl::producer<bool> ReadReceipts::scopeEnabledValue(Scope scope) const {
	return rpl::single(scopeEnabled(scope)) | rpl::then(
		_scopeChanges.events() | rpl::map([=] { return scopeEnabled(scope); }));
}

void ReadReceipts::setScopeEnabled(Scope scope, bool enabled) {
	if (scopeEnabled(scope) == enabled) {
		return;
	}
	_scopes = enabled ? (*_scopes | int(scope)) : (*_scopes & ~int(scope));
	invalidatePending();
	_session->local().writePref<QByteArray>(
		kScopesPreference,
		QByteArray::number(*_scopes));
	_scopeChanges.fire({});
}

void ReadReceipts::invalidatePending() {
	++_generation;
	_limits.clear();
	_contents.clear();
	_releasingContents.clear();
}

bool ReadReceipts::allowed(
		PeerId peer,
		MsgId till,
		MsgId root,
		PeerId sublist) const {
	if (!enabled(peer)) {
		return true;
	}
	const auto i = _limits.find(Key(peer, root, sublist));
	return IsServerMsgId(till)
		&& i != _limits.end()
		&& till <= i->second;
}

bool ReadReceipts::deferContents(not_null<HistoryItem*> item) {
	if (!enabled(item->history()->peer)
		|| _releasingContents.erase(item->fullId())) {
		return false;
	}
	if (item->isRegular()
		&& (item->isUnreadMention()
			|| item->isIncomingUnreadMedia()
			|| item->hasUnreadReaction()
			|| item->hasUnreadPollVote())) {
		_contents.emplace(item->fullId());
	}
	return true;
}

ReadReceipts::Key ReadReceipts::key(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const auto sublist = peer->monoforum() ? item->sublistPeerId() : PeerId();
	const auto root = peer->forum()
		? item->topicRootId()
		: peer->isMegagroup()
		? item->replyToTop()
		: MsgId();
	return Key(peer->id, sublist ? MsgId(0) : root, sublist);
}

void ReadReceipts::sent(not_null<HistoryItem*> local, MsgId serverId) {
	if (!enabled(local->history()->peer) || !IsServerMsgId(serverId)) {
		return;
	}
	const auto conversation = key(local);
	const auto generation = _generation;
	crl::on_main(_session, [=] {
		if (generation != _generation || !enabled(std::get<0>(conversation))) {
			return;
		}
		auto &limit = _limits[conversation];
		limit = std::max(limit, serverId);
		acknowledge(conversation, limit);
	});
}

void ReadReceipts::markRead(not_null<Data::Thread*> thread) {
	const auto history = thread->owningHistory();
	const auto peer = history->peer->id;
	if (const auto topic = thread->asTopic()) {
		markRead(Key(peer, topic->rootId(), {}),
			topic->lastKnownServerMessageId());
	} else if (const auto sublist = thread->asSublist()) {
		if (sublist->inMonoforum()) {
			markRead(Key(peer, 0, sublist->sublistPeer()->id),
				sublist->lastKnownServerMessageId());
		} else {
			sublist->readTillEnd();
		}
	} else if (thread->asHistory()) {
		if (!enabled(history->peer)) {
			history->owner().histories().readInbox(history);
		} else if (history->lastServerMessageKnown()) {
			if (const auto last = history->lastServerMessage()) {
				markRead(Key(peer, 0, {}), last->id);
			} else if (history->unreadMark()) {
				history->owner().histories().changeDialogUnreadMark(history, false);
			}
		} else if (const auto till = history->maxMsgId()) {
			markRead(Key(peer, 0, {}), till);
		} else {
			history->owner().histories().requestDialogEntry(history);
		}
	}
}

void ReadReceipts::markRead(Key conversation, MsgId till) {
	if (!IsServerMsgId(till)) {
		return;
	}
	if (enabled(std::get<0>(conversation))) {
		auto &limit = _limits[conversation];
		limit = std::max(limit, till);
	}
	acknowledge(conversation, till);
}

void ReadReceipts::acknowledge(Key conversation, MsgId till) {
	const auto [peerId, root, sublistPeer] = conversation;
	const auto history = _session->data().history(peerId);
	const auto peer = history->peer;
	if (sublistPeer) {
		if (const auto sublist = peer->monoforumSublistFor(sublistPeer)) {
			sublist->readTill(till);
		}
	} else if (root) {
		if (const auto topic = peer->forumTopicFor(root)) {
			topic->replies()->readTill(till);
		} else {
			_session->api().request(MTPmessages_ReadDiscussion(
				peer->input(),
				MTP_int(root),
				MTP_int(till)
			)).send();
		}
	} else {
		_session->data().histories().readInboxTill(history, till);
	}
	auto contents = base::flat_set<not_null<HistoryItem*>>();
	for (auto i = _contents.begin(); i != _contents.end();) {
		const auto item = _session->data().message(*i);
		if (!item) {
			i = _contents.erase(i);
		} else if (item->id <= till && key(item) == conversation) {
			contents.emplace(item);
			i = _contents.erase(i);
		} else {
			++i;
		}
	}
	for (const auto item : contents) {
		_releasingContents.emplace(item->fullId());
	}
	const auto guard = gsl::finally([&] { _releasingContents.clear(); });
	_session->api().markContentsRead(contents);
}

} // namespace Api
