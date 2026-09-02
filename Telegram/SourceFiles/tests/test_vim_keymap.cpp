/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_bindings.h"
#include "core/vim_keymap_geometry.h"
#include "core/vim_keymap.h"

#include <QtGui/QKeyEvent>

#include <iostream>

namespace {

using MatchOptions = Core::VimKeymap::Bindings::MatchOptions;

int FailedChecks = 0;
int TotalChecks = 0;

void Check(bool condition, const char *name) {
	++TotalChecks;
	if (!condition) {
		++FailedChecks;
		std::cout << "FAILED: " << name << std::endl;
	}
}

[[nodiscard]] bool MatchesKey(
		const QString &bindings,
		int key,
		Qt::KeyboardModifiers modifiers = Qt::NoModifier,
		const QString &text = QString(),
		MatchOptions options = {}) {
	auto event = QKeyEvent(QEvent::KeyPress, key, modifiers, text);
	return Core::VimKeymap::Bindings::Matches(bindings, &event, options);
}

void ExpectMatches(
		const char *name,
		const QString &bindings,
		int key,
		Qt::KeyboardModifiers modifiers = Qt::NoModifier,
		const QString &text = QString(),
		MatchOptions options = {}) {
	Check(MatchesKey(bindings, key, modifiers, text, options), name);
}

void ExpectDoesNotMatch(
		const char *name,
		const QString &bindings,
		int key,
		Qt::KeyboardModifiers modifiers = Qt::NoModifier,
		const QString &text = QString(),
		MatchOptions options = {}) {
	Check(!MatchesKey(bindings, key, modifiers, text, options), name);
}

void TestVimKeymapNavigationBindings() {
	ExpectMatches(
		"scroll down latin",
		u"j, \u043E"_q,
		Qt::Key_J,
		Qt::NoModifier,
		u"j"_q);
	ExpectMatches(
		"scroll down cyrillic",
		u"j, \u043E"_q,
		0x041E,
		Qt::NoModifier,
		u"\u043E"_q);
	ExpectMatches(
		"scroll up latin",
		u"k, \u043B"_q,
		Qt::Key_K,
		Qt::NoModifier,
		u"k"_q);
	ExpectMatches(
		"scroll up cyrillic",
		u"k, \u043B"_q,
		0x041B,
		Qt::NoModifier,
		u"\u043B"_q);
	ExpectDoesNotMatch(
		"plain scroll ignores ctrl",
		u"j, \u043E"_q,
		Qt::Key_J,
		Qt::ControlModifier);

	ExpectMatches(
		"jump bottom latin",
		u"Ctrl+G, Ctrl+\u043F"_q,
		Qt::Key_G,
		Qt::ControlModifier);
	ExpectMatches(
		"jump bottom cyrillic without text",
		u"Ctrl+G, Ctrl+\u043F"_q,
		0x041F,
		Qt::ControlModifier);
	ExpectDoesNotMatch(
		"jump bottom requires ctrl",
		u"Ctrl+G, Ctrl+\u043F"_q,
		Qt::Key_G);
	ExpectDoesNotMatch(
		"jump bottom ignores ctrl shift",
		u"Ctrl+G, Ctrl+\u043F"_q,
		Qt::Key_G,
		Qt::ControlModifier | Qt::ShiftModifier);

	ExpectMatches(
		"next chat",
		u"Ctrl+J, Ctrl+\u043E"_q,
		Qt::Key_J,
		Qt::ControlModifier);
	ExpectMatches(
		"previous chat",
		u"Ctrl+K, Ctrl+\u043B"_q,
		Qt::Key_K,
		Qt::ControlModifier);
	ExpectMatches("next folder tab", u"Tab"_q, Qt::Key_Tab);
	ExpectMatches(
		"previous folder backtab",
		u"Shift+Tab"_q,
		Qt::Key_Backtab,
		Qt::ShiftModifier);
	ExpectMatches(
		"previous folder shift tab",
		u"Shift+Tab"_q,
		Qt::Key_Tab,
		Qt::ShiftModifier);
}

void TestVimKeymapActionBindings() {
	const auto allowExtraShift = MatchOptions{ .allowExtraShift = true };

	ExpectMatches(
		"chat preview ctrl v",
		u"Ctrl+V, Ctrl+\u043C"_q,
		Qt::Key_V,
		Qt::ControlModifier);
	ExpectMatches(
		"chat preview cyrillic without text",
		u"Ctrl+\u043C"_q,
		0x041C,
		Qt::ControlModifier);
	ExpectDoesNotMatch(
		"chat preview ignores ctrl shift v",
		u"Ctrl+V, Ctrl+\u043C"_q,
		Qt::Key_V,
		Qt::ControlModifier | Qt::ShiftModifier);

	ExpectMatches(
		"select message text ctrl shift v",
		u"Ctrl+Shift+V, Ctrl+Shift+\u043C"_q,
		Qt::Key_V,
		Qt::ControlModifier | Qt::ShiftModifier);
	ExpectMatches(
		"select message text cyrillic without text",
		u"Ctrl+Shift+\u043C"_q,
		0x041C,
		Qt::ControlModifier | Qt::ShiftModifier);
	ExpectDoesNotMatch(
		"select message text ignores ctrl v",
		u"Ctrl+Shift+V, Ctrl+Shift+\u043C"_q,
		Qt::Key_V,
		Qt::ControlModifier);

	ExpectMatches(
		"copy message latin",
		u"y, \u043D"_q,
		Qt::Key_Y,
		Qt::NoModifier,
		u"y"_q);
	ExpectMatches(
		"copy message allows shift",
		u"y, \u043D"_q,
		Qt::Key_Y,
		Qt::ShiftModifier,
		u"Y"_q,
		allowExtraShift);
	ExpectDoesNotMatch(
		"copy message shift requires opt in",
		u"y, \u043D"_q,
		Qt::Key_Y,
		Qt::ShiftModifier,
		u"Y"_q);

	ExpectMatches(
		"reply hint",
		u"r, \u043A"_q,
		Qt::Key_R,
		Qt::NoModifier,
		u"r"_q);
	ExpectMatches(
		"edit hint",
		u"e, \u0443"_q,
		Qt::Key_E,
		Qt::NoModifier,
		u"e"_q);
	ExpectMatches(
		"edit hint cyrillic",
		u"e, \u0443"_q,
		0x0423,
		Qt::NoModifier,
		u"\u0443"_q);
	Check(
		Core::VimKeymap::ActionUsesMessageHints(
			Core::VimKeymap::Action::EditMessage),
		"edit action always uses message hints");
	Check(
		!Core::VimKeymap::ActionUsesMessageHints(
			Core::VimKeymap::Action::ChatPreview),
		"chat preview does not use message hints");
	ExpectMatches(
		"delete hint",
		u"d, \u0432"_q,
		Qt::Key_D,
		Qt::NoModifier,
		u"d"_q);
	ExpectMatches(
		"focus hints",
		u"f, \u0430"_q,
		Qt::Key_F,
		Qt::NoModifier,
		u"f"_q);
	ExpectMatches(
		"open chat hints",
		u"o, \u0449"_q,
		Qt::Key_O,
		Qt::NoModifier,
		u"o"_q);
}

void TestVimKeymapTransientUiKeys() {
	Check(
		Core::VimKeymap::TextVisualModeConsumesKey(true, true),
		"repeated visual key stays in visual mode");
	Check(
		!Core::VimKeymap::TextVisualModeConsumesKey(false, true),
		"visual key starts only after message cursor mode");
	auto enter = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Return,
		Qt::NoModifier);
	Check(
		Core::VimKeymap::Bindings::IsPlainEnter(&enter),
		"chat preview accepts return");
	auto keypadEnter = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Enter,
		Qt::KeypadModifier);
	Check(
		Core::VimKeymap::Bindings::IsPlainEnter(&keypadEnter),
		"chat preview accepts keypad enter");
	auto shiftedEnter = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Return,
		Qt::ShiftModifier);
	Check(
		!Core::VimKeymap::Bindings::IsPlainEnter(&shiftedEnter),
		"chat preview ignores shifted return");
	auto escape = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Escape,
		Qt::NoModifier);
	Check(
		Core::VimKeymap::Bindings::IsPlainEscape(&escape),
		"chat hints accept escape before popup");
	auto repeatedEscape = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Escape,
		Qt::NoModifier,
		QString(),
		true);
	Check(
		!Core::VimKeymap::Bindings::IsPlainEscape(&repeatedEscape),
		"chat hints ignore repeated escape");
	ExpectMatches(
		"chat preview scroll down",
		u"j, \u043E"_q,
		Qt::Key_J,
		Qt::NoModifier,
		u"j"_q);
	ExpectMatches(
		"chat preview scroll up",
		u"k, \u043B"_q,
		Qt::Key_K,
		Qt::NoModifier,
		u"k"_q);
}

