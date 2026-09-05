/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

void TestVimConfig() {
	using namespace Core::VimKeymap;
	const auto encode = [](const QJsonObject &object) { return QJsonDocument(object).toJson(); };
	const auto saved = ConfigSnapshot();
	const auto fields = ConfigOptions();
	const auto defaults = ConfigSnapshot(true);
	Check(fields.size() == 37 && defaults.size() == 37,
		"JSON includes all 37 real Vim options, including undo and redo");
	Check(ValidateConfig(encode(defaults)).error.isEmpty(), "actual defaults are valid JSON config");
	const auto schema = ConfigSchema().value(u"properties"_q).toObject();
	const auto guide = ConfigGuide();
	auto changed = defaults;
	for (const auto &field : fields) {
		const auto key = QString::fromLatin1(field.id);
		Check(schema.contains(key) && !field.description.isEmpty()
			&& guide.contains(u"### `%1`"_q.arg(key)), "every option has a schema and a guide entry");
		Check(!ValidateConfig(encode({ { key, QJsonValue::Null } })).error.isEmpty(),
			"null is rejected for every option");
		const auto value = defaults.value(key);
		if (field.bounds) {
			Check(ValidateConfig(encode({ { key, field.bounds->min } })).error.isEmpty()
				&& ValidateConfig(encode({ { key, field.bounds->max } })).error.isEmpty(),
				"integer boundaries agree with the UI bounds");
			for (const auto invalid : { double(field.bounds->min - 1), double(field.bounds->max + 1), 1.5 }) {
				Check(!ValidateConfig(encode({ { key, invalid } })).error.isEmpty(),
					"out of range and fractional settings are rejected");
			}
			changed.insert(key, field.bounds->min);
		} else if (value.isBool()) {
			changed.insert(key, !value.toBool());
			Check(!ValidateConfig(encode({ { key, u"true"_q } })).error.isEmpty(),
				"boolean strings are not silently converted");
		} else if (!field.choices.empty()) {
			for (const auto &choice : field.choices) {
				Check(ValidateConfig(encode({ { key, choice } })).error.isEmpty(), "all UI choices are accepted");
			}
			changed.insert(key, field.choices.back());
			Check(!ValidateConfig(encode({ { key, u"INVALID"_q } })).error.isEmpty(), "unknown choices are rejected");
		} else {
			changed.insert(key, u"Alt+Shift+X"_q);
			Check(ValidateConfig(encode({ { key, QString() } })).error.isEmpty(), "empty string disables a binding");
			Check(!ValidateConfig(encode({ { key, QString(1025, 'x') } })).error.isEmpty(), "long bindings are rejected");
		}
	}
	for (const auto &invalid : { u"Ctrl+"_q, u"Ctrl++V"_q, u"F1"_q, u"gg"_q, u"j,,k"_q, u"Magic+J"_q }) {
		Check(!Bindings::ValidBindings(invalid), "invalid shortcut syntax cannot silently disable commands");
	}
	Check(Bindings::ValidBindings(u"Ctrl+Shift+V, Ctrl+Shift+\u043c, Esc, ?, Shift+Tab"_q),
		"modifier aliases, punctuation and Cyrillic shortcuts remain valid");
	static auto unrelated = base::options::option<int>({ .id = "test-unrelated-config-option", .defaultValue = 23 });
	auto invalid = changed;
	invalid.insert(u"not-a-vim-setting"_q, true);
	Check(!ApplyConfig(encode(invalid)).isEmpty() && ConfigSnapshot() == saved && unrelated.value() == 23,
		"unknown key prevents all changes and leaves unrelated settings untouched");
	invalid = changed;
	invalid.insert(u"vim-keymap-scroll-step"_q, u"wrong"_q);
	Check(!ApplyConfig(encode(invalid)).isEmpty() && ConfigSnapshot() == saved,
		"invalid value rolls back the entire document before mutation");
	Check(ApplyConfig("{}").isEmpty() && ConfigSnapshot() == saved, "empty object preserves every setting");
	Check(ApplyConfig(encode(changed)).isEmpty() && ConfigSnapshot() == changed && unrelated.value() == 23,
		"all 37 settings apply through the real option registry");
	Check(ApplyConfig("{\"vim-keymap-scroll-step\":360}").isEmpty()
		&& VimKeymapScrollStepOption.value() == 360 && VimKeymapDefaultsMigratedOption.value(),
		"explicit values survive legacy-default migration");
	const auto persisted = QJsonDocument::fromJson(base::options::serialize().toUtf8()).object();
	Check(persisted.value(u"vim-keymap-scroll-step"_q).toInt() == 360
		&& persisted.value(u"vim-keymap-defaults-migrated"_q).toBool(),
		"explicit values and migration marker use the existing persistence format");
	Check(ApplyConfig(encode(saved)).isEmpty(), "restore original settings after import tests");
	for (const auto &invalidJson : { QByteArray("[]"), QByteArray("null"), QByteArray("{\"vim-keymap\":true,}"), QByteArray(65537, ' ') }) {
		Check(!ApplyConfig(invalidJson).isEmpty() && ConfigSnapshot() == saved, "invalid documents never apply partially");
	}
	const auto parse = ValidateConfig("{\n  \"vim-keymap\":\n  nope\n}");
	Check(parse.line == 3 && parse.column > 0 && !parse.error.isEmpty(), "syntax errors identify line and column");
}

