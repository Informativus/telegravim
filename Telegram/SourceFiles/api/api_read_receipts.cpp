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

rpl::producer<bool> ReadReceipts::enabledValue() const {
	return rpl::single(enabled()) | rpl::then(_changes.events());
}

void ReadReceipts::setEnabled(bool enabled) {
	if (this->enabled() == enabled) {
		return;
	}
	_enabled = enabled;
	++_generation;
	_limits.clear();
	_contents.clear();
	_releasingContents.clear();
	_session->local().writePref<bool>(kPreference, enabled);
	_changes.fire_copy(enabled);
}

bool ReadReceipts::allowed(
		PeerId peer,
		MsgId till,
		MsgId root,
		PeerId sublist) const {
	if (!enabled()) {
		return true;
	}
	const auto i = _limits.find(Key(peer, root, sublist));
	return IsServerMsgId(till)
		&& i != _limits.end()
		&& till <= i->second;
}

bool ReadReceipts::deferContents(not_null<HistoryItem*> item) {
	if (!enabled() || _releasingContents.erase(item->fullId())) {
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
		: item->replyToTop();
	return Key(peer->id, sublist ? MsgId(0) : root, sublist);
}

void ReadReceipts::sent(not_null<HistoryItem*> local, MsgId serverId) {
	if (!enabled() || !IsServerMsgId(serverId)) {
		return;
	}
	const auto conversation = key(local);
	const auto generation = _generation;
	crl::on_main(_session, [=] {
		if (generation != _generation || !enabled()) {
			return;
		}
		auto &limit = _limits[conversation];
		limit = std::max(limit, serverId);
		acknowledge(conversation, limit);
	});
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
