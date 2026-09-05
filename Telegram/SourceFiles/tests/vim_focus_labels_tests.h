/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

void TestVimFocusLabels() {
	class Label final : public Ui::FlatLabel {
	public:
		using Ui::FlatLabel::FlatLabel;
		using Ui::FlatLabel::contextMenuEvent;
		using Ui::FlatLabel::keyPressEvent;

	};

	auto root = Ui::RpWidget(nullptr);
	root.setAttribute(Qt::WA_DontShowOnScreen);
	root.resize(640, 480);
	root.show();
	QApplication::setActiveWindow(&root);
	const auto drain = [] {
		for (auto i = 0; i != 3; ++i) {
			auto loop = QEventLoop();
			auto completed = false;
			crl::on_main(&loop, [&] {
				completed = true;
				loop.quit();
			});
			QTimer::singleShot(1000, &loop, &QEventLoop::quit);
			loop.exec();
			Check(completed, "queued label actions finish before assertions");
		}
	};
	const auto focus = [&](Label &label) {
		label.resizeToWidth(300);
		label.show();
		QApplication::setActiveWindow(&root);
		label.setFocus(Qt::TabFocusReason);
		Check(label.hasFocus(), "keyboard label receives explicit focus");
	};
	const auto send = [](
			Label &label,
			int key,
			Qt::KeyboardModifiers modifiers = Qt::NoModifier,
			bool repeat = false) {
		auto event = QKeyEvent(QEvent::KeyPress, key, modifiers, {}, repeat);
		label.keyPressEvent(&event);
		return event.isAccepted();
	};

	auto empty = Label(&root);
	empty.setSelectable(true);
	Check(!empty.keyboardFocusAvailable(), "empty selectable label is not a target");
	auto plain = Label(&root, u"Phone 123"_q);
	Check(!plain.keyboardFocusAvailable(), "static text is not a target");
	plain.setClickHandlerFilter([](const auto &, auto) { return false; });
	plain.setContextMenuHook([](auto) {});
	Check(!plain.keyboardFocusAvailable(), "callbacks alone do not make a link");
	plain.setSelectable(true);
	Check(plain.keyboardFocusAvailable(), "selectable text is a target");
	Check(plain.focusPolicy() == Qt::NoFocus,
		"label leaves focus policy to the navigation controller");
	focus(plain);
	Check(!send(plain, Qt::Key_F), "label does not consume typing or focus hints");
	Check(!send(plain, Qt::Key_Escape), "label leaves escape to its surface");
	Check(!send(plain, Qt::Key_Return), "plain text has no enter action");
	Check(!send(plain, Qt::Key_A, Qt::AltModifier),
		"modified typing does not select text");

	auto savedClipboard = std::make_unique<QMimeData>();
	if (const auto original = QApplication::clipboard()->mimeData()) {
		for (const auto &format : original->formats()) {
			savedClipboard->setData(format, original->data(format));
		}
	}
	Check(send(plain, Qt::Key_A, Qt::ControlModifier),
		"native select-all selects label text");
	Check(send(plain, Qt::Key_C, Qt::ControlModifier),
		"native copy handles selected label text");
	Check(QApplication::clipboard()->text() == u"Phone 123"_q,
		"select-all and copy preserve plain text");

	auto linked = Label(&root);
	linked.setMarkedText(Ui::Text::Link(u"Open"_q, 1));
	linked.setLink(1, nullptr);
	Check(!linked.keyboardFocusAvailable(), "empty link slot is not a target");
	auto activated = 0;
	const auto actual = std::make_shared<LambdaClickHandler>([&] {
		++activated;
	});
	linked.setLink(1, actual);
	Check(linked.keyboardFocusAvailable(), "actual link is a target");
	focus(linked);
	const auto foreign = std::make_shared<LambdaClickHandler>([] {});
	auto text = Ui::Text::String();
	text.setMarkedText(st::defaultFlatLabel.style, Ui::Text::Link(u"Third"_q, 3));
	text.setLink(1, actual);
	text.setLink(3, foreign);
	Check(text.distinctLinks() == std::vector<ClickHandlerPtr>{ foreign },
		"semantic links exclude populated slots not referenced by text blocks");
	text.setLink(3, nullptr);
	Check(text.distinctLinks().empty(), "semantic links exclude null handlers");
	text.setMarkedText(st::defaultFlatLabel.style,
		Ui::Text::Link(u"One"_q, 1)
			.append(Ui::Text::Link(u"Two"_q, 2))
			.append(Ui::Text::Link(u"One"_q, 3)));
	text.setLink(1, actual);
	text.setLink(2, foreign);
	text.setLink(3, actual);
	Check(text.distinctLinks() == std::vector<ClickHandlerPtr>{ actual, foreign },
		"semantic links deduplicate handlers without losing middle actions");
	text.setMarkedText(
		st::defaultFlatLabel.style,
		Ui::Text::Wrapped(Ui::Text::Link(u"Hidden"_q, 1), EntityType::Spoiler),
		kMarkupTextOptions,
		{ .repaint = [] {} });
	text.setLink(1, actual);
	Check(text.distinctLinks().empty(), "unrevealed spoiler hides its link action");
	text.setSpoilerRevealed(true, anim::type::instant);
	Check(text.distinctLinks() == std::vector<ClickHandlerPtr>{ actual },
		"revealed spoiler exposes its actual link action");
	Check(ClickHandler::setActive(foreign), "test establishes unrelated hover");
	auto filtered = 0;
	linked.setClickHandlerFilter([&](const auto &link, auto button) {
		Check(link == actual && button == Qt::LeftButton,
			"enter passes this label's actual link to its filter");
		++filtered;
		return false;
	});
	Check(send(linked, Qt::Key_Return), "enter handles an unambiguous link");
	drain();
	Check(filtered == 1 && activated == 0, "link filter can suppress activation");
	linked.setClickHandlerFilter([&](const auto &, auto) {
		++filtered;
		return true;
	});
	Check(send(linked, Qt::Key_Enter, Qt::KeypadModifier),
		"keypad enter handles an unambiguous link");
	drain();
	Check(filtered == 2 && activated == 1, "permitted link activates once");
	Check(send(linked, Qt::Key_Return, Qt::NoModifier, true),
		"repeat enter is consumed without another activation");
	drain();
	Check(filtered == 2 && activated == 1, "repeat enter does not call the filter");
	Check(!send(linked, Qt::Key_Return, Qt::ControlModifier),
		"modified enter is not a link action");

	linked.setMarkedText(Ui::Text::Link(u"One"_q, 1)
		.append(u" and "_q).append(Ui::Text::Link(u"Two"_q, 2))
		.append(u" then "_q).append(Ui::Text::Link(u"One"_q, 3)));
	linked.setLink(1, actual);
	linked.setLink(2, foreign);
	linked.setLink(3, actual);
	Check(linked.keyboardFocusAvailable(), "multiple links remain focusable");
	Check(!send(linked, Qt::Key_Return),
		"matching first and last links do not hide an ambiguous middle link");
	drain();
	Check(filtered == 2 && activated == 1, "ambiguous links have no side effects");
	linked.setLink(2, actual);
	Check(send(linked, Qt::Key_Return),
		"multiple spans of the same handler have one unambiguous action");
	drain();
	Check(activated == 2, "shared link handler activates once");

	auto menuCalls = 0;
	auto menuSelection = false;
	auto menuLink = ClickHandlerPtr();
	auto menu = QPointer<Ui::PopupMenu>();
	auto submenu = QPointer<Ui::PopupMenu>();
	plain.setContextMenuHook([&](Ui::FlatLabel::ContextMenuRequest request) {
		++menuCalls;
		menu = request.menu.get();
		menu->setAttribute(Qt::WA_DontShowOnScreen);
		menuSelection = request.fullSelection && request.uponSelection;
		menuLink = request.link;
		plain.fillContextMenu(request);
		auto nested = std::make_unique<Ui::PopupMenu>(menu.data());
		nested->setAttribute(Qt::WA_DontShowOnScreen);
		nested->addAction(u"Nested action"_q, [] {});
		submenu = nested.get();
		menu->addAction(u"Nested"_q, std::move(nested));
	});
	focus(plain);
	Check(send(plain, Qt::Key_A, Qt::ControlModifier),
		"plain label selects text before opening its menu");
	auto context = QContextMenuEvent(
		QContextMenuEvent::Keyboard,
		QPoint(-1000, -1000),
		QPoint(-1000, -1000));
	context.ignore();
	plain.contextMenuEvent(&context);
	Check(context.isAccepted() && menu && submenu && menuCalls == 1,
		"keyboard menu uses the existing hook and submenu ownership");
	Check(menuSelection && !menuLink,
		"keyboard menu uses selected text without unrelated hovered link");
	if (menu) {
		const auto position = menu->pos();
		Check(menu->prepareGeometryFor(plain.mapToGlobal(plain.rect().center())),
			"label anchor produces valid menu geometry");
		Check(menu->pos() == position,
			"keyboard menu is anchored at label, ignoring supplied mouse position");
		auto out = QFocusEvent(QEvent::FocusOut, Qt::PopupFocusReason);
		QApplication::sendEvent(&plain, &out);
		Check(menu && submenu, "focus-out preserves menu and nested menu lifetime");
		QApplication::clipboard()->setText(u"unchanged"_q);
		menu->actions().front()->trigger();
		drain();
		Check(QApplication::clipboard()->text() == u"Phone 123"_q,
			"menu copies the selection saved on focus-out");
		if (menu) {
			menu->hideMenu(true);
		}
	}
	drain();
	QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	focus(plain);
	Check(send(plain, Qt::Key_F10, Qt::ShiftModifier),
		"shift-F10 opens the focused label's keyboard menu");
	if (menu) {
		menu->hideMenu(true);
	}
	drain();
	QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	focus(plain);
	Check(send(plain, Qt::Key_Menu), "menu key opens the focused label's menu");
	if (menu) {
		menu->hideMenu(true);
	}
	drain();
	QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

	auto disposable = std::make_unique<Label>(&root);
	disposable->setMarkedText(Ui::Text::Link(u"Close"_q, 1));
	disposable->setLink(1, actual);
	disposable->setClickHandlerFilter([&](const auto &, auto) {
		disposable.reset();
		return true;
	});
	focus(*disposable);
	Check(send(*disposable, Qt::Key_Return), "enter queues guarded activation");
	drain();
	Check(!disposable && activated == 2,
		"filter may destroy label without running its default action");

	QApplication::clipboard()->setMimeData(savedClipboard.release());
}
