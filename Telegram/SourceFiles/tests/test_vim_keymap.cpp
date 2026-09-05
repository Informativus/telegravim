/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_bindings.h"
#include "core/vim_keymap_geometry.h"
#include "core/vim_keymap_widgets.h"
#include "core/vim_keymap.h"
#include "base/qt/qt_tab_key.h"
#include "base/flat_map.h"
#include "ui/abstract_button.h"
#include "ui/integration.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/style/style_core.h"
#include "styles/style_widgets.h"

#include <rpl/never.h>

#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtCore/QRandomGenerator>
#include <QtWidgets/QApplication>

#include <iostream>
#include <algorithm>

namespace crl {

rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}

} // namespace crl

namespace {

using MatchOptions = Core::VimKeymap::Bindings::MatchOptions;

int FailedChecks = 0;
int TotalChecks = 0;

class TestIntegration final : public Ui::Integration {
public:
	void postponeCall(FnMut<void()> &&callable) override {
		callable();
	}
	void registerLeaveSubscription(not_null<QWidget*>) override {
	}
	void unregisterLeaveSubscription(not_null<QWidget*>) override {
	}
	QString emojiCacheFolder() override {
		return {};
	}
	QString openglCheckFilePath() override {
		return {};
	}
	QString angleBackendFilePath() override {
		return {};
	}
	void touchCounterIncrement() override {
		++_touchCounter;
	}
	int touchCounterNow() override {
		return _touchCounter;
	}

private:
	int _touchCounter = 0;

};

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

#ifdef Q_OS_MAC
	const auto strictPhysicalModifiers = MatchOptions{
		.allowMacControlCommandEquivalent = false,
	};
	ExpectMatches(
		"chat preview accepts physical ctrl v",
		u"Ctrl+V, Ctrl+\u043C"_q,
		Qt::Key_V,
		Qt::MetaModifier,
		u"v"_q,
		strictPhysicalModifiers);
	ExpectDoesNotMatch(
		"chat preview rejects command v",
		u"Ctrl+V, Ctrl+\u043C"_q,
		Qt::Key_V,
		Qt::ControlModifier,
		u"v"_q,
		strictPhysicalModifiers);
	auto commandPaste = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_V,
		Qt::ControlModifier,
		u"v"_q);
	Check(
		Core::VimKeymap::Bindings::IsSystemPaste(&commandPaste),
		"command v is system paste");
	auto commandCyrillicPaste = QKeyEvent(
		QEvent::KeyPress,
		0x041C,
		Qt::ControlModifier,
		u"\u043C"_q);
	Check(
		Core::VimKeymap::Bindings::IsSystemPaste(&commandCyrillicPaste),
		"command cyrillic v position is system paste");
	auto commandPhysicalPaste = QKeyEvent(
		QEvent::KeyPress,
		0,
		Qt::ControlModifier,
		0,
		9,
		0);
	Check(
		Core::VimKeymap::Bindings::IsSystemPaste(&commandPhysicalPaste),
		"command physical v is system paste with empty text");
	auto controlPaste = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_V,
		Qt::MetaModifier,
		u"v"_q);
	Check(
		!Core::VimKeymap::Bindings::IsSystemPaste(&controlPaste),
		"physical ctrl v is not system paste");
#endif // Q_OS_MAC

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
	auto textlessHint = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_G,
		Qt::NoModifier);
	Check(
		Core::VimKeymap::Bindings::HintCharacter(&textlessHint) == u"g"_q,
		"textless g selects an active chat hint");
	auto pipe = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Bar,
		Qt::ShiftModifier);
	Check(
		Core::VimKeymap::Bindings::IsLineStart(&pipe),
		"textless pipe moves to the line start");
	auto shiftedBackslash = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Backslash,
		Qt::ShiftModifier);
	Check(
		Core::VimKeymap::Bindings::IsLineStart(&shiftedBackslash),
		"textless shifted backslash moves to the line start");
	auto slash = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Slash,
		Qt::NoModifier);
	Check(
		Core::VimKeymap::Bindings::IsLineStart(&slash),
		"slash moves to the line start");
#ifdef Q_OS_MAC
	auto physicalPipe = QKeyEvent(
		QEvent::KeyPress,
		0,
		Qt::ShiftModifier,
		0,
		42,
		0);
	Check(
		Core::VimKeymap::Bindings::IsLineStart(&physicalPipe),
		"physical pipe moves to the line start with empty text");
	auto physicalSlash = QKeyEvent(
		QEvent::KeyPress,
		0,
		Qt::NoModifier,
		0,
		44,
		0);
	Check(
		Core::VimKeymap::Bindings::IsLineStart(&physicalSlash),
		"physical slash moves to the line start with empty text");