void TestVimKeymapCursorGeometry() {
	const auto character = QRect(20, 30, 14, 24);
	Check(
		Core::VimKeymap::CursorPaintRect(
			character,
			u"block"_q,
			5,
			24) == QRect(20, 30, 14, 24),
		"message block cursor follows character width");
	Check(
		Core::VimKeymap::CursorPaintRect(
			character,
			u"bar"_q,
			5,
			12) == QRect(20, 36, 5, 12),
		"message bar cursor keeps configured width");
	Check(
		Core::VimKeymap::CursorPaintRect(
			character,
			u"underline"_q,
			4,
			24) == QRect(20, 50, 14, 4),
		"message underline cursor follows character width");

	Check(
		Core::VimKeymap::MakeVisualSelectionRange(5, 5, 12)
			== Core::VimKeymap::VisualSelectionRange{ 5, 6 },
		"visual selection starts on current character");
	Check(
		Core::VimKeymap::MakeVisualSelectionRange(5, 4, 12)
			== Core::VimKeymap::VisualSelectionRange{ 4, 6 },
		"visual selection moves left without collapsing");
	Check(
		Core::VimKeymap::MakeVisualSelectionRange(5, 6, 12)
			== Core::VimKeymap::VisualSelectionRange{ 5, 7 },
		"visual selection moves right without collapsing");
	Check(
		Core::VimKeymap::MakeVisualSelectionRange(0, -1, 12)
			== Core::VimKeymap::VisualSelectionRange{ 0, 1 },
		"visual selection stays visible at left edge");
	Check(
		Core::VimKeymap::MakeVisualSelectionRange(11, 12, 12)
			== Core::VimKeymap::VisualSelectionRange{ 11, 12 },
		"visual selection stays visible at right edge");
}

