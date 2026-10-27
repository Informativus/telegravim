/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/local_data_store.h"

namespace Storage::LocalData::NavigationPreferences {

constexpr auto kMaximumOrder = 4096;

[[nodiscard]] bool ValidReference(const QString &reference);
[[nodiscard]] QString SettingsId(const Account &account);
[[nodiscard]] Result Validate(
	const QJsonObject &module,
	const Account &account);

} // namespace Storage::LocalData::NavigationPreferences
