/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"
#include "data/data_msg_id.h"

#include <tuple>

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Api {

class ReadReceipts final {
public:
	explicit ReadReceipts(not_null<Main::Session*> session);

	[[nodiscard]] bool enabled() const;
	[[nodiscard]] rpl::producer<bool> enabledValue() const;
	void setEnabled(bool enabled);
	[[nodiscard]] bool allowed(
		PeerId peer,
		MsgId till,
		MsgId root = 0,
		PeerId sublist = {}) const;
	bool deferContents(not_null<HistoryItem*> item);
	void sent(not_null<HistoryItem*> local, MsgId serverId);

private:
	using Key = std::tuple<PeerId, MsgId, PeerId>;
	[[nodiscard]] static Key key(not_null<HistoryItem*> item);
	void acknowledge(Key key, MsgId till);

	const not_null<Main::Session*> _session;
	mutable std::optional<bool> _enabled;
	rpl::event_stream<bool> _changes;
	base::flat_map<Key, MsgId> _limits;
	base::flat_set<FullMsgId> _contents;
	uint64 _generation = 0;
	bool _releasingContents = false;

};

} // namespace Api