#endif // Q_OS_MAC
	auto dollar = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Dollar,
		Qt::ShiftModifier);
	Check(
		Core::VimKeymap::Bindings::IsLineEnd(&dollar),
		"textless dollar moves to the line end");
	auto shiftedFour = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_4,
		Qt::ShiftModifier);
	Check(
		Core::VimKeymap::Bindings::IsLineEnd(&shiftedFour),
		"textless shift four moves to the line end");
	auto semicolon = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Semicolon,
		Qt::NoModifier);
	Check(
		Core::VimKeymap::Bindings::IsLineEnd(&semicolon),
		"semicolon moves to the line end");
#ifdef Q_OS_MAC
	auto physicalSemicolon = QKeyEvent(
		QEvent::KeyPress,
		0,
		Qt::NoModifier,
		0,
		41,
		0);
	Check(
		Core::VimKeymap::Bindings::IsLineEnd(&physicalSemicolon),
		"physical semicolon moves to the line end with empty text");
#endif // Q_OS_MAC
	auto yank = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Y,
		Qt::NoModifier,
		u"y"_q);
	Check(
		Core::VimKeymap::Bindings::IsTextYank(&yank),
		"plain y yanks a visual text selection");
	auto cyrillicYank = QKeyEvent(
		QEvent::KeyPress,
		0x041D,
		Qt::NoModifier,
		u"\u043D"_q);
	Check(
		Core::VimKeymap::Bindings::IsTextYank(&cyrillicYank),
		"cyrillic y key yanks a visual text selection");
	auto modifiedYank = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Y,
		Qt::ControlModifier,
		u"y"_q);
	Check(
		!Core::VimKeymap::Bindings::IsTextYank(&modifiedYank),
		"modified y does not yank a visual text selection");
	auto repeatedYank = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Y,
		Qt::NoModifier,
		u"y"_q,
		true);
	Check(
		!Core::VimKeymap::Bindings::IsTextYank(&repeatedYank),
		"repeated y does not rewrite the clipboard");
	Check(
		Core::VimKeymap::TextVisualYankCompletes(true, true),
		"successful visual yank exits message visual mode");
	Check(
		!Core::VimKeymap::TextVisualYankCompletes(true, false),
		"failed visual yank keeps the selection active");
	Check(
		!Core::VimKeymap::TextVisualYankCompletes(false, true),
		"cursor-only yank does not exit through visual selection state");

	Check(
		Core::VimKeymap::TextVisualModeConsumesKey(true, true),
		"repeated visual key stays in visual mode");
	Check(
		!Core::VimKeymap::TextVisualModeConsumesKey(false, true),
		"visual key starts only after message cursor mode");
	Check(
		Core::VimKeymap::EmptyComposeDefersToMessageAction(
			false,
			true,
			true),
		"empty composer defers reply and edit keys to message actions");
	Check(
		!Core::VimKeymap::EmptyComposeDefersToMessageAction(
			true,
			true,
			true),
		"active composer command keeps reply and edit keys");
	Check(
		!Core::VimKeymap::EmptyComposeDefersToMessageAction(
			false,
			false,
			true),
		"nonempty composer keeps reply and edit keys");
	Check(
		Core::VimKeymap::ShouldAddScannedLinkHint(true, true, false),
		"clickable service message is exposed as a link hint");
	Check(
		!Core::VimKeymap::ShouldAddScannedLinkHint(true, false, false),
		"service message without a link has no hint");
	Check(
		Core::VimKeymap::ShouldAddScannedLinkHint(false, true, true),
		"regular message text link keeps its hint");
	Check(
		!Core::VimKeymap::ShouldAddScannedLinkHint(false, true, false),
		"regular non-text link is not added by the text scan");
	Check(
		Core::VimKeymap::ShouldAddInlinePlaybackHint(
			true,
			true,
			false,
			true),
		"inline media playback uses the direct link when scanning misses");
	Check(
		!Core::VimKeymap::ShouldAddInlinePlaybackHint(
			true,
			true,
			true,
			true),
		"inline media playback does not duplicate a scanned hint");
	Check(
		!Core::VimKeymap::ShouldAddInlinePlaybackHint(
			true,
			true,
			false,
			false),
		"inline media playback skips a missing direct link");
	Check(
		!Core::VimKeymap::ShouldAddInlinePlaybackHint(
			false,
			true,
			false,
			true),
		"non-inline media does not use the playback fallback");
	Check(
		!Core::VimKeymap::ShouldAddInlinePlaybackHint(
			true,
			false,
			false,
			true),
		"hidden inline media does not receive a playback hint");
	Check(
		Core::VimKeymap::ShouldAddStickerHint(true, true, true),
		"visible clickable sticker receives a link hint");
	Check(
		!Core::VimKeymap::ShouldAddStickerHint(true, false, true),
		"hidden sticker does not receive a link hint");
	Check(
		!Core::VimKeymap::ShouldAddStickerHint(true, true, false),
		"sticker without an action does not receive a link hint");
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

	Check(
		Core::VimKeymap::GroupedMediaHintRect(
			QRect(300, 400, 120, 90),
			QPoint(80, 70),
			900) == QRect(380, 1370, 120, 90),
		"grouped media hint uses the visible item origin");

	const auto actualTextLength = 13;
	const auto layoutTextLength = 14;
	const auto selectable = [=](int offset) {
		return offset >= 0 && offset < actualTextLength;
	};
	Check(
		Core::VimKeymap::ResolveTextCursorOffset(
			layoutTextLength - 1,
			1,
			layoutTextLength,
			selectable) == actualTextLength - 1,
		"message cursor ignores a trailing layout placeholder");
	Check(
		Core::VimKeymap::ResolveTextCursorOffset(
			0,
			1,
			layoutTextLength,
			selectable) == 0,
		"message cursor keeps a selectable character");
}

