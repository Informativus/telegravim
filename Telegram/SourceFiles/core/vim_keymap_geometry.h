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

struct VisualSelectionRange {
	int from = 0;
	int till = 0;

	[[nodiscard]] bool operator==(const VisualSelectionRange &other) const
		= default;
};

[[nodiscard]] QRect CursorPaintRect(
	QRect characterRect,
	const QString &style,
	int requestedWidth,
	int requestedHeight);
[[nodiscard]] VisualSelectionRange MakeVisualSelectionRange(
	int anchor,
	int focus,
	int textLength);
[[nodiscard]] bool TextVisualModeConsumesKey(
	bool visualMode,
	bool visualKey);
[[nodiscard]] bool EmptyComposeDefersToMessageAction(
	bool composeStateActive,
	bool fieldEmpty,
	bool messageAction);
[[nodiscard]] QRect GroupedMediaHintRect(
	QRect groupItemRect,
	QPoint itemInnerTopLeft,
	int itemTop);
[[nodiscard]] int ResolveTextCursorOffset(
	int wanted,
	int direction,
	int textLength,
	Fn<bool(int)> isSelectable);

} // namespace Core::VimKeymap
