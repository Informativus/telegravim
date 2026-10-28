/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/local_data_store.h"

namespace Storage::LocalData::QuickReplies {

inline constexpr auto kMaximumAttachments = 10;
inline constexpr auto kMaximumMessageAssets = 8192;
inline constexpr auto kMaximumAttachedStickerSets = 1024;
inline constexpr auto kMaximumText = 32768;

[[nodiscard]] bool ValidShortcut(const QString &value);
[[nodiscard]] bool ValidAttachedStickerSets(
	const QJsonObject &media,
	bool rejectUnknownFields = false);
[[nodiscard]] Result Validate(const QJsonObject &module, const Account &account);
[[nodiscard]] QStringList AssetReferences(const QJsonObject &module);

} // namespace Storage::LocalData::QuickReplies