void TestVimKeymapPickerNavigation() {
	auto tab = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Tab,
		Qt::NoModifier);
	Check(
		Core::VimKeymap::Bindings::TabNavigationDelta(&tab) == 1,
		"tab advances modal focus");
	auto backtab = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Backtab,
		Qt::ShiftModifier);
	Check(
		Core::VimKeymap::Bindings::TabNavigationDelta(&backtab) == -1,
		"backtab reverses modal focus");
	auto shiftedTab = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Tab,
		Qt::ShiftModifier);
	Check(
		Core::VimKeymap::Bindings::TabNavigationDelta(&shiftedTab) == -1,
		"shift tab reverses modal focus");
	auto modifiedTab = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Tab,
		Qt::ControlModifier);
	Check(
		Core::VimKeymap::Bindings::TabNavigationDelta(&modifiedTab) == 0,
		"modified tab leaves modal focus unchanged");
	auto repeatedTab = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Tab,
		Qt::NoModifier,
		QString(),
		true);
	Check(
		Core::VimKeymap::Bindings::TabNavigationDelta(&repeatedTab) == 0,
		"repeated tab leaves modal focus unchanged");
	auto next = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_J,
		Qt::NoModifier,
		u"j"_q);
	Check(
		Core::VimKeymap::Bindings::PickerNavigationDelta(&next) == 1,
		"j advances the time picker");
	auto previous = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_K,
		Qt::NoModifier,
		u"k"_q);
	Check(
		Core::VimKeymap::Bindings::PickerNavigationDelta(&previous) == -1,
		"k reverses the time picker");
	auto nextCyrillic = QKeyEvent(
		QEvent::KeyPress,
		0x041E,
		Qt::NoModifier,
		u"\u043E"_q);
	Check(
		Core::VimKeymap::Bindings::PickerNavigationDelta(
			&nextCyrillic) == 1,
		"cyrillic j position advances the time picker");
	auto previousCyrillic = QKeyEvent(
		QEvent::KeyPress,
		0x041B,
		Qt::NoModifier,
		u"\u043B"_q);
	Check(
		Core::VimKeymap::Bindings::PickerNavigationDelta(
			&previousCyrillic) == -1,
		"cyrillic k position reverses the time picker");
	auto modified = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_J,
		Qt::ControlModifier,
		u"j"_q);
	Check(
		Core::VimKeymap::Bindings::PickerNavigationDelta(&modified) == 0,
		"modified j does not move the time picker");
#ifdef Q_OS_MAC
	auto physicalNext = QKeyEvent(
		QEvent::KeyPress,
		0,
		Qt::NoModifier,
		0,
		38,
		0);
	Check(
		Core::VimKeymap::Bindings::PickerNavigationDelta(
			&physicalNext) == 1,
		"physical j advances the time picker with empty text");
	auto physicalPrevious = QKeyEvent(
		QEvent::KeyPress,
		0,
		Qt::NoModifier,
		0,
		40,
		0);
	Check(
		Core::VimKeymap::Bindings::PickerNavigationDelta(
			&physicalPrevious) == -1,
		"physical k reverses the time picker with empty text");
