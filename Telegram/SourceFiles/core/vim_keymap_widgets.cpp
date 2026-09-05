/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_widgets.h"

#include "base/qt/qt_tab_key.h"
#include "ui/abstract_button.h"

namespace Core::VimKeymap {

bool KeyHandlerInScope(QObject *owner, QWidget *scope) {
	if (!scope) {
		return true;
	}
	const auto widget = qobject_cast<QWidget*>(owner);
	return widget
		&& widget->window() == scope->window()
		&& (widget == scope || scope->isAncestorOf(widget))
		&& widget->isVisibleTo(scope);
}

void FocusModalNextPrevChild(not_null<Ui::RpWidget*> box, bool next) {
	for (const auto widget : box->findChildren<QWidget*>()) {
		if (const auto button = dynamic_cast<Ui::AbstractButton*>(widget)) {
			button->setFocusPolicy(button->isDisabled()
				? Qt::NoFocus
				: Qt::StrongFocus);
		}
	}
	box->setVisualTabOrder(true);
	box->refreshVisualTabOrder();
	base::FocusNextPrevChildBlocked(box, next);
}

} // namespace Core::VimKeymap
