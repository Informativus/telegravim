/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#ifdef _DEBUG
#include "base/unixtime.h"
#include "boxes/delete_messages_box.h"
#include "chat_helpers/field_autocomplete.h"
#include "core/application.h"
#include "core/vim_keymap.h"
#include "core/vim_keymap_options.h"
#include "data/data_peer_bot_command.h"
#include "data/data_photo.h"
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

struct Fixture {
	QPointer<Ui::InputField> composer;
	QPointer<HistoryInner> inner;
	QPointer<Ui::ElasticScroll> scroll;
	std::vector<HistoryItem*> items;
	MessageIdsList ids;
	UserData *bot = nullptr;
	QStringList commands;
	MessageIdsList selected;
	int scrollTop = 0;
	int rangeOrigin = 0;
	crl::time deadline = 0;
};

void Key(QWidget *target, Qt::Key key,
		Qt::KeyboardModifiers modifiers = Qt::NoModifier,
		QString text = {}, bool repeat = false) {
	const auto native = key == Qt::Key_J ? 38U
		: key == Qt::Key_K ? 40U
		: key == Qt::Key_M ? 46U
		: key == Qt::Key_S ? 1U
		: key == Qt::Key_Tab || key == Qt::Key_Backtab ? 48U
		: key == Qt::Key_Escape ? 53U
		: key == Qt::Key_Return ? 36U
		: key == Qt::Key_D ? 2U : 255U;
	const auto guard = QPointer<QWidget>(target);
	for (const auto type : { QEvent::KeyPress, QEvent::KeyRelease }) {
		if (!guard) {
			break;
		}
		auto event = QKeyEvent(type, key, modifiers, 0, native, 0, text, repeat);
		Settle([&] { QApplication::sendEvent(guard.data(), &event); });
	}
}

void Hint(HistoryInner *inner, int index, int count, bool individual = true) {
	const auto label = individual
		? Core::VimKeymap::SelectionHintLabel(index, count)
		: Core::VimKeymap::HintLabel(index, count);
	for (const auto ch : label) {
		Key(inner, Qt::Key(ch.toUpper().unicode()), Qt::NoModifier, QString(ch));
	}
}

void Selected(const std::shared_ptr<Fixture> &fixture,
		std::initializer_list<int> indices, QString name) {
	auto expected = MessageIdsList();
	for (const auto index : indices) {
		expected.push_back(fixture->ids[index]);
	}
	auto actual = fixture->inner->getSelectedItems();
	ranges::sort(expected);
	ranges::sort(actual);
	Check(actual == expected, name,
		u"actual=%1 expected=%2"_q.arg(actual.size()).arg(expected.size()));
}

} // namespace