void TestVimConfigEditor() {
	using namespace Core::VimKeymap;
	const auto saved = ConfigSnapshot();
	auto window = QWidget();
	window.setAttribute(Qt::WA_DontShowOnScreen);
	window.setAutoFillBackground(true);
	window.resize(720, 850);
	auto pendingDiscard = Fn<void()>();
	auto confirmations = 0;
	auto guides = 0;
	auto editor = Settings::VimKeymapEditor(&window,
		[](not_null<Ui::VerticalLayout*> layout) {
			layout->add(object_ptr<Ui::AbstractButton>(layout));
		},
		[&](Fn<void()> done) { ++confirmations; pendingDiscard = std::move(done); },
		[&] { ++guides; });
	editor.resizeToWidth(400);
	window.show();
	QApplication::setActiveWindow(&window);
	const auto find = [&](const QString &name) { return editor.findChild<QWidget*>(name); };
	const auto click = [&](const QString &name) {
		const auto target = find(name);
		target->setFocus(Qt::TabFocusReason);
		DrainMainQueue();
		if (name.startsWith(u"vim-mode-"_q)) {
			return;
		}
		auto press = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
		auto release = QKeyEvent(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
		QApplication::sendEvent(target, &press);
		QApplication::sendEvent(target, &release);
		DrainMainQueue();
	};
	click(u"vim-mode-json"_q);
	const auto text = editor.findChild<QPlainTextEdit*>(u"vim-json-editor"_q);
	Check(text && text->isVisible() && !editor.dirty(), "JSON mode opens with a clean live snapshot");
	Check(QJsonDocument::fromJson(text->toPlainText().toUtf8()).object() == saved,
		"JSON mode reflects current UI values");
	const auto navigation = KeyboardNavigation::Get(&editor);
	const auto targets = KeyboardFocusTargets(&editor);
	Check(std::find(targets.begin(), targets.end(), text) != targets.end(), "Tab includes the actual JSON editor");
	navigation->focusTarget(text);
	Check(KeyboardScopeHasTextInput(&editor, text), "JSON typing bypasses Vim navigation handlers");
	text->selectAll();
	auto j = QKeyEvent(QEvent::KeyPress, Qt::Key_J, Qt::NoModifier, u"j"_q);
	QApplication::sendEvent(text, &j);
	Check(text->toPlainText() == u"j"_q, "j inserts text inside JSON");
	click(u"vim-json-apply"_q);
	Check(editor.dirty() && ConfigSnapshot() == saved, "invalid JSON remains editable without changing settings");
	click(u"vim-mode-ui"_q);
	Check(confirmations == 1 && text->isVisible() && editor.dirty(), "switching mode prompts before discarding JSON");
	Check(text->hasFocus(), "cancelled mode switches return to the editor instead of reopening confirmation");
	pendingDiscard = nullptr;
	Check(editor.dirty(), "cancelling the confirmation preserves draft text");
	text->setPlainText(u"{\"vim-keymap-scroll-step\":200}"_q);
	click(u"vim-json-apply"_q);
	Check(VimKeymapScrollStepOption.value() == 200 && !editor.dirty(), "Apply updates the same live setting used by UI");
	VimKeymapScrollStepOption.set(240);
	Check(QJsonDocument::fromJson(text->toPlainText().toUtf8()).object().value(u"vim-keymap-scroll-step"_q) == 240,
		"clean JSON refreshes when settings change elsewhere");
	text->setPlainText(u"{\"vim-keymap-scroll-step\":260}"_q);
	VimKeymapScrollStepOption.set(280);
	click(u"vim-json-apply"_q);
	Check(editor.dirty() && VimKeymapScrollStepOption.value() == 280,
		"stale drafts cannot overwrite settings changed in another window");
	click(u"vim-json-reload"_q);
	Check(confirmations == 2 && editor.dirty(), "reread also protects unsaved changes");
	if (pendingDiscard) {
		base::take(pendingDiscard)();
	}
	Check(!editor.dirty() && text->toPlainText().contains(u"280"_q), "confirmed reread loads current values");
	text->setPlainText(u"{}"_q);
	auto closed = false;
	editor.checkBeforeClose([&] { closed = true; });
	Check(!closed && confirmations == 3, "closing the settings waits for dirty confirmation");
	if (pendingDiscard) {
		base::take(pendingDiscard)();
	}
	Check(closed && !editor.dirty(), "confirmed discard permits closing");
	click(u"vim-guide"_q);
	Check(guides == 1, "guide button uses its local guide callback");
	const auto output = qEnvironmentVariable("VIM_KEYMAP_FOCUS_SNAPSHOTS");
	for (const auto width : { 320, 400, 720 }) {
		editor.resizeToWidth(width);
		DrainMainQueue();
		Check(text->width() <= width && text->height() > 0 && editor.height() < window.height(),
			"JSON editor fits narrow and wide settings panels");
		if (!output.isEmpty()) {
			Check(editor.grab().save(output + u"/json-%1.png"_q.arg(width)), "JSON editor screenshot saved");
		}
	}
	click(u"vim-mode-ui"_q);
	Check(!text->isVisible(), "clean JSON switches directly back to UI");
	auto guide = Settings::CreateVimKeymapGuide(&window);
	guide->resize(640, 480);
	guide->show();
	const auto browser = guide->findChild<QTextBrowser*>();
	browser->setFocus();
	DrainMainQueue();
	Check(browser->isReadOnly() && !KeyboardScopeHasTextInput(guide, browser), "guide allows scrolling rather than text-input mode");
	Check(KeyboardNavigation::Get(guide)->scroll(100, false, 0)
		&& browser->verticalScrollBar()->value() > 0, "j/k scrolling reaches the local guide text");
	Check(browser->toPlainText().contains(u"vim-keymap-key-redo"_q) && !browser->openExternalLinks(),
		"offline guide includes all parameter descriptions without external navigation");
	if (!output.isEmpty()) {
		Check(guide->grab().save(output + u"/json-guide.png"_q), "offline guide screenshot saved");
	}
	Check(ApplyConfig(QJsonDocument(saved).toJson()).isEmpty(), "restore original values after editor tests");
}

void TestVimConfigDocuments() {
	using namespace Core::VimKeymap;
	const auto files = std::vector<std::pair<QString, QByteArray>>{
		{ u"vim-keymap.md"_q, ConfigGuide().trimmed().toUtf8() + '\n' },
		{ u"vim-keymap.example.json"_q, QJsonDocument(ConfigSnapshot(true)).toJson() },
		{ u"vim-keymap.schema.json"_q, QJsonDocument(ConfigSchema()).toJson() },
	};
	for (const auto &[name, expected] : files) {
		const auto path = QString::fromUtf8(VIM_KEYMAP_DOCS_DIR) + '/' + name;
		if (qEnvironmentVariableIsSet("VIM_KEYMAP_GENERATE_DOCS")) {
			auto file = QSaveFile(path);
			Check(file.open(QIODevice::WriteOnly) && file.write(expected) == expected.size() && file.commit(),
				"generate guide, example and schema from the real option registry");
		}
		auto file = QFile(path);
		Check(file.open(QIODevice::ReadOnly) && file.readAll() == expected,
			"checked-in documentation exactly matches current Vim settings and defaults");
	}
}