void TestVimKeymapCommandBindings() {
	const auto allowExtraShift = MatchOptions{ .allowExtraShift = true };

	ExpectMatches(
		"chat search",
		u"/"_q,
		Qt::Key_Slash,
		Qt::NoModifier,
		u"/"_q);
	ExpectMatches(
		"help",
		u"?"_q,
		Qt::Key_Question,
		Qt::ShiftModifier,
		u"?"_q,
		allowExtraShift);
	ExpectMatches(
		"global search ctrl f",
		u"Ctrl+F, Ctrl+\u0430, Cmd+Shift+F, "
		u"Cmd+Shift+\u0430, Ctrl+Shift+F, Ctrl+Shift+\u0430"_q,
		Qt::Key_F,
		Qt::ControlModifier);
	ExpectMatches(
		"global search ctrl shift f",
		u"Ctrl+F, Ctrl+\u0430, Cmd+Shift+F, "
		u"Cmd+Shift+\u0430, Ctrl+Shift+F, Ctrl+Shift+\u0430"_q,
		Qt::Key_F,
		Qt::ControlModifier | Qt::ShiftModifier);
	ExpectMatches(
		"undo compose",
		u"u, \u0433"_q,
		Qt::Key_U,
		Qt::NoModifier,
		u"u"_q);
	ExpectMatches(
		"redo compose",
		u"Ctrl+R, Ctrl+\u043A"_q,
		Qt::Key_R,
		Qt::ControlModifier);
	ExpectMatches(
		"focus chat",
		u"Ctrl+H, Ctrl+\u0440"_q,
		Qt::Key_H,
		Qt::ControlModifier);
	ExpectMatches(
		"focus emoji",
		u"Ctrl+L, Ctrl+\u0434"_q,
		Qt::Key_L,
		Qt::ControlModifier);
	ExpectMatches(
		"call",
		u"Ctrl+T, Ctrl+\u0435"_q,
		Qt::Key_T,
		Qt::ControlModifier);
}

} // namespace

int main(int, char *[]) {
	TestVimKeymapNavigationBindings();
	TestVimKeymapActionBindings();
	TestVimKeymapCommandBindings();
	TestVimKeymapTransientUiKeys();
	TestVimKeymapCursorGeometry();

	std::cout << (TotalChecks - FailedChecks) << "/" << TotalChecks
		<< " checks passed." << std::endl;
	return FailedChecks ? 1 : 0;
}
