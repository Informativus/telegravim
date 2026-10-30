/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/local_data_store.h"

#include <atomic>

namespace Storage::LocalTransfer {

struct Progress {
	quint64 completed = 0;
	quint64 total = 0;
};

using ProgressCallback = std::function<void(Progress)>;

enum class PasswordRequirement {
	MinimumLength,
	NonEmpty,
};

struct Inspection {
	LocalData::Snapshot snapshot;
	QString packageId;
	quint64 packageBytes = 0;
	quint64 assetBytes = 0;
};

[[nodiscard]] LocalData::Result Export(
	const LocalData::Snapshot &snapshot,
	const QString &path,
	QByteArray password,
	const std::atomic<bool> *cancel = nullptr,
	ProgressCallback progress = nullptr,
	PasswordRequirement passwordRequirement = PasswordRequirement::MinimumLength);

[[nodiscard]] LocalData::Result Inspect(
	const QString &path,
	QByteArray password,
	Inspection &result,
	const std::atomic<bool> *cancel = nullptr,
	ProgressCallback progress = nullptr);

} // namespace Storage::LocalTransfer
