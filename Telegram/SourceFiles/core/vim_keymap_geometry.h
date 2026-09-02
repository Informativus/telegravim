/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QRect>
#include <QtCore/QString>

namespace Core::VimKeymap {

[[nodiscard]] QRect CursorPaintRect(
	QRect characterRect,
	const QString &style,
	int requestedWidth,
	int requestedHeight);

} // namespace Core::VimKeymap
