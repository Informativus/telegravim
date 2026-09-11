/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_bindings.h"
#include "core/vim_keymap.h"
#include "media/media_common.h"

#include <QtCore/QStringList>
#include <QtGui/QKeyEvent>

#include <optional>
#include <utility>
#include <cmath>

namespace Core::VimKeymap::Bindings {

bool IsCloseHints(not_null<QKeyEvent*> e) {
	return CleanModifiers(e) == Qt::NoModifier
		&& KeyIs(e, Qt::Key_C, u"c"_q, u"\u0441"_q);
}

bool IsShowMessageHints(not_null<QKeyEvent*> e) {
	return CleanModifiers(e) == Qt::ShiftModifier
		&& KeyIs(e, Qt::Key_F, u"f"_q, u"\u0430"_q);
}

QString ShortestHintLabel(int index, int total, const QString &alphabet) {
	const auto base = int(alphabet.size());
	if (base < 2 || index < 0 || index >= total) {
		return {};
	} else if (total <= base) {
		return alphabet.mid(index, 1);
	}
	auto capacity = int64(base);
	auto length = 1;
	while (capacity <= total / base) {
		capacity *= base;
		++length;
	}
	const auto expanded = (total - capacity + base - 2) / (base - 1);
	const auto shortCount = capacity - expanded;
	auto value = int64(index);
	if (index >= shortCount) {
		value += shortCount * (base - 1);
		++length;
	}
	auto result = QString(length, alphabet.front());
	for (auto i = length; i > 0; value /= base) {
		result[--i] = alphabet[value % base];
	}
	return result;
}

bool IsTextFollowLink(not_null<QKeyEvent*> e, bool pendingStart) {
	return pendingStart
		&& !e->isAutoRepeat()
		&& CleanModifiers(e) == Qt::NoModifier
		&& KeyIs(e, Qt::Key_D, u"d"_q, u"в"_q);
}

std::optional<TextMotion> TextMotionKey(
		not_null<QKeyEvent*> e,
		bool &pendingStart) {
	if (e->isAutoRepeat()) {
		return std::nullopt;
	}
	const auto wasPending = std::exchange(pendingStart, false);
	const auto modifiers = CleanModifiers(e);
	if (modifiers != Qt::NoModifier && modifiers != Qt::ShiftModifier) {
		return std::nullopt;
	} else if (KeyIs(e, Qt::Key_G, u"g"_q, u"п"_q)) {
		if (modifiers == Qt::ShiftModifier) {
			return TextMotion::TextEnd;
		} else if (wasPending) {
			return TextMotion::TextStart;
		}
		pendingStart = true;
		return std::nullopt;
	} else if (e->key() == Qt::Key_BraceLeft
		|| e->text() == u"{"_q
		|| (modifiers == Qt::ShiftModifier
			&& KeyIs(e, Qt::Key_BracketLeft, u"["_q, u"х"_q))) {
		return TextMotion::ParagraphPrevious;
	} else if (e->key() == Qt::Key_BraceRight
		|| e->text() == u"}"_q
		|| (modifiers == Qt::ShiftModifier
			&& KeyIs(e, Qt::Key_BracketRight, u"]"_q, u"ъ"_q))) {
		return TextMotion::ParagraphNext;
	} else if (KeyIs(e, Qt::Key_J, u"j"_q, u"\u043E"_q)) {
		return TextMotion::LineDown;
	} else if (KeyIs(e, Qt::Key_K, u"k"_q, u"\u043B"_q)
		|| (modifiers == Qt::ShiftModifier
			&& KeyIs(e, Qt::Key_H, u"h"_q, u"\u0440"_q))) {
		return TextMotion::LineUp;
	} else if (KeyIs(e, Qt::Key_H, u"h"_q, u"\u0440"_q)) {
		return TextMotion::CharacterLeft;
	} else if (KeyIs(e, Qt::Key_L, u"l"_q, u"\u0434"_q)) {
		return TextMotion::CharacterRight;
	} else if (KeyIs(e, Qt::Key_B, u"b"_q, u"\u0438"_q)) {
		return TextMotion::WordLeft;
	} else if (KeyIs(e, Qt::Key_W, u"w"_q, u"\u0446"_q)
		|| KeyIs(e, Qt::Key_E, u"e"_q, u"\u0443"_q)) {
		return TextMotion::WordRight;
	}
	if (IsLineStart(e)) {
		return TextMotion::LineStart;
	} else if (IsLineEnd(e)) {
		return TextMotion::LineEnd;
	}
	return std::nullopt;
}

bool IsMessageSelection(not_null<QKeyEvent*> e) {
	return CleanModifiers(e) == Qt::NoModifier
		&& !e->isAutoRepeat()
		&& KeyIs(e, Qt::Key_S, u"s"_q, u"\u044B"_q);
}

bool IsMessageReaction(not_null<QKeyEvent*> e) {
	return Matches(u"Shift+r, Shift+к"_q, e);
}

bool IsSelectionForward(not_null<QKeyEvent*> e) {
	return CleanModifiers(e) == Qt::NoModifier
		&& KeyIs(e, Qt::Key_F, u"f"_q, u"\u0430"_q);
}

std::optional<float64> MediaPlaybackSpeed(
		not_null<QKeyEvent*> e,
		float64 current) {
	if (CleanModifiers(e) != Qt::NoModifier) {
		return std::nullopt;
	} else if (KeyIs(e, Qt::Key_Q, u"q"_q, u"\u0439"_q)) {
		return 1.;
	}
	const auto delta = KeyIs(e, Qt::Key_D, u"d"_q, u"\u0432"_q) ? 1
		: KeyIs(e, Qt::Key_A, u"a"_q, u"\u0444"_q) ? -1 : 0;
	return delta
		? std::optional<float64>(std::clamp(
			(std::round(current * 100.) + delta * 10.) / 100.,
			Media::kSpeedMin,
			Media::kSpeedMax))
		: std::nullopt;
}

namespace {

[[nodiscard]] bool TextIs(not_null<QKeyEvent*> e, const QString &latin) {
	return PlainText(e) == latin;
}

[[nodiscard]] bool TextIs(
		not_null<QKeyEvent*> e,
		const QString &latin,
		const QString &cyrillic) {
	const auto text = PlainText(e);
	return text == latin || text == cyrillic;
}

[[nodiscard]] bool MacVirtualKeyIs(not_null<QKeyEvent*> e, Qt::Key key) {
#ifdef Q_OS_MAC
	switch (key) {
	case Qt::Key_A: return e->nativeVirtualKey() == 0;
	case Qt::Key_B: return e->nativeVirtualKey() == 11;
	case Qt::Key_C: return e->nativeVirtualKey() == 8;
	case Qt::Key_D: return e->nativeVirtualKey() == 2;
	case Qt::Key_E: return e->nativeVirtualKey() == 14;
	case Qt::Key_F: return e->nativeVirtualKey() == 3;
	case Qt::Key_G: return e->nativeVirtualKey() == 5;
	case Qt::Key_H: return e->nativeVirtualKey() == 4;
	case Qt::Key_I: return e->nativeVirtualKey() == 34;
	case Qt::Key_J: return e->nativeVirtualKey() == 38;
	case Qt::Key_K: return e->nativeVirtualKey() == 40;
	case Qt::Key_L: return e->nativeVirtualKey() == 37;
	case Qt::Key_M: return e->nativeVirtualKey() == 46;
	case Qt::Key_N: return e->nativeVirtualKey() == 45;
	case Qt::Key_O: return e->nativeVirtualKey() == 31;
	case Qt::Key_P: return e->nativeVirtualKey() == 35;
	case Qt::Key_Q: return e->nativeVirtualKey() == 12;
	case Qt::Key_R: return e->nativeVirtualKey() == 15;
	case Qt::Key_S: return e->nativeVirtualKey() == 1;
	case Qt::Key_T: return e->nativeVirtualKey() == 17;
	case Qt::Key_U: return e->nativeVirtualKey() == 32;
	case Qt::Key_V: return e->nativeVirtualKey() == 9;
	case Qt::Key_W: return e->nativeVirtualKey() == 13;
	case Qt::Key_X: return e->nativeVirtualKey() == 7;
	case Qt::Key_Y: return e->nativeVirtualKey() == 16;
	case Qt::Key_Z: return e->nativeVirtualKey() == 6;
	case Qt::Key_BracketLeft: return e->nativeVirtualKey() == 33;
	case Qt::Key_BracketRight: return e->nativeVirtualKey() == 30;
	default: return false;
	}
#else // Q_OS_MAC
	return false;
#endif // Q_OS_MAC
}

[[nodiscard]] Qt::KeyboardModifier PhysicalControlModifier() {
#ifdef Q_OS_MAC
	return Qt::MetaModifier;
#else // Q_OS_MAC
	return Qt::ControlModifier;
#endif // Q_OS_MAC
}

struct KeyBinding {
	Qt::KeyboardModifiers modifiers = Qt::NoModifier;
	Qt::Key key = Qt::Key_unknown;
	QString text;
};

[[nodiscard]] std::optional<Qt::KeyboardModifier> ModifierFromToken(
		const QString &token,
		bool realMacModifiers = false) {
	if (token == u"ctrl"_q || token == u"control"_q) {
#ifdef Q_OS_MAC
		return realMacModifiers ? Qt::ControlModifier : PhysicalControlModifier();
#else // Q_OS_MAC
		return Qt::ControlModifier;
#endif // Q_OS_MAC
	} else if (token == u"cmd"_q
		|| token == u"command"_q
		|| token == u"meta"_q) {
#ifdef Q_OS_MAC
		return realMacModifiers ? Qt::MetaModifier : PhysicalControlModifier();
#else // Q_OS_MAC
		return PhysicalControlModifier();
#endif // Q_OS_MAC
	} else if (token == u"shift"_q) {
		return Qt::ShiftModifier;
	} else if (token == u"alt"_q || token == u"option"_q) {
		return Qt::AltModifier;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<Qt::Key> SpecialKeyFromToken(
		const QString &token) {
	if (token == u"esc"_q || token == u"escape"_q) {
		return Qt::Key_Escape;
	} else if (token == u"tab"_q) {
		return Qt::Key_Tab;
	} else if (token == u"backtab"_q) {
		return Qt::Key_Backtab;
	} else if (token == u"enter"_q || token == u"return"_q) {
		return Qt::Key_Return;
	} else if (token == u"space"_q) {
		return Qt::Key_Space;
	} else if (token == u"backspace"_q) {
		return Qt::Key_Backspace;
	} else if (token == u"delete"_q) {
		return Qt::Key_Delete;
	} else if (token == u"up"_q) {
		return Qt::Key_Up;
	} else if (token == u"down"_q) {
		return Qt::Key_Down;
	} else if (token == u"left"_q) {
		return Qt::Key_Left;
	} else if (token == u"right"_q) {
		return Qt::Key_Right;
	} else if (token == u"home"_q) {
		return Qt::Key_Home;
	} else if (token == u"end"_q) {
		return Qt::Key_End;
	} else if (token == u"pageup"_q) {
		return Qt::Key_PageUp;
	} else if (token == u"pagedown"_q) {
		return Qt::Key_PageDown;
	}
	return std::nullopt;
}

[[nodiscard]] Qt::Key KeyFromCharacter(QChar ch) {
	const auto lower = ch.toCaseFolded();
	const auto unicode = lower.unicode();
	if (unicode >= 'a' && unicode <= 'z') {
		return Qt::Key(Qt::Key_A + unicode - 'a');
	} else if (unicode >= '0' && unicode <= '9') {
		return Qt::Key(Qt::Key_0 + unicode - '0');
	}
	switch (unicode) {
	case '/': return Qt::Key_Slash;
	case '?': return Qt::Key_Question;
	case '.': return Qt::Key_Period;
	case ',': return Qt::Key_Comma;
	case ';': return Qt::Key_Semicolon;
	case ':': return Qt::Key_Colon;
	case '-': return Qt::Key_Minus;
	case '_': return Qt::Key_Underscore;
	case '=': return Qt::Key_Equal;
	default: return Qt::Key(unicode);
	}
}

[[nodiscard]] std::optional<KeyBinding> ParseBinding(
		QString token,
		bool realMacModifiers = false) {
	token = NormalizeToken(std::move(token));
	if (token.isEmpty()) {
		return std::nullopt;
	}
	const auto parts = token.split('+', Qt::SkipEmptyParts);
	if (parts.isEmpty()) {
		return std::nullopt;
	}
	auto binding = KeyBinding();
	for (auto i = qsizetype(0), count = parts.size() - 1; i != count; ++i) {
		if (const auto modifier = ModifierFromToken(
				parts[i],
				realMacModifiers)) {
			binding.modifiers |= *modifier;
		} else {
			return std::nullopt;
		}
	}
	const auto key = parts.back();
	if (ModifierFromToken(key, realMacModifiers)) {
		return std::nullopt;
	} else if (const auto special = SpecialKeyFromToken(key)) {
		binding.key = *special;
		if (*special == Qt::Key_Space) {
			binding.text = u" "_q;
		}
		return binding;
	} else if (key.size() == 1) {
		binding.key = KeyFromCharacter(key.front());
		binding.text = key;
		return binding;
	}
	return std::nullopt;
}

[[nodiscard]] bool KeyMatches(
		const KeyBinding &binding,
		not_null<QKeyEvent*> e) {
	if (binding.key != Qt::Key_unknown) {
		if (e->key() == binding.key || MacVirtualKeyIs(e, binding.key)) {
			return true;
		} else if (binding.key == Qt::Key_Tab
			&& e->key() == Qt::Key_Backtab) {
			return true;
		} else if (binding.key == Qt::Key_Backtab
			&& e->key() == Qt::Key_Tab) {
			return true;
		}
	}
	if (binding.text.isEmpty()) {
		return false;
	} else if (PlainText(e) == binding.text) {
		return true;
	} else if (binding.text.size() == 1) {
		const auto ch = binding.text.front();
		return e->key() == ch.unicode()
			|| e->key() == ch.toUpper().unicode();
	}
	return false;
}

[[nodiscard]] bool ModifiersMatch(
		const KeyBinding &binding,
		not_null<QKeyEvent*> e,
		MatchOptions options) {
	const auto modifiers = CleanModifiers(e);
	const auto matches = [](Qt::KeyboardModifiers expected, auto actual) {
		if (actual == expected) {
			return true;
		}
#ifdef Q_OS_MAC
		const auto controlOrCommand
			= Qt::KeyboardModifiers(Qt::ControlModifier | Qt::MetaModifier);
		const auto expectedControl = expected & controlOrCommand;
		const auto actualControl = actual & controlOrCommand;
		const auto expectedControlValue = int(expectedControl);
		const auto actualControlValue = int(actualControl);
		if ((expected & ~controlOrCommand) == (actual & ~controlOrCommand)
			&& expectedControlValue
			&& actualControlValue
			&& !(expectedControlValue & (expectedControlValue - 1))
			&& !(actualControlValue & (actualControlValue - 1))) {
			return true;
		}
#endif // Q_OS_MAC
		return false;
	};
	const auto strictMatches = [&](Qt::KeyboardModifiers expected) {
		return options.allowMacControlCommandEquivalent
			? matches(expected, modifiers)
			: (expected == modifiers);
	};
	if (strictMatches(binding.modifiers)) {
		return true;
	} else if (!options.allowExtraShift
		|| (binding.modifiers & Qt::ShiftModifier)) {
		return false;
	}
	return strictMatches(binding.modifiers | Qt::ShiftModifier);
}

} // namespace

bool IsPlainEnter(not_null<QKeyEvent*> e) {
	return !e->isAutoRepeat()
		&& (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
		&& CleanModifiers(e) == Qt::NoModifier;
}

bool IsPlainEscape(not_null<QKeyEvent*> e) {
	return !e->isAutoRepeat()
		&& e->key() == Qt::Key_Escape
		&& CleanModifiers(e) == Qt::NoModifier;
}

bool IsSystemPaste(not_null<QKeyEvent*> e) {
#ifdef Q_OS_MAC
	return !e->isAutoRepeat()
		&& CleanModifiers(e) == Qt::ControlModifier
		&& KeyIs(e, Qt::Key_V, u"v"_q, u"\u043C"_q);
#else // Q_OS_MAC
	return false;
#endif // Q_OS_MAC
}

bool IsTextYank(not_null<QKeyEvent*> e) {
	return !e->isAutoRepeat()
		&& CleanModifiers(e) == Qt::NoModifier
		&& KeyIs(e, Qt::Key_Y, u"y"_q, u"\u043D"_q);
}

Qt::KeyboardModifiers CleanModifiers(not_null<QKeyEvent*> e) {
	return e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
}

QString PlainText(not_null<QKeyEvent*> e) {
	return e->text().toCaseFolded();
}

QString HintCharacter(not_null<QKeyEvent*> e) {
	const auto text = PlainText(e);
	if (!text.isEmpty()) {
		return text.left(1);
	}
	if (e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) {
		return QString(QChar('a' + e->key() - Qt::Key_A));
	}
	switch (e->key()) {
	case Qt::Key_Comma: return u","_q;
	case Qt::Key_Period: return u"."_q;
	default: break;
	}
#ifdef Q_OS_MAC
	for (auto key = Qt::Key_A; key <= Qt::Key_Z; key = Qt::Key(key + 1)) {
		if (MacVirtualKeyIs(e, key)) {
			return QString(QChar('a' + key - Qt::Key_A));
		}
	}
	if (e->nativeVirtualKey() == 43) {
		return u","_q;
	} else if (e->nativeVirtualKey() == 47) {
		return u"."_q;
	}
#endif // Q_OS_MAC
	return QString();
}

bool IsLineStart(not_null<QKeyEvent*> e) {
	const auto modifiers = CleanModifiers(e);
	if (e->isAutoRepeat()
		|| (modifiers != Qt::NoModifier && modifiers != Qt::ShiftModifier)) {
		return false;
	}
	const auto text = PlainText(e);
	if (text == u"0"_q
		|| text == u"^"_q
		|| text == u"|"_q
		|| text == u"/"_q) {
		return true;
	}
	const auto matched = (modifiers == Qt::NoModifier
			&& (e->key() == Qt::Key_0 || e->key() == Qt::Key_Slash))
		|| e->key() == Qt::Key_AsciiCircum
		|| e->key() == Qt::Key_Bar
		|| (modifiers == Qt::ShiftModifier
			&& (e->key() == Qt::Key_6
				|| e->key() == Qt::Key_Backslash));
#ifdef Q_OS_MAC
	return matched
		|| (modifiers == Qt::NoModifier && e->nativeVirtualKey() == 44)
		|| (modifiers == Qt::ShiftModifier && e->nativeVirtualKey() == 42);
#else // Q_OS_MAC
	return matched;
#endif // Q_OS_MAC
}

bool IsLineEnd(not_null<QKeyEvent*> e) {
	const auto modifiers = CleanModifiers(e);
	if (e->isAutoRepeat()
		|| (modifiers != Qt::NoModifier && modifiers != Qt::ShiftModifier)) {
		return false;
	}
	const auto text = PlainText(e);
	const auto matched = text == u"$"_q
		|| text == u";"_q
		|| e->key() == Qt::Key_Dollar
		|| (modifiers == Qt::ShiftModifier && e->key() == Qt::Key_4)
		|| (modifiers == Qt::NoModifier && e->key() == Qt::Key_Semicolon);
#ifdef Q_OS_MAC
	return matched
		|| (modifiers == Qt::ShiftModifier && e->nativeVirtualKey() == 21)
		|| (modifiers == Qt::NoModifier && e->nativeVirtualKey() == 41);
#else // Q_OS_MAC
	return matched;
#endif // Q_OS_MAC
}

QString NormalizeToken(QString value) {
	value = value.trimmed().toCaseFolded();
	value.remove(QChar(' '));
	return value;
}

bool KeyIs(
		not_null<QKeyEvent*> e,
		Qt::Key key,
		const QString &latin,
		const QString &cyrillic) {
	return (e->key() == key)
		|| MacVirtualKeyIs(e, key)
		|| (!cyrillic.isEmpty()
			&& (e->key() == cyrillic.front().unicode()
				|| e->key() == cyrillic.front().toUpper().unicode()))
		|| TextIs(e, latin, cyrillic);
}

bool ValidBindings(const QString &bindings) {
	if (bindings.trimmed().isEmpty()) {
		return true;
	}
	for (const auto &part : bindings.split(',')) {
		for (const auto &token : part.split('+')) {
			if (token.trimmed().isEmpty()) {
				return false;
			}
		}
		if (!ParseBinding(part)) {
			return false;
		}
	}
	return true;
}

bool Matches(
		const QString &bindings,
		not_null<QKeyEvent*> e,
		MatchOptions options) {
	for (const auto &part : bindings.split(',', Qt::SkipEmptyParts)) {
		const auto binding = ParseBinding(part, options.realMacModifiers);
		if (binding
			&& ModifiersMatch(*binding, e, options)
			&& KeyMatches(*binding, e)) {
			return true;
		}
	}
	return false;
}

int TabNavigationDelta(not_null<QKeyEvent*> e) {
	if (e->isAutoRepeat()) {
		return 0;
	}
	const auto modifiers = CleanModifiers(e);
	if (e->key() == Qt::Key_Backtab
		&& (modifiers == Qt::NoModifier
			|| modifiers == Qt::ShiftModifier)) {
		return -1;
	} else if (e->key() != Qt::Key_Tab) {
		return 0;
	} else if (modifiers == Qt::ShiftModifier) {
		return -1;
	} else if (modifiers == Qt::NoModifier) {
		return 1;
	}
	return 0;
}

int PickerNavigationDelta(not_null<QKeyEvent*> e) {
	if (CleanModifiers(e) != Qt::NoModifier) {
		return 0;
	} else if (KeyIs(e, Qt::Key_J, u"j"_q, u"\u043E"_q)) {
		return 1;
	} else if (KeyIs(e, Qt::Key_K, u"k"_q, u"\u043B"_q)) {
		return -1;
	}
	return 0;
}

StickerGridAction StickerGridActionKey(not_null<QKeyEvent*> e) {
	if (IsPlainEscape(e)) {
		return StickerGridAction::ClosePreview;
	}
	const auto modifiers = CleanModifiers(e);
	const auto left = KeyIs(e, Qt::Key_H, u"h"_q, u"\u0440"_q);
	const auto down = KeyIs(e, Qt::Key_J, u"j"_q, u"\u043E"_q);
	const auto up = KeyIs(e, Qt::Key_K, u"k"_q, u"\u043B"_q);
	const auto right = KeyIs(e, Qt::Key_L, u"l"_q, u"\u0434"_q);
	if (modifiers == Qt::ControlModifier && up) {
		return StickerGridAction::ScrollUp;
	} else if (modifiers == Qt::ControlModifier && down) {
		return StickerGridAction::ScrollDown;
	} else if (modifiers == Qt::NoModifier && left) {
		return StickerGridAction::MoveLeft;
	} else if (modifiers == Qt::NoModifier && down) {
		return StickerGridAction::MoveDown;
	} else if (modifiers == Qt::NoModifier && up) {
		return StickerGridAction::MoveUp;
	} else if (modifiers == Qt::NoModifier && right) {
		return StickerGridAction::MoveRight;
	} else if (IsPlainEnter(e)) {
		return StickerGridAction::Choose;
	} else if (!e->isAutoRepeat()
		&& modifiers == Qt::NoModifier
		&& KeyIs(e, Qt::Key_W, u"w"_q, u"\u0446"_q)) {
		return StickerGridAction::Preview;
	}
	return StickerGridAction::None;
}

PaneNavigationAction PaneNavigationKey(
		not_null<QKeyEvent*> e,
		bool &pending) {
	const auto modifiers = CleanModifiers(e);
	if (modifiers == PhysicalControlModifier()
		&& KeyIs(e, Qt::Key_A, u"a"_q, u"\u0444"_q)) {
		if (!e->isAutoRepeat()) {
			pending = true;
		}
		return PaneNavigationAction::Prefix;
	} else if (!pending) {
		return PaneNavigationAction::None;
	} else if (e->isAutoRepeat()) {
		return PaneNavigationAction::Cancel;
	}
	pending = false;
	if (modifiers == Qt::NoModifier) {
		if (KeyIs(e, Qt::Key_H, u"h"_q, u"\u0440"_q)) {
			return PaneNavigationAction::Left;
		} else if (KeyIs(e, Qt::Key_L, u"l"_q, u"\u0434"_q)) {
			return PaneNavigationAction::Right;
		}
	}
	return PaneNavigationAction::Cancel;
}

int InterfaceHistoryDelta(not_null<QKeyEvent*> e) {
	if (CleanModifiers(e) != PhysicalControlModifier()) {
		return 0;
	} else if (KeyIs(e, Qt::Key_H, u"h"_q, u"\u0440"_q)) {
		return -1;
	} else if (KeyIs(e, Qt::Key_L, u"l"_q, u"\u0434"_q)) {
		return 1;
	}
	return 0;
}

bool SpellcheckKey(not_null<QKeyEvent*> e) {
	return CleanModifiers(e) == PhysicalControlModifier()
		&& KeyIs(e, Qt::Key_E, u"e"_q, u"\u0443"_q);
}

int MediaNavigationDelta(not_null<QKeyEvent*> e) {
	const auto modifiers = CleanModifiers(e);
	if (modifiers != Qt::ControlModifier
		&& modifiers != Qt::MetaModifier) {
		return 0;
	} else if (KeyIs(e, Qt::Key_H, u"h"_q, u"\u0440"_q)) {
		return -1;
	} else if (KeyIs(e, Qt::Key_L, u"l"_q, u"\u0434"_q)) {
		return 1;
	}
	return 0;
}

} // namespace Core::VimKeymap::Bindings
