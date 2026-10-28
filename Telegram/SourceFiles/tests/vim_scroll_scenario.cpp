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
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/history_widget.h"
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
#include <QtWidgets/QApplication>

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


struct ScrollFixture {
	History *history = nullptr;
	QPointer<HistoryInner> inner;
	QPointer<Ui::ElasticScroll> scroll;
	crl::time started = 0;
	int before = 0;
	int settled = 0;
};

void SendScrollKey(QObject *target, QEvent::Type type, int key,
		Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool repeat = false,
		quint32 nativeKey = 0, quint32 scanCode = 0) {
	auto event = QKeyEvent(type, key, modifiers, scanCode, nativeKey, 0, QString(), repeat);
	Settle([&] { QApplication::sendEvent(target, &event); });
}

} // namespace

void SetupScenario(not_null<Runner*> runner) {
	using namespace Core::VimKeymap;
	runner->waitEvent(u"launch_finished"_q);
	const auto fixture = std::make_shared<ScrollFixture>();
	runner->add({
		.name = u"create isolated scroll fixture"_q,
		.run = [=] {
			auto &account = Core::App().domain().active();
			Check(!account.sessionExists(), u"scroll test uses no real account"_q);
			if (account.sessionExists()) {
				return;
			}
			MTP::details::pause();
			account.createSession(FixtureUser(1, true));
			VimKeymapOption.set(true);
			auto &session = account.session();
			fixture->history = session.data().history(
				session.data().processUser(FixtureUser(20, false)));
			fixture->history->clearFolder();
			for (auto i = 0; i != 150; ++i) {
				const auto item = fixture->history->addNewLocalMessage({
					.id = session.data().nextLocalMessageId(),
					.flags = MessageFlag::Local | MessageFlag::Outgoing | MessageFlag::BeingSent,
					.from = session.user()->id,
					.date = base::unixtime::now(),
				}, { u"Scroll fixture message %1"_q.arg(i) }, MTP_messageMediaEmpty());
				item->setRealId(100 + i);
			}
			const auto active = Core::App().activePrimaryWindow();
			active->widget()->resize(1100, 850);
			active->sessionController()->showPeerHistory(fixture->history,
				Window::SectionShow::Way::ClearStack, ShowAtTheEndMsgId);
		},
	});
	runner->actOnWidget(u"resolve history scroll"_q, [=]() -> QWidget* {
		const auto active = Core::App().activePrimaryWindow();
		const auto inners = FindVisible<HistoryInner>(active->widget());
		return inners.empty() ? nullptr : inners.front();
	}, [=](QWidget *widget) {
		fixture->inner = static_cast<HistoryInner*>(widget);
		for (auto parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
			if (const auto scroll = dynamic_cast<Ui::ElasticScroll*>(parent)) {
				fixture->scroll = scroll;
				break;
			}
		}
		Check(fixture->scroll && fixture->scroll->scrollTopMax() > 2000,
			u"fixture has a scrollable chat history"_q);
	}, [=](QWidget*) { return !fixture->history->hasPendingResizedItems(); });
	for (const auto key : { Qt::Key_J, Qt::Key_K }) {
		for (const auto name : { u"tap"_q, u"held"_q, u"release elsewhere"_q,
				u"modifier changed"_q, u"insert mode"_q, u"focus lost"_q,
				u"window deactivated"_q, u"layout changed"_q,
				u"scan code"_q, u"auto repeat"_q }) {
			const auto label = QString(QChar(ushort(key))) + u": "_q + name;
			const auto nativePress = name == u"layout changed"_q
				? (key == Qt::Key_J ? 38 : 40)
				: name == u"scan code"_q ? int(key) : 0;
			const auto nativeRelease = name == u"scan code"_q
				? 0x041e : nativePress;
			const auto scanCode = name == u"scan code"_q ? 44 : 0;
			runner->add({
				.name = u"press "_q + label,
				.run = [=] {
					ForceWindowActive(fixture->inner->window());
					fixture->inner->setFocus();
					SetNormalMode(true);
					fixture->scroll->scrollToY(fixture->scroll->scrollTopMax() / 2);
					fixture->before = fixture->scroll->scrollTop();
					SendScrollKey(fixture->inner, QEvent::KeyPress, key, Qt::NoModifier, false,
						nativePress, scanCode);
					fixture->started = crl::now();
					if (name == u"tap"_q) {
						SendScrollKey(fixture->inner, QEvent::KeyRelease, key);
					}
				},
				.until = [=] { return crl::now() - fixture->started > 350; },
				.then = [=] {
					const auto delta = fixture->scroll->scrollTop() - fixture->before;
					Check((key == Qt::Key_J ? delta > 0 : delta < 0)
						&& (name == u"tap"_q || std::abs(delta) > ScrollStep()),
						u"key scrolls in the expected direction: "_q + label);
					if (name == u"focus lost"_q || name == u"window deactivated"_q) {
						auto event = QEvent(name == u"focus lost"_q
							? QEvent::FocusOut : QEvent::WindowDeactivate);
						QApplication::sendEvent(fixture->inner, &event);
					} else if (name != u"tap"_q) {
						if (name == u"auto repeat"_q) {
							SendScrollKey(fixture->inner, QEvent::KeyRelease, key, Qt::NoModifier, true);
							SendScrollKey(fixture->inner, QEvent::KeyPress, key, Qt::NoModifier, true);
						}
						if (name == u"insert mode"_q) {
							SetNormalMode(false);
						}
						SendScrollKey(name == u"release elsewhere"_q
							? qApp : static_cast<QObject*>(fixture->inner.data()),
							QEvent::KeyRelease, name == u"layout changed"_q ? 0x041e : key,
							name == u"modifier changed"_q ? Qt::ShiftModifier : Qt::NoModifier,
							false, nativeRelease, scanCode);
					}
					fixture->started = crl::now();
				},
			});
			runner->add({
				.name = u"settle "_q + label,
				.until = [=] { return crl::now() - fixture->started > 300; },
				.then = [=] {
					fixture->settled = fixture->scroll->scrollTop();
					fixture->started = crl::now();
				},
			});
			runner->add({
				.name = u"verify stop "_q + label,
				.until = [=] { return crl::now() - fixture->started > 300; },
				.then = [=] {
					Check(fixture->scroll->scrollTop() == fixture->settled,
						u"scroll stays stopped: "_q + label,
						u"before=%1 after=%2"_q.arg(fixture->settled).arg(fixture->scroll->scrollTop()));
					SetNormalMode(true);
					SendScrollKey(fixture->inner, QEvent::KeyRelease, key);
				},
			});
		}
	}
}
} // namespace Test
#endif // _DEBUG
