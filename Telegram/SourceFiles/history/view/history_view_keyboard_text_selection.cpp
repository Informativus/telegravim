/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_keyboard_text_selection.h"

#include "core/vim_keymap_bindings.h"
#include "core/vim_keymap_geometry.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_element.h"
#include "history/history_item.h"
#include "layout/layout_selection.h"
#include "ui/text/text.h"
#include "styles/style_chat.h"

#include <QtGui/QKeyEvent>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <vector>

namespace HistoryView {
namespace {

constexpr auto kInvalidTextOffset = 0xFFFF;
constexpr auto kMaxCommandCount = 999;

class MessageText final {
public:
	MessageText(not_null<Element*> view, int length)
	: _view(view)
	, _cache(length) {
	}

	[[nodiscard]] int size() const {
		return int(_cache.size());
	}

	[[nodiscard]] QChar at(int offset) {
		if (offset < 0 || offset >= size()) {
			return QChar::LineFeed;
		}
		auto &cached = _cache[offset];
		if (cached.isNull()) {
			const auto text = _view->selectedText(TextSelection(
				uint16(offset), uint16(offset + 1))).rich.text;
			cached = text.isEmpty() ? QChar(QChar::LineFeed) : text.front();
		}
		return cached;
	}

	[[nodiscard]] bool newline(int offset) {
		const auto ch = at(offset);
		return ch == QChar::LineFeed || ch == QChar::CarriageReturn
			|| ch == QChar::LineSeparator || ch == QChar::ParagraphSeparator;
	}

	[[nodiscard]] int wordClass(int offset, bool big) {
		return at(offset).isSpace() ? 0 : (big ? 1
			: Ui::Text::IsWordSeparator(
				[&](int i) { return at(i); }, size(), offset) ? 2 : 1);
	}

	[[nodiscard]] int lineStart(int position) {
		while (position > 0 && !newline(position - 1)) {
			--position;
		}
		return position;
	}

	[[nodiscard]] int lineEnd(int position) {
		while (position < size() && !newline(position)) {
			++position;
		}
		return position;
	}

