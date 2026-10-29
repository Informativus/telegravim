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
class PeerData;

namespace Data {
class Thread;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Api {

class ReadReceipts final {
public:
	enum class Scope {
		PrivateChats = 1,
		Groups = 2,
		Channels = 4,
		Bots = 8,
	};

	explicit ReadReceipts(not_null<Main::Session*> session);

	[[nodiscard]] bool enabled() const;
	[[nodiscard]] bool enabled(not_null<PeerData*> peer) const;
	[[nodiscard]] bool enabled(PeerId peer) const;
	[[nodiscard]] rpl::producer<bool> enabledValue() const;
	void setEnabled(bool enabled);
	[[nodiscard]] bool scopeEnabled(Scope scope) const;
	[[nodiscard]] rpl::producer<bool> scopeEnabledValue(Scope scope) const;
	void setScopeEnabled(Scope scope, bool enabled);
	[[nodiscard]] bool allowed(
		PeerId peer,
		MsgId till,
		MsgId root = 0,
		PeerId sublist = {}) const;
	bool deferContents(not_null<HistoryItem*> item);
	void sent(not_null<HistoryItem*> local, MsgId serverId);
	void markRead(not_null<Data::Thread*> thread);

private:
	using Key = std::tuple<PeerId, MsgId, PeerId>;
	[[nodiscard]] static Key key(not_null<HistoryItem*> item);
	[[nodiscard]] static Scope scopeFor(not_null<PeerData*> peer);
	void invalidatePending();
	void invalidatePending(Scope scope);
	void markRead(Key key, MsgId till);
	void acknowledge(Key key, MsgId till);

	const not_null<Main::Session*> _session;
	mutable std::optional<bool> _enabled;
	mutable std::optional<int> _scopes;
	rpl::event_stream<bool> _changes;
	rpl::event_stream<> _scopeChanges;
	base::flat_map<Key, MsgId> _limits;
	base::flat_set<FullMsgId> _contents;
	uint64 _generation = 0;
	base::flat_map<Scope, uint64> _scopeGenerations;
	base::flat_set<FullMsgId> _releasingContents;

};

} // namespace Api
