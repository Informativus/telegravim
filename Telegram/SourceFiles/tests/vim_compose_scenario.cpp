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
#include "core/vim_keymap_options.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_widget.h"
#include "ui/widgets/fields/input_field.h"
#include "history/view/history_view_element.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mtproto/facade.h"
#include "test/test_log.h"
#include "test/test_runner.h"
#include "test/test_widgets.h"
#include "ui/widgets/elastic_scroll.h"
#include "window/main_window.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include <QtGui/QKeyEvent>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>
namespace Test {
namespace {
MTPUser FixtureUser(int id, bool self) {
	using Flag = MTPDuser::Flag;
	return MTP_user(
		MTP_flags(Flag::f_first_name | (self ? Flag::f_self : Flag::f_contact)),
		MTP_long(id),
		MTPlong(),
		MTP_string("Composer fixture"),
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

struct Fixture {
	QPointer<HistoryInner> inner;
	QPointer<HistoryWidget> history;
	QPointer<Ui::InputField> field;
	std::vector<HistoryItem*> items;
	MessageIdsList ids;
};

void Key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
		QString text = {}, quint32 native = 255) {
	const auto guard = QPointer<QWidget>(QApplication::focusWidget());
	for (const auto type : { QEvent::KeyPress, QEvent::KeyRelease }) {
		if (!guard) {
			break;
		}
		auto event = QKeyEvent(type, key, modifiers, 0, native, 0, text);
		Settle([&] { QApplication::sendEvent(guard.data(), &event); });
	}
}

bool Prepare(const std::shared_ptr<Fixture> &fixture,
		const QString &text, int position, bool historyFocus) {
	using namespace Core::VimKeymap;
	Key(Qt::Key_Escape, Qt::NoModifier, {}, 53);
	if (!fixture->inner || !fixture->history || !fixture->field) {
		Check(false, u"composer remains open after commands"_q);
		return false;
	}
	SetNormalMode(false);
	fixture->field->setFocusFast();
	fixture->field->setTextWithTags({ text });
	auto cursor = fixture->field->textCursor();
	cursor.setPosition(position);
	fixture->field->setTextCursor(cursor);
	Key(Qt::Key_Escape, Qt::NoModifier, {}, 53);
	Check(NormalMode(), u"Escape enters View mode"_q);
	if (historyFocus) {
		fixture->history->setInnerFocus();
		Check(fixture->inner && fixture->inner->hasFocus(), u"View focus is on history"_q);
	}
	return fixture->inner && fixture->field;
}

void Position(const std::shared_ptr<Fixture> &fixture, int expected,
		const QString &name) {
	const auto actual = fixture->field->textCursor().position();
	Check(actual == expected, name,
		u"actual=%1 expected=%2"_q.arg(actual).arg(expected));
}

void Text(const std::shared_ptr<Fixture> &fixture, const QString &expected,
		const QString &name) {
	const auto actual = fixture->field->getTextWithTags().text;
	Check(actual == expected, name, u"actual=[%1] expected=[%2]"_q.arg(actual, expected));
}

} // namespace

void SetupScenario(not_null<Runner*> runner) {
	using namespace Core::VimKeymap;
	const auto fixture = std::make_shared<Fixture>();
	const auto network = std::make_shared<QTcpServer>();
	runner->waitEvent(u"launch_finished"_q);
	runner->add({
		.name = u"create isolated composer history without network access"_q,
		.run = [=] {
			auto &account = Core::App().domain().active();
			Check(!account.sessionExists(), u"test never uses a real account"_q);
			if (account.sessionExists()) {
				return;
			}
			Check(network->listen(QHostAddress::LocalHost), u"test owns a local connection sink"_q);
			const auto sink = network.get();
			QObject::connect(sink, &QTcpServer::newConnection, sink, [=] {
				while (const auto socket = sink->nextPendingConnection()) {
					socket->close();
					socket->deleteLater();
				}
			});
			Core::App().setCurrentProxy({
				.type = MTP::ProxyData::Type::Socks5,
				.host = u"127.0.0.1"_q,
				.port = network->serverPort(),
			}, MTP::ProxyData::Settings::Enabled);
			account.createSession(FixtureUser(1, true));
			auto &session = account.session();
			const auto peer = session.data().processUser(FixtureUser(2, false));
			const auto history = session.data().history(peer);
			history->clearFolder();
			for (auto i = 0; i != 6; ++i) {
				const auto item = history->addNewLocalMessage({
					.id = session.data().nextLocalMessageId(),
					.flags = MessageFlag::Local | MessageFlag::Outgoing | MessageFlag::BeingSent,
					.from = session.user()->id,
					.date = base::unixtime::now(),
				}, { u"Selection fixture %1"_q.arg(i) }, MTP_messageMediaEmpty());
				item->setRealId(1000 + i);
				Check(item->canDelete(), u"fixture message can be deleted"_q);
				fixture->items.push_back(item);
				fixture->ids.push_back(item->fullId());
			}
			VimKeymapOption.set(true);
			VimKeymapHintAlphabetOption.set(u"en"_q);
			const auto active = Core::App().activePrimaryWindow();
			active->widget()->resize(1100, 850);
			active->sessionController()->showPeerHistory(history,
				Window::SectionShow::Way::ClearStack, ShowAtTheEndMsgId);
		},
	});
	const auto resolve = [=]() -> QWidget* {
		const auto active = Core::App().activePrimaryWindow();
		for (const auto history : FindVisible<HistoryWidget>(active->widget())) {
			for (const auto inner : FindVisible<HistoryInner>(history)) {
				if (!inner->viewByItem(fixture->items.front())) {
					continue;
				}
				const auto fields = FindVisible<Ui::InputField>(history);
				if (!fields.empty()) {
					fixture->inner = inner;
					fixture->history = history;
					fixture->field = fields.back();
					return history;
				}
			}
		}
		return nullptr;
	};
	runner->actOnWidget(u"composer commands after Escape and history focus"_q,
		resolve, [=](QWidget *widget) {
			ForceWindowActive(widget->window());
			for (const auto historyFocus : { false, true }) {
				const auto context = historyFocus ? u"history focus: "_q : u"composer focus: "_q;
				if (!Prepare(fixture, u"  alpha beta\nsecond line\nlast"_q, 4, historyFocus)) {
					return;
				}
				Key(Qt::Key_Dollar, Qt::ShiftModifier, u"$"_q, 21);
				Position(fixture, 11, context + u"dollar reaches last character"_q);
				if (!Prepare(fixture, u"  alpha beta\nsecond line\nlast"_q, 4, historyFocus)) {
					return;
				}
				Key(Qt::Key_Bar, Qt::ShiftModifier, u"|"_q, 42);
				Position(fixture, 0, context + u"pipe reaches line start"_q);
				if (!Prepare(fixture, u"  alpha beta\nsecond line\nlast"_q, 4, historyFocus)) {
					return;
				}
				Key(Qt::Key_AsciiCircum, Qt::ShiftModifier, u"^"_q, 22);
				Position(fixture, 2, context + u"caret skips indentation"_q);
				if (!Prepare(fixture, u"first\nsecond\nlast"_q, 8, historyFocus)) {
					return;
				}
				Key(Qt::Key_D, Qt::NoModifier, u"d"_q, 2);
				Text(fixture, u"first\nsecond\nlast"_q, context + u"first d waits without editing"_q);
				Key(Qt::Key_D, Qt::NoModifier, u"d"_q, 2);
				Text(fixture, u"first\nlast"_q, context + u"dd deletes only current line"_q);
				Check(NormalMode(), context + u"dd stays in View"_q);
				if (!Prepare(fixture, u"первая\nвторая\nпоследняя"_q, 9, historyFocus)) {
					return;
				}
				Key(Qt::Key(0x0412), Qt::NoModifier, u"в"_q, 2);
				Key(Qt::Key(0x0412), Qt::NoModifier, u"в"_q, 2);
				Text(fixture, u"первая\nпоследняя"_q, context + u"Russian dd deletes only current line"_q);
				if (!Prepare(fixture, u"first\n  second\nlast"_q, 10, historyFocus)) {
					return;
				}
				Key(Qt::Key_Bar, Qt::ShiftModifier, {}, 42);
				Position(fixture, 6, context + u"textless pipe stops at current line start"_q);
				Key(Qt::Key_H, Qt::NoModifier, u"h"_q, 4);
				Position(fixture, 6, context + u"h cannot cross the explicit line break"_q);
				Key(Qt::Key_Dollar, Qt::ShiftModifier, {}, 21);
				Position(fixture, 13, context + u"textless dollar stops before next line"_q);
				Key(Qt::Key_L, Qt::NoModifier, u"l"_q, 37);
				Position(fixture, 13, context + u"l cannot cross the explicit line break"_q);
				if (!Prepare(fixture, u"first\n\nlast"_q, 6, historyFocus)) {
					return;
				}
				Key(Qt::Key_D, Qt::NoModifier, u"d"_q, 2);
				Key(Qt::Key_D, Qt::NoModifier, u"d"_q, 2);
				Text(fixture, u"first\nlast"_q, context + u"dd removes only the empty line"_q);
				if (!Prepare(fixture, u"one line"_q, 3, historyFocus)) {
					return;
				}
				Key(Qt::Key_D, Qt::NoModifier, u"d"_q, 2);
				Key(Qt::Key_D, Qt::NoModifier, u"d"_q, 2);
				Text(fixture, {}, context + u"dd clears single line draft"_q);
				Key(Qt::Key_U, Qt::NoModifier, u"u"_q, 32);
				Text(fixture, u"one line"_q, context + u"undo restores deleted draft"_q);
				if (!Prepare(fixture, u"alpha beta\nnext"_q, 6, historyFocus)) {
					return;
				}
				Key(Qt::Key_D, Qt::NoModifier, u"d"_q, 2);
				Key(Qt::Key_Dollar, Qt::ShiftModifier, u"$"_q, 21);
				Text(fixture, u"alpha \nnext"_q, context + u"dollar completes delete operator"_q);
				if (!Prepare(fixture, u"  текст строки"_q, 5, historyFocus)) {
					return;
				}
				Key(Qt::Key_Semicolon, Qt::ShiftModifier, u";"_q, 21);
				Position(fixture, 13, context + u"physical dollar on Russian layout"_q);
				if (!Prepare(fixture, u"  текст строки"_q, 5, historyFocus)) {
					return;
				}
				Key(Qt::Key_Slash, Qt::ShiftModifier, u"/"_q, 42);
				Position(fixture, 0, context + u"physical pipe on Russian layout"_q);
				if (!Prepare(fixture, u"  alpha beta"_q, 4, historyFocus)) {
					return;
				}
				Key(Qt::Key_V, Qt::NoModifier, u"v"_q, 9);
				Key(Qt::Key_Dollar, Qt::ShiftModifier, u"$"_q, 21);
				Check(fixture->field->textCursor().selectedText() == u"pha beta"_q,
					context + u"visual dollar extends selection"_q);
			}
			if (!Prepare(fixture, u"first\nsecond"_q, 8, false)) {
				return;
			}
			Key(Qt::Key_J, Qt::NoModifier, u"j"_q, 38);
			Position(fixture, 8, u"plain j leaves draft cursor unchanged"_q);
			Key(Qt::Key_K, Qt::NoModifier, u"k"_q, 40);
			Position(fixture, 8, u"plain k leaves draft cursor unchanged"_q);
			if (!Prepare(fixture, {}, 0, false)) {
				return;
			}
			SetNormalMode(false);
			fixture->field->setFocusFast();
			Key(Qt::Key_Dollar, Qt::ShiftModifier, u"$"_q, 21);
			Key(Qt::Key_Bar, Qt::ShiftModifier, u"|"_q, 42);
			Key(Qt::Key_D, Qt::NoModifier, u"d"_q, 2);
			Key(Qt::Key_D, Qt::NoModifier, u"d"_q, 2);
			Key(Qt::Key(0x0412), Qt::NoModifier, u"в"_q, 2);
			Text(fixture, u"$|ddв"_q, u"Insert mode types punctuation and letters unchanged"_q);
			if (!Prepare(fixture, {}, 0, true)) {
				return;
			}
			Key(Qt::Key_S, Qt::NoModifier, u"s"_q, 1);
			const auto label = HintLabel(0, 6);
			for (const auto ch : label) {
				Key(Qt::Key(ch.toUpper().unicode()), Qt::NoModifier, QString(ch));
			}
			Check(fixture->inner->getSelectedItems().size() == 1,
				u"empty draft still allows selecting messages"_q);
			Key(Qt::Key_Escape, Qt::NoModifier, {}, 53);
		});
}

} // namespace Test
#endif // _DEBUG
