/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"
#include "ui/effects/animations.h"

#include <QFont>
#include <QPointer>

class QObject;
class QWidget;

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Core::VimKeymap {

[[nodiscard]] bool KeyHandlerInScope(QObject *owner, QWidget *scope);
[[nodiscard]] QWidget *FindKeyboardScope(not_null<QWidget*> window);
[[nodiscard]] bool KeyboardScopeHasTextInput(
	not_null<QWidget*> scope,
	QObject *receiver);
[[nodiscard]] bool CloseKeyboardScope(not_null<QWidget*> scope);
[[nodiscard]] std::vector<QPointer<QWidget>> KeyboardFocusTargets(
	not_null<QWidget*> scope);
void SetKeyboardFocusFrameEnabled(not_null<QWidget*> widget, bool enabled);
void FocusModalNextPrevChild(not_null<QWidget*> scope, bool next);

class KeyboardNavigation final : public Ui::RpWidget {
public:
	static KeyboardNavigation *Find(not_null<QWidget*> scope);
	static not_null<KeyboardNavigation*> Get(not_null<QWidget*> scope);

	explicit KeyboardNavigation(not_null<QWidget*> scope);
	void focusNext(bool next);
	void focusTarget(not_null<QWidget*> target);
	void restoreFocus();
	bool scroll(int delta, bool autoRepeat, int duration);
	void showHints(
		Fn<QString(int, int)> label,
		QFont font,
		QSize padding,
		int gap);
	[[nodiscard]] bool hasHints() const;
	void clearHints();
	bool handleHintKey(not_null<QKeyEvent*> e, const QString &input);

protected:
	void paintEvent(QPaintEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	void trackFocus(QWidget *widget);
	[[nodiscard]] QRect targetRect(not_null<QWidget*> target) const;

	struct Hint {
		QPointer<QWidget> widget;
		QString label;
	};
	QPointer<QWidget> _scope;
	QPointer<QWidget> _focused;
	QPointer<QWidget> _lastFocused;
	std::vector<QPointer<QWidget>> _watched;
	std::vector<Hint> _hints;
	QString _prefix;
	QFont _font;
	QSize _padding;
	int _gap = 0;
	QPointer<QWidget> _scrollArea;
	Ui::Animations::Simple _scrollAnimation;
	int _scrollTarget = 0;

};

} // namespace Core::VimKeymap
