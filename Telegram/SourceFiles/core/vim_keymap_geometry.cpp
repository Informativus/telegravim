/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_geometry.h"

#include <algorithm>

namespace Core::VimKeymap {

QRect CursorPaintRect(
		QRect characterRect,
		const QString &style,
		int requestedWidth,
		int requestedHeight) {
	const auto lineHeight = std::max(1, characterRect.height());
	const auto characterWidth = std::max(
		std::max(1, characterRect.width()),
		requestedWidth);
	const auto height = std::clamp(requestedHeight, 1, lineHeight);
	if (style == u"underline"_q) {
		const auto thickness = std::clamp(requestedWidth, 1, lineHeight);
		return QRect(
			characterRect.left(),
			characterRect.bottom() - thickness + 1,
			characterWidth,
			thickness);
	}
	return QRect(
		characterRect.left(),
		characterRect.top() + (lineHeight - height) / 2,
		(style == u"block"_q) ? characterWidth : requestedWidth,
		height);
}

VisualSelectionRange MakeVisualSelectionRange(
		int anchor,
		int focus,
		int textLength) {
	if (textLength <= 0) {
		return {};
	}
	anchor = std::clamp(anchor, 0, textLength - 1);
	focus = std::clamp(focus, 0, textLength - 1);
	return {
		.from = std::min(anchor, focus),
		.till = std::max(anchor, focus) + 1,
	};
}

bool TextVisualModeConsumesKey(bool visualMode, bool visualKey) {
	return visualMode && visualKey;
}

} // namespace Core::VimKeymap