#endif // Q_OS_MAC
}

void TestVimKeymapStickerSetNavigation() {
	using Action = Core::VimKeymap::Bindings::StickerGridAction;
	const auto action = [](Qt::Key key, Qt::KeyboardModifiers modifiers,
			const QString &text = QString()) {
		auto event = QKeyEvent(QEvent::KeyPress, key, modifiers, text);
		return Core::VimKeymap::Bindings::StickerGridActionKey(&event);
	};
	Check(
		action(Qt::Key_H, Qt::NoModifier, u"h"_q) == Action::MoveLeft,
		"h selects the sticker to the left");
	Check(
		action(Qt::Key_J, Qt::NoModifier, u"j"_q) == Action::MoveDown,
		"j selects the sticker below");
	Check(
		action(Qt::Key_K, Qt::NoModifier, u"k"_q) == Action::MoveUp,
		"k selects the sticker above");
	Check(
		action(Qt::Key_L, Qt::NoModifier, u"l"_q) == Action::MoveRight,
		"l selects the sticker to the right");
	Check(
		action(Qt::Key_J, Qt::ControlModifier, u"j"_q)
			== Action::ScrollDown,
		"control j scrolls the sticker set down");
	Check(
		action(Qt::Key_K, Qt::ControlModifier, u"k"_q)
			== Action::ScrollUp,
		"control k scrolls the sticker set up");
	Check(
		action(Qt::Key_Return, Qt::NoModifier) == Action::Choose,
		"enter chooses the selected sticker");
	Check(
		action(Qt::Key_W, Qt::NoModifier, u"w"_q) == Action::Preview,
		"w previews the selected sticker");
	Check(
		action(Qt::Key_Escape, Qt::NoModifier) == Action::ClosePreview,
		"escape is offered to the sticker preview before the box");
	Check(
		action(Qt::Key_Escape, Qt::ControlModifier) == Action::None,
		"modified escape does not dismiss the sticker preview");
	Check(
		action(Qt::Key_W, Qt::ControlModifier, u"w"_q) == Action::None,
		"modified w leaves the sticker preview unchanged");
	Check(
		action(Qt::Key_unknown, Qt::NoModifier, u"\u043E"_q)
			== Action::MoveDown,
		"cyrillic j position selects the sticker below");
	Check(
		action(Qt::Key_unknown, Qt::NoModifier, u"\u043B"_q)
			== Action::MoveUp,
		"cyrillic k position selects the sticker above");
	Check(
		action(Qt::Key_unknown, Qt::NoModifier, u"\u0440"_q)
			== Action::MoveLeft,
		"cyrillic h position selects the sticker to the left");
	Check(
		action(Qt::Key_unknown, Qt::NoModifier, u"\u0434"_q)
			== Action::MoveRight,
		"cyrillic l position selects the sticker to the right");
	Check(
		action(Qt::Key_unknown, Qt::NoModifier, u"\u0446"_q)
			== Action::Preview,
		"cyrillic w position previews the selected sticker");
	Check(
		Core::VimKeymap::MoveStickerGridSelection(-1, 17, 1) == 0,
		"next starts sticker selection at the first item");
	Check(
		Core::VimKeymap::MoveStickerGridSelection(-1, 17, -1) == 0,
		"any direction starts sticker selection at the first item");
	Check(
		Core::VimKeymap::MoveStickerGridSelection(7, 17, 1) == 8,
		"next advances sticker selection by one item");
	Check(
		Core::VimKeymap::MoveStickerGridSelection(16, 17, 1) == 16,
		"sticker selection stops at the last item");
	Check(
		Core::VimKeymap::MoveStickerGridSelection(7, 17, 5) == 12,
		"down advances sticker selection by one row");
	Check(
		Core::VimKeymap::MoveStickerGridSelection(7, 17, -5) == 2,
		"up moves sticker selection by one row");
	Check(
		Core::VimKeymap::MoveStickerGridSelection(0, 17, -1) == 0,
		"sticker selection stops at the first item");
	Check(
		Core::VimKeymap::MoveStickerGridSelection(-1, 0, 1) == -1,
		"empty sticker set has no keyboard selection");
}

