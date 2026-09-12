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

class QFont;
class QPainter;

namespace Ui::Text {
class String;
} // namespace Ui::Text

namespace Core::VimKeymap {

struct HintBadge {
	QString label;
	QPoint anchor;
	QRect target;
	QString suffix;
};

[[nodiscard]] QRect LinkHintTargetRect(
	QPoint hit,
	QRect bounds,
	Fn<bool(QPoint)> matches);
[[nodiscard]] std::vector<QRect> LayoutHintBadges(
	const std::vector<QRect> &desired,
	QRect bounds,
	int gap,
	const std::vector<QRect> &targets = {});
void PaintHintBadges(
	QPainter &p,
	const std::vector<HintBadge> &hints,
	const QString &prefix,
	const QFont &font,
	QRect bounds,
	QSize padding,
	int gap,
	bool visual);

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
void PaintMessageCursor(QPainter &p, QRect characterRect);
[[nodiscard]] QRect TextCursorRect(
	const Ui::Text::String &text,
	int width,
	int symbol);
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
[[nodiscard]] bool ShouldAddScannedLinkHint(
	bool serviceMessage,
	bool hasLink,
	bool overMessageText);
[[nodiscard]] bool ShouldAddInlinePlaybackHint(
	bool voiceOrVideoMessage,
	bool mediaVisible,
	bool scannedPlaybackLink,
	bool directPlaybackLink);
[[nodiscard]] bool ShouldAddStickerHint(
	bool sticker,
	bool mediaVisible,
	bool hasLink);
[[nodiscard]] int MoveStickerGridSelection(
	int selected,
	int count,
	int delta);
[[nodiscard]] QRect GroupedMediaHintRect(
	QRect groupItemRect,
	QPoint itemInnerTopLeft,
	int itemTop);
[[nodiscard]] int FindTextSelectionLength(
	int upperBound,
	Fn<bool(int)> hasTextFrom);
[[nodiscard]] int TextParagraphOffset(
	int position,
	int direction,
	int textLength,
	Fn<bool(int)> isBreak);
[[nodiscard]] int ResolveTextCursorOffset(
	int wanted,
	int direction,
	int textLength,
	Fn<bool(int)> isSelectable);

} // namespace Core::VimKeymap
