/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_local_transcription.h"

#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "data/data_file_origin.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings.h"
#include "storage/file_download.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtNetwork/QNetworkRequest>
#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

namespace Api {
namespace {

using Error = Media::Transcription::Result::Error;

[[nodiscard]] QString ErrorText(Error error) {
	switch (error) {
	case Error::Model: return tr::lng_local_transcribe_model_failed(tr::now);
	case Error::TooLong: return tr::lng_local_transcribe_too_large(tr::now);
	case Error::NoSpeech: return tr::lng_local_transcribe_no_speech(tr::now);
	default: return tr::lng_local_transcribe_failed(tr::now);
	}
}

} // namespace

LocalTranscription::LocalTranscription(
	not_null<Main::Session*> session,
	Update update)
: _session(session)
, _update(std::move(update)) {
	_timer.setInterval(250);
	QObject::connect(&_timer, &QTimer::timeout, this, [=] { check(); });
}

LocalTranscription::~LocalTranscription() {
	if (_cancelled) {
		_cancelled->store(true);
	}
	if (_reply) {
		_reply->disconnect(this);
		_reply->abort();
	}
}

QString LocalTranscription::modelPath() const {
	return cWorkingDir() + u"tdata/models/ggml-small.bin"_q;
}

bool LocalTranscription::needsModel() const {
	return _modelInvalid
		|| QFileInfo(modelPath()).size() != Media::Transcription::ModelSize;
}

void LocalTranscription::enqueue(FullMsgId id) {
	if ((_active == id && !_cancelled->load()) || ranges::contains(_queue, id)) {
		return;
	}
	_queue.push_back(id);
	_update(id, tr::lng_local_transcribe_waiting(tr::now), false, false);
	next();
}

void LocalTranscription::cancel(FullMsgId id) {
	_queue.erase(std::remove(_queue.begin(), _queue.end(), id), _queue.end());
	if (_active == id) {
		_cancelled->store(true);
		if (_reply) {
			_reply->abort();
		} else if (!_running) {
			finish({}, true);
		}
	}
}

void LocalTranscription::next() {
	if (_active || _queue.empty()) {
		return;
	}
	_active = _queue.front();
	_queue.pop_front();
	_cancelled = std::make_shared<std::atomic<bool>>(false);
	const auto item = _session->data().message(*_active);
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!document || media->ttlSeconds()
		|| (!document->isVoiceMessage() && !document->isVideoMessage())) {
		finish(ErrorText(Error::Audio), true);
		return;
	}
	if (document->size > Media::Transcription::MaxAudioBytes
		|| document->duration() > crl::time(60 * 60 * 1000)) {
		finish(ErrorText(Error::TooLong), true);
		return;
	}
	_media = document->createMediaView();
	_elapsed.start();
	_audioOffset = 0;
	_audioStarted = false;
	_timer.start();
	if (needsModel()) {
		downloadModel();
	} else {
		check();
	}
}

void LocalTranscription::check() {
	if (!_active) {
		return;
	}
	const auto item = _session->data().message(*_active);
	if (!item || !item->media() || item->media()->ttlSeconds()
		|| item->media()->document() != _media->owner()) {
		_cancelled->store(true);
		if (_reply) {
			_reply->abort();
		} else if (!_running) {
			finish({}, true);
		}
		return;
	}
	if (_running || _reply) {
		return;
	}
	if (_media->loaded(true)) {
		recognize();
		return;
	}
	const auto document = _media->owner();
	if (!_audioStarted) {
		_audioStarted = true;
		_elapsed.restart();
		publish(tr::lng_local_transcribe_audio_loading(tr::now));
		if (document->loading()) {
			document->permitLoadFromCloud();
		} else if (document->size > Storage::kMaxFileInMemory) {
			_audioDirectory = std::make_shared<QTemporaryDir>();
			if (!_audioDirectory->isValid()) {
				finish(ErrorText(Error::Audio), true);
				return;
			}
			document->save(*_active, _audioDirectory->filePath(u"audio"_q));
		} else {
			document->save(*_active, QString());
		}
		return;
	}
	if (document->cancelled() || document->status == FileDownloadFailed) {
		finish(ErrorText(Error::Audio), true);
		return;
	}
	if (_audioOffset != document->loadOffset()) {
		_audioOffset = document->loadOffset();
		_elapsed.restart();
	}
	if (_elapsed.elapsed() > 120000) {
		finish(ErrorText(Error::Audio), true);
		return;
	}
	if (!document->loading()) {
		finish(ErrorText(Error::Audio), true);
	}
}

