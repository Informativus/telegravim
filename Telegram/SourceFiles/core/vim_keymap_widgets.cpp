/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_widgets.h"

#include "core/vim_keymap_bindings.h"
#include "core/vim_keymap_geometry.h"

#include <rpl/rpl.h>

#include "ui/abstract_button.h"
#include "ui/layers/layer_widget.h"
#include "ui/widgets/elastic_scroll.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "styles/style_widgets.h"
#include "styles/palette.h"

#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QScrollArea>
#include <QScrollBar>

#include <algorithm>
#include <unordered_set>

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

bool KeyboardScopeHasTextInput(not_null<QWidget*> scope, QObject *receiver) {
	const auto isInput = [&](QWidget *widget) {
		for (auto current = widget; current && KeyHandlerInScope(current, scope);
			current = current->parentWidget()) {
			if (qobject_cast<QLineEdit*>(current)
				|| qobject_cast<QTextEdit*>(current)
				|| qobject_cast<QPlainTextEdit*>(current)
				|| dynamic_cast<Ui::InputField*>(current)) {
				return true;
			}
		}
		return false;
	};
	return isInput(qobject_cast<QWidget*>(receiver))
		|| isInput(QApplication::focusWidget());
}

namespace {

[[nodiscard]] bool Available(not_null<QWidget*> widget, QWidget *scope) {
	if (!KeyHandlerInScope(widget, scope) || !widget->isEnabled()) {
		return false;
	}
	for (auto parent = widget.get(); parent && parent != scope;
		parent = parent->parentWidget()) {
		if (dynamic_cast<Ui::LayerWidget*>(parent)) {
			return false;
		}
	}
	if (const auto item = dynamic_cast<Ui::Menu::ItemBase*>(widget.get())) {
		return item->isEnabled() && !item->action()->isSeparator();
	} else if (const auto button = dynamic_cast<Ui::AbstractButton*>(widget.get())) {
		return !button->isDisabled();
	}
	return true;
}

[[nodiscard]] bool Focusable(not_null<QWidget*> widget) {
	if (dynamic_cast<KeyboardNavigation*>(widget.get())) {
		return false;
	} else if (dynamic_cast<Ui::AbstractButton*>(widget.get())) {
		return true;
	} else if (const auto label = dynamic_cast<Ui::FlatLabel*>(widget.get())) {
		return label->keyboardFocusAvailable();
	} else if (widget->focusPolicy() & Qt::TabFocus) {
		return true;
	} else if (const auto rp = qobject_cast<Ui::RpWidget*>(widget.get())) {
		return (rp->accessibilityFocusPolicy() & Qt::TabFocus) != 0;
	}
	return false;
}

} // namespace

QWidget *FindKeyboardScope(not_null<QWidget*> window) {
	const auto find = [&](const auto &self, QWidget *root) -> QWidget* {
		if (const auto stack = dynamic_cast<Ui::LayerStackWidget*>(root)) {
			if (const auto top = stack->topShownLayer()) {
				return self(self, const_cast<Ui::LayerWidget*>(top));
			}
		}
		const auto children = root->children();
		for (auto i = children.rbegin(); i != children.rend(); ++i) {
			const auto child = qobject_cast<QWidget*>(*i);
			if (child && !child->isWindow() && child->isVisibleTo(root)) {
				if (const auto nested = self(self, child)) {
					return nested;
				}
			}
		}
		return dynamic_cast<Ui::LayerWidget*>(root);
	};
	return find(find, window);
}

bool CloseKeyboardScope(not_null<QWidget*> scope) {
	if (const auto navigation = KeyboardNavigation::Find(scope)) {
		if (navigation->hasHints()) {
			navigation->clearHints();
			return true;
		}
	}
	if (const auto layer = dynamic_cast<Ui::LayerWidget*>(scope.get())) {
		layer->closeByBackButton();
		return true;
	}
	return false;
}

