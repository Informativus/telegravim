/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/spellchecker_menu.h"

#ifndef TDESKTOP_DISABLE_SPELLCHECK

#include "base/unique_qptr.h"
#include "chat_helpers/spellchecker_bundled.h"
#include "core/vim_keymap_bindings.h"
#include "spellcheck/spelling_highlighter.h"
#include "ui/widgets/menu/menu.h"
#include "ui/widgets/popup_menu.h"

#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>

namespace Spellchecker {
namespace {

class SuggestionsMenu final : public QObject {
public:
	SuggestionsMenu(
		not_null<Ui::InputField*> field,
		not_null<SpellingHighlighter*> highlighter);

	bool show();

private:
	bool eventFilter(QObject *object, QEvent *event) override;
	void cancel();
	void finish(QPoint position, int firstSuggestion);

	const not_null<Ui::InputField*> _field;
	const not_null<SpellingHighlighter*> _highlighter;
	base::unique_qptr<Ui::PopupMenu> _menu;
	std::unique_ptr<QMenu> _lookupMenu;
	QPointer<QWidget> _pendingFocus;
	int _generation = 0;
	bool _pending = false;
	rpl::lifetime _lifetime;

};

SuggestionsMenu::SuggestionsMenu(
	not_null<Ui::InputField*> field,
	not_null<SpellingHighlighter*> highlighter)
: QObject(field)
, _field(field)
, _highlighter(highlighter) {
	field->changes() | rpl::on_next([=] { cancel(); }, _lifetime);
	connect(field->rawTextEdit(), &QTextEdit::cursorPositionChanged,
		this, [=] { cancel(); });
}

void SuggestionsMenu::cancel() {
	++_generation;
	_lookupMenu = nullptr;
	if (_pending) {
		_pending = false;
		qApp->removeEventFilter(this);
	}
	if (_menu) {
		_menu->hideMenu(true);
	}
}

bool SuggestionsMenu::eventFilter(QObject *object, QEvent *event) {
	if (_pending) {
		if (event->type() == QEvent::KeyPress
			|| event->type() == QEvent::MouseButtonPress
			|| (event->type() == QEvent::FocusIn
				&& QApplication::focusWidget() != _pendingFocus)
			|| (event->type() == QEvent::WindowDeactivate
				&& (object == _field->window()
					|| object == _field->window()->windowHandle()))) {
			cancel();
		}
		return false;
	}
	if (_menu && event->type() == QEvent::KeyPress) {
		const auto key = static_cast<QKeyEvent*>(event);
		const auto delta = Core::VimKeymap::Bindings::PickerNavigationDelta(key);
		if (delta) {
			auto translated = QKeyEvent(
				QEvent::KeyPress,
				(delta > 0) ? Qt::Key_Down : Qt::Key_Up,
				Qt::NoModifier);
			_menu->menu()->handleKeyPress(&translated);
			return true;
		}
	}
	return false;
}

bool SuggestionsMenu::show() {
	if (!_highlighter->enabled() || !_field->isVisible()) {
		return false;
	}
	cancel();
	const auto generation = _generation;
	const auto weak = QPointer<SuggestionsMenu>(this);
	const auto focus = QPointer<QWidget>(QApplication::focusWidget());
	const auto cursor = _field->textCursor();
	const auto rect = _field->rawTextEdit()->cursorRect(cursor);
	const auto position = _field->rawTextEdit()->viewport()->mapToGlobal(
		rect.bottomLeft());
	auto menu = std::make_unique<QMenu>();
	const auto raw = menu.get();
	_pendingFocus = focus;
	_pending = true;
	qApp->installEventFilter(this);
	_highlighter->fillSpellcheckerMenu(raw, cursor, [=,
		menu = std::move(menu)
	](int firstSuggestion) mutable {
		if (!weak || generation != _generation) {
			return;
		}
		_lookupMenu = std::move(menu);
		if (firstSuggestion < 0) {
			finish(position, firstSuggestion);
			return;
		}
		auto selection = cursor;
		selection.select(QTextCursor::WordUnderCursor);
		const auto word = selection.selectedText();
		auto suggestions = std::vector<QString>();
		const auto actions = _lookupMenu->actions();
		for (auto i = firstSuggestion; i != actions.size(); ++i) {
			suggestions.push_back(actions[i]->text());
		}
		SuggestRussianWords(word, std::move(suggestions), [=](
				std::vector<QString> improved) {
			if (!weak || generation != _generation) {
				return;
			}
			const auto menu = _lookupMenu.get();
			const auto actions = menu->actions().mid(firstSuggestion);
			for (const auto action : actions) {
				menu->removeAction(action);
			}
			for (const auto &suggestion : improved) {
				const auto existing = ranges::find(actions, suggestion, &QAction::text);
				if (existing != actions.end()) {
					menu->addAction(*existing);
				} else {
					menu->addAction(suggestion, [=] {
						if (generation != _generation || selection.selectedText() != word) {
							return;
						}
						const auto edit = QPointer<QTextEdit>(_field->rawTextEdit());
						const auto saved = edit->textCursor();
						auto replacement = selection;
						replacement.insertText(suggestion);
						if (edit) {
							edit->setTextCursor(saved);
						}
					});
				}
			}
			finish(position, firstSuggestion);
		});
	});
	return true;
}

void SuggestionsMenu::finish(QPoint position, int firstSuggestion) {
	_pending = false;
	qApp->removeEventFilter(this);
	auto menu = std::move(_lookupMenu);
	if (menu->isEmpty()
		|| !_highlighter->enabled()
		|| !_field->isVisible()
		|| !_field->window()->isActiveWindow()
		|| _pendingFocus != QApplication::focusWidget()) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		_field,
		menu.release(),
		_field->st().menu);
	_menu->installEventFilter(this);
	_menu->menu()->installEventFilter(this);
	_menu->popup(position);
	if (_menu && firstSuggestion >= 0) {
		_menu->menu()->setSelected(firstSuggestion, false);
	}
}

} // namespace

void InitSuggestionsMenu(
		not_null<Ui::InputField*> field,
		not_null<SpellingHighlighter*> highlighter) {
	new SuggestionsMenu(field, highlighter);
}

bool ShowSuggestionsMenu(not_null<Ui::InputField*> field) {
	for (const auto child : field->children()) {
		if (const auto menu = dynamic_cast<SuggestionsMenu*>(child)) {
			return menu->show();
		}
	}
	return false;
}

} // namespace Spellchecker

#endif // !TDESKTOP_DISABLE_SPELLCHECK
