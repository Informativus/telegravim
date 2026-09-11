/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#ifdef _DEBUG

#include "base/unixtime.h"
#include "core/application.h"
#include "core/vim_keymap.h"
#include "core/vim_keymap_bindings.h"
#include "core/vim_keymap_options.h"
#include "core/vim_keymap_widgets.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/view/history_view_element.h"
#include "history/history.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mtproto/facade.h"
#include "test/test_capture.h"
#include "test/test_log.h"
#include "test/test_runner.h"
#include "test/test_widgets.h"
#include "ui/abstract_button.h"
#include "window/main_window.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QMimeData>
#include <QtGui/QClipboard>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTextEdit>

namespace Test {
namespace {

MTPUser FixtureUser(int id, bool self) {
	using Flag = MTPDuser::Flag;
	return MTP_user(
		MTP_flags(Flag::f_first_name | (self ? Flag::f_self : Flag::f_contact)),
		MTP_long(id),
		MTPlong(),
		MTP_string("Clipboard fixture"),
		MTPstring(),
		MTPstring(),
		MTPstring(),
		MTPUserProfilePhoto(),
		MTPUserStatus(),
		MTPint(),
		MTPVector<MTPRestrictionReason>(),
		MTPstring(),
		MTPstring(),
		MTPEmojiStatus(),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPint(),
		MTPlong(),
		MTPlong(),
		MTPlong());
}

} // namespace

void SetupScenario(not_null<Runner*> runner) {
	runner->waitEvent(u"launch_finished"_q);
	runner->add({
		.name = u"chat clipboard commands through the application dispatcher"_q,
		.run = [] {
			using namespace Core::VimKeymap;
			const auto active = Core::App().activePrimaryWindow();
			Check(active != nullptr, u"primary window exists without signing in"_q);
			if (!active) {
				return;
			}
			const auto window = active->widget();
			ForceWindowActive(window);
			auto chat = QWidget(window);
			chat.setGeometry(0, 0, 400, 300);
			chat.show();
			auto composer = QTextEdit(&chat);
			composer.setGeometry(0, 200, 200, 60);
			composer.show();
			auto message = QTextEdit(&chat);
			message.setPlainText(u"Clipboard regression fixture"_q);
			message.setReadOnly(true);
			message.setGeometry(0, 0, 200, 150);
			message.show();
			auto pane = QWidget(window);
			pane.setGeometry(410, 0, 200, 300);
			pane.show();
			RegisterGlobalFocusRoot(&pane, KeyboardFocusRootKind::Pane);
			auto control = Ui::AbstractButton(&pane);
			control.setGeometry(0, 0, 160, 40);
			control.show();
			auto input = QLineEdit(&pane);
			input.setGeometry(0, 60, 160, 40);
			input.show();

			const auto clipboard = QApplication::clipboard();
			auto saved = std::make_unique<QMimeData>();
			for (const auto &format : clipboard->mimeData()->formats()) {
				saved->setData(format, clipboard->mimeData()->data(format));
			}
			const auto restore = gsl::finally([&] {
				clipboard->setMimeData(saved.release());
				UnregisterPreLayerKeyHandler(&chat);
				UnregisterActionHandler(&chat);
			});
			auto pending = std::optional<Action>();
			auto calls = 0;
			RegisterActionHandler(&chat, [&](Action action) {
				++calls;
				pending = action;
				composer.setFocus();
				return true;
			});
			RegisterPreLayerKeyHandler(&chat, [&](not_null<QKeyEvent*> e) {
				if (pending && Bindings::HintCharacter(e) == u"a"_q) {
					if (e->type() == QEvent::KeyPress) {
						message.selectAll();
						if (*pending == Action::CopyMessage) {
							message.copy();
						}
						pending.reset();
					}
					return true;
				} else if (message.textCursor().hasSelection()
					&& (Bindings::IsTextYank(e) || e->matches(QKeySequence::Copy))) {
					if (e->type() == QEvent::KeyPress) {
						message.copy();
					}
					return true;
				}
				return false;
			}, true);
			VimKeymapOption.set(true);
			SetNormalMode(true);
			const auto press = [&](int key, Qt::KeyboardModifiers modifiers, QString text) {
				ForceWindowActive(window);
				auto event = QKeyEvent(QEvent::KeyPress, key, modifiers, text);
				return HandleApplicationKeyPress(QApplication::focusWidget(), &event);
			};
			for (const auto fromPane : { false, true }) {
				for (const auto russian : { false, true }) {
					const auto origin = fromPane ? static_cast<QWidget*>(&control) : &composer;
					origin->setFocus();
					message.moveCursor(QTextCursor::Start);
					clipboard->setText(u"unchanged"_q);
					Check(press(Qt::Key_Y, Qt::NoModifier, russian ? u"н"_q : u"y"_q)
						&& pending == Action::CopyMessage, u"y reaches chat copy hints from chat and pane"_q);
					press(Qt::Key_A, Qt::NoModifier, u"a"_q);
					Check(clipboard->text() == message.toPlainText(),
						u"copy hint completes at the clipboard"_q);
					message.moveCursor(QTextCursor::Start);
					origin->setFocus();
					auto override = QKeyEvent(QEvent::ShortcutOverride, Qt::Key_V,
						Qt::ControlModifier | Qt::ShiftModifier, russian ? u"М"_q : u"V"_q);
					const auto before = calls;
					Check(HandleApplicationShortcutOverride(&override) && calls == before,
						u"text selection claims the Qt shortcut without executing during the probe"_q);
					Check(press(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier, override.text())
						&& pending == Action::SelectMessageText,
						u"selection shortcut reaches chat from both panes"_q);
					press(Qt::Key_A, Qt::NoModifier, u"a"_q);
					for (const auto yank : { false, true }) {
						clipboard->setText(u"unchanged"_q);
						press(yank ? Qt::Key_Y : Qt::Key_C,
							yank ? Qt::NoModifier : Qt::ControlModifier, yank ? u"y"_q : u"c"_q);
						Check(clipboard->text() == message.toPlainText(),
							u"both y and system Copy reach the selected message"_q);
					}
					Check(pane.isVisible(), u"clipboard commands keep the right pane open"_q);
				}
			}
			message.moveCursor(QTextCursor::Start);
			input.setFocus();
			const auto before = calls;
			Check(!press(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier, u"V"_q)
				&& calls == before, u"pane text input retains its own shortcuts"_q);
			input.setText(u"Pane input clipboard fixture"_q);
			input.selectAll();
			PressKey(&input, Qt::Key_C, Qt::ControlModifier);
			Check(clipboard->text() == input.text(),
				u"system Copy retains the selected text in a pane input"_q);
			composer.setFocus();
			SetNormalMode(false);
			composer.setPlainText(u"Composer clipboard fixture"_q);
			composer.selectAll();
			PressKey(&composer, Qt::Key_C, Qt::ControlModifier);
			Check(clipboard->text() == composer.toPlainText(),
				u"system Copy retains the composer selection in input mode"_q);
			Check(!press(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier, u"V"_q)
				&& calls == before, u"input mode does not start message selection"_q);
		},
	});
	auto item = std::make_shared<HistoryItem*>(nullptr);
	runner->add({
		.name = u"create an unsent message in a disposable synthetic session"_q,
		.run = [=] {
			auto &account = Core::App().domain().active();
			Check(!account.sessionExists(), u"fixture never uses a signed-in account"_q);
			if (account.sessionExists()) {
				return;
			}
			MTP::details::pause();
			account.createSession(FixtureUser(1, true));
			auto &session = account.session();
			const auto peer = session.data().processUser(FixtureUser(2, false));
			const auto history = session.data().history(peer);
			history->clearFolder();
			*item = history->addNewLocalMessage({
				.id = session.data().nextLocalMessageId(),
				.flags = MessageFlag::Local | MessageFlag::Outgoing | MessageFlag::BeingSent,
				.from = session.user()->id,
				.date = base::unixtime::now(),
			}, { u"Clipboard regression fixture"_q }, MTP_messageMediaEmpty());
			(*item)->setRealId(1);
			const auto active = Core::App().activePrimaryWindow();
			active->widget()->resize(1100, 700);
			active->sessionController()->showPeerHistory(history,
				Window::SectionShow::Way::ClearStack, ShowAtTheEndMsgId);
		},
	});
	const auto resolve = [=]() -> QWidget* {
		const auto active = Core::App().activePrimaryWindow();
		if (!active || !*item) {
			return nullptr;
		}
		ForceWindowActive(active->widget());
		for (const auto inner : FindVisible<HistoryInner>(active->widget())) {
			if (inner->viewByItem(*item)) {
				return inner;
			}
		}
		return nullptr;
	};
	runner->actOnWidget(u"copy and select a real HistoryInner message"_q,
		resolve,
		[=](QWidget *widget) {
			using namespace Core::VimKeymap;
			const auto inner = static_cast<HistoryInner*>(widget);
			const auto window = inner->window();
			ForceWindowActive(window);
			VimKeymapHintAlphabetOption.set(u"abc"_q);
			SetNormalMode(true);
			const auto clipboard = QApplication::clipboard();
			auto saved = std::make_unique<QMimeData>();
			for (const auto &format : clipboard->mimeData()->formats()) {
				saved->setData(format, clipboard->mimeData()->data(format));
			}
			const auto restore = gsl::finally([&] { clipboard->setMimeData(saved.release()); });
			const auto press = [&](int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
				PressKey(QApplication::focusWidget(), key, modifiers);
			};
			const auto focus = [&](bool right) {
				inner->setFocus();
				if (right) {
#ifdef Q_OS_MAC
					press(Qt::Key_A, Qt::MetaModifier);
#else // Q_OS_MAC
					press(Qt::Key_A, Qt::ControlModifier);
#endif // Q_OS_MAC
					press(Qt::Key_L);
					const auto focused = QApplication::focusWidget();
					Check(IsKeyboardPane(GlobalFocusRoot(focused)),
						u"Ctrl+A then l enters the profile pane before copying"_q,
						u"focus=%1 log=%2"_q
							.arg(focused ? WidgetDescription(focused) : u"none"_q)
							.arg(RecentKeyLogText().right(500)));
				}
			};
			for (const auto fromPane : { false, true }) {
				focus(fromPane);
				clipboard->setText(u"unchanged"_q);
				press(Qt::Key_Y);
				press(Qt::Key_A);
				Check(clipboard->text() == u"Clipboard regression fixture"_q,
					u"real message y plus hint copies the full text"_q,
					u"fromPane=%1"_q.arg(fromPane));
				for (const auto yank : { false, true }) {
					focus(fromPane);
					press(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
					press(Qt::Key_A);
					press(Qt::Key_V);
					press(Qt::Key_G, Qt::ShiftModifier);
					clipboard->setText(u"unchanged"_q);
					press(yank ? Qt::Key_Y : Qt::Key_C, yank ? Qt::NoModifier : Qt::ControlModifier);
					Check(clipboard->text() == u"Clipboard regression fixture"_q,
						u"real selected message copies through y and system Copy"_q,
						u"fromPane=%1 yank=%2"_q.arg(fromPane).arg(yank));
					press(Qt::Key_Escape);
					press(Qt::Key_Escape);
					SetNormalMode(true);
				}
			}
		});
	runner->actOnWidget(u"copy mouse and message selections after layout settles"_q,
		resolve,
		[=](QWidget *widget) {
			using namespace Core::VimKeymap;
			const auto inner = static_cast<HistoryInner*>(widget);
			ForceWindowActive(inner->window());
			SetNormalMode(true);
			const auto clipboard = QApplication::clipboard();
			auto saved = std::make_unique<QMimeData>();
			for (const auto &format : clipboard->mimeData()->formats()) {
				saved->setData(format, clipboard->mimeData()->data(format));
			}
			const auto restore = gsl::finally([&] { clipboard->setMimeData(saved.release()); });
			const auto press = [&](int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
				PressKey(QApplication::focusWidget(), key, modifiers);
			};
			inner->setFocus();
			const auto view = inner->viewByItem(*item);
			const auto rect = view->innerGeometry().translated(0, inner->itemTop(view));
			const auto from = QPoint(rect.left() + rect.width() / 8, rect.center().y());
			for (const auto yank : { false, true }) {
				Click(inner, from);
				Drag(inner, from, rect.center());
				const auto selected = inner->getSelectedText().rich.text;
				Check(!selected.isEmpty(), u"mouse drag selects real message text"_q);
				auto copy = QKeyEvent(QEvent::ShortcutOverride,
					yank ? Qt::Key_Y : Qt::Key_C,
					yank ? Qt::NoModifier : Qt::ControlModifier);
				clipboard->setText(u"unchanged"_q);
				Check(HandleApplicationShortcutOverride(&copy)
					&& clipboard->text() == u"unchanged"_q,
					u"mouse selection claims Copy without changing the clipboard during the probe"_q);
				press(yank ? Qt::Key_Y : Qt::Key_C, yank ? Qt::NoModifier : Qt::ControlModifier);
				Check(!selected.isEmpty() && clipboard->text() == selected,
					u"mouse-selected text reaches the clipboard through y and system Copy"_q);
			}
			for (const auto yank : { false, true }) {
				press(Qt::Key_Escape);
				press(Qt::Key_Escape);
				SetNormalMode(true);
				inner->setFocus();
				press(Qt::Key_S);
				press(Qt::Key_A);
				const auto expected = inner->getSelectedText().rich.text;
				if (!yank) {
					SetNormalMode(false);
				}
				clipboard->setText(u"unchanged"_q);
				press(yank ? Qt::Key_Y : Qt::Key_C, yank ? Qt::NoModifier : Qt::ControlModifier);
				Check(expected.contains(u"Clipboard regression fixture"_q) && clipboard->text() == expected,
					u"message selection copies with y and system Copy in input mode"_q);
			}
		},
		[=](QWidget*) { return !(*item)->history()->hasPendingResizedItems(); });
}

} // namespace Test

#endif // _DEBUG
