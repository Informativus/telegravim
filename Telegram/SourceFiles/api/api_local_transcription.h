/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/transcription/transcription_engine.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QSaveFile>
#include <QtCore/QCryptographicHash>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <deque>

namespace Main {
class Session;
} // namespace Main
namespace Data {
class DocumentMedia;
} // namespace Data

namespace Api {

class LocalTranscription final : public QObject {
public:
	using Update = Fn<void(FullMsgId, QString, bool, bool)>;

	LocalTranscription(not_null<Main::Session*> session, Update update);
	~LocalTranscription();

	[[nodiscard]] bool needsModel() const;
	void enqueue(FullMsgId id);
	void cancel(FullMsgId id);

private:
	void next();
	void check();
	void downloadModel();
	void recognize();
	void clearAudioDownload();
	void finish(QString text, bool failed);
	void publish(QString text);
	[[nodiscard]] QString modelPath() const;

	const not_null<Main::Session*> _session;
	const Update _update;
	std::deque<FullMsgId> _queue;
	std::optional<FullMsgId> _active;
	std::shared_ptr<Data::DocumentMedia> _media;
	std::shared_ptr<QTemporaryDir> _audioDirectory;
	std::shared_ptr<std::atomic<bool>> _cancelled;
	QTimer _timer;
	QElapsedTimer _elapsed;
	qint64 _audioOffset = 0;
	bool _running = false;
	bool _audioStarted = false;
	bool _modelInvalid = false;
	QNetworkAccessManager _network;
	QNetworkReply *_reply = nullptr;
	std::unique_ptr<QSaveFile> _download;
	QCryptographicHash _hash{ QCryptographicHash::Sha256 };
	qint64 _received = 0;
	int _progress = -1;

};

} // namespace Api
