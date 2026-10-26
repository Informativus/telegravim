/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

void TestVimKeyLogPrivacy() {
	using namespace Core::VimKeymap;
	auto root = QWidget();
	root.setFocusPolicy(Qt::StrongFocus);
	root.show();
	root.activateWindow();
	QApplication::setActiveWindow(&root);
	root.setFocus();
	QApplication::processEvents();
	auto log = KeyEventLog();
	auto key = QKeyEvent(QEvent::KeyPress, Qt::Key_J, Qt::NoModifier, u"j"_q);
	log.record(&key, u"chat scroll"_q);
	Check(log.text().contains(u"chat scroll"_q), "navigation diagnostics remain available");
	log.clear();
	Check(log.text().isEmpty(), "locking and logout can erase diagnostic history");

	auto input = QLineEdit(&root);
	input.show();
	for (const auto mode : {
		QLineEdit::Normal,
		QLineEdit::Password,
		QLineEdit::PasswordEchoOnEdit,
		QLineEdit::NoEcho,
	}) {
		input.setEchoMode(mode);
		input.setFocus();
		QApplication::processEvents();
		Check(QApplication::focusWidget() == &input,
			"privacy regression exercise owns the focused input");
		Check(KeyboardInputActive(&input), "text and password receivers are private");
		const auto generation = log.generation();
		for (const auto character : u"aP9j/%"_q) {
			auto event = QKeyEvent(QEvent::KeyPress, character.toUpper().unicode(),
				Qt::ShiftModifier, QString(character));
			log.record(&event, u"input ignored"_q);
		}
		Check(log.text().isEmpty() && log.generation() == generation,
			"typing never adds password fragments or partial input to diagnostics");
	}
	root.setFocus();
	auto rich = QTextEdit(&root);
	auto plain = QPlainTextEdit(&root);
	auto receiver = QObject(rich.viewport());
	Check(KeyboardInputActive(&receiver), "nested rich text receivers are private");
	Check(KeyboardInputActive(plain.viewport()), "JSON and plain text receivers are private");
	auto sensitive = QWidget(&root);
	sensitive.setInputMethodHints(Qt::ImhSensitiveData);
	Check(KeyboardInputActive(&sensitive), "custom sensitive inputs are private");
	sensitive.setInputMethodHints(Qt::ImhHiddenText);
	Check(KeyboardInputActive(&sensitive), "custom hidden inputs are private");
	Check(!KeyboardInputActive(&root), "ordinary navigation is not classified as typing");

	log.setSuppressed(true);
	log.record(&key, u"focus changed while handling input"_q);
	Check(log.text().isEmpty(), "input stays private after a handler moves focus");
	log.setSuppressed(false);
	log.recordCommand(u"Copy selection"_q, u"text copied to clipboard"_q);
	Check(log.text().contains(u"Copy selection"_q),
		"named command diagnostics do not depend on recording typed characters");
	log.clear();

#ifdef Q_OS_MAC
	auto command = QKeyEvent(QEvent::KeyPress, Qt::Key_V,
		Qt::ControlModifier | Qt::ShiftModifier, 0, 9, 0, u"V"_q);
	log.record(&command, u"select message text"_q);
	Check(log.text().contains(u"Cmd+Shift+V -> select message text"_q),
		"macOS Command is labeled using its physical modifier");
	log.clear();
	auto prefix = QKeyEvent(QEvent::KeyPress, Qt::Key_A,
		Qt::MetaModifier, 0, 0, 0, u"a"_q);
	log.record(&prefix, u"pane prefix"_q);
	Check(log.text().contains(u"Ctrl+A -> pane prefix"_q),
		"macOS Control is distinct from Command in diagnostics");
	log.clear();
#endif // Q_OS_MAC
	auto tab = QKeyEvent(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
	log.record(&tab, u"focus next"_q);
	Check(log.text().contains(u"Tab -> focus next"_q),
		"synthetic special keys do not become the macOS A key");
	log.clear();
	auto injected = QKeyEvent(QEvent::KeyPress, Qt::Key_unknown,
		Qt::NoModifier, u"private multi-character payload\nforged log entry"_q);
	log.record(&injected, u"navigation"_q);
	Check(!log.text().contains(u"private"_q) && !log.text().contains('\n'),
		"multi-character input cannot inject diagnostic contents or new lines");
	log.clear();
	auto control = QKeyEvent(QEvent::KeyPress, Qt::Key_Return,
		Qt::NoModifier, u"\n"_q);
	log.record(&control, u"navigation"_q);
	Check(!log.text().contains('\n'),
		"control characters cannot split diagnostic entries");
	log.clear();
	for (auto i = 0; i != 250; ++i) {
		log.record(&key, u"chat scroll"_q);
	}
	Check(log.text().split('\n').size() == 200, "diagnostics have bounded retention");
	log.clear();
}

void TestLocalSocketSecurity() {
	auto socket = QLocalSocket();
	Check(!Core::LocalSocketPeerAllowed(&socket),
		"unconnected local clients have no trusted identity");
	auto directory = QTemporaryDir();
	auto server = QLocalServer();
	server.setSocketOptions(QLocalServer::UserAccessOption);
#ifdef Q_OS_WIN
	const auto path = u"telegravim-peer-check-"_q
		+ QDir(directory.path()).dirName();
#else // Q_OS_WIN
	const auto path = directory.filePath(u"peer-check"_q);
#endif // !Q_OS_WIN
	Check(server.listen(path), "restricted local socket can listen");
	socket.connectToServer(path);
	Check(socket.waitForConnected(1000), "same-user local client can connect");
	Check(server.waitForNewConnection(1000), "local server receives the client");
	const auto peer = server.nextPendingConnection();
	Check(peer && Core::LocalSocketPeerAllowed(peer),
		"kernel credentials allow the current user");
	if (peer) {
		peer->abort();
		Check(!Core::LocalSocketPeerAllowed(peer),
			"a closed local connection cannot retain authorization");
	}
	socket.abort();
}
