/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_bindings.h"

#include <QtCore/QStringList>
#include <QtGui/QKeyEvent>

#include <optional>

namespace Core::VimKeymap::Bindings {
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

Qt::KeyboardModifiers CleanModifiers(not_null<QKeyEvent*> e) {
	return e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
}

QString PlainText(not_null<QKeyEvent*> e) {
	return e->text().toCaseFolded();
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
