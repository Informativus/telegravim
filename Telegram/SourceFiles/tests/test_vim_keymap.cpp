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
#include "core/vim_keymap_config.h"
#include "core/vim_keymap_log.h"
#include "core/local_socket_security.h"
#include "core/vim_keymap_options.h"
#include "settings/settings_vim_editor.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/wrap/vertical_layout.h"
#include "info/info_navigation_history.h"
#include "base/qt/qt_tab_key.h"
#include "base/flat_map.h"
#include "base/integration.h"
#include "ui/abstract_button.h"
#include "ui/integration.h"
#include "ui/layers/layer_widget.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/text/text_utilities.h"
#include "ui/text/text.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_palette.h"
#include "styles/style_widgets.h"
#include "styles/style_vim_keymap.h"

#include <crl/crl_on_main.h>
#include <rpl/never.h>

#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QClipboard>
#include <QtGui/QtEvents>
#include <QtCore/QMimeData>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QSaveFile>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSlider>
#include <QtWidgets/QPlainTextEdit>
#include <QtGui/QSyntaxHighlighter>

#include <iostream>
#include <algorithm>
#include <memory>

namespace crl {

rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}

} // namespace crl

namespace {

using MatchOptions = Core::VimKeymap::Bindings::MatchOptions;

int FailedChecks = 0;
int TotalChecks = 0;

class TestBaseIntegration final : public base::Integration {
public:
	using base::Integration::Integration;
	void enterFromEventLoop(FnMut<void()> &&method) override {
		method();
	}
	bool logSkipDebug() override {
		return true;
	}
	void logMessageDebug(const QString &message) override {
	}
	void logMessage(const QString &message) override {
	}
	void logAssertionViolation(const QString &info) override {
		std::cerr << info.toStdString() << std::endl;
	}

};

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

void DrainMainQueue() {
	auto drained = true;
	for (auto i = 0; i != 3; ++i) {
		auto loop = QEventLoop();
		auto completed = false;
		crl::on_main(&loop, [&] {
			completed = true;
			loop.quit();
		});
		QTimer::singleShot(1000, &loop, &QEventLoop::quit);
		loop.exec();
		drained &= completed;
	}
	Check(drained, "queued UI actions finish before assertions");
}

#include "tests/vim_focus_labels_tests.h"

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

void TestMessageTextNavigation() {
	using Core::VimKeymap::TextMotion;
	using Core::VimKeymap::Bindings::TextMotionKey;
	auto pending = false;
	auto g = QKeyEvent(QEvent::KeyPress, Qt::Key_G, Qt::NoModifier, u"g"_q);
	Check(!TextMotionKey(&g, pending) && pending, "first g waits inside text");
	Check(TextMotionKey(&g, pending) == TextMotion::TextStart && !pending,
		"gg moves to the beginning of message text");
	auto repeated = QKeyEvent(
		QEvent::KeyPress, Qt::Key_G, Qt::NoModifier, u"g"_q, true);
	Check(!TextMotionKey(&g, pending) && pending, "g arms a new sequence");
	Check(!TextMotionKey(&repeated, pending) && pending,
		"holding g does not complete gg");
	auto unknown = QKeyEvent(
		QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, u"q"_q);
	Check(!TextMotionKey(&unknown, pending) && !pending,
		"unsupported keys cancel a pending sequence without a motion");
	Check(!TextMotionKey(&g, pending) && pending,
		"g after an unsupported key starts a fresh sequence");
	auto end = QKeyEvent(
		QEvent::KeyPress, Qt::Key_G, Qt::ShiftModifier, u"G"_q);
	Check(TextMotionKey(&end, pending) == TextMotion::TextEnd && !pending,
		"Shift G moves to the end and cancels a pending g");
	auto russianG = QKeyEvent(
		QEvent::KeyPress, 0x041F, Qt::NoModifier, u"п"_q);
	Check(!TextMotionKey(&russianG, pending) && pending,
		"Russian g starts the text sequence");
	Check(TextMotionKey(&russianG, pending) == TextMotion::TextStart,
		"Russian gg moves to the beginning of message text");
	auto russianEnd = QKeyEvent(
		QEvent::KeyPress, 0x041F, Qt::ShiftModifier, u"П"_q);
	Check(TextMotionKey(&russianEnd, pending) == TextMotion::TextEnd,
		"Russian Shift G moves to the end of message text");
	const auto paragraphKeys = std::vector<std::pair<int, QString>>{
		{ Qt::Key_BraceLeft, u"{"_q },
		{ Qt::Key_BraceRight, u"}"_q },
		{ Qt::Key_BracketLeft, u"{"_q },
		{ Qt::Key_BracketRight, u"}"_q },
		{ 0x0425, u"Х"_q },
		{ 0x042A, u"Ъ"_q },
	};
	for (auto i = 0; i != int(paragraphKeys.size()); ++i) {
		const auto &[key, text] = paragraphKeys[i];
		auto event = QKeyEvent(QEvent::KeyPress, key, Qt::ShiftModifier, text);
		Check(TextMotionKey(&event, pending) == ((i % 2)
			? TextMotion::ParagraphNext : TextMotion::ParagraphPrevious),
			"paragraph motions accept English and Russian layouts");
	}
	for (const auto key : { Qt::Key_Q, Qt::Key_I, Qt::Key_Tab,
			Qt::Key_Return, Qt::Key_Backspace, Qt::Key_Delete, Qt::Key_Escape }) {
		auto event = QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier);
		Check(!TextMotionKey(&event, pending),
			"non-motion keys leave the text cursor unchanged");
	}
	auto controlG = QKeyEvent(
		QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier, u"g"_q);
	Check(!TextMotionKey(&controlG, pending) && !pending,
		"Ctrl G cannot jump from message text to chat history");
	const auto paragraph = [&](const QString &text, int position, int direction) {
		return Core::VimKeymap::TextParagraphOffset(
			position,
			direction,
			int(text.size()),
			[&](int offset) {
				Check(offset >= 0 && offset < text.size(),
					"paragraph lookup stays inside selected message text");
				return text[offset] == u'\n' || text[offset] == u'\r'
					|| text[offset] == QChar::ParagraphSeparator
					|| text[offset] == QChar::LineSeparator;
			});
	};
	const auto text = u"Первый\n\nВторой\nТретий"_q;
	Check(paragraph(text, 0, 1) == 8, "next paragraph skips blank lines");
	Check(paragraph(text, 10, -1) == 8, "backward moves to current paragraph start");
	Check(paragraph(text, 8, -1) == 0, "backward from start moves to previous paragraph");
	Check(paragraph(text, 8, 1) == 15, "single newline separates paragraphs");
	Check(paragraph(text, 15, 1) == text.size() - 1,
		"last paragraph stops at the last character");
	Check(paragraph(text, text.size() - 1, 1) == text.size() - 1,
		"forward at the end stays in the message");
	Check(paragraph(text, 0, -1) == 0, "backward at the beginning stays in the message");
	Check(paragraph(u"a\r\n\r\nb"_q, 0, 1) == 5, "CRLF boundaries skip blank paragraphs");
	Check(paragraph(u"a\u2029b\u2028c"_q, 0, 1) == 2, "Unicode paragraph separator");
	Check(paragraph(u"a\u2029b\u2028c"_q, 2, 1) == 4, "Unicode line separator");
	Check(paragraph(u"😀\nя"_q, 0, 1) == 3, "paragraph offsets retain UTF-16 coordinates");
	Check(paragraph(QString(), 0, 1) == -1, "empty messages have no paragraph target");
	Check(paragraph(u"я"_q, 0, 1) == 0, "single-character messages stay bounded");
	Check(paragraph(u"\n\nя"_q, 0, 1) == 2, "leading blank paragraphs are skipped");
	Check(paragraph(u"я\n\n"_q, 0, 1) == 2, "trailing blank paragraphs stay bounded");
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
	root.resize(300, 200);
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

class NavigationTestLayer final : public Ui::LayerWidget {
public:
	using Ui::LayerWidget::LayerWidget;
	void parentResized() override {
	}

};

void TestKeyboardLayerStack() {
	using namespace Core::VimKeymap;
	auto root = QWidget();
	root.setAttribute(Qt::WA_DontShowOnScreen);
	root.resize(500, 500);
	auto stack = Ui::LayerStackWidget(&root, {});
	auto profile = object_ptr<NavigationTestLayer>(&stack);
	const auto profilePointer = QPointer<NavigationTestLayer>(profile.data());
	profile->setGeometry(30, 30, 400, 400);
	root.show();
	QApplication::setActiveWindow(&root);
	stack.showSpecialLayer(std::move(profile), anim::type::instant);
	Check(FindKeyboardScope(&root) == profilePointer,
		"real layer stack exposes special profile layer as scope");
	auto dialog = std::make_unique<NavigationTestLayer>(&stack);
	const auto dialogPointer = QPointer<NavigationTestLayer>(dialog.get());
	dialog->setGeometry(50, 50, 300, 300);
	stack.showLayer(std::move(dialog), Ui::LayerOption::KeepOther, anim::type::instant);
	Check(FindKeyboardScope(&root) == dialogPointer,
		"stack dialog overrides special profile layer");
	auto inner = NavigationTestLayer(dialogPointer);
	inner.setGeometry(10, 10, 100, 100);
	inner.show();
	Check(FindKeyboardScope(&root) == &inner,
		"scope resolver descends into nested layer inside stack top");
	inner.hide();
	Check(FindKeyboardScope(&root) == dialogPointer,
		"hidden nested layer no longer captures keyboard scope");
}

void TestNestedKeyboardNavigation() {
	using namespace Core::VimKeymap;
	auto root = Ui::RpWidget(nullptr);
	root.setAttribute(Qt::WA_DontShowOnScreen);
	root.resize(500, 500);
	auto background = Ui::AbstractButton(&root);
	auto layer = NavigationTestLayer(&root);
	layer.setGeometry(20, 20, 420, 420);
	auto header = QWidget(&layer);
	header.setGeometry(0, 0, 400, 60);
	auto close = Ui::AbstractButton(&header);
	close.setGeometry(360, 0, 30, 30);
	auto body = QWidget(&layer);
	body.setGeometry(0, 60, 400, 300);
	auto message = Ui::AbstractButton(&body);
	message.setGeometry(20, 0, 150, 40);
	auto embedded = Ui::AbstractButton(&message);
	embedded.setGeometry(115, 0, 30, 30);
	auto checkbox = Ui::Checkbox(&body, u"Enabled"_q, false, st::defaultCheckbox);
	checkbox.setGeometry(20, 60, 150, 30);
	auto input = QLineEdit(&body);
	input.setGeometry(20, 110, 150, 30);
	auto disabled = Ui::AbstractButton(&body);
	disabled.setDisabled(true);
	auto hidden = QWidget(&body);
	auto hiddenButton = Ui::AbstractButton(&hidden);
	hidden.hide();
	auto popup = QWidget(&layer, Qt::Popup);
	auto popupButton = Ui::AbstractButton(&popup);
	root.show();
	QApplication::setActiveWindow(&root);
	background.setFocus();
	Check(FindKeyboardScope(&root) == &layer,
		"profile-like non-box layer is found even when focus remains in background");
	const auto targets = KeyboardFocusTargets(&layer);
	Check(targets == std::vector<QPointer<QWidget>>{
		&close, &message, &embedded, &checkbox, &input },
		"recursive visual order includes nested button, checkbox and input only");
	const auto navigation = KeyboardNavigation::Get(&layer);
	Check(!navigation->scroll(80, false, 0),
		"surface without scrolling defers j/k to native media controls");
	for (const auto target : targets) {
		navigation->focusNext(true);
		Check(QApplication::focusWidget() == target,
			"Tab reaches each nested target in visual order");
	}
	navigation->focusNext(true);
	Check(close.hasFocus(), "Tab stays in profile and wraps after input");
	navigation->focusNext(false);
	Check(input.hasFocus(), "Shift Tab wraps backwards inside profile");
	auto typed = QKeyEvent(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier, u"f"_q);
	QApplication::sendEvent(&input, &typed);
	Check(input.text() == u"f"_q, "focused editor retains text input");
	Check(KeyboardScopeHasTextInput(&layer, &input),
		"scope recognizes its nested editor for text passthrough");
	Check(!KeyboardScopeHasTextInput(&popup, &popup),
		"editor behind popup does not disable popup keyboard navigation");

	auto nested = NavigationTestLayer(&layer);
	nested.setGeometry(40, 40, 200, 160);
	auto nestedButton = Ui::AbstractButton(&nested);
	nestedButton.setGeometry(20, 20, 100, 35);
	auto closed = 0;
	nested.setClosedCallback([&] { ++closed; nested.hide(); });
	nested.show();
	nestedButton.show();
	Check(FindKeyboardScope(&root) == &nested,
		"deepest visible layer wins over parent profile focus");
	Check(!KeyHandlerInScope(&message, &nested)
		&& KeyHandlerInScope(&nestedButton, &nested),
		"nested layer blocks all parent key handlers");
	const auto childNavigation = KeyboardNavigation::Get(&nested);
	childNavigation->focusNext(true);
	childNavigation->focusNext(true);
	Check(nestedButton.hasFocus(), "single nested target wraps without losing focus");
	childNavigation->showHints([](int, int) { return u"a"_q; },
		QFont(u"Menlo"_q, 13), { 7, 3 }, 3);
	Check(CloseKeyboardScope(&nested) && !closed
		&& !childNavigation->hasHints() && nested.isVisible(),
		"first Escape dismisses only focus hints");
	Check(CloseKeyboardScope(&nested) && closed == 1 && layer.isVisible(),
		"next Escape closes only the nested layer");
	Check(FindKeyboardScope(&root) == &layer,
		"parent profile becomes active after nested dismissal");
	navigation->restoreFocus();
	Check(input.hasFocus(), "returning to parent restores its previous target");
	navigation->focusTarget(&message);
	navigation->showHints([](int index, int) {
		return QString(QChar('a' + index));
	}, QFont(u"Menlo"_q, 13), { 7, 3 }, 3);
	input.setFocus(Qt::MouseFocusReason);
	Check(!navigation->hasHints() && input.hasFocus(),
		"clicking into an input cancels hints before typing");

	auto activated = 0;
	message.setClickedCallback([&] { ++activated; });
	navigation->showHints([](int index, int) {
		return QString(QChar('a' + index));
	}, QFont(u"Menlo"_q, 13), { 7, 3 }, 3);
	auto choose = QKeyEvent(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, u"b"_q);
	Check(navigation->handleHintKey(&choose, u"b"_q)
		&& message.hasFocus() && !activated && !navigation->hasHints(),
		"hint selection focuses target without activating its action");
	auto press = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	auto release = QKeyEvent(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(&message, &press);
	QApplication::sendEvent(&message, &release);
	Check(activated == 1, "Enter activates hinted button exactly once");
	navigation->focusTarget(&checkbox);
	QApplication::sendEvent(&checkbox, &press);
	QApplication::sendEvent(&checkbox, &release);
	Check(checkbox.checked(), "nested checkbox toggles from keyboard");

	auto dynamic = std::make_unique<Ui::AbstractButton>(&body);
	dynamic->setGeometry(20, 170, 150, 30);
	dynamic->show();
	navigation->showHints([](int index, int) {
		return QString(QChar('a' + index));
	}, QFont(u"Menlo"_q, 13), { 7, 3 }, 3);
	dynamic.reset();
	Check(navigation->handleHintKey(&choose, u"f"_q)
		&& navigation->hasHints(), "deleted hint target is ignored without closing hints");
	auto escape = QKeyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	Check(navigation->handleHintKey(&escape, {}) && !navigation->hasHints(),
		"Escape cancels hints after target deletion");
	auto right = Ui::AbstractButton(&body);
	right.setGeometry(250, 0, 100, 40);
	right.show();
	root.setLayoutDirection(Qt::RightToLeft);
	const auto rtl = KeyboardFocusTargets(&body);
	Check(rtl.size() >= 2 && rtl[0] == &right && rtl[1] == &message,
		"same-row target order follows right-to-left layout");
}

void TestShortestHintLabels() {
	using Core::VimKeymap::Bindings::ShortestHintLabel;
	for (const auto &alphabet : {
		u"abc"_q,
		u"abcdefghijklmnopqrstuvwxyz"_q,
		u"\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439"_q }) {
		const auto base = int(alphabet.size());
		for (const auto total : { 1, base, base + 1, base * 2,
			base * base, base * base + 1, 2000 }) {
			auto labels = std::vector<QString>();
			auto single = 0;
			auto valid = true;
			for (auto i = 0; i != total; ++i) {
				const auto label = ShortestHintLabel(i, total, alphabet);
				single += (label.size() == 1);
				valid &= !label.isEmpty()
					&& std::all_of(label.begin(), label.end(), [&](QChar c) {
						return alphabet.contains(c);
					});
				labels.push_back(label);
			}
			std::sort(labels.begin(), labels.end());
			for (auto i = 1; i != total; ++i) {
				valid &= !labels[i].startsWith(labels[i - 1]);
			}
			Check(valid, "hint labels stay unique and prefix-free beyond two-letter capacity");
			if (total <= base) {
				Check(single == total, "all hints stay single-letter when the alphabet is sufficient");
			} else if (total == base + 1) {
				Check(single == base - 1,
					"one extra target expands only one single-letter prefix");
			}
		}
	}
	Check(ShortestHintLabel(0, 27, u"abcdefghijklmnopqrstuvwxyz"_q) == u"a"_q
		&& ShortestHintLabel(24, 27, u"abcdefghijklmnopqrstuvwxyz"_q) == u"y"_q
		&& ShortestHintLabel(25, 27, u"abcdefghijklmnopqrstuvwxyz"_q) == u"za"_q
		&& ShortestHintLabel(26, 27, u"abcdefghijklmnopqrstuvwxyz"_q) == u"zb"_q,
		"crowded hint sets keep priority targets short");
	Check(ShortestHintLabel(-1, 1, u"abc"_q).isEmpty()
		&& ShortestHintLabel(1, 1, u"abc"_q).isEmpty()
		&& ShortestHintLabel(0, 1, {}).isEmpty(), "invalid hint requests return no label");
}

void TestProfileKeyboardNavigation() {
	using namespace Core::VimKeymap;
	auto window = QWidget();
	window.setAttribute(Qt::WA_DontShowOnScreen);
	window.resize(500, 400);
	auto profile = NavigationTestLayer(&window);
	profile.setGeometry(20, 20, 380, 300);
	auto scroll = QScrollArea(&profile);
	scroll.setGeometry(0, 0, 360, 280);
	const auto content = new QWidget();
	content->resize(330, 800);
	scroll.setWidget(content);
	auto qr = Ui::AbstractButton(content);
	qr.setGeometry(280, 10, 30, 30);
	auto empty = Ui::AbstractButton(content);
	empty.setGeometry(0, 45, 300, 0);
	auto collapsed = QWidget(content);
	collapsed.setGeometry(0, 50, 300, 0);
	auto collapsedButton = Ui::AbstractButton(&collapsed);
	collapsedButton.setGeometry(0, 0, 300, 40);
	auto photos = Ui::SlideWrap<Ui::SettingsButton>(content,
		object_ptr<Ui::SettingsButton>(content, rpl::single(u"261 photos"_q)));
	photos.resizeToWidth(330);
	photos.move(0, 70);
	auto clipped = QWidget(content);
	clipped.setGeometry(0, 110, 330, 10);
	auto clippedButton = Ui::AbstractButton(&clipped);
	clippedButton.setGeometry(0, 20, 330, 40);
	auto videos = Ui::SlideWrap<Ui::SettingsButton>(content,
		object_ptr<Ui::SettingsButton>(content, rpl::single(u"5 videos"_q)));
	videos.resizeToWidth(330);
	videos.move(0, 140);
	auto last = Ui::AbstractButton(content);
	last.setGeometry(0, 720, 330, 40);
	window.show();
	QApplication::setActiveWindow(&window);
	DrainMainQueue();
	const auto expected = std::vector<QPointer<QWidget>>{
		&qr, photos.entity(), videos.entity(), &last };
	Check(KeyboardFocusTargets(&profile) == expected,
		"profile cycle excludes scroll containers and fully clipped controls");
	const auto navigation = KeyboardNavigation::Get(&profile);
	navigation->focusTarget(&qr);
	navigation->focusNext(true);
	Check(photos.entity()->hasFocus(), "one Tab moves directly from QR to photos");
	navigation->focusNext(true);
	Check(videos.entity()->hasFocus(), "one Tab moves directly from photos to videos");
	navigation->focusNext(false);
	Check(photos.entity()->hasFocus(), "one Shift Tab returns directly to photos");
	navigation->focusNext(true);
	navigation->focusNext(true);
	Check(last.hasFocus() && scroll.verticalScrollBar()->value() > 0,
		"profile keeps reachable offscreen actions in its cycle and reveals them");
	Check(KeyboardFocusTargets(&profile) == expected,
		"profile target order stays stable after automatic scrolling");
	navigation->focusNext(true);
	Check(qr.hasFocus() && scroll.verticalScrollBar()->value() <= qr.y(),
		"profile wraps to its first visible control without an empty stop");
	scroll.verticalScrollBar()->setValue(0);
	Check(navigation->scroll(80, false, 0) && scroll.verticalScrollBar()->value() == 80,
		"profile navigation scrolls its own contents down");
	Check(navigation->scroll(-80, false, 0) && scroll.verticalScrollBar()->value() == 0,
		"profile navigation scrolls its own contents up");
}

void TestGlobalPlayerFocusHints() {
	using namespace Core::VimKeymap;
	auto window = QWidget();
	window.setAttribute(Qt::WA_DontShowOnScreen);
	window.resize(440, 200);
	auto player = QWidget(&window);
	player.setGeometry(0, 0, 440, 35);
	auto controls = QWidget(&player);
	controls.setGeometry(337, 0, 103, 35);
	auto buttons = std::vector<std::unique_ptr<Ui::AbstractButton>>();
	const auto rightRects = std::array{
		QRect(0, 0, 34, 35),
		QRect(34, 0, 30, 30),
		QRect(64, 0, 39, 35),
	};
	for (auto i = 0; i != 6; ++i) {
		const auto parent = i < 3 ? &player : &controls;
		auto button = std::make_unique<Ui::AbstractButton>(parent);
		button->setGeometry(i < 3 ? QRect(9 + i * 24, 0, 24, 35) : rightRects[i - 3]);
		buttons.push_back(std::move(button));
	}
	auto caption = Ui::LabelSimple(&player);
	caption.setGeometry(126, 0, 120, 30);
	auto hidden = Ui::AbstractButton(&player);
	auto disabled = Ui::AbstractButton(&player);
	disabled.setDisabled(true);
	auto clipped = Ui::AbstractButton(&controls);
	clipped.setGeometry(200, 0, 40, 40);
	window.show();
	hidden.hide();
	QApplication::setActiveWindow(&window);
	RegisterGlobalFocusRoot(&player);
	RegisterGlobalFocusRoot(&player);
	Check(GlobalFocusRoots(&window).size() == 1, "global player registration is idempotent");
	const auto targets = VisibleKeyboardHintTargets(&player);
	Check(targets.size() == 6, "only visible enabled player controls take hint letters");
	Check(GlobalFocusRoot(buttons.back().get()) == &player,
		"nested right-side player controls retain their global focus scope");
	const auto navigation = KeyboardNavigation::Get(&player);
	const auto alphabet = u"abcdefghijklmnopqrstuvwxyz"_q;
	const auto total = 29;
	navigation->showHints([&](int i, int) {
		return Bindings::ShortestHintLabel(i, total, alphabet);
	}, QFont(u"Menlo"_q, 12), QSize(4, 2), 3);
	const auto output = qEnvironmentVariable("VIM_KEYMAP_FOCUS_SNAPSHOTS");
	for (const auto ratio : { 1, 2 }) {
		auto image = QImage(player.size() * ratio, QImage::Format_ARGB32_Premultiplied);
		image.setDevicePixelRatio(ratio);
		image.fill(Qt::transparent);
		{
			auto painter = QPainter(&image);
			navigation->render(&painter);
		}
		auto painted = true;
		for (const auto target : targets) {
			const auto bounds = QRect(
				target->mapTo(&player, QPoint()) * ratio,
				target->size() * ratio);
			auto pixels = 0;
			for (auto y = bounds.top(); y <= bounds.bottom(); ++y) {
				for (auto x = bounds.left(); x <= bounds.right(); ++x) {
					pixels += qAlpha(image.pixel(x, y)) > 0;
				}
			}
			painted &= pixels > 0;
		}
		Check(painted, "every narrow player button receives a visible hint at its own position");
		if (!output.isEmpty()) {
			Check(image.save(output + u"/player-hints-%1x.png"_q.arg(ratio)),
				"player hint snapshot is saved at its display pixel ratio");
		}
	}
	auto choose = QKeyEvent(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, u"b"_q);
	Check(navigation->handleHintKey(&choose, u"b"_q) && buttons[1]->hasFocus(),
		"player hint focuses the correct button without activating it");
	auto clicks = 0;
	buttons[1]->setClickedCallback([&] { ++clicks; });
	auto press = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	auto release = QKeyEvent(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(buttons[1].get(), &press);
	QApplication::sendEvent(buttons[1].get(), &release);
	Check(clicks == 1, "Enter uses the player's native button callback exactly once");
	navigation->focusTarget(buttons.back().get());
	navigation->focusNext(true);
	Check(buttons.front()->hasFocus(), "Tab wraps through player controls");
	navigation->focusNext(false);
	Check(buttons.back()->hasFocus(), "Shift Tab wraps backwards through player controls");
	navigation->showHints([](int i, int) {
		return i == 0 ? u"za"_q : u"zb"_q + QString::number(i);
	}, QFont(u"Menlo"_q, 12), QSize(4, 2), 3);
	navigation->setHintPrefix(u"z"_q);
	auto finish = QKeyEvent(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, u"a"_q);
	Check(navigation->handleHintKey(&finish, u"a"_q) && buttons.front()->hasFocus(),
		"widget hints accept the same multi-letter prefix as message hints");
	navigation->showHints([](int i, int) { return QString(QChar('a' + i)); },
		QFont(u"Menlo"_q, 12), QSize(4, 2), 3);
	auto escape = QKeyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	Check(navigation->handleHintKey(&escape, {}) && !navigation->hasHints()
		&& player.isVisible(), "Escape removes hints without closing playback");
	player.hide();
	Check(GlobalFocusRoots(&window).empty(), "a hidden player contributes no global hints");
	player.show();
	auto other = QWidget();
	other.setAttribute(Qt::WA_DontShowOnScreen);
	other.show();
	Check(GlobalFocusRoots(&other).empty(), "player hints never leak into another window");
	const auto temporary = new QWidget(&window);
	RegisterGlobalFocusRoot(temporary);
	delete temporary;
	Check(GlobalFocusRoots(&window).size() == 1, "destroyed player roots are removed safely");
}

void TestMediaPlaybackAndShareBindings() {
	using namespace Core::VimKeymap::Bindings;
	const auto speed = [&](int key, const QString &text, float64 current,
			Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
		auto event = QKeyEvent(QEvent::KeyPress, key, modifiers, text);
		return MediaPlaybackSpeed(&event, current);
	};
	Check(speed(Qt::Key_D, u"d"_q, 1.) == 1.1, "d increases video speed by one tenth");
	Check(speed(Qt::Key_A, u"a"_q, 1.) == 0.9, "a decreases video speed by one tenth");
	Check(speed(Qt::Key_Q, u"q"_q, 2.3) == 1., "q resets video speed to exactly 1x");
	Check(speed(Qt::Key_D, u"d"_q, 1.25) == 1.35
		&& speed(Qt::Key_A, u"a"_q, 1.25) == 1.15,
		"speed increments preserve the offset of native quarter-speed presets");
	Check(speed(Qt::Key_D, u"d"_q, 2.5) == 2.5
		&& speed(Qt::Key_A, u"a"_q, 0.5) == 0.5,
		"video speed stays within the player-supported limits");
	Check(speed(0x0412, u"\u0432"_q, 1.1) == 1.2
		&& speed(0x0424, u"\u0444"_q, 1.1) == 1.
		&& speed(0x0419, u"\u0439"_q, 2.5) == 1.,
		"video speed keys work in the Cyrillic layout");
	for (const auto modifiers : { Qt::ControlModifier, Qt::MetaModifier,
		Qt::AltModifier, Qt::ShiftModifier }) {
		Check(!speed(Qt::Key_A, u"a"_q, 1., modifiers)
			&& !speed(Qt::Key_D, u"d"_q, 1., modifiers)
			&& !speed(Qt::Key_Q, u"q"_q, 1., modifiers),
			"speed keys leave modified system and editing shortcuts alone");
	}
	auto current = 1.;
	for (auto i = 0; i != 10; ++i) {
		current = *speed(Qt::Key_D, u"d"_q, current);
	}
	Check(current == 2., "repeated speed increments do not accumulate floating-point drift");
	for (const auto &[key, text] : {
		std::pair(Qt::Key_S, u"s"_q),
		std::pair(Qt::Key_unknown, u"\u044B"_q) }) {
		auto share = QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier, text);
		Check(IsMessageShare(&share), "s enters message-sharing hints in either layout");
		auto repeat = QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier, text, true);
		Check(!IsMessageShare(&repeat), "holding s cannot repeatedly open sharing");
		auto save = QKeyEvent(QEvent::KeyPress, key, Qt::ControlModifier, text);
		Check(!IsMessageShare(&save), "share hints do not steal system save shortcuts");
	}
	Check(Core::VimKeymap::ActionUsesMessageHints(Core::VimKeymap::Action::ShareMessage),
		"sharing is a message-hint action");
}

void TestShareKeyboardFocusCycle() {
	using namespace Core::VimKeymap;
	auto root = QWidget();
	root.setAttribute(Qt::WA_DontShowOnScreen);
	root.resize(400, 300);
	auto search = QLineEdit(&root);
	search.setGeometry(20, 10, 340, 30);
	auto strip = QWidget(&root);
	strip.setGeometry(20, 50, 340, 30);
	auto active = -1;
	auto folders = std::vector<QWidget*>();
	for (auto i = 0; i != 3; ++i) {
		const auto folder = CreateKeyboardTabTarget(&strip, [&, i] { active = i; });
		folder->setGeometry(i * 100, 0, 90, 30);
		folders.push_back(folder);
	}
	auto cancel = Ui::AbstractButton(&root);
	cancel.setGeometry(20, 250, 120, 30);
	auto submit = Ui::AbstractButton(&root);
	submit.setGeometry(200, 250, 160, 30);
	root.show();
	QApplication::setActiveWindow(&root);
	const auto navigation = KeyboardNavigation::Get(&root);
	navigation->focusTarget(&search);
	for (auto i = 0; i != 3; ++i) {
		FocusModalNextPrevChild(&root, true);
		DrainMainQueue();
		Check(folders[i]->hasFocus() && active == i,
			"Tab focuses and activates each folder after the search field");
	}
	FocusModalNextPrevChild(&root, true);
	Check(cancel.hasFocus(), "Tab leaves the last folder for the first dialog button");
	FocusModalNextPrevChild(&root, true);
	Check(submit.hasFocus(), "Tab visits the submit button");
	FocusModalNextPrevChild(&root, true);
	Check(search.hasFocus(), "Tab wraps from buttons to search");
	FocusModalNextPrevChild(&root, false);
	Check(submit.hasFocus(), "Shift Tab wraps from search to buttons");
	FocusModalNextPrevChild(&root, false);
	FocusModalNextPrevChild(&root, false);
	DrainMainQueue();
	Check(folders.back()->hasFocus() && active == 2,
		"Shift Tab enters folders from the last folder");
	FocusModalNextPrevChild(&root, false);
	DrainMainQueue();
	Check(folders[1]->hasFocus() && active == 1, "Shift Tab activates the previous folder");
	navigation->focusTarget(&search);
	search.setText(u"recipient"_q);
	auto escape = QKeyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	Check(LeaveKeyboardInput(&root, &search, &escape)
		&& root.hasFocus() && root.isVisible() && search.text() == u"recipient"_q,
		"first Escape leaves search without clearing query or closing the dialog");
	Check(!LeaveKeyboardInput(&root, &search, &escape),
		"second Escape passes through to normal dialog closing");
	navigation->focusTarget(&search);
	Check(search.hasFocus(), "Tab focus can restore text editing after Escape");
	const auto temporary = CreateKeyboardTabTarget(&strip, [&] { active = 99; });
	temporary->show();
	navigation->focusTarget(temporary);
	delete temporary;
	DrainMainQueue();
	Check(active != 99, "removed folder cannot run a queued activation");
}

void TestKeyboardMenuNavigation() {
	using namespace Core::VimKeymap;
	auto root = QWidget();
	root.setAttribute(Qt::WA_DontShowOnScreen);
	root.resize(400, 300);
	auto header = Ui::AbstractButton(&root);
	header.setGeometry(300, 0, 40, 40);
	auto first = Ui::AbstractButton(&root);
	first.setGeometry(0, 50, 350, 40);
	auto disabled = Ui::AbstractButton(&root);
	disabled.setGeometry(0, 100, 350, 40);
	disabled.setDisabled(true);
	auto group = QWidget(&root);
	group.setGeometry(0, 150, 350, 100);
	auto second = Ui::AbstractButton(&group);
	second.setGeometry(0, 0, 350, 40);
	auto hidden = Ui::AbstractButton(&group);
	hidden.setGeometry(0, 50, 350, 40);
	hidden.hide();
	auto input = QLineEdit(&root);
	input.setGeometry(0, 260, 350, 30);
	input.hide();
	root.show();
	QApplication::setActiveWindow(&root);
	const auto navigation = KeyboardNavigation::Get(&root);
	navigation->focusTarget(&header);
	const auto send = [&](int key, const QString &text = {},
			Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
		auto event = QKeyEvent(QEvent::KeyPress, key, modifiers, text);
		return navigation->handleMenuNavigation(&event);
	};
	Check(send(Qt::Key_J, u"j"_q) && first.hasFocus()
		&& first.isOver() && !header.isOver(),
		"menu j transfers real focus and hover from the Tab-selected header");
	FocusModalNextPrevChild(&root, true);
	Check(second.hasFocus() && !first.isOver(),
		"Tab continues from j through nested items and skips disabled items");
	Check(send(Qt::Key_K, u"k"_q) && first.hasFocus() && !second.isOver(),
		"menu k continues from Tab without a second selection");
	Check(send(Qt::Key_Tab, {}, Qt::ShiftModifier) && header.hasFocus(),
		"Shift Tab continues from k in the same focus cycle");
	Check(send(Qt::Key_Up) && second.hasFocus(),
		"menu up wraps backwards and skips hidden items");
	Check(send(Qt::Key_Down) && header.hasFocus(),
		"menu down wraps forwards to the header");
	Check(send(0x041E, u"\u043E"_q) && first.hasFocus()
		&& send(0x041B, u"\u043B"_q) && header.hasFocus(),
		"Cyrillic j and k share the menu focus cycle");
	Check(!send(Qt::Key_J, u"j"_q, Qt::ControlModifier) && header.hasFocus(),
		"modified j is not stolen by menu navigation");
	auto configured = QKeyEvent(QEvent::KeyPress, Qt::Key_N, Qt::NoModifier, u"n"_q);
	Check(navigation->handleMenuNavigation(&configured, Qt::Key_Down)
		&& first.hasFocus(), "configured navigation uses the same focus cycle");
	input.show();
	navigation->focusTarget(&input);
	Check(!send(Qt::Key_J, u"j"_q) && !send(Qt::Key_K, u"k"_q)
		&& input.hasFocus(), "menu j and k pass through while editing text");
	auto typed = QKeyEvent(QEvent::KeyPress, Qt::Key_J, Qt::NoModifier, u"j"_q);
	QApplication::sendEvent(&input, &typed);
	Check(input.text() == u"j"_q, "input receives the j character");
	input.hide();
	const auto nestedNavigation = KeyboardNavigation::Get(&group);
	nestedNavigation->focusTarget(&second);
	Check(nestedNavigation->handleMenuNavigation(&configured, Qt::Key_Down)
		&& second.hasFocus(), "nested menu navigation cannot select background controls");
}

void TestKeyboardStickerFramePainting() {
	using namespace Core::VimKeymap;
	const auto cell = QRect(12, 12, 64, 64);
	const auto palette = style::main_palette::save();
	for (const auto dpr : { 1, 2, 3 }) {
		for (const auto background : { QColor(15, 18, 23), QColor(255, 255, 255) }) {
			const auto dark = background.lightness() < 128;
			auto theme = style::palette();
			theme.setColor(QLatin1String("windowBg"), background);
			theme.setColor(QLatin1String("windowFg"), dark ? Qt::white : Qt::black);
			theme.finalize();
			style::main_palette::apply(theme);
			auto image = QImage(QSize(88, 88) * dpr, QImage::Format_ARGB32_Premultiplied);
			image.setDevicePixelRatio(dpr);
			image.fill(background);
			{
				auto painter = QPainter(&image);
				PaintKeyboardStickerFrame(painter, cell);
			}
			auto contained = true;
			auto contrastPixels = 0;
			for (auto y = 0; y != image.height(); ++y) {
				for (auto x = 0; x != image.width(); ++x) {
					const auto pixel = image.pixelColor(x, y);
					if (pixel != background) {
						contained &= cell.contains(QPoint(x / dpr, y / dpr));
						contrastPixels += std::abs(pixel.lightness() - background.lightness()) > 100;
					}
				}
			}
			Check(contained && contrastPixels > 100 * dpr * dpr,
				"sticker frame is high-contrast on dark and light imagery at each DPR");
			Check(image.pixelColor(cell.center() * dpr) == background,
				"sticker frame leaves the sticker interior unobscured");
			const auto output = qEnvironmentVariable("VIM_KEYMAP_FOCUS_SNAPSHOTS");
			if (!output.isEmpty() && dpr == 2) {
				Check(image.save(output + (dark ? u"/sticker-dark.png"_q : u"/sticker-light.png"_q)),
					"sticker focus rendering snapshot is saved");
			}
		}
	}
	Check(style::main_palette::load(palette), "sticker test restores the UI palette");
}

void TestMessageCursorPainting() {
	using namespace Core::VimKeymap;
	const auto text = Ui::Text::String(st::defaultTextStyle, u"Wi  m"_q);
	const auto bounds = QRect(0, 0, 200, 40);
	auto request = Ui::Text::StateRequest();
	request.flags = Ui::Text::StateRequest::Flag::LookupSymbol;
	auto cells = std::vector<QRect>();
	for (auto symbol = 0; symbol != 5; ++symbol) {
		const auto matches = [&](QPoint point) {
			const auto state = text.getState(point, bounds.width(), request);
			return state.uponSymbol && state.symbol == symbol;
		};
		auto hit = QPoint(-1, -1);
		for (auto y = 0; y != bounds.height() && hit.x() < 0; ++y) {
			for (auto x = 0; x != bounds.width(); ++x) {
				if (matches({ x, y })) {
					hit = { x, y };
					break;
				}
			}
		}
		const auto cell = LinkHintTargetRect(hit, bounds, matches);
		Check(!cell.isEmpty(), "letters and consecutive spaces have distinct cursor cells");
		cells.push_back(cell);
		for (const auto dpr : { 1, 2, 3 }) {
			for (const auto dark : { false, true }) {
				auto image = QImage(bounds.size() * dpr, QImage::Format_ARGB32_Premultiplied);
				image.setDevicePixelRatio(dpr);
				image.fill(dark ? QColor(16, 20, 24) : QColor(245, 245, 245));
				{
					auto painter = QPainter(&image);
					painter.setPen(dark ? Qt::white : Qt::black);
					text.draw(painter, { .availableWidth = bounds.width() });
				}
				const auto original = image.copy();
				{
					auto painter = QPainter(&image);
					PaintMessageCursor(painter, cell);
					Check(painter.compositionMode() == QPainter::CompositionMode_SourceOver,
						"cursor painting restores the message painter state");
				}
				auto inverted = true;
				for (auto y = 0; y != image.height(); ++y) {
					for (auto x = 0; x != image.width(); ++x) {
						const auto before = original.pixelColor(x, y);
						const auto expected = cell.contains(QPoint(x / dpr, y / dpr))
							? QColor(255 - before.red(), 255 - before.green(), 255 - before.blue())
							: before;
						inverted &= image.pixelColor(x, y) == expected;
					}
				}
				Check(inverted, "cursor inverts the entire glyph or space without painting neighbors");
				const auto output = qEnvironmentVariable("VIM_KEYMAP_FOCUS_SNAPSHOTS");
				if (!output.isEmpty() && dpr == 2 && dark) {
					Check(image.save(output + u"/cursor-%1.png"_q.arg(symbol)),
						"message cursor rendering snapshot is saved");
				}
			}
		}
	}
	Check(cells[0].width() > cells[1].width(), "block cursor follows proportional character widths");
	for (auto i = 1; i != cells.size(); ++i) {
		Check(!cells[i - 1].intersects(cells[i])
			&& cells[i - 1].right() + 1 == cells[i].left()
			&& cells[i - 1].top() == cells[i].top()
			&& cells[i - 1].height() == cells[i].height(),
			"cursor cells stay adjacent and aligned across letters and spaces");
	}
}

void TestKeyboardFocusScrollingAndPainting() {
	using namespace Core::VimKeymap;
	auto root = QWidget();
	root.setAttribute(Qt::WA_DontShowOnScreen);
	root.resize(400, 240);
	auto scroll = QScrollArea(&root);
	scroll.setGeometry(0, 0, 350, 200);
	const auto content = new QWidget();
	content->resize(300, 800);
	scroll.setWidget(content);
	auto first = Ui::AbstractButton(content);
	first.setGeometry(20, 20, 100, 40);
	auto last = Ui::AbstractButton(content);
	last.setGeometry(20, 700, 100, 40);
	root.show();
	QApplication::setActiveWindow(&root);
	const auto navigation = KeyboardNavigation::Get(&root);
	navigation->focusTarget(&last);
	Check(last.hasFocus() && scroll.verticalScrollBar()->value() > 0,
		"Tab target below viewport scrolls the containing area into view");
	for (const auto dpr : { 1, 2, 3 }) {
		auto image = QImage(root.size() * dpr, QImage::Format_ARGB32_Premultiplied);
		image.setDevicePixelRatio(dpr);
		image.fill(Qt::transparent);
		{
			auto painter = QPainter(&image);
			navigation->render(&painter, {}, {}, QWidget::RenderFlags());
		}
		const auto target = QRect(last.mapTo(&root, QPoint()), last.size());
		auto ink = 0;
		auto contained = true;
		for (auto y = 0; y != image.height(); ++y) {
			for (auto x = 0; x != image.width(); ++x) {
				if (image.pixelColor(x, y).alpha()) {
					++ink;
					contained &= target.contains(QPoint(x / dpr, y / dpr));
				}
			}
		}
		Check(ink > 0 && contained,
			"keyboard focus outline is visible and follows scrolled target at each DPR");
	}
	const auto previous = scroll.verticalScrollBar()->value();
	navigation->scroll(-80, false, 0);
	Check(scroll.verticalScrollBar()->value() == previous - 80,
		"scoped scrolling moves the focused scroll area");
	navigation->scroll(10000, false, 0);
	Check(scroll.verticalScrollBar()->value() == scroll.verticalScrollBar()->maximum(),
		"scoped scrolling clamps at the end without moving a background");
}

void TestCustomKeyboardFocusFrame() {
	using namespace Core::VimKeymap;
	auto root = QWidget();
	root.setAttribute(Qt::WA_DontShowOnScreen);
	root.resize(400, 400);
	auto grid = QWidget(&root);
	grid.setGeometry(20, 20, 360, 280);
	grid.setFocusPolicy(Qt::StrongFocus);
	SetKeyboardFocusTargetEnabled(&grid, false);
	auto proxy = QWidget(&root);
	proxy.setFocusPolicy(Qt::StrongFocus);
	proxy.setFocusProxy(&grid);
	auto button = Ui::AbstractButton(&root);
	button.setGeometry(20, 320, 360, 40);
	root.show();
	QApplication::setActiveWindow(&root);
	const auto navigation = KeyboardNavigation::Get(&root);
	Check(KeyboardFocusTargets(&root) == std::vector<QPointer<QWidget>>{
		&button }, "sticker grid is excluded from Tab and focus hints");
	navigation->focusTarget(&button);
	navigation->focusNext(false);
	Check(button.hasFocus(), "Shift Tab skips excluded grid and wraps to the button");
	grid.setFocus(Qt::OtherFocusReason);
	for (const auto dpr : { 1, 2, 3 }) {
		auto image = QImage(root.size() * dpr, QImage::Format_ARGB32_Premultiplied);
		image.setDevicePixelRatio(dpr);
		image.fill(Qt::transparent);
		{
			auto painter = QPainter(&image);
			navigation->render(&painter, {}, {}, QWidget::RenderFlags());
		}
		auto ink = 0;
		for (auto y = 0; y != image.height(); ++y) {
			for (auto x = 0; x != image.width(); ++x) {
				ink += image.pixelColor(x, y).alpha() != 0;
			}
		}
		Check(!ink, "self-painted grid has no duplicate container frame at each DPR");
	}
	navigation->focusNext(true);
	Check(button.hasFocus(), "Tab leaves the programmatically focused grid for a button");
	navigation->showHints([](int index, int) {
		return QString(QChar('a' + index));
	}, QFont(u"Menlo"_q, 13), { 7, 3 }, 3);
	auto choose = QKeyEvent(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, u"a"_q);
	Check(navigation->handleHintKey(&choose, u"a"_q)
		&& button.hasFocus() && !navigation->hasHints(),
		"focus hint skips the excluded grid");
	navigation->focusTarget(&grid);
	Check(button.hasFocus(), "excluded grid cannot be focused by the navigation controller");
}

void TestInterfaceHistory() {
#ifdef Q_OS_MAC
	const auto control = Qt::MetaModifier;
	const auto command = Qt::ControlModifier;
#else // Q_OS_MAC
	const auto control = Qt::ControlModifier;
	const auto command = Qt::MetaModifier;
#endif // !Q_OS_MAC
	for (const auto &[key, text, expected] : {
		std::tuple(Qt::Key_H, u"h"_q, -1),
		std::tuple(Qt::Key_L, u"l"_q, 1),
		std::tuple(Qt::Key_unknown, u"\u0440"_q, -1),
		std::tuple(Qt::Key_unknown, u"\u0434"_q, 1) }) {
		auto event = QKeyEvent(QEvent::KeyPress, key, control, text);
		Check(Core::VimKeymap::Bindings::InterfaceHistoryDelta(&event) == expected,
			"physical Ctrl H/L navigates history in Latin and Cyrillic layouts");
		for (const auto modifiers : {
			Qt::KeyboardModifiers(command), Qt::KeyboardModifiers(Qt::NoModifier),
			control | Qt::ShiftModifier, control | Qt::AltModifier }) {
			auto ignored = QKeyEvent(QEvent::KeyPress, key, modifiers, text);
			Check(!Core::VimKeymap::Bindings::InterfaceHistoryDelta(&ignored),
				"history ignores command, unmodified, shift and alt keys");
		}
	}
	auto back = std::vector<std::unique_ptr<int>>();
	auto forward = std::vector<std::unique_ptr<int>>();
	auto current = 3;
	back.push_back(std::make_unique<int>(1));
	back.push_back(std::make_unique<int>(2));
	const auto save = [&] { return std::make_unique<int>(current); };
	const auto valid = [](const auto &entry) { return entry != nullptr; };
	for (const auto expected : { 2, 1 }) {
		const auto entry = Info::TakeHistoryStep(back, forward, save, valid);
		Check(entry && **entry == expected, "back restores the previous memento in order");
		current = **entry;
	}
	auto saved = 0;
	Check(!Info::TakeHistoryStep(back, forward, [&] {
		++saved;
		return save();
	}, valid) && !saved && forward.size() == 2,
		"empty back history does not save or mutate current state");
	for (const auto expected : { 2, 3 }) {
		const auto entry = Info::TakeHistoryStep(forward, back, save, valid);
		Check(entry && **entry == expected, "forward restores mementos in reverse back order");
		current = **entry;
	}
	Check(forward.empty() && back.size() == 2 && current == 3,
		"back-forward round trip preserves the original history");
	back.push_back(nullptr);
	const auto restored = Info::TakeHistoryStep(back, forward, save, valid);
	Check(restored && **restored == 2,
		"history skips removed destinations before restoring a valid page");
}

void TestKeyboardSliderEditing() {
	using namespace Core::VimKeymap;
	auto root = QWidget();
	root.setAttribute(Qt::WA_DontShowOnScreen);
	root.resize(400, 300);
	auto slider = Ui::MediaSlider(&root, st::defaultContinuousSlider);
	slider.setGeometry(20, 30, 300, 24);
	slider.setValue(0.5);
	auto native = QSlider(Qt::Horizontal, &root);
	native.setGeometry(20, 90, 300, 24);
	native.setRange(0, 10);
	native.setValue(5);
	auto nativeFinished = 0;
	QObject::connect(&native, &QSlider::sliderReleased, &root, [&] { ++nativeFinished; });
	auto button = Ui::AbstractButton(&root);
	button.setGeometry(20, 150, 100, 40);
	root.show();
	QApplication::setActiveWindow(&root);
	const auto navigation = KeyboardNavigation::Get(&root);
	const auto key = [&](int key, const QString &text = {},
			Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool repeat = false) {
		auto event = QKeyEvent(QEvent::KeyPress, key, modifiers, text, repeat);
		const auto handled = HandleKeyboardControlKey(&root, &event);
		if (key == Qt::Key_Return || key == Qt::Key_Tab || key == Qt::Key_Escape) {
			DrainMainQueue();
		}
		return handled;
	};
	auto progress = 0;
	auto finished = 0;
	slider.setChangeProgressCallback([&](float64) { ++progress; });
	slider.setChangeFinishedCallback([&](float64) { ++finished; });
	navigation->focusTarget(&slider);
	navigation->showHints([](int i, int) { return QString(QChar('a' + i)); },
		QFont(u"Menlo"_q, 12), QSize(4, 2), 3);
	Check(!key(Qt::Key_Return) && !key(Qt::Key_Tab) && !key(Qt::Key_H, u"h"_q)
		&& navigation->hasHints() && slider.hasFocus() && progress == 0,
		"active hints take precedence over focused slider controls");
	navigation->clearHints();
	Check(!key(Qt::Key_H, u"h"_q) && slider.value() == 0.5,
		"h/l cannot edit a slider before Enter");
	Check(key(Qt::Key_Return) && slider.value() == 0.5,
		"Enter starts slider editing without changing its value");
	Check(key(Qt::Key_H, u"h"_q) && slider.value() < 0.5
		&& progress == 1 && finished == 0,
		"h previews its value without opening the scale confirmation");
	Check(key(Qt::Key_unknown, u"\u0434"_q) && slider.value() == 0.5,
		"Cyrillic l increases the edited slider");
	Check(key(Qt::Key_Return, {}, Qt::NoModifier, true)
		&& key(Qt::Key_L, u"l"_q), "held Enter does not toggle editing off");
	Check(key(Qt::Key_Escape) && !key(Qt::Key_H, u"h"_q)
		&& root.isVisible() && slider.value() == 0.5 && finished == 1,
		"Escape restores the original slider value without closing its scope");
	Check(key(Qt::Key_Return) && key(Qt::Key_L, u"l"_q)
		&& key(Qt::Key_L, u"l"_q) && finished == 1,
		"multiple slider steps do not confirm until editing finishes");
	Check(key(Qt::Key_Return) && finished == 2 && slider.value() > 0.5,
		"Enter commits the edited slider value exactly once");
	Check(key(Qt::Key_Return) && key(Qt::Key_H, u"h"_q) && finished == 2,
		"the next editing session starts without an extra confirmation");
	Check(key(Qt::Key_Tab) && native.hasFocus(),
		"Tab commits and moves from a Telegram slider to the next control");
	Check(finished == 3, "leaving a changed slider confirms it once");
	Check(!key(Qt::Key_H, u"h"_q) && native.value() == 5,
		"editing mode is not inherited by the next slider");
	Check(key(Qt::Key_Return) && key(Qt::Key_L, u"l"_q) && native.value() == 6
		&& nativeFinished == 0,
		"native Qt sliders use the same Enter and h/l workflow");
	Check(key(Qt::Key_Return) && !key(Qt::Key_H, u"h"_q) && nativeFinished == 1,
		"second Enter finishes slider editing");
	Check(key(Qt::Key_Return) && key(Qt::Key_H, u"h"_q) && native.value() == 5
		&& key(Qt::Key_Escape) && native.value() == 6 && nativeFinished == 2,
		"Escape cancels native slider editing and restores its starting value");
	navigation->focusTarget(&slider);
	slider.setAdjustCallback([](float64 value) { return std::round(value * 4) / 4; });
	slider.setValue(0.5);
	Check(key(Qt::Key_Return) && key(Qt::Key_L, u"l"_q) && slider.value() == 0.75,
		"discrete settings sliders advance to the next allowed step");
	slider.setValue(1.);
	const auto count = finished;
	Check(key(Qt::Key_L, u"l"_q) && slider.value() == 1. && finished == count,
		"slider limits do not emit duplicate changes");
	slider.setDisabled(true);
	Check(!key(Qt::Key_H, u"h"_q) && slider.value() == 1.,
		"disabled slider cannot be changed from the keyboard");
	const auto targets = KeyboardFocusTargets(&root);
	Check(std::find(targets.begin(), targets.end(), &slider) == targets.end(),
		"disabled Telegram slider is not a focus target");
	slider.setDisabled(false);
	navigation->focusTarget(&button);
	navigation->focusTarget(&slider);
	Check(!key(Qt::Key_H, u"h"_q), "focus changes terminate slider editing");
	Check(key(Qt::Key_Return) && key(Qt::Key_Tab, {}, Qt::ShiftModifier)
		&& button.hasFocus(), "Shift Tab exits editing and wraps backwards");
	auto disposable = std::make_unique<Ui::MediaSlider>(&root, st::defaultContinuousSlider);
	disposable->setGeometry(20, 210, 300, 24);
	disposable->show();
	disposable->setValue(0.5);
	disposable->setChangeProgressCallback([&](float64) { disposable.reset(); });
	navigation->focusTarget(disposable.get());
	Check(key(Qt::Key_Return) && key(Qt::Key_L, u"l"_q) && !disposable,
		"slider callbacks may destroy the focused control safely");
}

void TestKeyboardFocusShapes() {
	using namespace Core::VimKeymap;
	auto root = QWidget();
	root.setAttribute(Qt::WA_DontShowOnScreen);
	root.resize(420, 280);
	auto icon = Ui::IconButton(&root, st::menuToggle);
	icon.setGeometry(20, 20, 40, 56);
	auto swatch = Ui::AbstractButton(&root);
	swatch.setGeometry(100, 30, 24, 24);
	SetKeyboardFocusCircle(&swatch);
	auto button = Ui::RoundButton(&root, rpl::single(u"Add stickers"_q), st::defaultActiveButton);
	button.setGeometry(20, 100, 360, 42);
	button.setFullRadius(true);
	auto slider = Ui::MediaSlider(&root, st::defaultContinuousSlider);
	slider.setGeometry(20, 190, 360, 24);
	slider.setAlwaysDisplayMarker(true);
	slider.setValue(0.5);
	root.show();
	QApplication::setActiveWindow(&root);
	const auto navigation = KeyboardNavigation::Get(&root);
	for (const auto target : std::vector<QWidget*>{ &icon, &swatch, &button, &slider }) {
		navigation->focusTarget(target);
		for (const auto dpr : { 1, 2, 3 }) {
			auto image = QImage(root.size() * dpr, QImage::Format_ARGB32_Premultiplied);
			image.setDevicePixelRatio(dpr);
			image.fill(Qt::transparent);
			{
				auto painter = QPainter(&image);
				navigation->render(&painter, {}, {}, QWidget::RenderFlags());
			}
			auto bounds = QRect();
			for (auto y = 0; y != image.height(); ++y) {
				for (auto x = 0; x != image.width(); ++x) {
					if (image.pixelColor(x, y).alpha()) {
						bounds |= QRect(x, y, 1, 1);
					}
				}
			}
			Check(!bounds.isEmpty(), "each control has a visible focus indicator at every DPR");
			Check(!image.pixelColor(bounds.topLeft()).alpha()
				&& !image.pixelColor(bounds.bottomRight()).alpha(),
				"focus indicator corners are rounded rather than rectangular");
			if (target == &slider) {
				const auto center = slider.mapTo(&root, slider.keyboardFocusPoint()) * dpr;
				Check(bounds.width() <= st::vimFocusSliderSize * dpr
					&& (bounds.center() - center).manhattanLength() <= 2,
					"slider focus surrounds its handle instead of the whole track");
			} else if (target == &icon || target == &swatch) {
				Check(std::abs(bounds.width() - bounds.height()) <= 1,
					"icon and swatch focus indicators are circular");
			}
			const auto output = qEnvironmentVariable("VIM_KEYMAP_FOCUS_SNAPSHOTS");
			if (!output.isEmpty() && dpr == 2) {
				const auto name = target == &icon ? u"icon"_q
					: target == &swatch ? u"swatch"_q
					: target == &button ? u"button"_q : u"slider"_q;
				auto snapshot = QImage(image.size(), QImage::Format_ARGB32_Premultiplied);
				snapshot.setDevicePixelRatio(dpr);
				snapshot.fill(QColor(24, 33, 41));
				{
					auto painter = QPainter(&snapshot);
					painter.drawImage(QPoint(), image);
				}
				Check(snapshot.save(output + '/' + name + u".png"_q),
					"focus rendering snapshot is saved");
			}
		}
	}
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

void TestPopupFocusHints() {
	using namespace Core::VimKeymap;
	auto owner = QWidget();
	owner.setAttribute(Qt::WA_DontShowOnScreen);
	owner.resize(400, 400);
	owner.show();
	auto menu = Ui::PopupMenu(&owner, st::defaultPopupMenu);
	menu.setAttribute(Qt::WA_DontShowOnScreen);
	menu.deleteOnHide(false);
	menu.addAction(u"First"_q, [] {});
	auto activated = 0;
	const auto second = menu.addAction(u"Second"_q, [&] { ++activated; });
	const auto third = menu.addAction(u"Third"_q, [] {});
	menu.popup(owner.mapToGlobal(QPoint(20, 20)));
	{
		auto loop = QEventLoop();
		QTimer::singleShot(st::defaultPopupMenu.showDuration + 50, &loop, &QEventLoop::quit);
		loop.exec();
	}
	const auto rendered = menu.grab();
	Check(!rendered.isNull(), "popup completes its hidden paint pass before navigation");
	const auto selected = [&] {
		const auto item = menu.menu()->findSelectedAction();
		return item ? item->action().get() : nullptr;
	};
	auto tab = QKeyEvent(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
	QApplication::sendEvent(&menu, &tab);
	QApplication::sendEvent(&menu, &tab);
	Check(selected() == second, "native menu starts on second item for hint navigation");
	const auto navigation = KeyboardNavigation::Get(&menu);
	const auto hints = [&] {
		navigation->showHints([](int index, int) {
			return QString(QChar('a' + index));
		}, QFont(u"Menlo"_q, 13), { 7, 3 }, 3);
	};
	hints();
	Check(navigation->hasHints(), "popup exposes its actions as focus hints");
	Check(!navigation->handleHintKey(&tab, {}) && !navigation->hasHints(),
		"Tab during popup hints defers to native menu navigation");
	QApplication::sendEvent(&menu, &tab);
	Check(selected() == third, "popup Tab retains selection after cancelling hints");
	hints();
	auto letter = QKeyEvent(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, u"b"_q);
	Check(navigation->handleHintKey(&letter, u"b"_q)
		&& selected() == second && !activated,
		"hint focuses native menu action without triggering it");
	auto enter = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(&menu, &enter);
	QApplication::processEvents();
	Check(activated == 1, "Enter triggers the hinted native menu action once");
	menu.hideMenu(true);
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

#include "tests/vim_config_tests.h"
#include "tests/vim_security_tests.h"

} // namespace

int main(int argc, char *argv[]) {
#ifndef Q_OS_MAC
	qputenv("QT_QPA_PLATFORM", "offscreen");
#endif // !Q_OS_MAC
	auto application = QApplication(argc, argv);
	auto baseIntegration = TestBaseIntegration(argc, argv);
	base::Integration::Set(&baseIntegration);
	auto integration = TestIntegration();
	Ui::Integration::Set(&integration);
	style::StartManager(100);
	Ui::Animations::Manager::SetScheduleWithInvokeQueued(true);
	auto animations = Ui::Animations::Manager();

	TestVimKeymapNavigationBindings();
	TestVimKeymapActionBindings();
	TestVimKeymapCommandBindings();
	TestVimKeymapTransientUiKeys();
	TestMessageTextNavigation();
	TestVimKeymapCursorGeometry();
	TestVimKeymapPickerNavigation();
	TestVimKeymapStickerSetNavigation();
	TestVimKeymapMediaNavigation();
	TestAbstractButtonKeyboardActivation();
	TestHintBadgeLayoutAndPainting();
	TestHintTargetAnchoring();
	TestModalTabCycle();
	TestKeyboardLayerStack();
	TestNestedKeyboardNavigation();
	TestKeyboardMenuNavigation();
	TestMediaPlaybackAndShareBindings();
	TestShortestHintLabels();
	TestProfileKeyboardNavigation();
	TestGlobalPlayerFocusHints();
	TestShareKeyboardFocusCycle();
	TestKeyboardStickerFramePainting();
	TestMessageCursorPainting();
	TestKeyboardFocusScrollingAndPainting();
	TestCustomKeyboardFocusFrame();
	TestInterfaceHistory();
	TestKeyboardSliderEditing();
	TestKeyboardFocusShapes();
	TestVimFocusLabels();
	TestPopupMenuKeyboardCycle();
	TestPopupFocusHints();
	TestPopupHandlerScope();
	TestVimConfig();
	TestVimConfigEditor();
	TestVimConfigDocuments();
	TestVimKeyLogPrivacy();
	TestLocalSocketSecurity();

	std::cout << (TotalChecks - FailedChecks) << "/" << TotalChecks
		<< " checks passed." << std::endl;
	return FailedChecks ? 1 : 0;
}