std::vector<QPointer<QWidget>> KeyboardFocusTargets(not_null<QWidget*> scope) {
	auto result = std::vector<QPointer<QWidget>>();
	auto seen = std::unordered_set<QWidget*>();
	const auto collect = [&](const auto &self, QWidget *parent) -> void {
		for (const auto object : parent->children()) {
			const auto widget = qobject_cast<QWidget*>(object);
			if (!widget || widget->isWindow() || !Available(widget, scope)) {
				continue;
			}
			if (Focusable(widget)) {
				auto target = widget;
				while (target->focusProxy()) {
					target = target->focusProxy();
				}
				if (Available(target, scope)
					&& seen.emplace(target).second) {
					result.push_back(target);
				}
			}
			self(self, widget);
		}
	};
	collect(collect, scope);
	const auto rtl = scope->layoutDirection() == Qt::RightToLeft;
	std::stable_sort(result.begin(), result.end(), [&](const auto &a, const auto &b) {
		const auto ap = a->mapTo(scope, QPoint());
		const auto bp = b->mapTo(scope, QPoint());
		return (ap.y() != bp.y())
			? ap.y() < bp.y()
			: rtl ? ap.x() + a->width() > bp.x() + b->width() : ap.x() < bp.x();
	});
	return result;
}

void FocusModalNextPrevChild(not_null<QWidget*> scope, bool next) {
	KeyboardNavigation::Get(scope)->focusNext(next);
}

KeyboardNavigation *KeyboardNavigation::Find(not_null<QWidget*> scope) {
	for (const auto child : scope->children()) {
		if (const auto result = dynamic_cast<KeyboardNavigation*>(child)) {
			return result;
		}
	}
	return nullptr;
}

not_null<KeyboardNavigation*> KeyboardNavigation::Get(not_null<QWidget*> scope) {
	if (const auto result = Find(scope)) {
		return result;
	}
	return new KeyboardNavigation(scope);
}

KeyboardNavigation::KeyboardNavigation(not_null<QWidget*> scope)
: Ui::RpWidget(scope)
, _scope(scope) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setFocusPolicy(Qt::NoFocus);
	setGeometry(scope->rect());
	scope->installEventFilter(this);
	QObject::connect(qApp, &QApplication::focusChanged, this, [=](QWidget*, QWidget *now) {
		trackFocus(now);
	});
	trackFocus(QApplication::focusWidget());
	show();
	raise();
}

void KeyboardNavigation::trackFocus(QWidget *widget) {
	if (widget != _focused) {
		clearHints();
	}
	if (const auto button = dynamic_cast<Ui::AbstractButton*>(_focused.data())) {
		button->setSynteticOver(false);
	}
	for (const auto watched : _watched) {
		if (watched) {
			watched->removeEventFilter(this);
		}
	}
	_watched.clear();
	_focused = (_scope && widget && widget != _scope
		&& Available(widget, _scope) && Focusable(widget)) ? widget : nullptr;
	if (_focused) {
		_lastFocused = _focused;
		if (const auto button = dynamic_cast<Ui::AbstractButton*>(_focused.data())) {
			button->setSynteticOver(true);
		}
		for (auto parent = _focused.data(); parent && parent != _scope;
			parent = parent->parentWidget()) {
			parent->installEventFilter(this);
			_watched.push_back(parent);
		}
	}
	update();
}