void LocalTranscription::publish(QString text) {
	if (_active && !_cancelled->load()) {
		_update(*_active, std::move(text), false, false);
	}
}

void LocalTranscription::downloadModel() {
	if (!QDir().mkpath(QFileInfo(modelPath()).absolutePath())) {
		finish(ErrorText(Error::Model), true);
		return;
	}
	_download = std::make_unique<QSaveFile>(modelPath());
	if (!_download->open(QIODevice::WriteOnly)) {
		finish(ErrorText(Error::Model), true);
		return;
	}
	if (!_download->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
		finish(ErrorText(Error::Model), true);
		_download.reset();
		return;
	}
	_hash.reset();
	_received = 0;
	_progress = -1;
	publish(tr::lng_local_transcribe_model_loading(tr::now, lt_percent, u"0"_q));
	auto request = QNetworkRequest(QUrl(Media::Transcription::ModelUrl()));
	request.setTransferTimeout(30000);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
		QNetworkRequest::Manual);
	request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
		QNetworkRequest::Manual);
	_reply = _network.get(request);
	_reply->setReadBufferSize(1024 * 1024);
	QObject::connect(_reply, &QNetworkReply::readyRead, this, [=] {
		const auto data = _reply->readAll();
		_received += data.size();
		if (_received > Media::Transcription::ModelSize
			|| _download->write(data) != data.size()) {
			_reply->abort();
			return;
		}
		_hash.addData(data);
		const auto progress = int(100 * _received / Media::Transcription::ModelSize);
		if (_progress != progress) {
			_progress = progress;
			publish(tr::lng_local_transcribe_model_loading(
				tr::now,
				lt_percent, QString::number(progress)));
		}
	});
	QObject::connect(_reply, &QNetworkReply::finished, this, [=] {
		const auto reply = std::exchange(_reply, nullptr);
		const auto valid = !_cancelled->load()
			&& reply->error() == QNetworkReply::NoError
			&& reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
			&& _received == Media::Transcription::ModelSize
			&& _hash.result() == Media::Transcription::ModelDigest();
		reply->deleteLater();
		const auto saved = valid && _download->commit();
		_download.reset();
		if (!saved) {
			finish(ErrorText(Error::Model), true);
			return;
		}
		_modelInvalid = false;
		_elapsed.restart();
		check();
	});
}

void LocalTranscription::recognize() {
	_running = true;
	publish(tr::lng_local_transcribe_recognizing(tr::now, lt_percent, u"0"_q));
	const auto bytes = _media->bytes();
	const auto audioDirectory = _audioDirectory;
	const auto location = bytes.isEmpty()
		? _media->owner()->location(true)
		: Core::FileLocation();
	const auto model = modelPath();
	const auto cancelled = _cancelled;
	const auto progress = crl::guard(this, [=](int value) {
		publish(tr::lng_local_transcribe_recognizing(
			tr::now,
			lt_percent, QString::number(value)));
	});
	const auto done = crl::guard(this, [=](Media::Transcription::Result result) {
		_running = false;
		_modelInvalid = (result.error == Error::Model);
		finish(result.error == Error::None ? result.text : ErrorText(result.error),
			result.error != Error::None);
	});
	crl::async([=] {
		auto audio = bytes;
		if (audio.isEmpty() && location.accessEnable()) {
			const auto path = audioDirectory
				? audioDirectory->filePath(u"audio"_q)
				: location.name();
			{
				auto file = QFile(path);
				if (file.open(QIODevice::ReadOnly)
					&& file.size() <= Media::Transcription::MaxAudioBytes) {
					audio = file.read(Media::Transcription::MaxAudioBytes + 1);
				}
			}
			location.accessDisable();
		}
		auto result = Media::Transcription::Recognize(model, audio, *cancelled, [=](int value) {
			crl::on_main([=] { progress(value); });
		});
		crl::on_main([=] { done(result); });
	});
}

void LocalTranscription::clearAudioDownload() {
	if (_media && _audioDirectory) {
		const auto document = _media->owner();
		const auto path = _audioDirectory->filePath(u"audio"_q);
		if (document->loading() && document->loadingFilePath() == path) {
			document->cancel();
		}
		if (document->filepath() == path) {
			document->setLocation(Core::FileLocation());
		}
	}
	_audioDirectory.reset();
}

void LocalTranscription::finish(QString text, bool failed) {
	const auto id = *_active;
	_timer.stop();
	clearAudioDownload();
	_media.reset();
	_active.reset();
	if (!_cancelled->load()) {
		_update(id, std::move(text), true, failed);
	}
	_cancelled.reset();
	crl::on_main(crl::guard(this, [=] { next(); }));
}

} // namespace Api