void TestVimKeymapMediaNavigation() {
	auto previous = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_H,
		Qt::ControlModifier,
		u"h"_q);
	Check(
		Core::VimKeymap::Bindings::MediaNavigationDelta(&previous) == -1,
		"ctrl h opens previous media");
	auto next = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_L,
		Qt::ControlModifier,
		u"l"_q);
	Check(
		Core::VimKeymap::Bindings::MediaNavigationDelta(&next) == 1,
		"ctrl l opens next media");
	auto previousCyrillic = QKeyEvent(
		QEvent::KeyPress,
		0x0420,
		Qt::ControlModifier,
		u"\u0440"_q);
	Check(
		Core::VimKeymap::Bindings::MediaNavigationDelta(
			&previousCyrillic) == -1,
		"ctrl cyrillic h position opens previous media");
	auto nextCyrillic = QKeyEvent(
		QEvent::KeyPress,
		0x0414,
		Qt::ControlModifier,
		u"\u0434"_q);
	Check(
		Core::VimKeymap::Bindings::MediaNavigationDelta(
			&nextCyrillic) == 1,
		"ctrl cyrillic l position opens next media");
#ifdef Q_OS_MAC
	auto physicalPrevious = QKeyEvent(
		QEvent::KeyPress,
		0,
		Qt::ControlModifier,
		0,
		4,
		0);
	Check(
		Core::VimKeymap::Bindings::MediaNavigationDelta(
			&physicalPrevious) == -1,
		"ctrl physical h opens previous media with empty text");
	auto physicalNext = QKeyEvent(
		QEvent::KeyPress,
		0,
		Qt::ControlModifier,
		0,
		37,
		0);
	Check(
		Core::VimKeymap::Bindings::MediaNavigationDelta(
			&physicalNext) == 1,
		"ctrl physical l opens next media with empty text");
#endif // Q_OS_MAC
	auto commandNext = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_L,
		Qt::MetaModifier,
		u"l"_q);
	Check(
		Core::VimKeymap::Bindings::MediaNavigationDelta(&commandNext) == 1,
		"command l opens next media on macos");
	auto shiftedNext = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_L,
		Qt::ControlModifier | Qt::ShiftModifier,
		u"L"_q);
	Check(
		Core::VimKeymap::Bindings::MediaNavigationDelta(&shiftedNext) == 0,
		"ctrl shift l does not navigate media");
	auto plainNext = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_L,
		Qt::NoModifier,
		u"l"_q);
	Check(
		Core::VimKeymap::Bindings::MediaNavigationDelta(&plainNext) == 0,
		"plain l does not navigate media");
}

void TestAbstractButtonKeyboardActivation() {
	auto button = Ui::AbstractButton(nullptr);
	auto clicks = 0;
	button.setClickedCallback([&] {
		++clicks;
	});
	const auto pressEnter = [&] {
		auto press = QKeyEvent(
			QEvent::KeyPress,
			Qt::Key_Return,
			Qt::NoModifier);
		QApplication::sendEvent(&button, &press);
		auto release = QKeyEvent(
			QEvent::KeyRelease,
			Qt::Key_Return,
			Qt::NoModifier);
		QApplication::sendEvent(&button, &release);
	};

	button.setSynteticOver(true);
	pressEnter();
	Check(
		clicks == 1,
		"enter activates a synthetically hovered button exactly once");

	button.setSynteticOver(false);
	clicks = 0;
	pressEnter();
	Check(clicks == 1, "enter activates a normal button exactly once");
}

