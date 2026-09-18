/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <atomic>
#include <functional>
#include <vector>

namespace Media::Transcription {

inline constexpr auto ModelSize = qint64(487601967);
inline constexpr auto MaxAudioBytes = qint64(64 * 1024 * 1024);
inline constexpr auto SampleRate = 16000;
inline constexpr auto MaxSamples = SampleRate * 60 * 60;

[[nodiscard]] QString ModelUrl();
[[nodiscard]] QByteArray ModelDigest();
[[nodiscard]] bool VerifyModel(const QString &path, const std::atomic<bool> &cancelled);

struct Result {
	QString text;
	enum class Error { None, Cancelled, Model, Audio, TooLong, Recognition, NoSpeech };
	Error error = Error::None;
};

struct Audio {
	std::vector<float> samples;
	Result::Error error = Result::Error::None;
};

[[nodiscard]] Audio Decode(
	const QByteArray &bytes,
	const std::atomic<bool> &cancelled);
[[nodiscard]] Result Recognize(
	const QString &model,
	const QByteArray &audio,
	const std::atomic<bool> &cancelled,
	const std::function<void(int)> &progress = {});

} // namespace Media::Transcription