void SetupScenario(not_null<Runner*> runner) {
	using namespace Core::VimKeymap;
	const auto fixture = std::make_shared<Fixture>();
	const auto network = std::make_shared<QTcpServer>();
	runner->waitEvent(u"launch_finished"_q);
	runner->add({
		.name = u"create isolated selection history without network access"_q,
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
		for (const auto inner : FindVisible<HistoryInner>(active->widget())) {
			if (inner->viewByItem(fixture->items.front())) {
				fixture->inner = inner;
				return inner;
			}
		}
		return nullptr;
	};
	runner->actOnWidget(u"select a range immediately without changing the draft"_q,
		resolve, [=](QWidget *widget) {
			const auto inner = fixture->inner.data();
			ForceWindowActive(widget->window());
			const auto histories = FindVisible<HistoryWidget>(widget->window());
			Check(!histories.empty(), u"range fixture has a history"_q);
			if (histories.empty()) {
				return;
			}
			const auto inputs = FindVisible<Ui::InputField>(histories.front());
			Check(!inputs.empty(), u"range fixture has a composer"_q);
			if (inputs.empty()) {
				return;
			}
			const auto input = inputs.back();
			fixture->composer = input;
			SetNormalMode(false);
			input->setTextWithTags({ u"Keep this draft"_q });
			SetNormalMode(true);
			widget->setFocus();
			const auto target = inner->vimKeymapTargetView();
			Check(target != nullptr, u"range has a visible starting message"_q);
			if (!target) {
				return;
			}
			const auto origin = int(ranges::find(fixture->items, target->data().get())
				- begin(fixture->items));
			fixture->rangeOrigin = origin;
			Key(inner, Qt::Key_S, Qt::ShiftModifier);
			Selected(fixture, { origin }, u"Shift S immediately selects the starting message"_q);
			Check(input->getTextWithTags().text == u"Keep this draft"_q,
				u"Shift S leaves a nonempty draft unchanged"_q);
#ifdef Q_OS_MAC
			const auto control = Qt::MetaModifier;
#else // Q_OS_MAC
			const auto control = Qt::ControlModifier;
#endif // Q_OS_MAC
			const auto step = (origin < 4) ? 1 : -1;
			const auto extend = (step > 0) ? Qt::Key_J : Qt::Key_K;
			const auto shrink = (step > 0) ? Qt::Key_K : Qt::Key_J;
			Key(inner, extend, control);
			Selected(fixture, { origin, origin + step }, u"Ctrl J K extends the range instead of changing chats"_q);
			Key(inner, extend, control, (step > 0) ? u"о"_q : u"л"_q);
			Selected(fixture, { origin, origin + step, origin + 2 * step },
				u"Russian Ctrl J K continues the range"_q);
			Key(inner, shrink, control);
			Selected(fixture, { origin, origin + step }, u"reverse movement shrinks the range"_q);
			Key(inner, shrink, control);
			Selected(fixture, { origin }, u"reverse movement returns to the starting message"_q);
			Key(inner, Qt::Key_D);
		});
	runner->actOnWidget(u"d confirms deletion directly after Shift S"_q,
		[]() -> QWidget* {
			const auto boxes = FindVisible<DeleteMessagesBox>(Core::App().activePrimaryWindow()->widget());
			return boxes.empty() ? nullptr : boxes.front();
		}, [=](QWidget *box) {
			Selected(fixture, { fixture->rangeOrigin }, u"delete confirmation retains the exact range"_q);
			auto &data = Core::App().domain().active().session().data();
			Check(ranges::all_of(fixture->ids, [&](FullMsgId id) { return data.message(id); }),
				u"range deletion requires confirmation"_q);
			Key(box, Qt::Key_Escape);
			Key(fixture->inner, Qt::Key_Escape);
			Selected(fixture, {}, u"Escape cancels the range"_q);
			Check(fixture->composer
				&& fixture->composer->getTextWithTags().text == u"Keep this draft"_q,
				u"range navigation and deletion leave the draft unchanged"_q);
			if (fixture->composer) {
				fixture->composer->setTextWithTags({});
			}
		});
	runner->actOnWidget(u"toggle nonadjacent messages and retain picking mode"_q,
		resolve, [=](QWidget *widget) {
			const auto inner = fixture->inner.data();
			ForceWindowActive(widget->window());
			widget->setFocus();
			SetNormalMode(true);
			Key(inner, Qt::Key_S);
			Selected(fixture, {}, u"s waits for a message hint"_q);
			Hint(inner, 0, 6);
			Selected(fixture, { 0 }, u"first hint selects exactly its message"_q);
			if (inner->getSelectedItems().empty()) {
				return;
			}
			Hint(inner, 4, 6);
			Selected(fixture, { 0, 4 }, u"second hint adds a nonadjacent message"_q);
			Hint(inner, 0, 6);
			Selected(fixture, { 4 }, u"repeated hint removes only that message"_q);
			Hint(inner, 2, 6);
			Selected(fixture, { 2, 4 }, u"picking continues after removing a message"_q);
			Key(inner, Qt::Key_Return);
			Selected(fixture, { 2, 4 }, u"Enter preserves selected messages"_q);
			Key(inner, Qt::Key_K);
			Selected(fixture, { 2, 4 }, u"j k after Enter cannot extend the selection"_q);
			Key(inner, Qt::Key_Escape);
			Selected(fixture, {}, u"Escape cancels completed selection"_q);
			Key(inner, Qt::Key_S);
			Hint(inner, 1, 6);
			Key(inner, Qt::Key_Escape);
			Selected(fixture, {}, u"Escape cancels selection while hints remain"_q);
			Key(inner, Qt::Key_S);
			Hint(inner, 0, 6);
			Hint(inner, 4, 6);
			Key(inner, Qt::Key_Return);
			Selected(fixture, { 0, 4 }, u"selection is ready for deletion"_q);
			Key(inner, Qt::Key_D);

		}, [=](QWidget*) {
			return !Core::App().activePrimaryWindow()->sessionController()->isLayerShown()
				&& !fixture->items.front()->history()->hasPendingResizedItems();
		});
	runner->actOnWidget(u"delete only the selected nonadjacent messages"_q,
		[=]() -> QWidget* {
			const auto boxes = FindVisible<DeleteMessagesBox>(Core::App().activePrimaryWindow()->widget());
			return boxes.empty() ? nullptr : boxes.front();
		}, [=](QWidget *box) {
			auto &data = Core::App().domain().active().session().data();
			Check(ranges::all_of(fixture->ids, [&](FullMsgId id) { return data.message(id); }),
				u"deletion waits for native confirmation"_q);
			Key(box, Qt::Key_Return);
			for (auto i = 0; i != 6; ++i) {
				Check(bool(data.message(fixture->ids[i])) == (i != 0 && i != 4),
					u"deletion affects exactly the requested set"_q, u"index=%1"_q.arg(i));
			}
		});

	runner->add({
		.name = u"create a scrollable selection history"_q,
		.run = [=] {
			auto &session = Core::App().domain().active().session();
			const auto peer = session.data().processUser(FixtureUser(4, false));
			const auto history = session.data().history(peer);
			history->clearFolder();
			fixture->items.clear();
			for (auto i = 0; i != 80; ++i) {
				const auto item = history->addNewLocalMessage({
					.id = session.data().nextLocalMessageId(),
					.flags = MessageFlag::Local | MessageFlag::Outgoing | MessageFlag::BeingSent,
					.from = session.user()->id,
					.date = base::unixtime::now(),
				}, { u"Scroll selection fixture %1"_q.arg(i) }, MTP_messageMediaEmpty());
				item->setRealId(200 + i);
				fixture->items.push_back(item);
			}
			Core::App().activePrimaryWindow()->sessionController()->showPeerHistory(
				history, Window::SectionShow::Way::ClearStack, ShowAtTheEndMsgId);
		},
	});
	const auto visibleItems = [=] {
		auto result = MessageIdsList();
		const auto inner = fixture->inner.data();
		const auto scroll = fixture->scroll.data();
		const auto top = scroll->scrollTop();
		const auto bottom = top + scroll->height();
		for (const auto item : fixture->items) {
			const auto view = inner->viewByItem(item);
			if (!view) {
				continue;
			}
			const auto y = inner->itemTop(view);
			if (view->data()->canBeSelected()
				&& std::min(y + view->height(), bottom) - std::max(y, top) >= 32) {
				result.push_back(view->data()->fullId());
			}
		}
		return result;
	};
	runner->actOnWidget(u"scroll while retaining picked messages"_q, resolve,
		[=](QWidget *widget) {
			ForceWindowActive(widget->window());
			widget->setFocus();
			SetNormalMode(true);
			for (auto parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
				if (const auto scroll = dynamic_cast<Ui::ElasticScroll*>(parent)) {
					fixture->scroll = scroll;
					break;
				}
			}
			Check(bool(fixture->scroll), u"scrolling fixture has a real history viewport"_q);
			if (!fixture->scroll) {
				return;
			}
			const auto visible = visibleItems();
			Check(visible.size() >= 3, u"scrolling fixture exposes multiple messages"_q);
			Key(widget, Qt::Key_S);
			Hint(fixture->inner, 0, visible.size());
			Hint(fixture->inner, 2, visible.size());
			fixture->selected = fixture->inner->getSelectedItems();
			Check(fixture->selected.size() == 2, u"scrolling starts with two selected messages"_q);
			fixture->scrollTop = fixture->scroll->scrollTop();
			Key(widget, Qt::Key_K);
			fixture->deadline = crl::now() + 2 * SingleScrollDurationMs();
		}, [=](QWidget*) {
			return !fixture->items.front()->history()->hasPendingResizedItems();
		});
	runner->add({
		.name = u"keep hints and exact selection after scrolling"_q,
		.until = [=] { return crl::now() >= fixture->deadline; },
		.then = [=] {
			Check(fixture->scroll->scrollTop() < fixture->scrollTop,
				u"k actually moves the history viewport"_q);
			Check(fixture->inner->getSelectedItems() == fixture->selected,
				u"scroll preserves the exact selected IDs"_q);
			const auto visible = visibleItems();
			const auto next = ranges::find_if(visible, [&](FullMsgId id) {
				return !ranges::contains(fixture->selected, id);
			});
			Check(next != end(visible), u"another visible message is available after scrolling"_q);
			if (next != end(visible)) {
				Hint(fixture->inner, next - begin(visible), visible.size());
				const auto selected = fixture->inner->getSelectedItems();
				Check(selected.size() == 3 && ranges::contains(selected, *next),
					u"persistent hints select another message after scrolling"_q);
			}
			Key(fixture->inner, Qt::Key_Escape);
			Check(fixture->inner->getSelectedItems().empty(), u"Escape clears scrolled selection"_q);
			for (const auto alphabet : { u"en"_q, u"ru"_q }) {
				VimKeymapHintAlphabetOption.set(alphabet);
				for (auto total = 1; total != 80; ++total) {
					auto labels = QStringList();
					for (auto i = 0; i != total; ++i) {
						labels.push_back(SelectionHintLabel(i, total));
					}
					const auto valid = ranges::all_of(labels, [&](const QString &label) {
						return !label.isEmpty() && !label.contains('j') && !label.contains('k')
							&& !label.contains(QChar(0x043e)) && !label.contains(QChar(0x043b))
							&& ranges::count_if(labels, [&](const QString &other) {
								return other.startsWith(label);
							}) == 1;
					});
					Check(valid, u"selection labels reserve scrolling keys and remain unambiguous"_q,
						alphabet + u" total=%1"_q.arg(total));
				}
			}
			VimKeymapHintAlphabetOption.set(u"en"_q);
		},
	});

	runner->add({
		.name = u"create a two-photo album"_q,
		.run = [=] {
			auto &session = Core::App().domain().active().session();
			const auto peer = session.data().processUser(FixtureUser(5, false));
			const auto history = session.data().history(peer);
			history->clearFolder();
			fixture->items.clear();
			fixture->ids.clear();
			for (auto i = 0; i != 2; ++i) {
				const auto photo = session.data().processPhoto(MTP_photo(
					MTP_flags(0), MTP_long(5000 + i), MTP_long(1), MTP_bytes(QByteArray()),
					MTP_int(base::unixtime::now()),
					MTP_vector<MTPPhotoSize>({ MTP_photoSize(
						MTP_string("x"), MTP_int(480), MTP_int(320), MTP_int(1000)) }),
					MTPVector<MTPVideoSize>(), MTP_int(2)));
				const auto item = history->addNewLocalMessage({
					.id = session.data().nextLocalMessageId(),
					.flags = MessageFlag::Local | MessageFlag::Outgoing | MessageFlag::BeingSent,
					.from = session.user()->id,
					.date = base::unixtime::now(),
					.groupedId = 500,
				}, photo, { u"Album selection fixture"_q });
				item->setRealId(2000 + i);
				fixture->items.push_back(item);
				fixture->ids.push_back(item->fullId());
			}
			Core::App().activePrimaryWindow()->sessionController()->showPeerHistory(
				history, Window::SectionShow::Way::ClearStack, ShowAtTheEndMsgId);
		},
	});
	runner->actOnWidget(u"toggle the entire album as one selection"_q, resolve,
		[=](QWidget *widget) {
			ForceWindowActive(widget->window());
			widget->setFocus();
			SetNormalMode(true);
			Key(widget, Qt::Key_S);
			Hint(fixture->inner, 0, 1);
			Selected(fixture, { 0, 1 }, u"one hint selects every album member"_q);
			Hint(fixture->inner, 0, 1);
			Selected(fixture, {}, u"repeated hint deselects the entire album"_q);
			Key(widget, Qt::Key_Escape);
		}, [=](QWidget*) {
			return !fixture->items.front()->history()->hasPendingResizedItems();
		});
	runner->add({
		.name = u"create a bot with START and native commands"_q,
		.run = [=] {
			auto &session = Core::App().domain().active().session();
			const auto bot = session.data().processUser(FixtureUser(3, false));
			bot->setBotInfoVersion(1);
			bot->botInfo->inited = true;
			bot->botInfo->startToken = u"fixture-token"_q;
			bot->botInfo->commands = {
				{ u"first"_q, u"First command"_q },
				{ u"second"_q, u"Second command"_q },
				{ u"third"_q, u"Third command"_q },
			};
			fixture->bot = bot;
			const auto history = session.data().history(bot);
			history->clearFolder();
			const auto item = history->addNewLocalMessage({
				.id = session.data().nextLocalMessageId(),
				.flags = MessageFlag::Local | MessageFlag::Outgoing | MessageFlag::BeingSent,
				.from = session.user()->id,
				.date = base::unixtime::now(),
			}, { u"Bot fixture"_q }, MTP_messageMediaEmpty());
			item->setRealId(100);
			fixture->items = { item };
			Core::App().activePrimaryWindow()->sessionController()->showPeerHistory(
				history, Window::SectionShow::Way::ClearStack, ShowAtTheEndMsgId);
		},
	});
	runner->actOnWidget(u"activate START with Enter"_q, resolve, [=](QWidget *widget) {
		ForceWindowActive(widget->window());
		widget->setFocus();
		SetNormalMode(true);
		Check(!fixture->bot->botInfo->startToken.isEmpty(), u"START is initially available"_q);
		Key(widget, Qt::Key_Return, Qt::ShiftModifier);
		Check(!fixture->bot->botInfo->startToken.isEmpty(), u"Shift Enter does not press START"_q);
		Key(widget, Qt::Key_Return, Qt::NoModifier, {}, true);
		Check(!fixture->bot->botInfo->startToken.isEmpty(), u"repeated Enter does not press START"_q);
		Key(widget, Qt::Key_Return);
		Check(fixture->bot->botInfo->startToken.isEmpty(), u"Enter invokes native START action"_q);
	});
	runner->actOnWidget(u"open bot menu using m"_q, resolve, [=](QWidget *widget) {
		widget->setFocus();
		SetNormalMode(true);
		for (const auto menu : FindAll<ChatHelpers::FieldAutocomplete>(widget->window())) {
			menu->botCommandChosen() | rpl::on_next([=](auto chosen) {
				fixture->commands.push_back(chosen.command);
			}, menu->lifetime());
		}
		Key(widget, Qt::Key_M, Qt::NoModifier, u"m"_q);
	});
	runner->actOnWidget(u"navigate native bot menu using Tab and Shift Tab"_q,
		[=]() -> QWidget* {
			const auto menus = FindVisible<ChatHelpers::FieldAutocomplete>(fixture->inner->window());
			return menus.empty() ? nullptr : menus.front();
		}, [=](QWidget*) {
			const auto inner = fixture->inner.data();
			Key(inner, Qt::Key_Tab);
			Key(inner, Qt::Key_Tab);
			Key(inner, Qt::Key_Backtab, Qt::ShiftModifier);
			Key(inner, Qt::Key_Return);
			Check(fixture->commands == QStringList{ u"/second"_q },
				u"Tab Tab Shift Tab Enter activates the second bot command"_q,
				fixture->commands.join(u","_q));
			Key(inner, Qt::Key_Return, Qt::NoModifier, {}, true);
			Check(fixture->commands.size() == 1, u"held Enter does not send another command"_q);
		});

	runner->add({
		.name = u"reopen bot menu using Russian layout"_q,
		.until = [=] {
			return FindVisible<ChatHelpers::FieldAutocomplete>(fixture->inner->window()).empty();
		},
		.then = [=] {
			const auto inner = fixture->inner.data();
			inner->setFocus();
			SetNormalMode(true);
			Key(inner, Qt::Key(0x042c), Qt::NoModifier, u"ь"_q);
		},
	});
	runner->actOnWidget(u"cycle backward through bot commands"_q,
		[=]() -> QWidget* {
			const auto menus = FindVisible<ChatHelpers::FieldAutocomplete>(fixture->inner->window());
			return menus.empty() ? nullptr : menus.front();
		}, [=](QWidget*) {
			Key(fixture->inner, Qt::Key_Backtab, Qt::ShiftModifier);
			Key(fixture->inner, Qt::Key_Return);
			Check(fixture->commands == QStringList{ u"/second"_q, u"/third"_q },
				u"Russian m opens menu and Shift Tab wraps to the last command"_q,
				fixture->commands.join(u","_q));
		});
	runner->add({
		.name = u"ordinary text input retains menu shortcut letters"_q,
		.run = [=] {
			const auto histories = FindVisible<HistoryWidget>(fixture->inner->window());
			Check(!histories.empty(), u"bot history is visible"_q);
			if (histories.empty()) {
				return;
			}
			const auto inputs = FindVisible<Ui::InputField>(histories.front());
			Check(!inputs.empty(), u"bot composer is visible"_q);
			if (inputs.empty()) {
				return;
			}
			const auto input = inputs.back()->rawTextEdit();
			input->setFocus();
			SetNormalMode(false);
			Key(input, Qt::Key_M, Qt::NoModifier, u"m"_q);
			Key(input, Qt::Key_S, Qt::NoModifier, u"s"_q);
			Check(input->toPlainText() == u"ms"_q, u"m and s are typed normally in Insert mode"_q);
		},
	});
}

} // namespace Test
#endif // _DEBUG
