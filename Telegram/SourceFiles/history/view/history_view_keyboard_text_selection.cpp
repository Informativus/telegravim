/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_keyboard_text_selection.h"

#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_element.h"
#include "history/history_item.h"
#include "layout/layout_selection.h"
#include "ui/text/text.h"
#include "styles/style_chat.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace HistoryView {
namespace {

constexpr auto kInvalidTextOffset = 0xFFFF;

[[nodiscard]] std::optional<int> MaxTextOffset(not_null<Element*> view) {
	const auto whole = view->adjustSelection(
		TextSelection(0, kInvalidTextOffset),
		TextSelectType::Letters);
	const auto maxOffset = int(whole.to);
	return (maxOffset <= 0 || maxOffset >= kInvalidTextOffset)
		? std::nullopt
		: std::optional<int>(maxOffset);
}

[[nodiscard]] MessageSelectionFlatEndpoint CursorAtOffset(
		int offset,
		int maxOffset) {
	return {
		.symbol = uint16(std::clamp(offset, 0, maxOffset - 1)),
		.afterSymbol = false,
	};
}

[[nodiscard]] StateRequest LookupSymbolRequest() {
	auto request = StateRequest();
	request.flags = Ui::Text::StateRequest::Flag::LookupSymbol;
	request.onlyMessageText = true;
	return request;
}

[[nodiscard]] std::optional<MessageSelectionFlatEndpoint> FlatEndpoint(
		const TextState &state) {
	return (state.cursor == CursorState::Text)
		&& state.selectionCursor.isFlat()
		? std::optional<MessageSelectionFlatEndpoint>(
			state.selectionCursor.flat)
		: std::nullopt;
}

[[nodiscard]] std::optional<QPoint> FindCursorPoint(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current) {
	const auto maxOffset = MaxTextOffset(view);
	if (!maxOffset) {
		return std::nullopt;
	}
	const auto target = int(CursorAtOffset(
		current.offset(),
		*maxOffset).symbol);
	const auto inner = view->innerGeometry();
	if (inner.width() <= 0 || inner.height() <= 0) {
		return std::nullopt;
	}
	const auto request = LookupSymbolRequest();
	const auto origin = inner.topLeft();
	const auto lineHeight = st::messageTextStyle.font->height;
	const auto forY = [&](int y) {
		return int(view->textState(origin + QPoint(0, y), request).symbol);
	};

	auto yFrom = 0;
	auto yTill = inner.height() - 1;
	auto symbolFrom = forY(yFrom);
	auto symbolTill = forY(yTill);
	if (symbolFrom < target && symbolTill > target) {
		while (yTill - yFrom >= 2 * lineHeight) {
			const auto middle = (yFrom + yTill) / 2;
			const auto found = forY(middle);
			if (found == target
				|| symbolFrom > found
				|| symbolTill < found) {
				yFrom = yTill = middle;
				break;
			} else if (found < target) {
				yFrom = middle;
				symbolFrom = found;
			} else {
				yTill = middle;
				symbolTill = found;
			}
		}
	} else if (symbolFrom >= target) {
		yTill = yFrom;
	} else {
		yFrom = yTill;
	}

	const auto rowY = origin.y() + (yFrom + yTill) / 2;
	const auto top = std::max(inner.top(), rowY - lineHeight / 2);
	const auto bottom = std::min(inner.bottom(), rowY + lineHeight / 2);
	auto best = std::optional<QPoint>();
	auto bestScore = std::numeric_limits<int>::max();
	for (auto y = top; y <= bottom; y += std::max(1, lineHeight / 3)) {
		for (auto x = inner.left(); x <= inner.right(); ++x) {
			const auto state = view->textState(QPoint(x, y), request);
			const auto endpoint = FlatEndpoint(state);
			if (!endpoint) {
				continue;
			}
			const auto symbol = int(endpoint->symbol);
			const auto symbolDistance = std::abs(symbol - target);
			const auto score = symbolDistance * 10000
				+ std::abs(y - rowY) * 100
				+ std::abs(x - inner.left());
			if (score < bestScore) {
				best = QPoint(x, y);
				bestScore = score;
			}
			if (symbol == target && !endpoint->afterSymbol) {
				return QPoint(x, y);
			}
		}
	}
	return best;
}

[[nodiscard]] std::optional<MessageSelectionFlatEndpoint> EndpointAtLine(
		not_null<Element*> view,
		int x,
		int y) {
	const auto maxOffset = MaxTextOffset(view);
	if (!maxOffset) {
		return std::nullopt;
	}
	const auto inner = view->innerGeometry();
	if (inner.width() <= 0 || inner.height() <= 0) {
		return std::nullopt;
	}
	const auto request = LookupSymbolRequest();
	y = std::clamp(y, inner.top(), inner.bottom());

	auto best = std::optional<MessageSelectionFlatEndpoint>();
	auto bestScore = std::numeric_limits<int>::max();
	for (auto scanX = inner.left(); scanX <= inner.right(); ++scanX) {
		const auto state = view->textState(QPoint(scanX, y), request);
		const auto endpoint = FlatEndpoint(state);
		if (!endpoint) {
			continue;
		}
		const auto score = std::abs(scanX - x);
		if (score < bestScore) {
			best = CursorAtOffset(endpoint->offset(), *maxOffset);
			bestScore = score;
		}
	}
	return best;
}

[[nodiscard]] std::optional<MessageSelectionFlatEndpoint> MoveByLine(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current,
		int direction) {
	const auto point = FindCursorPoint(view, current);
	if (!point) {
		return std::nullopt;
	}
	const auto maxOffset = MaxTextOffset(view);
	if (!maxOffset) {
		return std::nullopt;
	}
	const auto inner = view->innerGeometry();
	const auto lineHeight = st::messageTextStyle.font->height;
	const auto targetY = std::clamp(
		point->y() + direction * lineHeight,
		inner.top(),
		inner.bottom());
	if (targetY == point->y()) {
		return CursorAtOffset(current.offset(), *maxOffset);
	}
	return EndpointAtLine(view, point->x(), targetY);
}

} // namespace

