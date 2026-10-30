/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_archive_chats.h"

#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/local_pinned_dialogs.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"

#include <QtCore/QDataStream>

namespace Api {
namespace {

constexpr auto kPageSize = 100;
constexpr auto kBatchSize = 20;
constexpr auto kBatchDelay = crl::time(1000);
constexpr auto kMaxPeers = 100000;
constexpr auto kSettingsVersion = quint32(2);

} // namespace

ArchiveChats::ArchiveChats(not_null<Main::Session*> session)
: _session(session)
, _api(&session->mtp())
, _next([=] { nextBatch(); }) {
	readSettings();
	session->data().newItemAdded() | rpl::on_next([=](
			not_null<HistoryItem*> item) {
		incoming(item);
	}, _lifetime);
	session->data().chatsListLoadedEvents() | rpl::on_next([=] {
		startAutomatic();
	}, _lifetime);
}

ArchiveChats::~ArchiveChats() = default;

bool ArchiveChats::eligible(not_null<PeerData*> peer) const {
	if (peer->isSelf()
		|| peer->isServiceUser()
		|| peer->isRepliesChat()
		|| peer->isVerifyCodes()
		|| peer->isFreezeAppealChat()) {
		return false;
	}
	if (const auto channel = peer->asChannel()) {
		if (!channel->isMegagroup() || channel->isCommunity()) {
			return false;
		}
	} else if (!peer->isChat()
		&& !(peer->isUser() && peer->isBot())) {
		return false;
	}
	if (_session->data().localPinnedDialogs().isPinned(0, peer->id)) {
		return false;
	}
	if (const auto history = _session->data().historyLoaded(peer->id)) {
		if (history->isPinnedDialog(0)) {
			return false;
		}
	}
	return true;
}

bool ArchiveChats::eligible(const MTPDdialog &dialog) const {
	const auto folder = dialog.vfolder_id();
	if (dialog.is_pinned() || (folder && folder->v)) {
		return false;
	}
	const auto peerId = peerFromMTP(dialog.vpeer());
	const auto peer = _session->data().peerLoaded(peerId);
	return peer && eligible(peer);
}

void ArchiveChats::prepare() {
	if (_mode != Mode::Idle) {
		return;
	}
	_mode = Mode::PreviewScan;
	_busy = true;
	_preview = -1;
	_status = tr::lng_archive_cleanup_scanning(tr::now);
	_scan.clear();
	_folder = 0;
	_offsetDate = 0;
	_offsetId = 0;
	_offsetPeer = 0;
	_seenOffsets.clear();
	scanPage();
}

void ArchiveChats::setAutomatic(bool enabled) {
	if (!enabled) {
		_automatic = false;
		_pending.clear();
		saveSettings();
		if (_mode == Mode::BaselineScan) {
			cancel();
		}
		return;
	}
	if (_mode != Mode::Idle || _automatic.current()) {
		return;
	}
	_mode = Mode::BaselineScan;
	_busy = true;
	_status = tr::lng_archive_cleanup_preparing(tr::now);
	_scan.clear();
	_folder = 0;
	scanPinned();
}

void ArchiveChats::scanPinned() {
	_offsetDate = 0;
	_offsetId = 0;
	_offsetPeer = 0;
	_seenOffsets.clear();
	_request = _api.request(MTPmessages_GetPinnedDialogs(
		MTP_int(_folder)
	)).done([=](const MTPmessages_PeerDialogs &result) {
		_request = 0;
		const auto &data = result.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		collect(data.vdialogs().v);
		scanPage();
	}).fail([=] { fail(); }).handleFloodErrors().send();
}

void ArchiveChats::collect(const QVector<MTPDialog> &dialogs) {
	for (const auto &dialog : dialogs) {
		dialog.match([&](const MTPDdialog &data) {
			const auto peer = peerFromMTP(data.vpeer());
			if (peer && (_mode == Mode::BaselineScan || eligible(data))) {
				_scan.emplace(peer);
			}
		}, [](const auto &) {});
	}
}

void ArchiveChats::scanPage() {
	_request = _api.request(MTPmessages_GetDialogs(
		MTP_flags(MTPmessages_GetDialogs::Flag::f_exclude_pinned
			| MTPmessages_GetDialogs::Flag::f_folder_id),
		MTP_int(_folder),
		MTP_int(_offsetDate),
		MTP_int(_offsetId),
		_offsetPeer
			? _session->data().peer(_offsetPeer)->input()
			: MTP_inputPeerEmpty(),
		MTP_int(kPageSize),
		MTP_long(0)
	)).done([=](const MTPmessages_Dialogs &result) {
		_request = 0;
		result.match([&](const MTPDmessages_dialogsNotModified &) {
			fail();
		}, [&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			collect(data.vdialogs().v);
			if (_scan.size() > kMaxPeers || _seenOffsets.size() > kMaxPeers) {
				fail();
				return;
			}
			if (result.type() == mtpc_messages_dialogs
				|| data.vdialogs().v.empty()) {
				scanFinished();
				return;
			}
			auto peer = PeerId();
			auto id = MsgId();
			auto date = TimeId();
			for (const auto &dialog : ranges::views::reverse(data.vdialogs().v)) {
				dialog.match([&](const MTPDdialog &entry) {
					const auto candidate = peerFromMTP(entry.vpeer());
					for (const auto &message : data.vmessages().v) {
						if (PeerFromMessage(message) == candidate
							&& IdFromMessage(message) == entry.vtop_message().v
							&& DateFromMessage(message)) {
							peer = candidate;
							id = IdFromMessage(message);
							date = DateFromMessage(message);
							break;
						}
					}
				}, [](const auto &) {});
				if (peer) {
					break;
				}
			}
			if (!peer || !_seenOffsets.emplace(peer).second) {
				fail();
				return;
			}
			_offsetPeer = peer;
			_offsetId = id;
			_offsetDate = date;
			scanPage();
		});
	}).fail([=] { fail(); }).handleFloodErrors().send();
}

void ArchiveChats::scanFinished() {
	if (_mode == Mode::BaselineScan) {
		if (!_folder) {
			_folder = Data::Folder::kId;
			scanPinned();
			return;
		}
		_known = std::move(_scan);
		_pending.clear();
		_automatic = true;
		_automaticFailed = false;
		saveSettings();
		_mode = Mode::Idle;
		_status = tr::lng_archive_cleanup_enabled(tr::now);
		_busy = false;
	} else {
		_work.assign(_scan.begin(), _scan.end());
		_mode = _work.empty() ? Mode::Idle : Mode::Preview;
		_status = _work.empty()
			? tr::lng_archive_cleanup_empty(tr::now)
			: tr::lng_archive_cleanup_ready(tr::now);
		_preview = int(_work.size());
		_busy = false;
	}
}

void ArchiveChats::confirm() {
	if (_mode != Mode::Preview) {
		return;
	}
	_mode = Mode::Manual;
	_busy = true;
	_preview = -1;
	_position = 0;
	_archived = 0;
	_lastArchived.clear();
	_undoAvailable = false;
	saveSettings();
	nextBatch();
}

void ArchiveChats::cancel() {
	if (_mode == Mode::Manual
		|| _mode == Mode::Automatic
		|| _mode == Mode::Undo) {
		return;
	}
	if (_request) {
		_api.request(base::take(_request)).cancel();
	}
	_mode = Mode::Idle;
	_scan.clear();
	_work.clear();
	_preview = -1;
	_busy = false;
	startAutomatic();
}

void ArchiveChats::undo() {
	if (_mode != Mode::Idle || _lastArchived.empty()) {
		return;
	}
	_work.assign(_lastArchived.begin(), _lastArchived.end());
	_mode = Mode::Undo;
	_busy = true;
	_preview = -1;
	_position = 0;
	_archived = 0;
	nextBatch();
}

void ArchiveChats::incoming(not_null<HistoryItem*> item) {
	if (!_automatic.current() || item->out() || !item->isRegular()) {
		return;
	}
	const auto peer = item->history()->peer;
	if (!eligible(peer) || _known.contains(peer->id)) {
		return;
	}
	_known.emplace(peer->id);
	_pending.emplace(peer->id);
	saveSettings();
	startAutomatic();
}

void ArchiveChats::startAutomatic() {
	if (!_automatic.current() || _automaticFailed
		|| _mode != Mode::Idle || _pending.empty()) {
		return;
	}
	_work.clear();
	for (const auto peer : _pending) {
		if (_session->data().peerLoaded(peer)) {
			_work.push_back(peer);
		}
	}
	if (_work.empty()) {
		return;
	}
	_mode = Mode::Automatic;
	_busy = true;
	_position = 0;
	_archived = 0;
	nextBatch();
}

void ArchiveChats::retryAutomatic() {
	_automaticFailed = false;
	startAutomatic();
}

void ArchiveChats::nextBatch() {
	if (_position == _work.size()
		|| (_mode == Mode::Automatic && !_automatic.current())) {
		finish();
		return;
	}
	const auto automatic = (_mode == Mode::Automatic);
	const auto undo = (_mode == Mode::Undo);
	_status = undo
		? tr::lng_archive_cleanup_undoing(tr::now)
		: tr::lng_archive_cleanup_progress(tr::now, lt_count, _archived);
	const auto end = std::min(_position + size_t(kBatchSize), _work.size());
	auto peers = std::vector<PeerId>();
	peers.reserve(end - _position);
	for (auto i = _position; i != end; ++i) {
		const auto peer = _work[i];
		if (undo || (eligible(_session->data().peer(peer))
			&& (!automatic || _automatic.current()))) {
			peers.push_back(peer);
		}
	}
	_position = end;
	sendBatch(std::move(peers));
}

void ArchiveChats::sendBatch(std::vector<PeerId> peers) {
	if (peers.empty()) {
		_next.callOnce(kBatchDelay);
		return;
	}
	auto batch = QVector<MTPInputFolderPeer>();
	const auto undo = (_mode == Mode::Undo);
	const auto manual = (_mode == Mode::Manual);
	for (const auto peer : peers) {
		batch.push_back(MTP_inputFolderPeer(
			_session->data().peer(peer)->input(),
			MTP_int(undo ? 0 : Data::Folder::kId)));
	}
	_request = _api.request(MTPfolders_EditPeerFolders(
		MTP_vector(std::move(batch))
	)).done([=](const MTPUpdates &updates) {
		_request = 0;
		_session->api().applyUpdates(updates);
		for (const auto peer : peers) {
			if (undo) {
				_session->data().history(peer)->clearFolder();
				_lastArchived.erase(peer);
			} else {
				_session->data().history(peer)->setFolder(
					_session->data().folder(Data::Folder::kId));
				if (manual) {
					_lastArchived.emplace(peer);
				}
			}
		}
		if (undo || manual) {
			_undoAvailable = !_lastArchived.empty();
			saveSettings();
		}
		_archived += int(peers.size());
		_next.callOnce(kBatchDelay);
	}).fail([=] { fail(); }).handleFloodErrors().send();
}

void ArchiveChats::finish() {
	const auto undo = (_mode == Mode::Undo);
	if (_mode == Mode::Automatic) {
		for (const auto peer : _work) {
			_pending.remove(peer);
		}
		saveSettings();
	}
	if (undo) {
		_lastArchived.clear();
		_undoAvailable = false;
		saveSettings();
	}
	_mode = Mode::Idle;
	_work.clear();
	_status = undo
		? tr::lng_archive_cleanup_undone(tr::now, lt_count, _archived)
		: tr::lng_archive_cleanup_done(tr::now, lt_count, _archived);
	_busy = false;
	startAutomatic();
}

void ArchiveChats::fail() {
	_request = 0;
	_next.cancel();
	if (_mode == Mode::Automatic) {
		_automaticFailed = true;
	}
	_status = (_mode == Mode::Manual || _mode == Mode::Automatic)
		? tr::lng_archive_cleanup_failed(tr::now, lt_count, _archived)
		: (_mode == Mode::Undo
			? tr::lng_archive_cleanup_undo_failed(tr::now, lt_count, _archived)
			: tr::lng_archive_cleanup_scan_failed(tr::now));
	_mode = Mode::Idle;
	_work.clear();
	_scan.clear();
	_preview = -1;
	_busy = false;
}

void ArchiveChats::readSettings() {
	const auto bytes = _session->settings().archiveChatsSettings();
	if (bytes.isEmpty() || bytes.size() > 3 * kMaxPeers * 8 + 32) {
		return;
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = quint32();
	auto enabled = false;
	auto known = base::flat_set<PeerId>();
	auto pending = base::flat_set<PeerId>();
	auto lastArchived = base::flat_set<PeerId>();
	stream >> version >> enabled;
	if (version != 1 && version != kSettingsVersion) {
		return;
	}
	for (const auto set : { &known, &pending }) {
		auto count = quint32();
		stream >> count;
		if (count > kMaxPeers) {
			return;
		}
		for (auto i = quint32(); i != count; ++i) {
			auto peer = quint64();
			stream >> peer;
			if (!peer) {
				return;
			}
			set->emplace(DeserializePeerId(peer));
		}
	}
	if (version >= 2) {
		auto count = quint32();
		stream >> count;
		if (count > kMaxPeers) {
			return;
		}
		for (auto i = quint32(); i != count; ++i) {
			auto peer = quint64();
			stream >> peer;
			if (!peer) {
				return;
			}
			lastArchived.emplace(DeserializePeerId(peer));
		}
	}
	if (stream.status() == QDataStream::Ok && stream.atEnd()) {
		_known = std::move(known);
		_pending = std::move(pending);
		_lastArchived = std::move(lastArchived);
		_undoAvailable = !_lastArchived.empty();
		_automatic = enabled;
	}
}

void ArchiveChats::saveSettings() {
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kSettingsVersion << _automatic.current();
	for (const auto set : { &_known, &_pending, &_lastArchived }) {
		stream << quint32(set->size());
		for (const auto peer : *set) {
			stream << SerializePeerId(peer);
		}
	}
	_session->settings().setArchiveChatsSettings(std::move(bytes));
	_session->saveSettingsDelayed();
}

rpl::producer<bool> ArchiveChats::busyValue() const {
	return _busy.value();
}

rpl::producer<bool> ArchiveChats::automaticValue() const {
	return _automatic.value();
}

bool ArchiveChats::automatic() const {
	return _automatic.current();
}

rpl::producer<int> ArchiveChats::previewValue() const {
	return _preview.value();
}

rpl::producer<QString> ArchiveChats::statusValue() const {
	return _status.value();
}

rpl::producer<bool> ArchiveChats::undoAvailableValue() const {
	return _undoAvailable.value();
}

bool ArchiveChats::undoAvailable() const {
	return _undoAvailable.current();
}

} // namespace Api
