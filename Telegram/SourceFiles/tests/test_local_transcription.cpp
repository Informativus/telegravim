/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/transcription/transcription_engine.h"
#include "base/basic_types.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QTcpServer>

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace Media::Transcription;

namespace {

auto Failed = 0;
auto Checked = 0;

void Check(bool value, const char *name) {
	++Checked;
	if (!value) {
		++Failed;
		std::cerr << "FAIL: " << name << std::endl;
	}
}

QByteArray Wave(int rate, int channels, int samples) {
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setByteOrder(QDataStream::LittleEndian);
	stream.writeRawData("RIFF", 4);
	stream << quint32(36 + samples * channels * 2);
	stream.writeRawData("WAVEfmt ", 8);
	stream << quint32(16) << quint16(1) << quint16(channels) << quint32(rate)
		<< quint32(rate * channels * 2) << quint16(channels * 2) << quint16(16);
	stream.writeRawData("data", 4);
	stream << quint32(samples * channels * 2);
	bytes.append(QByteArray(samples * channels * 2, char(0)));
	return bytes;
}

} // namespace

int main(int argc, char *argv[]) {
	auto app = QCoreApplication(argc, argv);
	auto cancelled = std::atomic<bool>(false);
	using Error = Result::Error;
	Check(Decode({}, cancelled).error == Error::Audio, "empty input rejected");
	Check(Decode(QByteArray("not media"), cancelled).error == Error::Audio, "malformed input rejected");
	const auto audio = Wave(48000, 2, 48000);
	const auto decoded = Decode(audio, cancelled);
	Check(decoded.error == Error::None, "stereo WAV accepted");
	Check(decoded.samples.size() == SampleRate, "resampled to mono 16 kHz, including tail");
	Check(std::all_of(decoded.samples.begin(), decoded.samples.end(), [](float value) {
		return std::isfinite(value) && value == 0.;
	}), "decoded silence contains finite zero samples");
	cancelled.store(true);
	Check(Decode(audio, cancelled).error == Error::Cancelled, "cancelled decoding does not start");
	cancelled.store(false);
	Check(Decode(QByteArray(MaxAudioBytes + 1, char(0)), cancelled).error == Error::TooLong,
		"compressed input bound enforced before decoding");
	Check(Decode(Wave(8000, 1, 8000 * 3600), cancelled).error == Error::None,
		"exact duration limit accepted");
	Check(Decode(Wave(8000, 1, 8000 * 3601), cancelled).error == Error::TooLong,
		"decoded duration bound enforced");
	auto temporary = QTemporaryDir();
	Check(temporary.isValid(), "temporary directory available");
	auto model = QFile(temporary.filePath(u"model.bin"_q));
	Check(model.open(QIODevice::WriteOnly), "temporary model created");
	Check(model.resize(ModelSize), "sparse invalid model created");
	model.close();
	Check(!VerifyModel(model.fileName(), cancelled), "correct size with wrong digest rejected");
	Check(!VerifyModel(temporary.filePath(u"missing.bin"_q), cancelled), "missing model rejected");
	cancelled.store(true);
	Check(!VerifyModel(model.fileName(), cancelled), "model hashing can be cancelled");
	cancelled.store(false);
	auto server = QTcpServer();
	Check(server.listen(QHostAddress::LocalHost), "local network trap listening");
	const auto playlist = QByteArray("#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXTINF:10,\nhttp://127.0.0.1:")
		+ QByteArray::number(server.serverPort()) + "/audio.ts\n#EXT-X-ENDLIST\n";
	Check(Decode(playlist, cancelled).error == Error::Audio, "external playlist rejected");
	Check(!server.waitForNewConnection(50), "decoder makes no network request");
	auto external = QFile(temporary.filePath(u"external.wav"_q));
	Check(external.open(QIODevice::WriteOnly), "external audio fixture created");
	Check(external.write(audio) == audio.size(), "external audio fixture written");
	external.close();
	const auto concat = QByteArray("ffconcat version 1.0\nfile '")
		+ QFile::encodeName(external.fileName()) + "'\n";
	Check(Decode(concat, cancelled).error == Error::Audio, "external local file rejected");
	const auto args = app.arguments();
	if (args.size() == 3) {
		auto recording = QFile(args[2]);
		Check(recording.open(QIODevice::ReadOnly), "recognition fixture opened");
		const auto result = Recognize(args[1], recording.readAll(), cancelled);
		Check(result.error == Error::None && !result.text.isEmpty(), "real offline recognition succeeds");
		std::cout << "TRANSCRIPT: " << result.text.toStdString() << std::endl;
	}
	std::cout << (Checked - Failed) << "/" << Checked << " checks passed" << std::endl;
	return Failed ? 1 : 0;
}