bool KeyboardTextSelection::IsExtendKey(int key) {
	return (key == Qt::Key_Left)
		|| (key == Qt::Key_Right)
		|| (key == Qt::Key_Up)
		|| (key == Qt::Key_Down)
		|| (key == Qt::Key_Home)
		|| (key == Qt::Key_End);
}

std::optional<MessageSelectionFlatEndpoint> KeyboardTextSelection::beginCursor(
		not_null<Element*> view) {
	const auto maxOffset = MaxTextOffset(view);
	if (!maxOffset) {
		return std::nullopt;
	}
	for (auto from = 0; from != *maxOffset; ++from) {
		const auto adjusted = view->adjustSelection(
			TextSelection(uint16(from), uint16(from + 1)),
			TextSelectType::Letters);
		if (adjusted.empty()
			|| adjusted == FullSelection
			|| view->selectedText(adjusted).empty()) {
			continue;
		}
		return MessageSelectionFlatEndpoint{
			.symbol = adjusted.from,
			.afterSymbol = false,
		};
	}
	return std::nullopt;
}

std::optional<MessageSelectionFlatEndpoint> KeyboardTextSelection::moveCursor(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current,
		int key,
		Qt::KeyboardModifiers modifiers) {
	if (!IsExtendKey(key)) {
		return std::nullopt;
	} else if (key == Qt::Key_Up || key == Qt::Key_Down) {
		return MoveByLine(view, current, (key == Qt::Key_Down) ? 1 : -1);
	}
	const auto maxOffset = MaxTextOffset(view);
	if (!maxOffset) {
		return std::nullopt;
	}
	const auto position = int(current.offset());
	const auto forward = (key == Qt::Key_Right);
#ifdef Q_OS_MAC
	const auto byWord = (modifiers & Qt::AltModifier) != 0;
#else // Q_OS_MAC
	const auto byWord = (modifiers & Qt::ControlModifier) != 0;
#endif // Q_OS_MAC
	auto wanted = position;
	if (key == Qt::Key_Home) {
		wanted = 0;
	} else if (key == Qt::Key_End) {
		wanted = *maxOffset - 1;
	} else if (byWord) {
		const auto separator = [&](int symbol) {
			const auto one = view->selectedText(
				TextSelection(uint16(symbol), uint16(symbol + 1))).rich.text;
			return one.isEmpty() || Ui::Text::IsWordSeparator(one[0]);
		};
		auto symbol = std::clamp(position, 0, *maxOffset - 1);
		if (forward) {
			while (symbol < *maxOffset && separator(symbol)) {
				++symbol;
			}
			while (symbol < *maxOffset && !separator(symbol)) {
				++symbol;
			}
		} else {
			if (symbol > 0) {
				--symbol;
			}
			while (symbol > 0 && separator(symbol)) {
				--symbol;
			}
			while (symbol > 0 && !separator(symbol - 1)) {
				--symbol;
			}
		}
		wanted = symbol;
	} else {
		wanted = position + (forward ? 1 : -1);
	}
	return CursorAtOffset(wanted, *maxOffset);
}

