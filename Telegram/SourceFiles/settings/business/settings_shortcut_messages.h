/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_type.h"

#include <QtCore/QString>

#include <variant>

namespace Settings {

[[nodiscard]] Type ShortcutMessagesId(int shortcutId);
[[nodiscard]] Type ShortcutMessagesId(QString replyId);

} // namespace Settings
