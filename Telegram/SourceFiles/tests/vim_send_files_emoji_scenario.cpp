/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#ifdef _DEBUG
#include "api/api_common.h"
#include "boxes/send_files_box.h"
#include "chat_helpers/tabbed_panel.h"
#include "chat_helpers/tabbed_selector.h"
#include "core/application.h"
#include "core/vim_keymap.h"
#include "core/vim_keymap_options.h"
#include "core/vim_keymap_widgets.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "menu/menu_send.h"
#include "mtproto/facade.h"
#include "storage/storage_media_prepare.h"
#include "test/test_log.h"
#include "test/test_runner.h"
#include "test/test_widgets.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/controls/emoji_button.h"
#include "ui/ui_utility.h"
#include "ui/widgets/fields/input_field.h"
#include "window/main_window.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"

#include <QtCore/QTemporaryDir>
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


struct EmojiFixture {
	QTemporaryDir files;
	QPointer<SendFilesBox> box;
	QPointer<ChatHelpers::TabbedPanel> panel;
	QPointer<ChatHelpers::TabbedSelector> selector;
	QPointer<Ui::InputField> caption;
	QPointer<Ui::GenericBox> nested;
	QStringList chosen;
	crl::time started = 0;
	int sends = 0;
};

void Key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
		QString text = {}, bool repeat = false) {
	ForceWindowActive(Core::App().activePrimaryWindow()->widget());
	const auto target = QPointer<QWidget>(QApplication::focusWidget());
	Check(target != nullptr, u"keyboard event has a focused receiver"_q);
	if (!target) {
		return;
	}
	for (const auto type : { QEvent::KeyPress, QEvent::KeyRelease }) {
		if (!target) {
			break;
		}
		auto event = QKeyEvent(type, key, modifiers, text, repeat);
		Settle([&] { QApplication::sendEvent(target, &event); });
	}
}

} // namespace