std::optional<MessageSelection> KeyboardTextSelection::startSelection(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current) {
	const auto maxOffset = MaxTextOffset(view);
	if (!maxOffset) {
		return std::nullopt;
	}
	const auto from = uint16(std::clamp(
		int(current.offset()),
		0,
		*maxOffset - 1));
	const auto adjusted = view->adjustSelection(
		TextSelection(from, uint16(from + 1)),
		TextSelectType::Letters);
	if (adjusted.empty()
		|| adjusted == FullSelection
		|| view->selectedText(adjusted).empty()) {
		return std::nullopt;
	}
	const auto anchor = MessageSelectionFlatEndpoint{
		.symbol = adjusted.from,
		.afterSymbol = false,
	};
	const auto focus = MessageSelectionFlatEndpoint{
		.symbol = uint16(adjusted.to - 1),
		.afterSymbol = true,
	};
	auto result = MessageSelection::Flat(adjusted, anchor, focus);
	_item = view->data().get();
	_produced = result.flatSelection();
	_has = true;
	_anchor = anchor;
	_focus = focus;
	return result;
}

std::optional<MessageSelection> KeyboardTextSelection::begin(
		not_null<Element*> view) {
	const auto cursor = beginCursor(view);
	return cursor ? startSelection(view, *cursor) : std::nullopt;
}

std::optional<MessageSelection> KeyboardTextSelection::extend(
		not_null<Element*> view,
		const MessageSelection &current,
		int key,
		Qt::KeyboardModifiers modifiers) {
	if (!IsExtendKey(key)) {
		return std::nullopt;
	}
	const auto maxOffset = MaxTextOffset(view);
	if (!maxOffset) {
		return std::nullopt;
	}
	const auto item = view->data().get();
	const auto continuing = _has
		&& (_item == item)
		&& (current.flatSelection() == _produced);
	if (!continuing) {
		if (!current.isFlat()) {
			return std::nullopt;
		}
		const auto flat = current.flat;
		const auto rawAnchor = current.anchor.isFlat()
			? current.anchor.flat.offset()
			: flat.from;
		const auto rawFocus = current.focus.isFlat()
			? current.focus.flat.offset()
			: flat.to;
		const auto focusAtEnd = (rawFocus >= rawAnchor);
		_anchor = { focusAtEnd ? flat.from : flat.to, false };
		_focus = { focusAtEnd ? flat.to : flat.from, false };
	}

	const auto position = int(_focus.offset());
	if (key == Qt::Key_Up || key == Qt::Key_Down) {
		const auto next = MoveByLine(
			view,
			_focus,
			(key == Qt::Key_Down) ? 1 : -1);
		if (!next) {
			return std::nullopt;
		}
		_focus = *next;
		auto result = MessageSelection::Flat(_anchor, _focus);
		if (result.empty()) {
			return std::nullopt;
		}
		_has = true;
		_item = item;
		_produced = result.flatSelection();
		return result;
	}
	const auto forward = (key == Qt::Key_Right);
#ifdef Q_OS_MAC
	const auto byWord = (modifiers & Qt::AltModifier) != 0;
#else // Q_OS_MAC
	const auto byWord = (modifiers & Qt::ControlModifier) != 0;
#endif // Q_OS_MAC
	auto wanted = position;
	if (key == Qt::Key_Home) {
		wanted = 0;
	} else if (key == Qt::Key_End) {
		wanted = *maxOffset;
	} else if (byWord) {
		const auto separator = [&](int symbol) {
			const auto one = view->selectedText(
				TextSelection(uint16(symbol), uint16(symbol + 1))).rich.text;
			return one.isEmpty() || Ui::Text::IsWordSeparator(one[0]);
		};
		auto symbol = position;
		if (forward) {
			while (symbol < *maxOffset && separator(symbol)) {
				++symbol;
			}
			while (symbol < *maxOffset && !separator(symbol)) {
				++symbol;
			}
		} else {
			if (symbol > 0) {
				--symbol;
			}
			while (symbol > 0 && separator(symbol)) {
				--symbol;
			}
			while (symbol > 0 && !separator(symbol - 1)) {
				--symbol;
			}
		}
		wanted = symbol;
	} else {
		wanted = position + (forward ? 1 : -1);
	}
	_focus = { uint16(std::clamp(wanted, 0, *maxOffset)), false };

	auto result = MessageSelection::Flat(_anchor, _focus);
	_has = true;
	_item = item;
	_produced = result.flatSelection();
	return result;
}

std::optional<QPoint> KeyboardTextSelection::cursorPoint(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current) const {
	return FindCursorPoint(view, current);
}

} // namespace HistoryView
