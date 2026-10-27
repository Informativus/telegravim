/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "data/data_message_reaction_id.h"
#include "data/data_msg_id.h"
#include "mtproto/sender.h"

#include <QtCore/QObject>
#include <atomic>
#include <memory>
#include <rpl/event_stream.h>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Data {

class LocalSavedTags;

struct LocalSavedTagsSearchQuery {
	QString text;
	std::vector<ReactionId> tags;
	PeerId savedPeer;
	MsgId topMsgId;
	bool pinned = false;

	friend bool operator==(
		const LocalSavedTagsSearchQuery &,
		const LocalSavedTagsSearchQuery &) = default;
};

struct LocalSavedTagsSearchPage {
	MessageIdsList messages;
	int total = -1;
	bool start = false;
	bool complete = false;
	QString error;
};

class LocalSavedTagsSearch final : public QObject {
public:
	explicit LocalSavedTagsSearch(not_null<Main::Session*> session);
	~LocalSavedTagsSearch();

	void search(LocalSavedTagsSearchQuery query);
	void more();
	void cancel();
	[[nodiscard]] bool busy() const;
	[[nodiscard]] rpl::producer<LocalSavedTagsSearchPage> pages() const;

private:
	void prepare();
	void loadBatch();
	void finishBatch(int end);
	void publish(QString error = {});

	const not_null<Main::Session*> _session;
	LocalSavedTags &_model;
	MTP::Sender _api;
	base::Timer _requestTimer;
	LocalSavedTagsSearchQuery _query;
	std::shared_ptr<std::atomic<bool>> _cancel;
	MessageIdsList _candidates;
	MessageIdsList _page;
	mtpRequestId _requestId = 0;
	quint64 _generation = 0;
	quint64 _revision = 0;
	int _offset = 0;
	int _matched = 0;
	bool _active = false;
	bool _loading = false;
	bool _first = true;
	bool _complete = false;
	bool _waitingForModel = false;
	rpl::event_stream<LocalSavedTagsSearchPage> _pages;
	rpl::lifetime _lifetime;

};

} // namespace Data