void KeyboardNavigation::focusTarget(not_null<QWidget*> target) {
	if (!_scope || !Available(target, _scope)) {
		return;
	}
	if (!(target->focusPolicy() & Qt::TabFocus)) {
		target->setFocusPolicy(Qt::StrongFocus);
	}
	const auto weak = QPointer<KeyboardNavigation>(this);
	const auto guardedTarget = QPointer<QWidget>(target);
	for (auto parent = target->parentWidget(); parent && parent != _scope;
		parent = parent->parentWidget()) {
		const auto guardedParent = QPointer<QWidget>(parent);
		if (const auto elastic = dynamic_cast<Ui::ElasticScroll*>(parent)) {
			elastic->scrollToWidget(target);
		} else if (const auto scroll = dynamic_cast<Ui::ScrollArea*>(parent)) {
			scroll->scrollToWidget(target);
		} else if (const auto scroll = qobject_cast<QScrollArea*>(parent)) {
			scroll->ensureWidgetVisible(target, 0, 0);
		}
		if (!weak || !guardedTarget || !guardedParent) {
			return;
		}
	}
	if (const auto item = dynamic_cast<Ui::Menu::ItemBase*>(target.get())) {
		item->setSelected(true, Ui::Menu::TriggeredSource::Keyboard);
	}
	if (!weak || !guardedTarget) {
		return;
	}
	target->setFocus(Qt::TabFocusReason);
	if (weak && guardedTarget) {
		trackFocus(target);
		raise();
	}
}

void KeyboardNavigation::restoreFocus() {
	if (_lastFocused && _scope && Available(_lastFocused, _scope)) {
		focusTarget(_lastFocused);
	}
}

bool KeyboardNavigation::scroll(int delta, bool autoRepeat, int duration) {
	struct Target {
		QPointer<QWidget> widget;
		int position = 0;
		int maximum = 0;
		Fn<void(int)> move;
	};
	const auto inspect = [&](QWidget *widget) -> Target {
		if (!widget || !Available(widget, _scope)) {
			return {};
		} else if (const auto area = dynamic_cast<Ui::ElasticScroll*>(widget)) {
			return { area, area->scrollTop(), area->scrollTopMax(),
				[=](int value) { area->scrollToY(value); } };
		} else if (const auto area = dynamic_cast<Ui::ScrollArea*>(widget)) {
			return { area, area->scrollTop(), area->scrollTopMax(),
				[=](int value) { area->scrollToY(value); } };
		} else if (const auto area = qobject_cast<QAbstractScrollArea*>(widget)) {
			const auto bar = area->verticalScrollBar();
			return { area, bar->value(), bar->maximum(),
				[=](int value) { bar->setValue(value); } };
		}
		return {};
	};
	auto target = Target();
	for (auto widget = QApplication::focusWidget();
		widget && KeyHandlerInScope(widget, _scope);
		widget = widget->parentWidget()) {
		target = inspect(widget);
		if (target.widget && target.maximum > 0) {
			break;
		}
	}
	if (!target.widget || target.maximum <= 0) {
		for (const auto widget : _scope->findChildren<QWidget*>()) {
			target = inspect(widget);
			if (target.widget && target.maximum > 0) {
				break;
			}
		}
	}
	if (!target.widget || target.maximum <= 0) {
		return false;
	}
	const auto from = target.position;
	const auto base = (_scrollArea == target.widget
		&& autoRepeat && _scrollAnimation.animating()) ? _scrollTarget : from;
	_scrollAnimation.stop();
	_scrollArea = target.widget;
	_scrollTarget = std::clamp(base + delta, 0, target.maximum);
	if (duration <= 0) {
		target.move(_scrollTarget);
		return true;
	}
	_scrollAnimation.start([=] {
		if (target.widget) {
			target.move(qRound(_scrollAnimation.value(_scrollTarget)));
		}
	}, from, _scrollTarget, duration, anim::linear);
	return true;
}

void KeyboardNavigation::focusNext(bool next) {
	clearHints();
	const auto targets = KeyboardFocusTargets(_scope);
	if (targets.empty()) {
		return;
	}
	const auto i = std::find(targets.begin(), targets.end(), QApplication::focusWidget());
	const auto count = int(targets.size());
	const auto index = (i == targets.end())
		? (next ? 0 : count - 1)
		: (int(i - targets.begin()) + (next ? 1 : count - 1)) % count;
	focusTarget(targets[index]);
}

