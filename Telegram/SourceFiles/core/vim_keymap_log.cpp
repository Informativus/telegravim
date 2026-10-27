/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_log.h"

#include "core/vim_keymap_widgets.h"

#include <QtGui/QKeyEvent>

namespace Core::VimKeymap {
namespace {

[[nodiscard]] QString MacPhysicalLatinKey(not_null<QKeyEvent*> e) {
#ifdef Q_OS_MAC
	switch (e->nativeVirtualKey()) {
	case 0: return u"A"_q;
	case 11: return u"B"_q;
	case 8: return u"C"_q;
	case 2: return u"D"_q;
	case 14: return u"E"_q;
	case 3: return u"F"_q;
	case 5: return u"G"_q;
	case 4: return u"H"_q;
	case 34: return u"I"_q;
	case 38: return u"J"_q;
	case 40: return u"K"_q;
	case 37: return u"L"_q;
	case 46: return u"M"_q;
	case 45: return u"N"_q;
	case 31: return u"O"_q;
	case 35: return u"P"_q;
	case 12: return u"Q"_q;
	case 15: return u"R"_q;
	case 1: return u"S"_q;
	case 17: return u"T"_q;
	case 32: return u"U"_q;
	case 9: return u"V"_q;
	case 13: return u"W"_q;
	case 7: return u"X"_q;
	case 16: return u"Y"_q;
	case 6: return u"Z"_q;
	default: return QString();
	}
#else // Q_OS_MAC
	return QString();
#endif // Q_OS_MAC
}

[[nodiscard]] QString SpecialKeyName(not_null<QKeyEvent*> e) {
	switch (e->key()) {
	case Qt::Key_Control: return u"Ctrl"_q;
	case Qt::Key_Shift: return u"Shift"_q;
	case Qt::Key_Meta: return u"Cmd"_q;
	case Qt::Key_Alt: return u"Alt"_q;
	case Qt::Key_AltGr: return u"AltGr"_q;
	case Qt::Key_Escape: return u"Esc"_q;
	case Qt::Key_Tab: return u"Tab"_q;
	case Qt::Key_Backtab: return u"Shift+Tab"_q;
	case Qt::Key_Return: return u"Return"_q;
	case Qt::Key_Enter: return u"Enter"_q;
	case Qt::Key_Space: return u"Space"_q;
	case Qt::Key_Backspace: return u"Backspace"_q;
	case Qt::Key_Delete: return u"Delete"_q;
	case Qt::Key_Up: return u"Up"_q;
	case Qt::Key_Down: return u"Down"_q;
	case Qt::Key_Left: return u"Left"_q;
	case Qt::Key_Right: return u"Right"_q;
	default: return QString();
	}
}

[[nodiscard]] QString KeyNameForLog(not_null<QKeyEvent*> e) {
	auto key = SpecialKeyName(e);
	if (key.isEmpty()) {
		key = MacPhysicalLatinKey(e);
	}
	if (key.isEmpty()) {
		if (e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) {
			key = QString(QChar('A' + e->key() - Qt::Key_A));
		} else if (e->key() >= Qt::Key_0 && e->key() <= Qt::Key_9) {
			key = QString(QChar('0' + e->key() - Qt::Key_0));
		} else {
			key = SpecialKeyName(e);
		}
	}
	const auto text = (e->text().size() == 1 && e->text().front().isPrint())
		? e->text().toCaseFolded() : QString();
	if (key.isEmpty()) {
		key = text.isEmpty() ? u"key:%1"_q.arg(e->key()) : text;
	} else if (!text.isEmpty()
		&& text != key.toCaseFolded()
		&& text != u" "_q) {
		key += u"/"_q + text;
	}
	return key;
}

[[nodiscard]] QString KeyEventForLog(not_null<QKeyEvent*> e) {
	auto parts = QStringList();
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	if (modifiers & Qt::ControlModifier) {
#ifdef Q_OS_MAC
		parts.push_back(u"Cmd"_q);
#else // Q_OS_MAC
		parts.push_back(u"Ctrl"_q);
#endif // Q_OS_MAC
	}
	if (modifiers & Qt::MetaModifier) {
#ifdef Q_OS_MAC
		parts.push_back(u"Ctrl"_q);
#else // Q_OS_MAC
		parts.push_back(u"Cmd"_q);
#endif // Q_OS_MAC
	}
	if (modifiers & Qt::AltModifier) {
		parts.push_back(u"Alt"_q);
	}
	if ((modifiers & Qt::ShiftModifier) && e->key() != Qt::Key_Backtab) {
		parts.push_back(u"Shift"_q);
	}
	parts.push_back(KeyNameForLog(e));
	auto result = parts.join(u"+"_q);
	if (e->isAutoRepeat()) {
		result += u" repeat"_q;
	}
	return result;
}

} // namespace

void KeyEventLog::record(not_null<QKeyEvent*> event, const QString &status) {
	if (_suppressed || KeyboardInputActive(nullptr)) {
		return;
	}
	append(KeyEventForLog(event) + u" -> "_q + status);
}

void KeyEventLog::recordCommand(const QString &command, const QString &status) {
	append(command + u" -> "_q + status);
}

void KeyEventLog::append(const QString &entry) {
	_entries.push_back(QString::number(++_sequence).rightJustified(2, '0')
		+ u". "_q + entry);
	if (_entries.size() > kLimit) {
		_entries.removeFirst();
	}
	++_generation;
}

void KeyEventLog::clear() {
	_entries.clear();
	_sequence = 0;
	++_generation;
}

QString KeyEventLog::text() const {
	return _entries.join(u"\n"_q);
}

uint64 KeyEventLog::generation() const {
	return _generation;
}

bool KeyEventLog::suppressed() const {
	return _suppressed;
}

void KeyEventLog::setSuppressed(bool suppressed) {
	_suppressed = suppressed;
}

} // namespace Core::VimKeymap