void TestHintBadgeLayoutAndPainting() {
	using namespace Core::VimKeymap;
	const auto valid = [](const std::vector<QRect> &rects, QRect bounds, int gap) {
		for (auto i = 0; i != rects.size(); ++i) {
			if (rects[i].isEmpty()) {
				continue;
			}
			if (!bounds.contains(rects[i])) {
				return false;
			}
			for (auto j = 0; j != i; ++j) {
				if (!rects[j].isEmpty()
					&& rects[i].marginsAdded(QMargins(gap, gap, gap, gap))
						.intersects(rects[j])) {
					return false;
				}
			}
		}
		return true;
	};
	const auto bounds = QRect(4, 4, 300, 180);
	const auto clustered = std::vector<QRect>(24, QRect(10, 80, 25, 24));
	const auto placed = LayoutHintBadges(clustered, bounds, 3);
	Check(valid(placed, bounds, 3), "clustered badges never overlap");
	Check(std::ranges::none_of(placed, &QRect::isEmpty),
		"clustered badges remain visible when space is available");
	Check(placed == LayoutHintBadges(clustered, bounds, 3),
		"badge positions are deterministic across repaints");
	const auto tiny = QRect(0, 0, 25, 24);
	const auto overflow = LayoutHintBadges(clustered, tiny, 3);
	Check(!overflow.front().isEmpty()
		&& std::ranges::count_if(overflow, &QRect::isEmpty) == 23,
		"full viewport never falls back to overlapping badges");
	Check(LayoutHintBadges({ QRect(0, 0, 400, 24) }, bounds, 3)[0].isEmpty(),
		"oversized badge is not drawn clipped");
	auto random = QRandomGenerator(57931);
	auto allValid = true;
	for (auto run = 0; run != 200; ++run) {
		const auto viewport = QRect(0, run * 1000,
			random.bounded(40, 1200), random.bounded(40, 900));
		auto desired = std::vector<QRect>();
		for (auto i = 0; i != 60; ++i) {
			desired.emplace_back(random.bounded(-100, 1300),
				viewport.y() + random.bounded(-100, 1000),
				random.bounded(15, 120), random.bounded(15, 65));
		}
		allValid &= valid(LayoutHintBadges(desired, viewport, 3), viewport, 3);
	}
	Check(allValid, "randomized sizes, scroll offsets and edges never overlap");
	for (const auto dpr : { 1, 2, 3 }) {
		for (const auto size : { 10, 13, 24 }) {
			auto canvas = QImage(QSize(320, 200) * dpr,
				QImage::Format_ARGB32_Premultiplied);
			canvas.setDevicePixelRatio(dpr);
			canvas.fill(Qt::transparent);
			const auto font = QFont(u"Menlo"_q, size, QFont::DemiBold);
			const auto hints = std::vector<HintBadge>{
				{ u"fa"_q, { 12, 80 } }, { u"fb"_q, { 12, 80 } },
				{ u"fc"_q, { 12, 80 } }, { u"ga"_q, { 12, 80 } },
			};
			const auto metrics = QFontMetrics(font);
			const auto rects = LayoutHintBadges(std::vector<QRect>(3,
				QRect(QPoint(12, 80), QSize(metrics.horizontalAdvance(u"a"_q) + 14,
					metrics.height() + 6))), bounds, 3);
			{
				auto painter = QPainter(&canvas);
				PaintHintBadges(painter, hints, u"f"_q, font, bounds, { 7, 3 }, 3, false);
			}
			auto contained = true;
			auto ink = 0;
			for (auto y = 0; y != canvas.height(); ++y) {
				for (auto x = 0; x != canvas.width(); ++x) {
					const auto pixel = canvas.pixelColor(x, y);
					if (!pixel.alpha()) {
						continue;
					}
					contained &= std::ranges::any_of(rects, [&](QRect rect) {
						return rect.contains(QPoint(x / dpr, y / dpr));
					});
					ink += pixel.red() < 100;
				}
			}
			Check(contained && ink > 0,
				"painted labels fit disjoint badges at all font sizes and DPRs");
		}
	}
}