QRect KeyboardNavigation::targetRect(not_null<QWidget*> target) const {
	auto rect = QRect(target->mapTo(_scope, QPoint()), target->size());
	for (auto parent = target->parentWidget(); parent && parent != _scope;
		parent = parent->parentWidget()) {
		rect &= QRect(parent->mapTo(_scope, QPoint()), parent->size());
	}
	return rect.intersected(this->rect());
}

void KeyboardNavigation::showHints(
		Fn<QString(int, int)> label,
		QFont font,
		QSize padding,
		int gap) {
	clearHints();
	_font = std::move(font);
	_padding = padding;
	_gap = gap;
	const auto targets = KeyboardFocusTargets(_scope);
	for (const auto target : targets) {
		if (!targetRect(target).isEmpty()) {
			_hints.push_back({ target, {} });
			for (auto parent = target.data(); parent && parent != _scope;
				parent = parent->parentWidget()) {
				if (std::find(_watched.begin(), _watched.end(), parent) == _watched.end()) {
					parent->installEventFilter(this);
					_watched.push_back(parent);
				}
			}
		}
	}
	for (auto i = 0; i != _hints.size(); ++i) {
		_hints[i].label = label(i, int(_hints.size()));
	}
	raise();
	update();
}

bool KeyboardNavigation::hasHints() const {
	return !_hints.empty();
}

void KeyboardNavigation::clearHints() {
	_hints.clear();
	_prefix.clear();
	update();
}

bool KeyboardNavigation::handleHintKey(not_null<QKeyEvent*> e, const QString &input) {
	if (!hasHints()) {
		return false;
	} else if (Bindings::IsPlainEscape(e)) {
		clearHints();
		return true;
	} else if (Bindings::TabNavigationDelta(e)) {
		if (dynamic_cast<Ui::PopupMenu*>(_scope.data())) {
			clearHints();
			return false;
		}
		focusNext(Bindings::TabNavigationDelta(e) > 0);
		return true;
	} else if (e->isAutoRepeat()) {
		return true;
	} else if (e->key() == Qt::Key_Backspace) {
		_prefix.chop(1);
		update();
		return true;
	} else if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
		clearHints();
		return false;
	} else if (input.isEmpty()) {
		return true;
	}
	const auto prefix = _prefix + input;
	auto matches = false;
	for (const auto &hint : _hints) {
		if (!hint.widget || !Available(hint.widget, _scope)) {
			continue;
		} else if (hint.label == prefix) {
			const auto target = hint.widget;
			clearHints();
			focusTarget(target);
			return true;
		}
		matches |= hint.label.startsWith(prefix);
	}
	if (matches) {
		_prefix = prefix;
		update();
	}
	return true;
}

void KeyboardNavigation::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (_focused && Available(_focused, _scope)) {
		const auto rect = targetRect(_focused);
		const auto border = st::lineWidth * 2;
		p.setPen(QPen(st::windowBgActive->c, border));
		p.setBrush(Qt::NoBrush);
		p.drawRect(rect.marginsRemoved({ border, border, border, border }));
	}
	auto badges = std::vector<HintBadge>();
	for (const auto &hint : _hints) {
		if (hint.widget && Available(hint.widget, _scope)) {
			const auto rect = targetRect(hint.widget);
			if (!rect.isEmpty()) {
				badges.push_back({ hint.label, rect.topLeft(), rect });
			}
		}
	}
	PaintHintBadges(p, badges, _prefix, _font, rect(), _padding, _gap, false);
}

bool KeyboardNavigation::eventFilter(QObject *object, QEvent *event) {
	if (object == _scope && event->type() == QEvent::Resize) {
		setGeometry(_scope->rect());
	} else if (event->type() == QEvent::Hide) {
		clearHints();
		_scrollAnimation.stop();
	} else if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
		update();
	}
	return Ui::RpWidget::eventFilter(object, event);
}

} // namespace Core::VimKeymap
