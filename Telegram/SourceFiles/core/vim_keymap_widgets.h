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
[[nodiscard]] bool KeyboardInputActive(QObject *receiver);
[[nodiscard]] QWidget *FindKeyboardScope(not_null<QWidget*> window);
[[nodiscard]] bool KeyboardScopeHasTextInput(
	not_null<QWidget*> scope,
	QObject *receiver);
[[nodiscard]] bool CloseKeyboardScope(not_null<QWidget*> scope);
[[nodiscard]] std::vector<QPointer<QWidget>> KeyboardFocusTargets(
	not_null<QWidget*> scope);
[[nodiscard]] std::vector<QPointer<QWidget>> VisibleKeyboardHintTargets(
	not_null<QWidget*> scope);
void RegisterGlobalFocusRoot(not_null<QWidget*> root);
[[nodiscard]] std::vector<QPointer<QWidget>> GlobalFocusRoots(
	not_null<QWidget*> window);
[[nodiscard]] QWidget *GlobalFocusRoot(QWidget *widget);
void SetKeyboardFocusTargetEnabled(not_null<QWidget*> widget, bool enabled);
void SetKeyboardFocusCircle(not_null<QWidget*> widget);
void PaintKeyboardStickerFrame(QPainter &p, QRect rect);
[[nodiscard]] Ui::RpWidget *CreateKeyboardTabTarget(
	not_null<QWidget*> parent,
	Fn<void()> activate);
[[nodiscard]] bool LeaveKeyboardInput(
	not_null<QWidget*> scope,
	not_null<QWidget*> input,
	not_null<QKeyEvent*> e);
[[nodiscard]] bool HandleKeyboardControlKey(
	not_null<QWidget*> scope,
	not_null<QKeyEvent*> e);
void FocusModalNextPrevChild(not_null<QWidget*> scope, bool next);

class KeyboardNavigation final : public Ui::RpWidget {
public:
	static KeyboardNavigation *Find(not_null<QWidget*> scope);
	static not_null<KeyboardNavigation*> Get(not_null<QWidget*> scope);

	explicit KeyboardNavigation(not_null<QWidget*> scope);
	void focusNext(bool next);
	bool handleMenuNavigation(
		not_null<QKeyEvent*> e,
		std::optional<Qt::Key> navigationKey = std::nullopt);
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
	void setHintPrefix(const QString &prefix);
	bool handleHintKey(not_null<QKeyEvent*> e, const QString &input);
	bool handleControlKey(not_null<QKeyEvent*> e);

protected:
	void paintEvent(QPaintEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	void trackFocus(QWidget *widget);
	void finishControlEdit(bool cancel = false);
	[[nodiscard]] QRect targetRect(not_null<QWidget*> target) const;

	struct Hint {
		QPointer<QWidget> widget;
		QString label;
	};
	QPointer<QWidget> _scope;
	QPointer<QWidget> _focused;
	QPointer<QWidget> _lastFocused;
	QPointer<QWidget> _editingControl;
	int _editingNativeValue = 0;
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