void TestHintTargetAnchoring() {
	using namespace Core::VimKeymap;
	const auto viewport = QRect(0, 1000, 900, 500);
	const auto url = QRect(113, 1055, 330, 17);
	const auto avatar = QRect(65, 1048, 34, 34);
	const auto media = QRect(113, 1124, 224, 224);
	const auto matches = [&](QPoint point) { return url.contains(point); };
	Check(LinkHintTargetRect({ 144, 1062 }, viewport, matches) == url,
		"coarse link hit resolves to exact visible URL bounds");
	Check(LinkHintTargetRect({ 12, 1012 }, viewport, matches).isEmpty(),
		"placeholder in message margin cannot become a link target");
	const auto clipped = QRect(0, 1060, 900, 500);
	Check(LinkHintTargetRect({ 144, 1062 }, clipped, matches)
			== url.intersected(clipped),
		"partially scrolled link stays on its visible portion");
	const auto nextLine = QRect(113, 1072, 120, 17);
	Check(LinkHintTargetRect({ 144, 1062 }, viewport, [&](QPoint point) {
		return url.contains(point) || nextLine.contains(point);
	}) == url, "wrapped link does not include empty space beside its next line");
	const auto desired = std::vector<QRect>{
		QRect(url.topLeft(), QSize(25, 24)),
		QRect(avatar.topLeft(), QSize(25, 24)),
		QRect(media.topLeft() + QPoint(8, 8), QSize(25, 24)),
	};
	const auto targets = std::vector<QRect>{ url, avatar, media };
	const auto placed = LayoutHintBadges(desired, viewport, 3, targets);
	Check(placed == desired,
		"separate URL, avatar and media badges keep their original target positions");
	const auto crowded = LayoutHintBadges(
		{ desired[0], desired[0], desired[0] },
		viewport,
		3,
		{ url, url, url });
	Check(std::ranges::all_of(crowded, [&](QRect rect) {
		return !rect.isEmpty() && url.contains(rect.center());
	}), "colliding link badges move along the link, never into the margin");
	const auto small = QRect(113, 1055, 8, 17);
	const auto overflow = LayoutHintBadges(
		{ desired[0], desired[0] }, viewport, 3, { small, small });
	Check(!overflow[0].isEmpty() && small.contains(overflow[0].center())
		&& overflow[1].isEmpty(),
		"crowded small target never sends a badge to unrelated content");
	auto random = QRandomGenerator(92051);
	auto anchored = true;
	for (auto run = 0; run != 200; ++run) {
		auto inputs = std::vector<QRect>();
		auto areas = std::vector<QRect>();
		for (auto i = 0; i != 40; ++i) {
			const auto area = QRect(random.bounded(900), 1000 + random.bounded(500),
				random.bounded(8, 300), random.bounded(8, 100));
			areas.push_back(area);
			inputs.emplace_back(area.topLeft(), QSize(random.bounded(20, 100), 28));
		}
		const auto rects = LayoutHintBadges(inputs, viewport, 3, areas);
		for (auto i = 0; i != rects.size(); ++i) {
			if (rects[i].isEmpty()) {
				continue;
			}
			anchored &= areas[i].contains(rects[i].center())
				&& viewport.contains(rects[i]);
			for (auto j = 0; j != i; ++j) {
				anchored &= rects[j].isEmpty()
					|| !rects[i].marginsAdded({ 3, 3, 3, 3 }).intersects(rects[j]);
			}
		}
	}
	Check(anchored, "randomized target layouts preserve anchoring and separation");
	for (const auto dpr : { 1, 2, 3 }) {
		auto canvas = QImage(QSize(900, 500) * dpr, QImage::Format_ARGB32_Premultiplied);
		canvas.setDevicePixelRatio(dpr);
		canvas.fill(Qt::transparent);
		const auto font = QFont(u"Menlo"_q, 13, QFont::DemiBold);
		const auto hints = std::vector<HintBadge>{
			{ u"f"_q, avatar.topLeft(), avatar },
			{ u"s"_q, url.topLeft(), url },
			{ u"d"_q, media.topLeft() + QPoint(8, 8), media },
		};
		auto expected = std::vector<QRect>();
		auto areas = std::vector<QRect>();
		const auto metrics = QFontMetrics(font);
		for (const auto &hint : hints) {
			expected.emplace_back(hint.anchor, QSize(
				metrics.horizontalAdvance(hint.label) + 14,
				metrics.height() + 6));
			areas.push_back(hint.target);
		}
		expected = LayoutHintBadges(expected, viewport, 3, areas);
		{
			auto painter = QPainter(&canvas);
			painter.translate(0, -1000);
			PaintHintBadges(painter, hints, {}, font, viewport, { 7, 3 }, 3, false);
		}
		auto ink = std::array<int, 3>();
		auto located = true;
		for (auto i = 0; i != expected.size(); ++i) {
			located &= !expected[i].isEmpty()
				&& areas[i].contains(expected[i].center());
		}
		for (auto y = 0; y != canvas.height(); ++y) {
			for (auto x = 0; x != canvas.width(); ++x) {
				if (!canvas.pixelColor(x, y).alpha()) {
					continue;
				}
				const auto point = QPoint(x / dpr, 1000 + y / dpr);
				auto found = false;
				for (auto i = 0; i != expected.size(); ++i) {
					if (expected[i].contains(point)) {
						++ink[i];
						found = true;
					}
				}
				located &= found;
			}
		}
		Check(located && std::ranges::all_of(ink, [](int count) { return count > 0; }),
			"painted badges remain beside URL, avatar and media at each DPR");
	}
}

