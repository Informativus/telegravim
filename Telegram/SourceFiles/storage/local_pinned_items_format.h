/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/local_data_store.h"

namespace Storage::LocalData::PinnedItems {

constexpr auto kMaximumOrder = 4096;

[[nodiscard]] bool ValidScope(const QString &scope);
[[nodiscard]] bool ValidFilterId(const QString &id);
[[nodiscard]] QString PeerKey(const QJsonObject &peer);
[[nodiscard]] QString StableId(const Account &account, const QString &key);
[[nodiscard]] QString PinId(
	const Account &account,
	const QString &scope,
	const QJsonObject &peer);
[[nodiscard]] QString OrderId(const Account &account, const QString &scope);
[[nodiscard]] QString FilterId(const Account &account, const QString &id);
[[nodiscard]] Result Validate(const QJsonObject &module, const Account &account);

} // namespace Storage::LocalData::PinnedItems
