/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/vim_keymap.h"

#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <span>

namespace Core::VimKeymap {

struct ConfigOption {
	const char *id = nullptr;
	QString description;
	std::optional<IntOptionBounds> bounds;
	QStringList choices;
};

struct ConfigValidation {
	QJsonObject values;
	QString error;
	int line = 0;
	int column = 0;
};

[[nodiscard]] std::span<const ConfigOption> ConfigOptions();
[[nodiscard]] QJsonObject ConfigSnapshot(bool defaults = false);
[[nodiscard]] QJsonObject ConfigSchema();
[[nodiscard]] ConfigValidation ValidateConfig(const QByteArray &json);
[[nodiscard]] QString ApplyConfig(const QByteArray &json);

} // namespace Core::VimKeymap
