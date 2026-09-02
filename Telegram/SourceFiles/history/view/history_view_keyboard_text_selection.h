/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/history_message_selection.h"

#include <QtCore/QPoint>
#include <QtCore/QRect>

#include <optional>

class HistoryItem;

namespace HistoryView {

class Element;

class KeyboardTextSelection final {
public:
	[[nodiscard]] static bool IsExtendKey(int key);

	[[nodiscard]] std::optional<MessageSelectionFlatEndpoint> beginCursor(
		not_null<Element*> view);
	[[nodiscard]] std::optional<MessageSelectionFlatEndpoint> moveCursor(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current,
		int key,
		Qt::KeyboardModifiers modifiers);
	[[nodiscard]] std::optional<MessageSelection> startSelection(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current);
	[[nodiscard]] std::optional<MessageSelection> begin(
		not_null<Element*> view);
	[[nodiscard]] std::optional<MessageSelection> extend(
		not_null<Element*> view,
		const MessageSelection &current,
		int key,
		Qt::KeyboardModifiers modifiers);
	[[nodiscard]] std::optional<QPoint> cursorPoint(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current) const;
	[[nodiscard]] std::optional<QRect> cursorRect(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current) const;

private:
	HistoryItem *_item = nullptr;
	TextSelection _produced;
	bool _has = false;
	MessageSelectionFlatEndpoint _anchor;
	MessageSelectionFlatEndpoint _focus;

};

} // namespace HistoryView