	[[nodiscard]] bool blankLine(int position) {
		const auto end = lineEnd(position);
		for (auto i = lineStart(position); i < end; ++i) {
			if (!at(i).isSpace()) {
				return false;
			}
		}
		return true;
	}

private:
	not_null<Element*> _view;
	std::vector<QChar> _cache;

};

[[nodiscard]] int WordStart(
		MessageText &text,
		int position,
		bool forward,
		bool big) {
	if (forward) {
		const auto kind = text.wordClass(position, big);
		while (position < text.size() && text.wordClass(position, big) == kind) {
			++position;
		}
		while (position < text.size() && text.at(position).isSpace()) {
			++position;
		}
	} else {
		position = std::max(position - 1, 0);
		while (position > 0 && text.at(position).isSpace()) {
			--position;
		}
		const auto kind = text.wordClass(position, big);
		while (position > 0 && text.wordClass(position - 1, big) == kind) {
			--position;
		}
	}
	return position;
}

[[nodiscard]] int WordEnd(MessageText &text, int position, bool big) {
	position = std::min(position + 1, text.size() - 1);
	while (position < text.size() - 1 && text.at(position).isSpace()) {
		++position;
	}
	const auto kind = text.wordClass(position, big);
	while (position < text.size() - 1
		&& text.wordClass(position + 1, big) == kind) {
		++position;
	}
	return position;
}

[[nodiscard]] TextSelection WordObject(
		MessageText &text,
		int position,
		bool around,
		bool big,
		int count) {
	auto from = position;
	auto till = position + 1;
	const auto kind = text.wordClass(position, big);
	while (from > 0 && text.wordClass(from - 1, big) == kind) {
		--from;
	}
	while (till < text.size() && text.wordClass(till, big) == kind) {
		++till;
	}
	if (around && !kind && till < text.size()) {
		const auto next = text.wordClass(till, big);
		while (till < text.size() && text.wordClass(till, big) == next) {
			++till;
		}
	}
	for (auto i = 1; i < count && till < text.size(); ++i) {
		if (around) {
			while (till < text.size() && text.at(till).isSpace()) {
				++till;
			}
		}
		const auto next = text.wordClass(till, big);
		while (till < text.size() && text.wordClass(till, big) == next) {
			++till;
		}
	}
	if (around && kind) {
		if (till < text.size() && text.at(till).isSpace()) {
			while (till < text.size() && text.at(till).isSpace()) {
				++till;
			}
		} else {
			while (from > 0 && text.at(from - 1).isSpace()) {
				--from;
			}
		}
	}
	return TextSelection(uint16(from), uint16(till));
}

[[nodiscard]] TextSelection ParagraphObject(
		MessageText &text,
		int position,
		bool around,
		int count) {
	auto from = text.lineStart(position);
	auto till = text.lineEnd(position);
	const auto blank = text.blankLine(position);
	while (from > 0 && text.blankLine(from - 1) == blank) {
		from = text.lineStart(from - 1);
	}
	for (auto i = 0; i < count; ++i) {
		while (till < text.size() - 1 && text.blankLine(till + 1) == blank) {
			till = text.lineEnd(till + 1);
		}
		if (i + 1 < count) {
			while (till < text.size() - 1 && text.blankLine(till + 1)) {
				till = text.lineEnd(till + 1);
			}
			if (till < text.size() - 1) {
				till = text.lineEnd(till + 1);
			}
		}
	}
	if (around) {
		const auto end = till;
		if (blank && till < text.size() - 1) {
			till = text.lineEnd(till + 1);
			while (till < text.size() - 1 && !text.blankLine(till + 1)) {
				till = text.lineEnd(till + 1);
			}
		}
		while (till < text.size() - 1 && text.blankLine(till + 1)) {
			till = text.lineEnd(till + 1);
		}
		if (till == end) {
			while (from > 0 && text.blankLine(from - 1)) {
				from = text.lineStart(from - 1);
			}
		}
	}
	return TextSelection(uint16(from), uint16(std::min(till + 1, text.size())));
}

[[nodiscard]] std::optional<TextSelection> DelimitedObject(
		MessageText &text,
		int position,
		QChar open,
		QChar close,
		bool around,
		int count) {
	auto pairs = std::vector<std::pair<int, int>>();
	auto stack = std::vector<int>();
	const auto quote = (open == close);
	const auto from = quote ? text.lineStart(position) : 0;
	const auto till = quote ? text.lineEnd(position) : text.size();
	auto escaped = false;
	for (auto i = from; i < till; ++i) {
		const auto ch = text.at(i);
		if (escaped) {
			escaped = false;
			continue;
		} else if (ch == QChar('\\')) {
			escaped = true;
			continue;
		}
		if (ch == close && !stack.empty()) {
			const auto start = stack.back();
			stack.pop_back();
			if ((start <= position && position <= i) || start > position) {
				pairs.emplace_back(start, i);
			}
		} else if (ch == open) {
			stack.push_back(i);
		}
	}
	if (pairs.empty()) {
		return std::nullopt;
	}
	auto enclosing = std::vector<std::pair<int, int>>();
	for (const auto pair : pairs) {
		if (pair.first <= position) {
			enclosing.push_back(pair);
		}
	}
	auto selected = pairs.front();
	if (!enclosing.empty()) {
		if (!quote && count > int(enclosing.size())) {
			return std::nullopt;
		}
		selected = enclosing[quote ? 0 : count - 1];
	} else if (!quote) {
		if (open != QChar('(') || count != 1) {
			return std::nullopt;
		}
		selected = *std::min_element(pairs.begin(), pairs.end());
	}
	const auto includeDelimiters = around || (quote && count > 1);
	auto first = selected.first + (includeDelimiters ? 0 : 1);
	auto last = selected.second + (includeDelimiters ? 1 : 0);
	if (quote && around) {
		if (position >= first && last < till && text.at(last).isSpace()) {
			while (last < till && text.at(last).isSpace()) {
				++last;
			}
		} else {
			while (first > from && text.at(first - 1).isSpace()) {
				--first;
			}
		}
	}
	return TextSelection(uint16(first), uint16(last));
}

[[nodiscard]] TextSelection SentenceObject(
		MessageText &text,
		int position,
		bool around,
		int count) {
	auto from = 0;
	auto till = 0;
	while (till < text.size()) {
		from = till;
		while (from < text.size() && text.at(from).isSpace()) {
			++from;
		}
		till = from;
		while (till < text.size()) {
			const auto ch = text.at(till++);
			if (ch == QChar('.') || ch == QChar('!') || ch == QChar('?')) {
				auto end = till;
				while (end < text.size() && u")]}\"'"_q.contains(text.at(end))) {
					++end;
				}
				if (end == text.size() || text.at(end).isSpace()) {
					till = end;
					break;
				}
			} else if (text.newline(till - 1) && text.blankLine(till)) {
				break;
			}
		}
		if (till > position) {
			break;
		}
	}
	for (auto i = 1; i < count && till < text.size(); ++i) {
		till = SentenceObject(text, till, false, 1).to;
	}
	if (around) {
		const auto end = till;
		while (till < text.size() && text.at(till).isSpace()) {
			++till;
		}
		if (end == till) {
			while (from > 0 && text.at(from - 1).isSpace()) {
				--from;
			}
		}
	}
	return TextSelection(uint16(from), uint16(till));
}

[[nodiscard]] std::optional<TextSelection> TagObject(
		MessageText &text,
		int position,
		bool around,
		int count) {
	struct Tag {
		QString name;
		int from = 0;
		int till = 0;
	};
	auto stack = std::vector<Tag>();
	for (auto i = 0; i < text.size(); ++i) {
		if (text.at(i) != QChar('<')) {
			continue;
		}
		const auto start = i;
		const auto closing = text.at(i + 1) == QChar('/');
		i += closing ? 2 : 1;
		auto name = QString();
		while (i < text.size()) {
			const auto ch = text.at(i);
			if (!ch.isLetterOrNumber() && ch != QChar('-')
				&& ch != QChar('_') && ch != QChar(':')) {
				break;
			}
			name += ch.toLower();
			++i;
		}
		auto quote = QChar();
		for (; i < text.size(); ++i) {
			const auto ch = text.at(i);
			if (!quote.isNull()) {
				if (ch == quote) {
					quote = QChar();
				}
			} else if (ch == QChar('\'') || ch == QChar('"')) {
				quote = ch;
			} else if (ch == QChar('>')) {
				break;
			}
		}
		if (i == text.size() || name.isEmpty() || text.at(i - 1) == QChar('/')) {
			continue;
		} else if (!closing) {
			stack.push_back({ name, start, i + 1 });
		} else if (!stack.empty() && stack.back().name == name) {
			const auto open = stack.back();
			stack.pop_back();
			if (open.from <= position && position <= i && --count == 0) {
				return TextSelection(uint16(around ? open.from : open.till),
					uint16(around ? i + 1 : start));
			}
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<TextSelection> TextObject(
		MessageText &text,
		int position,
		QChar object,
		bool around,
		int count) {
	if (object == QChar('w') || object == QChar('W')) {
		return WordObject(text, position, around, object == QChar('W'), count);
	} else if (object == QChar('p')) {
		return ParagraphObject(text, position, around, count);
	} else if (object == QChar('s')) {
		return SentenceObject(text, position, around, count);
	} else if (object == QChar('t')) {
		return TagObject(text, position, around, count);
	}
	const auto pairs = u"()[]{}<>\"\"''``"_q;
	if (object == QChar('b')) {
		object = QChar('(');
	} else if (object == QChar('B')) {
		object = QChar('{');
	}
	const auto index = pairs.indexOf(object);
	return (index < 0) ? std::nullopt : DelimitedObject(
		text,
		position,
		pairs[index - index % 2],
		pairs[index - index % 2 + 1],
		around,
		count);
}

[[nodiscard]] MessageSelection FlatSelection(int from, int till) {
	return MessageSelection::Flat(
		TextSelection(uint16(from), uint16(till)),
		{ uint16(from), false },
		{ uint16(std::max(from, till - 1)), false });
}

[[nodiscard]] std::optional<int> MaxTextOffset(not_null<Element*> view) {
	const auto whole = view->adjustSelection(
		TextSelection(0, kInvalidTextOffset),
		TextSelectType::Letters);
	auto maxOffset = int(whole.to);
	if (maxOffset >= kInvalidTextOffset) {
		maxOffset = Core::VimKeymap::FindTextSelectionLength(
			kInvalidTextOffset,
			[=](int from) {
				return !view->selectedText(TextSelection(
					uint16(from),
					kInvalidTextOffset)).empty();
			});
	}
	return (maxOffset > 0) ? std::optional<int>(maxOffset) : std::nullopt;
}

[[nodiscard]] MessageSelectionFlatEndpoint CursorAtOffset(
		int offset,
		int maxOffset) {
	return {
		.symbol = uint16(std::clamp(offset, 0, maxOffset - 1)),
		.afterSymbol = false,
	};
}

[[nodiscard]] bool IsSelectableOffset(
		not_null<Element*> view,
		int offset) {
	return !view->selectedText(TextSelection(
		uint16(offset),
		uint16(offset + 1))).empty();
}

[[nodiscard]] std::optional<MessageSelectionFlatEndpoint>
SelectableCursorAtOffset(
		not_null<Element*> view,
		int wanted,
		int direction,
		int maxOffset) {
	const auto offset = Core::VimKeymap::ResolveTextCursorOffset(
		wanted,
		direction,
		maxOffset,
		[=](int value) { return IsSelectableOffset(view, value); });
	return (offset >= 0)
		? std::optional<MessageSelectionFlatEndpoint>(
			CursorAtOffset(offset, maxOffset))
		: std::nullopt;
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
	const auto normalized = SelectableCursorAtOffset(
		view,
		current.offset(),
		1,
		*maxOffset);
	if (!normalized) {
		return std::nullopt;
	}
	const auto target = int(normalized->symbol);
	if (const auto rect = view->textCursorRect(target); !rect.isEmpty()) {
		return rect.center();
	}
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

[[nodiscard]] std::optional<QRect> FindCursorRect(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current) {
	if (const auto rect = view->textCursorRect(current.offset())
		; !rect.isEmpty()) {
		return rect;
	}
	const auto point = FindCursorPoint(view, current);
	const auto maxOffset = MaxTextOffset(view);
	if (!point || !maxOffset) {
		return std::nullopt;
	}
	const auto normalized = SelectableCursorAtOffset(
		view,
		current.offset(),
		1,
		*maxOffset);
	if (!normalized) {
		return std::nullopt;
	}
	const auto target = int(normalized->symbol);
	const auto inner = view->innerGeometry();
	const auto request = LookupSymbolRequest();
	const auto rect = Core::VimKeymap::LinkHintTargetRect(
		*point,
		inner,
		[&](QPoint hit) {
			const auto endpoint = FlatEndpoint(view->textState(hit, request));
			return endpoint && int(endpoint->symbol) == target;
		});
	return rect.isEmpty() ? std::nullopt : std::optional<QRect>(rect);
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
		const auto endpoint = state.overMessageText
			&& state.selectionCursor.isFlat()
			? std::optional<MessageSelectionFlatEndpoint>(state.selectionCursor.flat)
			: FlatEndpoint(state);
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

[[nodiscard]] QChar CommandCharacter(not_null<QKeyEvent*> e) {
	const auto text = e->text();
	auto ch = text.isEmpty() ? QChar(e->key()) : text.front();
	const auto russian = u"йцукенгшщзхъфывапролджэячсмитьбюё"_q;
	const auto latin = u"qwertyuiop[]asdfghjkl;'zxcvbnm,.`"_q;
	const auto index = russian.indexOf(ch.toLower());
	if (index >= 0) {
		ch = latin[index];
		if (e->modifiers() & Qt::ShiftModifier) {
			const auto punctuation = u"[];',.`"_q;
			const auto shifted = u"{}:\"<>~"_q;
			const auto symbol = punctuation.indexOf(ch);
			if (symbol >= 0) {
				ch = shifted[symbol];
			}
		}
	}
	if (ch.isLetter()) {
		return (e->modifiers() & Qt::ShiftModifier)
			? ch.toUpper() : ch.toLower();
	}
	return ch;
}

struct CommandMotion {
	int position = 0;
	bool inclusive = false;
	bool linewise = false;
};

[[nodiscard]] std::optional<CommandMotion> FindCommandMotion(
		MessageText &text,
		int position,
		QChar command,
		QChar target,
		int count) {
	const auto step = command.isUpper() ? -1 : 1;
	const auto from = text.lineStart(position);
	const auto till = text.lineEnd(position);
	for (auto i = position + step; i >= from && i < till; i += step) {
		if (text.at(i) == target && --count == 0) {
			return CommandMotion{
				.position = i - (command.toLower() == QChar('t') ? step : 0),
				.inclusive = true,
			};
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<CommandMotion> ResolveCommandMotion(
		not_null<Element*> view,
		MessageText &text,
		int position,
		QChar key,
		int count,
		bool yank) {
	auto result = CommandMotion{ position };
	const auto last = text.size() - 1;
	for (auto i = 0; i < count; ++i) {
		const auto before = result.position;
		switch (key.unicode()) {
		case 'h': result.position = std::max(0, before - 1); break;
		case 'l': result.position = std::min(last + (yank ? 1 : 0), before + 1); break;
		case 'w': case 'W': case 'b': case 'B':
			result.position = WordStart(text, before,
				key.toLower() == QChar('w'), key.isUpper());
			break;
		case 'e': case 'E':
			result.position = WordEnd(text, before, key.isUpper());
			result.inclusive = true;
			break;
		case '0': case '|': case '/':
			result.position = text.lineStart(before);
			break;
		case '^':
			result.position = text.lineStart(before);
			while (result.position < text.lineEnd(before)
				&& text.at(result.position).isSpace()) {
				++result.position;
			}
			break;
		case '$': case ';':
			result.position = std::max(text.lineStart(before), text.lineEnd(before) - 1);
			result.inclusive = true;
			if (i + 1 < count && text.lineEnd(before) < last) {
				result.position = text.lineEnd(before) + 1;
			}
			break;
		case 'g': case 'G':
			result.position = (key == QChar('g')) ? 0 : last;
			result.inclusive = true;
			result.linewise = yank;
			break;
		case 'j': case 'k':
			if (yank) {
				result.position = (key == QChar('j'))
					? std::min(last, text.lineEnd(before) + 1)
					: std::max(0, text.lineStart(before) - 1);
				result.linewise = true;
			} else if (const auto next = MoveByLine(
					view,
					CursorAtOffset(before, text.size()),
					key == QChar('j') ? 1 : -1)) {
				result.position = next->offset();
			}
			break;
		case '{': case '}':
			result.position = Core::VimKeymap::TextParagraphOffset(
				before,
				key == QChar('}') ? 1 : -1,
				text.size(),
				[&](int offset) { return text.newline(offset); });
			break;
		default: return std::nullopt;
		}
		if (result.position == before || result.position >= text.size()) {
			break;
		}
	}
	return result;
}

} // namespace

void KeyboardTextSelection::resetCommand() {
	_commandItem = nullptr;
	_commandCount = 0;
	_operatorCount = 1;
	_yankPending = false;
	_gotoPending = false;
	_objectPending = QChar();
	_findPending = QChar();
}

std::optional<KeyboardTextCommand> KeyboardTextSelection::handleCommandKey(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current,
		const MessageSelection *visual,
		not_null<QKeyEvent*> e) {
	// Operators and text objects span separate key events. Keep their counts
	// separate so 2y3w selects six words, and consume an invalid continuation
	// without forwarding it to chat actions. Leaving the text cursor resets
	// this state; changing messages also cancels an unfinished command before
	// any key can apply it to a different message.
	if (_commandItem != view->data().get()) {
		resetCommand();
		_commandItem = view->data().get();
	}
	const auto pending = _yankPending || _gotoPending
		|| !_objectPending.isNull() || !_findPending.isNull() || _commandCount;
	if (e->isAutoRepeat()) {
		return pending ? std::optional(KeyboardTextCommand()) : std::nullopt;
	}
	const auto modifiers = Core::VimKeymap::Bindings::CleanModifiers(e);
	if (modifiers != Qt::NoModifier && modifiers != Qt::ShiftModifier) {
		resetCommand();
		return std::nullopt;
	} else if (Core::VimKeymap::Bindings::IsPlainEscape(e)) {
		resetCommand();
		return pending ? std::optional(KeyboardTextCommand()) : std::nullopt;
	}
	auto key = CommandCharacter(e);
	if (!pending && visual && key == QChar('y')) {
		return KeyboardTextCommand{ .selection = *visual, .copy = true };
	}
	if (_objectPending.isNull() && _findPending.isNull()
		&& key >= QChar('0') && key <= QChar('9')
		&& (key != QChar('0') || _commandCount)) {
		_commandCount = std::min(
			kMaxCommandCount,
			_commandCount * 10 + key.unicode() - '0');
		return KeyboardTextCommand();
	} else if (_findPending.isNull() && !_yankPending && !visual
		&& key == QChar('y')) {
		_yankPending = true;
		_operatorCount = std::max(1, _commandCount);
		_commandCount = 0;
		return KeyboardTextCommand();
	} else if (_objectPending.isNull() && _findPending.isNull()
		&& (_yankPending || visual)
		&& (key == QChar('i') || key == QChar('a'))) {
		_objectPending = key;
		return KeyboardTextCommand();
	} else if (_objectPending.isNull() && _findPending.isNull()
		&& (_yankPending || _commandCount) && key == QChar('g')
		&& !_gotoPending) {
		_gotoPending = true;
		return KeyboardTextCommand();
	} else if (_gotoPending && key != QChar('g')) {
		resetCommand();
		return KeyboardTextCommand();
	} else if (!pending && key == QChar('g')) {
		return std::nullopt;
	} else if (_objectPending.isNull() && _findPending.isNull()
		&& (key.toLower() == QChar('f') || key.toLower() == QChar('t'))) {
		_findPending = key;
		return KeyboardTextCommand();
	}
	const auto count = std::min(
		kMaxCommandCount,
		_operatorCount * std::max(1, _commandCount));
	const auto object = _objectPending;
	const auto find = _findPending;
	const auto yank = _yankPending || (find.isNull() && key == QChar('Y'));
	const auto line = find.isNull()
		&& ((yank && (key == QChar('y') || key == QChar('Y')))
			|| key == QChar('V'));
	resetCommand();
	const auto length = MaxTextOffset(view);
	if (!length) {
		return KeyboardTextCommand();
	}
	auto text = MessageText(view, *length);
	const auto position = std::clamp(int(current.offset()), 0, *length - 1);
	auto range = std::optional<TextSelection>();
	if (!object.isNull()) {
		range = TextObject(text, position, key, object == QChar('a'), count);
	} else if (line) {
		auto end = text.lineEnd(position);
		for (auto i = 1; i < count && end < *length - 1; ++i) {
			end = text.lineEnd(end + 1);
		}
		range = TextSelection(uint16(text.lineStart(position)),
			uint16(std::min(end + 1, *length)));
	} else {
		const auto motion = !find.isNull()
			? (e->text().isEmpty() ? std::nullopt : FindCommandMotion(
				text, position, find, e->text().front(), count))
			: ResolveCommandMotion(view, text, position, key, count, yank);
		if (!motion) {
			return pending ? std::optional(KeyboardTextCommand()) : std::nullopt;
		} else if (!yank) {
			const auto cursor = SelectableCursorAtOffset(
				view,
				motion->position,
				motion->position >= position ? 1 : -1,
				*length);
			if (!cursor) {
				return KeyboardTextCommand();
			} else if (!visual) {
				return KeyboardTextCommand{ .cursor = cursor };
			}
			const auto anchor = visual->anchor.isFlat()
				? visual->anchor.flat : CursorAtOffset(visual->flat.from, *length);
			return KeyboardTextCommand{
				.selection = MessageSelection::Flat(
					TextSelection(
						std::min(anchor.symbol, cursor->symbol),
						uint16(std::max(anchor.symbol, cursor->symbol) + 1)),
					anchor,
					*cursor),
			};
		}
		auto from = std::min(position, motion->position);
		auto till = std::max(position, motion->position) + (motion->inclusive ? 1 : 0);
		if (motion->linewise) {
			from = text.lineStart(from);
			till = text.lineEnd(std::min(till, *length - 1)) + 1;
		}
		range = TextSelection(uint16(from), uint16(std::min(till, *length)));
	}
	if (!range || range->empty() || view->selectedText(*range).empty()) {
		return KeyboardTextCommand();
	}
	return KeyboardTextCommand{
		.selection = FlatSelection(range->from, range->to),
		.copy = yank,
	};
}

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
	}
	const auto vertical = (key == Qt::Key_Up || key == Qt::Key_Down);
	const auto byParagraph = vertical && (modifiers & Qt::ControlModifier);
	if (vertical && !byParagraph) {
		return MoveByLine(view, current, (key == Qt::Key_Down) ? 1 : -1);
	}
	const auto maxOffset = MaxTextOffset(view);
	if (!maxOffset) {
		return std::nullopt;
	}
	const auto position = int(current.offset());
	const auto forward = (key == Qt::Key_Right || key == Qt::Key_Down);
#ifdef Q_OS_MAC
	const auto byWord = (modifiers & Qt::AltModifier) != 0;
#else // Q_OS_MAC
	const auto byWord = (modifiers & Qt::ControlModifier) != 0;
#endif // Q_OS_MAC
	auto wanted = position;
	if (byParagraph) {
		wanted = Core::VimKeymap::TextParagraphOffset(
			position,
			forward ? 1 : -1,
			*maxOffset,
			[=](int symbol) {
				const auto one = view->selectedText(TextSelection(
					uint16(symbol),
					uint16(symbol + 1))).rich.text;
				return one.isEmpty()
					|| one.front() == QChar::LineFeed
					|| one.front() == QChar::CarriageReturn
					|| one.front() == QChar::LineSeparator
					|| one.front() == QChar::ParagraphSeparator;
			});
	} else if (key == Qt::Key_Home) {
		wanted = 0;
	} else if (key == Qt::Key_End) {
		wanted = *maxOffset - 1;
	} else if (byWord) {
		const auto at = [&](int symbol) {
			const auto one = view->selectedText(
				TextSelection(uint16(symbol), uint16(symbol + 1))).rich.text;
			return one.isEmpty()
				? QChar(QChar::ObjectReplacementCharacter)
				: one[0];
		};
		const auto separator = [&](int symbol) {
			return Ui::Text::IsWordSeparator(at, *maxOffset, symbol);
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
	const auto direction = (key == Qt::Key_Home)
		? 1
		: (key == Qt::Key_End)
		? -1
		: (forward ? 1 : -1);
	return SelectableCursorAtOffset(
		view,
		wanted,
		direction,
		*maxOffset);
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
	const auto focus = anchor;
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
		_anchor = {
			uint16(focusAtEnd ? flat.from : (flat.to - 1)),
			false,
		};
		_focus = {
			uint16(focusAtEnd ? (flat.to - 1) : flat.from),
			false,
		};
	}

	const auto focus = moveCursor(view, _focus, key, modifiers);
	if (!focus) {
		return std::nullopt;
	}
	_focus = *focus;
	const auto makeResult = [&] {
		const auto range = Core::VimKeymap::MakeVisualSelectionRange(
			_anchor.symbol,
			_focus.symbol,
			*maxOffset);
		return MessageSelection::Flat(
			TextSelection(uint16(range.from), uint16(range.till)),
			_anchor,
			_focus);
	};

	auto result = makeResult();
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

std::optional<QRect> KeyboardTextSelection::cursorRect(
		not_null<Element*> view,
		MessageSelectionFlatEndpoint current) const {
	return FindCursorRect(view, current);
}

} // namespace HistoryView
