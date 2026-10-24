/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/local_data_store.h"

namespace Storage::LocalData::ChatFolders {

constexpr auto kMaximumFolders = 4096;
constexpr auto kMaximumMembers = 4096;
constexpr auto kMaximumPeersPerFolder = kMaximumMembers;

[[nodiscard]] bool ValidUuid(const QString &id);
[[nodiscard]] QString PeerKey(const QJsonObject &peer);
[[nodiscard]] QString StableId(const Account &account, const QString &key);
[[nodiscard]] QString FolderId(
	const Account &account,
	const QString &folderId);
[[nodiscard]] QString SettingsId(const Account &account);
[[nodiscard]] bool ValidCloudColorScope(const QString &scope);
[[nodiscard]] QString CloudColorId(const Account &account, const QString &scope);
[[nodiscard]] QString MemberId(
	const Account &account,
	const QString &folderId,
	const QJsonObject &peer,
	const QString &mode);
[[nodiscard]] Result Validate(
	const QJsonObject &module,
	const Account &account);

} // namespace Storage::LocalData::ChatFolders
