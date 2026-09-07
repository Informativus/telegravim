/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

void TestBuiltinSpellchecker() {
	using namespace Core::VimKeymap::Bindings;
#ifdef Q_OS_MAC
	const auto control = Qt::MetaModifier;
#else // Q_OS_MAC
	const auto control = Qt::ControlModifier;
#endif // Q_OS_MAC
	for (const auto key : { int(Qt::Key_E), 0x0423, 0x0443 }) {
		auto event = QKeyEvent(QEvent::KeyPress, key, control);
		Check(SpellcheckKey(&event), "spelling shortcut accepts Latin and Cyrillic keys");
	}
	for (const auto modifier : {
		Qt::KeyboardModifiers(Qt::NoModifier),
		Qt::KeyboardModifiers(control | Qt::ShiftModifier),
		Qt::KeyboardModifiers(control | Qt::AltModifier),
	}) {
		auto event = QKeyEvent(QEvent::KeyPress, Qt::Key_E, modifier);
		Check(!SpellcheckKey(&event), "spelling shortcut preserves other E commands");
	}
#ifdef Q_OS_MAC
	auto command = QKeyEvent(QEvent::KeyPress, Qt::Key_E, Qt::ControlModifier);
	Check(!SpellcheckKey(&command), "spelling uses physical Control, not Command");
	auto physical = QKeyEvent(QEvent::KeyPress, 0, control, 0, 14, 0);
	Check(SpellcheckKey(&physical), "spelling accepts textless macOS physical E");
#endif // Q_OS_MAC

	auto directory = QTemporaryDir();
	Check(directory.isValid(), "spellcheck uses an isolated profile");
	const auto path = directory.path();
	const auto installed = Spellchecker::InstallBundledDictionaries(path);
	Check(installed.size() == 2, "both bundled dictionaries install without downloading");
	const auto personalPath = path + u"/custom"_q;
	auto personal = QFile(personalPath);
	Check(personal.open(QIODevice::WriteOnly), "personal dictionary fixture is writable");
	Check(personal.write("personal-word") == 13, "personal dictionary fixture is complete");
	personal.close();
	auto damaged = QFile(path + u"/ru_RU/ru_RU.dic"_q);
	Check(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate), "dictionary corruption fixture opens");
	Check(damaged.write("broken") == 6, "dictionary corruption fixture is complete");
	damaged.close();
	Check(Spellchecker::InstallBundledDictionaries(path) == installed,
		"repeated installation repairs incomplete dictionaries");
	Check(personal.open(QIODevice::ReadOnly) && personal.readAll() == "personal-word",
		"bundled installation preserves personal words");
	personal.close();

	Spellchecker::SetWorkingDirPath(path);
	Platform::Spellchecker::UpdateLanguages(installed);
	Check(!Platform::Spellchecker::IsSystemSpellchecker(),
		"every platform uses the bundled backend");
	const auto lookup = [&](const QString &word) {
		auto loop = QEventLoop();
		auto completed = false;
		auto correct = false;
		auto suggestions = std::vector<QString>();
		const auto weak = QPointer<QEventLoop>(&loop);
		Platform::Spellchecker::LookupWord(word, [&, weak](
				bool result,
				std::vector<QString> &&variants) {
			if (!weak) {
				return;
			}
			correct = result;
			suggestions = std::move(variants);
			completed = true;
			loop.quit();
		});
		QTimer::singleShot(5000, &loop, &QEventLoop::quit);
		loop.exec(QEventLoop::ExcludeUserInputEvents);
		Check(completed, "background spelling request completes");
		return std::pair(correct, suggestions);
	};
	// The macOS test executable uses Cocoa and can receive physical input
	// from the desktop while an asynchronous lookup is pending. Exclude
	// that input while waiting, but keep the synthetic events sent below.
	// Hunspell posts its result before the first fence. That result queues
	// Russian ranking, whose second fence then proves the menu is ready.
	const auto flush = [&] {
		auto loop = QEventLoop();
		auto completed = false;
		const auto weak = QPointer<QEventLoop>(&loop);
		Platform::Spellchecker::CheckSpelling(u"привет"_q, [&, weak](bool correct) {
			if (weak) {
				completed = true;
				weak->quit();
			}
		});
		QTimer::singleShot(5000, &loop, &QEventLoop::quit);
		loop.exec(QEventLoop::ExcludeUserInputEvents);
		Check(completed, "queued spelling requests finish before assertions");
		completed = false;
		Spellchecker::SuggestRussianWords({}, {}, [&, weak](auto result) {
			if (weak) {
				completed = true;
				weak->quit();
			}
		});
		QTimer::singleShot(5000, &loop, &QEventLoop::quit);
		loop.exec(QEventLoop::ExcludeUserInputEvents);
		Check(completed, "queued Russian suggestions finish before assertions");
	};
	const auto cases = std::vector<std::pair<QString, QString>>{
		{ u"превет"_q, u"привет"_q },
		{ u"неправельно"_q, u"неправильно"_q },
		{ u"подсвечевалось"_q, u"подсвечивалось"_q },
		{ u"рбаотает"_q, u"работает"_q },
		{ u"ошыбка"_q, u"ошибка"_q },
		{ u"карова"_q, u"корова"_q },
		{ u"сабака"_q, u"собака"_q },
		{ u"speling"_q, u"spelling"_q },
	};
	for (const auto &[wrong, expected] : cases) {
		const auto [correct, variants] = lookup(wrong);
		Check(!correct, "known Russian and English typos are detected");
		Check(ranges::contains(variants, expected), "expected correction appears in suggestions");
		Check(lookup(expected).first, "correct Russian and English words are accepted");
	}
	Check(lookup(u"ёлка"_q).first && lookup(u"елка"_q).first,
		"Russian spelling accepts both yo and e forms");
	const auto improve = [&](const QString &word, std::vector<QString> original) {
		const auto started = crl::now();
		auto loop = QEventLoop();
		auto completed = false;
		auto result = std::vector<QString>();
		const auto weak = QPointer<QEventLoop>(&loop);
		Spellchecker::SuggestRussianWords(word, std::move(original),
			[&, weak](std::vector<QString> suggestions) {
				if (weak) {
					result = std::move(suggestions);
					completed = true;
					weak->quit();
				}
			});
		QTimer::singleShot(5000, &loop, &QEventLoop::quit);
		loop.exec(QEventLoop::ExcludeUserInputEvents);
		Check(completed, "Russian ranking completes off the UI thread");
		if (qEnvironmentVariableIsSet("TDESKTOP_TEST_RUSSIAN_SUGGESTIONS_REPORT")) {
			std::cout << "Russian suggestions (" << (crl::now() - started)
				<< " ms): " << word.toStdString();
			for (const auto &candidate : result) {
				std::cout << " | " << candidate.toStdString();
			}
			std::cout << std::endl;
		}
		return result;
	};
	for (const auto &[wrong, expected] : std::vector<std::pair<QString, QString>>{
		{ u"превет"_q, u"привет"_q },
		{ u"севодня"_q, u"сегодня"_q },
		{ u"зделать"_q, u"сделать"_q },
		{ u"рбаотает"_q, u"работает"_q },
		{ u"пожалуста"_q, u"пожалуйста"_q },
		{ u"спосибо"_q, u"спасибо"_q },
		{ u"неправельно"_q, u"неправильно"_q },
		{ u"конешно"_q, u"конечно"_q },
		{ u"нормальн"_q, u"нормально"_q },
		{ u"сообщениие"_q, u"сообщение"_q },
		{ u"клавиатруа"_q, u"клавиатура"_q },
		{ u"нажимашь"_q, u"нажимаешь"_q },
		{ u"обновлиние"_q, u"обновление"_q },
		{ u"работатет"_q, u"работает"_q },
		{ u"сделат"_q, u"сделать"_q },
		{ u"двикает"_q, u"двигает"_q },
		{ u"хочутся"_q, u"хочется"_q },
	}) {
		const auto result = improve(wrong, lookup(wrong).second);
		const auto first = !result.empty() && result.front() == expected;
		if (!first) {
			std::cout << "Russian ranking: " << wrong.toStdString();
			for (const auto &candidate : result) {
				std::cout << " " << candidate.toStdString();
			}
			std::cout << std::endl;
		}
		Check(first, "a common Russian correction ranks first");
		Check(result.size() <= 8, "Russian suggestions remain a short list");
	}
	const auto expanded = improve(u"появлсяет"_q, lookup(u"появлсяет"_q).second);
	Check(ranges::contains(expanded, u"появляется"_q),
		"Russian retrieval recovers a correction missing from Hunspell");
	for (const auto word : { u"ПРЕВЕТ"_q, u"Превет"_q }) {
		const auto result = improve(word, {});
		Check(!result.empty() && result.front() == (word == word.toUpper()
			? u"ПРИВЕТ"_q : u"Привет"_q), "Russian corrections retain capitalization");
	}
	for (const auto word : { u"speling"_q, u"ghbdtn"_q, u"привeт"_q,
		u"https://пример.рф"_q, QString(100, QChar(u'а')), QString() }) {
		const auto original = std::vector<QString>{ u"unchanged"_q };
		Check(improve(word, original) == original,
			"Russian ranking preserves other scripts, links and oversized words");
	}
	{
		const auto text = u"привет spelling, ошыбка speling!"_q;
		auto loop = QEventLoop();
		const auto weak = QPointer<QEventLoop>(&loop);
		auto found = false;
		Platform::Spellchecker::CheckSpellingText(text, [&, weak](
				MisspelledWords &&ranges) {
			if (!weak) {
				return;
			}
			found = ranges == MisspelledWords{
				{ int(text.indexOf(u"ошыбка"_q)), 6 },
				{ int(text.indexOf(u"speling"_q)), 7 },
			};
			loop.quit();
		});
		QTimer::singleShot(5000, &loop, &QEventLoop::quit);
		loop.exec(QEventLoop::ExcludeUserInputEvents);
		Check(found, "mixed Russian and English text marks only the misspelled words");
	}
	Platform::Spellchecker::AddWord(u"телегравимслово"_q);
	Check(lookup(u"телегравимслово"_q).first, "adding a personal word takes effect");
	Platform::Spellchecker::RemoveWord(u"телегравимслово"_q);
	Check(!lookup(u"телегравимслово"_q).first, "removing a personal word takes effect");

	auto root = QWidget();
	root.setFocusPolicy(Qt::StrongFocus);
	auto field = Ui::InputField(&root, st::defaultInputField, rpl::single(QString()));
	field.resize(400, 100);
	root.resize(420, 140);
	auto enabled = rpl::variable<bool>(true);
	const auto highlighter = new Spellchecker::SpellingHighlighter(&field, enabled.value());
	Spellchecker::InitSuggestionsMenu(&field, highlighter);
	root.show();
	root.activateWindow();
	QApplication::setActiveWindow(&root);
	root.setFocus();
	QGuiApplication::sync();
	const auto setText = [&](const QString &text) {
		field.setTextWithTags({ text, {} });
		auto cursor = field.textCursor();
		cursor.setPosition(2);
		field.setTextCursor(cursor);
		highlighter->checkCurrentText();
	};
	const auto visibleMenu = [&]() -> Ui::PopupMenu* {
		for (const auto child : field.children()) {
			if (const auto menu = dynamic_cast<Ui::PopupMenu*>(child)) {
				if (menu->isVisible()) {
					return menu;
				}
			}
		}
		return nullptr;
	};
	setText(u"ошыбка"_q);
	Check(Spellchecker::ShowSuggestionsMenu(&field), "view-mode field opens spelling suggestions");
	// Qt may report window-level changes after this request has started,
	// while the focused view and its active window stay the same. Such a
	// notification must not cancel the pending spelling lookup. Send it
	// before pumping the main queue, so asynchronous completion cannot
	// win the race and conceal cancellation. The later field-focus case
	// separately verifies that a real widget focus change still cancels.
	auto focusReceiver = QObject();
	auto focusNotice = QFocusEvent(QEvent::FocusIn);
	QApplication::sendEvent(&focusReceiver, &focusNotice);
	auto otherWindowDeactivated = QEvent(QEvent::WindowDeactivate);
	QApplication::sendEvent(&focusReceiver, &otherWindowDeactivated);
	Check(root.hasFocus(), "unrelated window notifications preserve the focused view widget");
	flush();
	const auto popup = visibleMenu();
	Check(popup && popup->isVisible(), "suggestions appear with focus outside the composer");
	if (popup && popup->isVisible()) {
		auto choose = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
		QApplication::sendEvent(popup, &choose);
		DrainMainQueue();
		Check(field.getTextWithTags().text == u"ошибка"_q, "Enter replaces the misspelled word");
		field.rawTextEdit()->undo();
		Check(field.getTextWithTags().text == u"ошыбка"_q, "undo restores the original word");
	}
	for (const auto reverse : { false, true }) {
		setText(u"неправельно"_q);
		root.activateWindow();
		QApplication::setActiveWindow(&root);
		root.setFocus();
		QGuiApplication::sync();
		Check(Spellchecker::ShowSuggestionsMenu(&field), "multi-suggestion menu starts");
		flush();
		const auto menu = visibleMenu();
		Check(menu != nullptr, "multi-suggestion menu appears");
		if (menu) {
			const auto &actions = menu->menu()->actions();
			const auto expected = actions[reverse ? 3 : 4]->text();
			auto down = QKeyEvent(QEvent::KeyPress, 0x041e, Qt::NoModifier);
			QApplication::sendEvent(menu, &down);
			if (reverse) {
				auto up = QKeyEvent(QEvent::KeyPress, Qt::Key_K, Qt::NoModifier);
				QApplication::sendEvent(menu, &up);
			}
			auto enter = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
			QApplication::sendEvent(menu, &enter);
			DrainMainQueue();
			Check(field.getTextWithTags().text == expected,
				"Latin and Cyrillic j/k select the requested correction");
		}
	}
	setText(u"появлсяет рядом"_q);
	root.activateWindow();
	QApplication::setActiveWindow(&root);
	root.setFocus();
	QGuiApplication::sync();
	Check(Spellchecker::ShowSuggestionsMenu(&field), "expanded correction menu starts");
	flush();
	const auto expandedMenu = visibleMenu();
	Check(expandedMenu != nullptr, "expanded correction menu appears");
	if (expandedMenu) {
		const auto &actions = expandedMenu->menu()->actions();
		const auto target = ranges::find(actions, u"появляется"_q, &QAction::text);
		Check(target != actions.end(), "menu includes a newly retrieved correction");
		if (target != actions.end()) {
			for (auto i = 3; i < target - actions.begin(); ++i) {
				auto down = QKeyEvent(QEvent::KeyPress, Qt::Key_J, Qt::NoModifier);
				QApplication::sendEvent(expandedMenu, &down);
			}
			auto enter = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
			QApplication::sendEvent(expandedMenu, &enter);
			DrainMainQueue();
			Check(field.getTextWithTags().text == u"появляется рядом"_q,
				"new corrections replace only the selected word");
			field.rawTextEdit()->undo();
			Check(field.getTextWithTags().text == u"появлсяет рядом"_q,
				"new corrections remain a single undo operation");
		}
	}
	setText(u"ошыбка"_q);
	root.activateWindow();
	QApplication::setActiveWindow(&root);
	root.setFocus();
	QGuiApplication::sync();
	Check(Spellchecker::ShowSuggestionsMenu(&field), "cancellable menu starts");
	flush();
	if (const auto menu = visibleMenu()) {
		auto escape = QKeyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
		QApplication::sendEvent(menu, &escape);
		auto loop = QEventLoop();
		QTimer::singleShot(field.st().menu.showDuration + 50, &loop, &QEventLoop::quit);
		loop.exec(QEventLoop::ExcludeUserInputEvents);
		DrainMainQueue();
	}
	Check(!visibleMenu() && field.getTextWithTags().text == u"ошыбка"_q,
		"Escape closes suggestions without changing the draft");
	setText(u"ошыбка"_q);
	Check(Spellchecker::ShowSuggestionsMenu(&field), "second lookup starts");
	setText(u"другой текст"_q);
	flush();
	auto staleVisible = false;
	for (const auto child : field.children()) {
		if (const auto menu = dynamic_cast<Ui::PopupMenu*>(child)) {
			staleVisible |= menu->isVisible();
		}
	}
	Check(!staleVisible, "editing the draft cancels stale suggestions");
	setText(u"ошыбка"_q);
	root.activateWindow();
	QApplication::setActiveWindow(&root);
	root.setFocus();
	QGuiApplication::sync();
	Check(Spellchecker::ShowSuggestionsMenu(&field), "lookup before focus change starts");
	field.setFocus();
	flush();
	Check(!visibleMenu(), "focus changes cancel delayed suggestions");
	root.activateWindow();
	QApplication::setActiveWindow(&root);
	root.setFocus();
	QGuiApplication::sync();
	Check(Spellchecker::ShowSuggestionsMenu(&field), "lookup before Escape starts");
	auto cancelLookup = QKeyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	QApplication::sendEvent(&root, &cancelLookup);
	flush();
	Check(!visibleMenu(), "Escape cancels pending suggestions");
	root.activateWindow();
	QApplication::setActiveWindow(&root);
	root.setFocus();
	QGuiApplication::sync();
	Check(Spellchecker::ShowSuggestionsMenu(&field), "lookup before window deactivation starts");
	auto windowDeactivated = QEvent(QEvent::WindowDeactivate);
	QApplication::sendEvent(&root, &windowDeactivated);
	flush();
	Check(!visibleMenu(), "the owning window deactivation cancels delayed suggestions");

	enabled = false;
	Check(!Spellchecker::ShowSuggestionsMenu(&field), "disabled spellchecking stays disabled");
	Platform::Spellchecker::UpdateLanguages({});
	lookup(u"cleanup"_q);
}