void SetupScenario(not_null<Runner*> runner) {
	using namespace Core::VimKeymap;
	runner->waitEvent(u"launch_finished"_q);
	const auto f = std::make_shared<EmojiFixture>();
	runner->add({
		.name = u"prepare disposable photo preview"_q,
		.run = [=] {
			auto &account = Core::App().domain().active();
			Check(!account.sessionExists(), u"test never uses a signed-in account"_q);
			if (account.sessionExists()) {
				return;
			}
			MTP::details::pause();
			account.createSession(FixtureUser(1, true));
			VimKeymapOption.set(true);
			auto urls = QList<QUrl>();
			for (auto i = 0; i != 8; ++i) {
				auto image = QImage(480, 320, QImage::Format_RGB32);
				image.fill(QColor(40 + 20 * i, 120, 160));
				const auto path = f->files.filePath(u"photo-%1.png"_q.arg(i));
				Check(image.save(path), u"fixture photo saved"_q);
				urls.push_back(QUrl::fromLocalFile(path));
			}
			const auto active = Core::App().activePrimaryWindow();
			active->widget()->resize(1100, 850);
			const auto controller = active->sessionController();
			auto box = Box<SendFilesBox>(controller,
				Storage::PrepareMediaList(urls, st::sendMediaPreviewSize, false),
				TextWithTags{ u"draft "_q }, account.session().user(),
				Api::SendType::Normal, SendMenu::Details());
			f->box = box.data();
			box->setConfirmedCallback([=](auto, auto, auto) { ++f->sends; });
			controller->show(std::move(box));
			f->started = crl::now();
		},
		.until = [=] { return crl::now() - f->started > 500; },
		.then = [=] {
			Check(f->box && f->box->isVisible(), u"eight-photo preview opens"_q);
			if (!f->box) {
				return;
			}
			ForceWindowActive(f->box->window());
			f->caption = FindFirst<Ui::InputField>(f->box);
			const auto toggle = FindFirst<Ui::EmojiButton>(f->box);
			Check(toggle && f->caption, u"preview exposes caption and emoji toggle"_q);
			if (toggle) {
				Click(toggle);
			}
		},
	});
	runner->add({
		.name = u"mouse opening focuses the emoji grid"_q,
		.run = [=] { f->started = crl::now(); },
		.until = [=] { return crl::now() - f->started > 700; },
		.then = [=] {
			const auto window = Core::App().activePrimaryWindow()->widget();
			const auto panels = FindVisible<ChatHelpers::TabbedPanel>(window);
			Check(panels.size() == 1, u"one caption picker remains open after mouse leave"_q);
			if (panels.size() != 1) {
				return;
			}
			f->panel = panels.front();
			f->selector = f->panel->selector().get();
			Check(Ui::InFocusChain(f->selector), u"mouse opening gives keyboard focus to grid"_q);
			f->selector->emojiChosen() | rpl::on_next([=](auto) {
				f->chosen.push_back(f->caption->getLastText());
			}, f->box->lifetime());
		},
	});
	runner->captureWidget(u"attachment-emoji-keyboard-focus"_q, [=]() -> QWidget* {
		return f->selector ? f->box->window() : nullptr;
	});
	runner->add({
		.name = u"select and move without sending attachments"_q,
		.run = [=] {
			if (!f->selector) {
				return;
			}
			auto previous = f->caption->getLastText();
			auto emoji = QStringList();
			for (const auto key : { Qt::Key_Return, Qt::Key_L, Qt::Key_H,
					Qt::Key_J, Qt::Key_K }) {
				if (key != Qt::Key_Return) {
					Key(key, Qt::NoModifier, QString(QChar(int(key)).toLower()));
				}
				Key(Qt::Key_Return);
				const auto current = f->caption->getLastText();
				emoji.push_back(current.mid(previous.size()));
				previous = current;
			}
			Check(!emoji[0].isEmpty() && emoji[0] != emoji[1]
				&& emoji[0] == emoji[2] && emoji[0] != emoji[3]
				&& emoji[0] == emoji[4], u"h j k l move between actual emoji cells"_q);
			Key(Qt::Key_L, Qt::NoModifier, u"д"_q);
			Key(Qt::Key_Space, Qt::NoModifier, u" "_q);
			Check(f->caption->getLastText().mid(previous.size()) == emoji[1],
				u"Russian layout navigation and Space select the same emoji"_q);
			const auto count = f->chosen.size();
			Key(Qt::Key_Return, Qt::NoModifier, {}, true);
			Check(f->chosen.size() == count, u"held Enter does not insert repeatedly"_q);
			Check(f->sends == 0, u"grid activation never sends the photos"_q);
			Key(Qt::Key_F, Qt::ControlModifier, u"f"_q);
			Check(f->selector->vimKeymapSearchHasFocus(), u"Ctrl F focuses picker search"_q);
			Key(Qt::Key_J, Qt::NoModifier, u"j"_q);
			Key(Qt::Key_K, Qt::NoModifier, u"k"_q);
			const auto search = FindFirst<Ui::InputField>(f->selector);
			Check(search && search->getLastText() == u"jk"_q,
				u"j k are inserted into picker search"_q);
			f->started = crl::now();
		},
		.until = [=] { return crl::now() - f->started > 700; },
		.then = [=] {
			if (!f->selector) {
				return;
			}
			Key(Qt::Key_Return);
			Check(!f->selector->vimKeymapSearchHasFocus()
				&& Ui::InFocusChain(f->selector), u"Enter in search returns to results"_q);
			Key(Qt::Key_Return);
			Key(Qt::Key_Enter, Qt::ControlModifier);
			Check(f->sends == 0 && f->box && f->box->isVisible(),
				u"empty results and modified Enter never send attachments"_q);
			Key(Qt::Key_Tab);
			Check(f->selector->vimKeymapSearchHasFocus(), u"Tab reaches picker search"_q);
			Key(Qt::Key_J, Qt::ControlModifier, u"j"_q);
			Check(!f->selector->vimKeymapSearchHasFocus(), u"Ctrl J returns to grid"_q);
			Key(Qt::Key_Backtab, Qt::ShiftModifier);
			Key(Qt::Key_K, Qt::ControlModifier, u"k"_q);
			Check(!f->selector->vimKeymapSearchHasFocus(), u"Ctrl K returns to grid"_q);
			Key(Qt::Key_Tab);
			Key(Qt::Key_Escape);
			Check(!f->selector->vimKeymapSearchHasFocus()
				&& f->panel->isVisible(), u"first Escape returns from search to grid"_q);
			Key(Qt::Key_Escape);
			Check(Ui::InFocusChain(f->caption) && !NormalMode(),
				u"second Escape returns to editable caption"_q);
			const auto before = f->caption->getLastText();
			Key(Qt::Key_J, Qt::NoModifier, u"j"_q);
			Key(Qt::Key_K, Qt::NoModifier, u"k"_q);
			Check(f->caption->getLastText() == before + u"jk"_q,
				u"caption remains editable after closing picker"_q);
			Key(Qt::Key_L, Qt::ControlModifier, u"l"_q);
			Key(Qt::Key_Return);
			Check(f->sends == 0, u"Enter during picker opening cannot send photos"_q);
			f->started = crl::now();
		},
	});
	runner->add({
		.name = u"picker respects a nested dialog"_q,
		.until = [=] { return crl::now() - f->started > 700; },
		.then = [=] {
			if (!f->selector) {
				return;
			}
			Check(Ui::InFocusChain(f->selector), u"Ctrl L reopens the caption picker"_q);
			auto box = Ui::MakeConfirmBox({ .text = u"Nested keyboard fixture"_q });
			f->nested = box.data();
			Core::App().activePrimaryWindow()->sessionController()->show(
				std::move(box), Ui::LayerOption::KeepOther);
			f->started = crl::now();
		},
	});
	runner->add({
		.name = u"nested focus and disabled keymap"_q,
		.until = [=] { return crl::now() - f->started > 700; },
		.then = [=] {
			if (!f->selector || !f->nested) {
				return;
			}
			const auto scope = FindKeyboardScope(f->box->window());
			Check(scope && KeyHandlerInScope(f->nested, scope),
				u"nested dialog owns the active keyboard scope"_q);
			Key(Qt::Key_L, Qt::ControlModifier, u"l"_q);
			Check(!Ui::InFocusChain(f->selector), u"parent picker cannot steal nested dialog keys"_q);
			f->nested->closeBox();
			f->panel->hideFast();
			f->caption->setFocusFast();
			VimKeymapOption.set(false);
			Key(Qt::Key_L, Qt::ControlModifier, u"l"_q);
			Check(f->panel->isHidden(), u"disabled Vim leaves picker shortcut inactive"_q);
			Check(f->sends == 0, u"all picker operations leave photos unsent"_q);
			f->box->closeBox();
		},
	});
}

} // namespace Test
#endif // _DEBUG
