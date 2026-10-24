/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/local_data_store.h"

namespace Storage::LocalTransferFormat {

inline constexpr auto kManifestLimit = 32 * 1024 * 1024;
inline constexpr auto kJsonLimit = 16 * 1024 * 1024;
inline constexpr auto kAllJsonLimit = 256 * 1024 * 1024;
inline constexpr auto kEntryLimit = 116512;
inline constexpr auto kPackageLimit = 33LL * 1024 * 1024 * 1024;
inline constexpr auto kAssetLimit = 32LL * 1024 * 1024 * 1024;
inline constexpr auto kDepthLimit = 32;

struct Entry {
	QString path;
	qint64 size = 0;
	QByteArray hash;
	QByteArray json;
	LocalData::AssetReader read;
	QJsonObject extra;
};

struct Manifest {
	QByteArray json;
	std::vector<Entry> entries;
	qint64 payloadBytes = 0;
};

[[nodiscard]] LocalData::Result Prepare(
	const LocalData::Snapshot &snapshot,
	Manifest &result,
	bool includeContent,
	LocalData::Cancel cancel = nullptr);
[[nodiscard]] LocalData::Result ValidateShape(
	const LocalData::Snapshot &snapshot,
	LocalData::Cancel cancel = nullptr);

} // namespace Storage::LocalTransferFormat