void TestModalTabCycle() {
	auto root = Ui::RpWidget(nullptr);
	root.setAttribute(Qt::WA_DontShowOnScreen);
	auto menu = Ui::AbstractButton(&root);
	auto hidden = Ui::AbstractButton(&root);
	auto close = Ui::AbstractButton(&root);
	auto disabled = Ui::AbstractButton(&root);
	auto vimDisabled = Ui::AbstractButton(&root);
	auto add = Ui::AbstractButton(&root);
	for (const auto button : { &menu, &hidden, &close, &disabled, &add }) {
		button->setFocusPolicy(Qt::StrongFocus);
	}
	menu.setGeometry(100, 0, 30, 30);
	close.setGeometry(140, 0, 30, 30);
	add.setGeometry(0, 100, 170, 30);
	hidden.hide();
	disabled.setEnabled(false);
	vimDisabled.setDisabled(true);
	root.setVisualTabOrder(true);
	root.show();
	root.refreshVisualTabOrder();
	QApplication::setActiveWindow(&root);
	menu.setFocus();
	Core::VimKeymap::FocusModalNextPrevChild(&root, true);
	Check(close.hasFocus(), "tab moves from menu to close, skipping hidden controls");
	Core::VimKeymap::FocusModalNextPrevChild(&root, true);
	Check(add.hasFocus(), "tab skips both native and Telegram-disabled controls");
	Core::VimKeymap::FocusModalNextPrevChild(&root, true);
	Check(menu.hasFocus(), "tab wraps from last button to first");
	Core::VimKeymap::FocusModalNextPrevChild(&root, false);
	Check(add.hasFocus(), "shift tab wraps from first button to last");
}

void TestPopupHandlerScope() {
	using Core::VimKeymap::KeyHandlerInScope;
	auto background = QWidget();
	auto popup = QWidget(&background, Qt::Popup);
	auto preview = QWidget(&popup);
	auto hidden = QWidget(&popup);
	auto otherPopup = QWidget(&popup, Qt::Popup);
	auto otherPreview = QWidget(&otherPopup);
	preview.show();
	hidden.hide();
	otherPreview.show();
	Check(KeyHandlerInScope(&preview, &popup),
		"popup keeps its own chat preview key handler for j/k and enter");
	Check(!KeyHandlerInScope(&background, &popup),
		"popup blocks background chat and sticker pack handlers");
	Check(!KeyHandlerInScope(&hidden, &popup),
		"popup ignores hidden handlers");
	Check(!KeyHandlerInScope(&otherPreview, &popup),
		"popup ignores handlers in a different popup window");
	Check(KeyHandlerInScope(&background, nullptr),
		"normal key dispatch remains unrestricted without a popup");
}

void TestPopupMenuKeyboardCycle() {
	auto owner = QWidget();
	auto menu = Ui::PopupMenu(&owner, st::defaultPopupMenu);
	menu.deleteOnHide(false);
	const auto first = menu.addAction(u"Share"_q, [] {});
	menu.addSeparator();
	const auto disabled = menu.addAction(u"Unavailable"_q, [] {});
	disabled->setEnabled(false);
	const auto last = menu.addAction(u"Copy link"_q, [] {});
	const auto send = [&](Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
		auto event = QKeyEvent(QEvent::KeyPress, key, mods);
		QApplication::sendEvent(&menu, &event);
	};
	const auto selected = [&] {
		const auto item = menu.menu()->findSelectedAction();
		return item ? item->action().get() : nullptr;
	};
	send(Qt::Key_Tab);
	Check(selected() == first, "menu tab selects first action");
	send(Qt::Key_Tab);
	Check(selected() == last, "menu tab skips separator and disabled action");
	send(Qt::Key_Tab);
	Check(selected() == first, "menu tab wraps");
	send(Qt::Key_Tab, Qt::ShiftModifier);
	Check(selected() == last, "menu shift tab wraps backwards");
	send(Qt::Key_Backtab, Qt::ShiftModifier);
	Check(selected() == first, "menu backtab moves backwards");
	auto escape = QKeyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	escape.ignore();
	QApplication::sendEvent(&menu, &escape);
	Check(escape.isAccepted(), "popup consumes escape instead of propagating to owner");
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

int main(int argc, char *argv[]) {
#ifndef Q_OS_MAC
	qputenv("QT_QPA_PLATFORM", "offscreen");
#endif // !Q_OS_MAC
	auto application = QApplication(argc, argv);
	auto integration = TestIntegration();
	Ui::Integration::Set(&integration);
	style::StartManager(100);

	TestVimKeymapNavigationBindings();
	TestVimKeymapActionBindings();
	TestVimKeymapCommandBindings();
	TestVimKeymapTransientUiKeys();
	TestVimKeymapCursorGeometry();
	TestVimKeymapPickerNavigation();
	TestVimKeymapStickerSetNavigation();
	TestVimKeymapMediaNavigation();
	TestAbstractButtonKeyboardActivation();
	TestHintBadgeLayoutAndPainting();
	TestHintTargetAnchoring();
	TestModalTabCycle();
	TestPopupMenuKeyboardCycle();
	TestPopupHandlerScope();

	std::cout << (TotalChecks - FailedChecks) << "/" << TotalChecks
		<< " checks passed." << std::endl;
	return FailedChecks ? 1 : 0;
}
