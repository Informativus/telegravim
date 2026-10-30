/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "mtproto/sender.h"
#include "data/data_peer_id.h"

namespace Main {
class Session;
} // namespace Main

class HistoryItem;
class PeerData;

namespace Api {

class ArchiveChats final {
public:
	explicit ArchiveChats(not_null<Main::Session*> session);
	~ArchiveChats();

	void prepare();
	void confirm();
	void cancel();
	void setAutomatic(bool enabled);
	void retryAutomatic();

	[[nodiscard]] rpl::producer<bool> busyValue() const;
	[[nodiscard]] rpl::producer<bool> automaticValue() const;
	[[nodiscard]] bool automatic() const;
	[[nodiscard]] rpl::producer<int> previewValue() const;
	[[nodiscard]] rpl::producer<QString> statusValue() const;

private:
	enum class Mode { Idle, PreviewScan, BaselineScan, Preview, Manual, Automatic };

	void scanPinned();
	void scanPage();
	void collect(const QVector<MTPDialog> &dialogs);
	void scanFinished();
	void incoming(not_null<HistoryItem*> item);
	void startAutomatic();
	void nextBatch();
	void sendBatch(std::vector<PeerId> peers);
	void finish();
	void fail();
	[[nodiscard]] bool eligible(not_null<PeerData*> peer) const;
	[[nodiscard]] bool eligible(const MTPDdialog &dialog) const;
	void readSettings();
	void saveSettings();

	const not_null<Main::Session*> _session;
	MTP::Sender _api;
	Mode _mode = Mode::Idle;
	mtpRequestId _request = 0;
	int _folder = 0;
	TimeId _offsetDate = 0;
	MsgId _offsetId = 0;
	PeerId _offsetPeer = 0;
	base::flat_set<PeerId> _seenOffsets;
	base::flat_set<PeerId> _scan;
	base::flat_set<PeerId> _known;
	base::flat_set<PeerId> _pending;
	std::vector<PeerId> _work;
	size_t _position = 0;
	int _archived = 0;
	bool _automaticFailed = false;
	rpl::variable<bool> _busy = false;
	rpl::variable<bool> _automatic = false;
	rpl::variable<int> _preview = -1;
	rpl::variable<QString> _status;
	base::Timer _next;
	rpl::lifetime _lifetime;

};

} // namespace Api
