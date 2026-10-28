/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_compose.h"

#include "base/event_filter.h"
#include "base/options.h"
#include "base/platform/base_platform_info.h"
#include "chat_helpers/spellchecker_menu.h"
#include "core/vim_keymap.h"
#include "core/vim_keymap_bindings.h"
#include "core/vim_keymap_geometry.h"
#include "core/vim_keymap_options.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/painter.h"
#include <QtCore/QTimer>
#include <QtGui/QTextBlock>
#include <QtGui/QTextLayout>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>

#include "styles/style_chat.h"

namespace Core::VimKeymap {
namespace {

constexpr auto kVimKeymapDeleteTimeout = crl::time(500);

[[nodiscard]] Qt::KeyboardModifiers VimKeymapCleanModifiers(
		not_null<QKeyEvent*> e) {
	return e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
}

[[nodiscard]] bool VimKeymapMacVirtualKeyIs(
		not_null<QKeyEvent*> e,
		Qt::Key key) {
#ifdef Q_OS_MAC
	switch (key) {
	case Qt::Key_A: return e->nativeVirtualKey() == 0;
	case Qt::Key_B: return e->nativeVirtualKey() == 11;
	case Qt::Key_C: return e->nativeVirtualKey() == 8;
	case Qt::Key_D: return e->nativeVirtualKey() == 2;
	case Qt::Key_E: return e->nativeVirtualKey() == 14;
	case Qt::Key_G: return e->nativeVirtualKey() == 5;
	case Qt::Key_H: return e->nativeVirtualKey() == 4;
	case Qt::Key_I: return e->nativeVirtualKey() == 34;
	case Qt::Key_J: return e->nativeVirtualKey() == 38;
	case Qt::Key_K: return e->nativeVirtualKey() == 40;
	case Qt::Key_L: return e->nativeVirtualKey() == 37;
	case Qt::Key_O: return e->nativeVirtualKey() == 31;
	case Qt::Key_P: return e->nativeVirtualKey() == 35;
	case Qt::Key_R: return e->nativeVirtualKey() == 15;
	case Qt::Key_S: return e->nativeVirtualKey() == 1;
	case Qt::Key_V: return e->nativeVirtualKey() == 9;
	case Qt::Key_W: return e->nativeVirtualKey() == 13;
	case Qt::Key_X: return e->nativeVirtualKey() == 7;
	case Qt::Key_Y: return e->nativeVirtualKey() == 16;
	default: return false;
	}
#else // Q_OS_MAC
	return false;
#endif // Q_OS_MAC
}

[[nodiscard]] bool VimKeymapTextKey(
		not_null<QKeyEvent*> e,
		Qt::Key key,
		const QString &latin,
		const QString &cyrillic) {
	const auto text = e->text().toCaseFolded();
	return (e->key() == key)
		|| VimKeymapMacVirtualKeyIs(e, key)
		|| (!cyrillic.isEmpty()
			&& (e->key() == cyrillic.front().unicode()
				|| e->key() == cyrillic.front().toUpper().unicode()))
		|| text == latin
		|| text == cyrillic;
}

[[nodiscard]] bool VimKeymapPlainTextKey(
		not_null<QKeyEvent*> e,
		Qt::Key key,
		const QString &latin,
		const QString &cyrillic) {
	return VimKeymapCleanModifiers(e) == Qt::NoModifier
		&& VimKeymapTextKey(e, key, latin, cyrillic);
}

[[nodiscard]] bool VimKeymapPlainOrShiftTextKey(
		not_null<QKeyEvent*> e,
		Qt::Key key,
		const QString &latin,
		const QString &cyrillic) {
	const auto modifiers = VimKeymapCleanModifiers(e);
	return (modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier)
		&& VimKeymapTextKey(e, key, latin, cyrillic);
}

[[nodiscard]] bool VimKeymapShiftTextKey(
		not_null<QKeyEvent*> e,
		Qt::Key key,
		const QString &latin,
		const QString &cyrillic) {
	return VimKeymapCleanModifiers(e) == Qt::ShiftModifier
		&& VimKeymapTextKey(e, key, latin, cyrillic);
}

[[nodiscard]] bool VimKeymapTypedText(
		not_null<QKeyEvent*> e,
		const QString &text) {
	const auto modifiers = VimKeymapCleanModifiers(e);
	return (modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier)
		&& e->text() == text;
}

[[nodiscard]] bool VimKeymapOptionTextKey(
		not_null<QKeyEvent*> e,
		Qt::Key key,
		const QString &latin,
		const QString &cyrillic) {
	const auto modifiers = VimKeymapCleanModifiers(e);
	return (modifiers == Qt::AltModifier
		|| modifiers == Qt::ControlModifier
		|| (Platform::IsMac() && modifiers == Qt::MetaModifier))
		&& VimKeymapTextKey(e, key, latin, cyrillic);
}

[[nodiscard]] QColor VimKeymapVisualModeColor() {
	return QColor(218, 91, 166);
}

struct VimKeymapComposeLineRange {
	int start = 0;
	int end = 0;
};

[[nodiscard]] VimKeymapComposeLineRange VimKeymapComposeLineAt(
		QTextCursor cursor) {
	const auto block = cursor.block();
	const auto text = block.text();
	const auto offset = cursor.positionInBlock();
	const auto separator = QChar(QChar::LineSeparator);
	const auto before = (offset > 0)
		? int(text.lastIndexOf(separator, offset - 1))
		: -1;
	const auto after = int(text.indexOf(separator, offset));
	return {
		.start = block.position() + before + 1,
		.end = block.position() + ((after >= 0) ? after : int(text.size())),
	};
}

bool VimKeymapNormalizeComposeCursor(QTextCursor &cursor) {
	if (cursor.hasSelection()) {
		return false;
	}
	const auto line = VimKeymapComposeLineAt(cursor);
	if (line.start == line.end || cursor.position() != line.end) {
		return false;
	}
	cursor.setPosition(line.end - 1);
	return true;
}

[[nodiscard]] QTextCursor VimKeymapComposePaintCursor(
		not_null<QTextEdit*> edit,
		bool visualMode) {
	auto cursor = edit->textCursor();
	if (visualMode
		&& cursor.hasSelection()
		&& cursor.position() > cursor.anchor()) {
		cursor.setPosition(std::max(0, cursor.position() - 1));
	} else if (!visualMode) {
		VimKeymapNormalizeComposeCursor(cursor);
	}
	return cursor;
}

[[nodiscard]] QString VimKeymapComposeCursorText(
		not_null<QTextEdit*> edit,
		QTextCursor cursor) {
	const auto document = edit->document();
	const auto maximum = std::max(0, document->characterCount() - 1);
	if (cursor.position() >= maximum) {
		return QString();
	}
	cursor.clearSelection();
	cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
	const auto result = cursor.selectedText();
	if (result == u"\u2028"_q
		|| result == u"\u2029"_q
		|| result == u"\uFFFC"_q) {
		return QString();
	}
	return result;
}

class VimKeymapComposeCursorOverlay final : public QWidget {
public:
	explicit VimKeymapComposeCursorOverlay(not_null<QTextEdit*> edit);

	~VimKeymapComposeCursorOverlay();

	void setActive(bool active, bool visualMode);

protected:
	bool eventFilter(QObject *object, QEvent *event) override;

	void paintEvent(QPaintEvent*) override;

private:
	void refreshGeometry();

	void refreshBlink();

	void resetBlink();

	QPointer<QTextEdit> _edit;
	QTimer _blinkTimer;
	rpl::lifetime _lifetime;
	bool _active = false;
	bool _visualMode = false;
	bool _blinkVisible = true;
};

VimKeymapComposeCursorOverlay::VimKeymapComposeCursorOverlay(
		not_null<QTextEdit*> edit)
: QWidget(edit->viewport())
, _edit(edit) {
	setObjectName(u"TelegraVimComposeCursorOverlay"_q);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_NoSystemBackground);
	setFocusPolicy(Qt::NoFocus);
	edit->viewport()->installEventFilter(this);
	QObject::connect(
		edit,
		&QTextEdit::cursorPositionChanged,
		this,
		[=] { resetBlink(); });
	QObject::connect(
		edit,
		&QTextEdit::textChanged,
		this,
		[=] { resetBlink(); });
	_blinkTimer.setTimerType(Qt::PreciseTimer);
	QObject::connect(&_blinkTimer, &QTimer::timeout, this, [=] {
		_blinkVisible = !_blinkVisible;
		update();
	});
	rpl::merge(
		base::options::lookup<QString>(
			Core::VimKeymap::kOptionVimKeymapComposeCursorStyle
		).changes(),
		base::options::lookup<int>(
			Core::VimKeymap::kOptionVimKeymapComposeCursorWidth
		).changes(),
		base::options::lookup<int>(
			Core::VimKeymap::kOptionVimKeymapComposeCursorHeight
		).changes(),
		base::options::lookup<int>(
			Core::VimKeymap::kOptionVimKeymapComposeCursorBlink
		).changes()
	) | rpl::on_next([=] {
		resetBlink();
	}, _lifetime);
	refreshGeometry();
	hide();
}

VimKeymapComposeCursorOverlay::~VimKeymapComposeCursorOverlay() {
	_lifetime.destroy();
	if (_edit && _edit->viewport()) {
		_edit->viewport()->removeEventFilter(this);
	}
}

void VimKeymapComposeCursorOverlay::setActive(bool active, bool visualMode) {
	const auto changed = (_active != active)
		|| (_visualMode != visualMode);
	if (changed) {
		_active = active;
		_visualMode = visualMode;
		_blinkVisible = true;
		setVisible(active);
	}
	refreshBlink();
	if (changed) {
		refreshGeometry();
		update();
	}
}

bool VimKeymapComposeCursorOverlay::eventFilter(QObject *object, QEvent *event) {
	if (_edit
		&& object == _edit->viewport()
		&& (event->type() == QEvent::Resize
			|| event->type() == QEvent::Move
			|| event->type() == QEvent::Show
			|| event->type() == QEvent::Hide)) {
		refreshGeometry();
		update();
	}
	return QWidget::eventFilter(object, event);
}

void VimKeymapComposeCursorOverlay::paintEvent(QPaintEvent*) {
	if (!_active || !_blinkVisible || !_edit) {
		return;
	}
	const auto paintCursor = VimKeymapComposePaintCursor(
		_edit.data(),
		_visualMode);
	const auto cursor = _edit->cursorRect(paintCursor);
	if (cursor.height() <= 0) {
		return;
	}
	const auto lineHeight = std::max(1, cursor.height());
	const auto metrics = _edit->fontMetrics();
	const auto text = VimKeymapComposeCursorText(
		_edit.data(),
		paintCursor);
	const auto requestedWidth = Core::VimKeymap::ComposeCursorWidth();
	const auto characterWidth = std::max(
		requestedWidth,
		text.isEmpty() ? 0 : metrics.horizontalAdvance(text));
	const auto requestedHeight = std::clamp(
		lineHeight * Core::VimKeymap::ComposeCursorHeight() / 100,
		1,
		lineHeight);
	const auto style = Core::VimKeymap::ComposeCursorStyle();
	auto rect = Core::VimKeymap::CursorPaintRect(
		QRect(cursor.topLeft(), QSize(characterWidth, lineHeight)),
		style,
		requestedWidth,
		requestedHeight);
	rect = rect.intersected(this->rect());
	if (rect.isEmpty()) {
		return;
	}
	auto painter = QPainter(this);
	auto color = _visualMode
		? VimKeymapVisualModeColor()
		: _edit->palette().color(QPalette::Text);
	if (style == u"block"_q) {
		color.setAlpha(210);
	}
	painter.fillRect(rect, color);
	if (style == u"block"_q && !text.isEmpty()) {
		painter.save();
		painter.setClipRect(rect);
		painter.setFont(_edit->font());
		painter.setPen(_edit->palette().color(QPalette::Base));
		painter.drawText(
			QRect(cursor.left(), cursor.top(), characterWidth, lineHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			text);
		painter.restore();
	}
}

void VimKeymapComposeCursorOverlay::refreshGeometry() {
	if (_edit && _edit->viewport()) {
		setGeometry(_edit->viewport()->rect());
		raise();
	}
}

void VimKeymapComposeCursorOverlay::refreshBlink() {
	const auto blink = Core::VimKeymap::ComposeCursorBlink();
	if (!_active || blink <= 0) {
		_blinkTimer.stop();
		_blinkVisible = true;
	} else if (!_blinkTimer.isActive()
		|| _blinkTimer.interval() != blink) {
		_blinkTimer.start(blink);
	}
}

void VimKeymapComposeCursorOverlay::resetBlink() {
	_blinkVisible = true;
	refreshBlink();
	update();
}

} // namespace

void RefreshComposeCursor(not_null<Ui::InputField*> field, bool visualMode) {
	const auto raw = field->rawTextEdit();
	const auto active = NormalMode() && field->isVisible();
	const auto width = active ? 0 : 1;
	if (raw->cursorWidth() != width) {
		raw->setCursorWidth(width);
	}
	if (active && !visualMode) {
		auto cursor = field->textCursor();
		if (VimKeymapNormalizeComposeCursor(cursor)) {
			field->setTextCursor(cursor);
		}
	}
	const auto widget = raw->viewport()->findChild<QWidget*>(
		u"TelegraVimComposeCursorOverlay"_q,
		Qt::FindDirectChildrenOnly);
	auto overlay = dynamic_cast<VimKeymapComposeCursorOverlay*>(widget);
	if (!overlay) {
		overlay = new VimKeymapComposeCursorOverlay(raw);
	}
	overlay->setActive(active, active && visualMode);
}

bool HandleComposeTextKey(ComposeKeyContext context, not_null<QKeyEvent*> e) {
	constexpr auto kComposeOpDelete = 1;
	constexpr auto kComposeOpDeleteInner = 2;
	constexpr auto kComposeOpYank = 3;
	constexpr auto kComposeOpChange = 4;
	constexpr auto kComposeOpChangeInner = 5;
	constexpr auto kComposeOpYankInner = 6;
	constexpr auto kComposePendingGoto = 1;
	constexpr auto kComposePendingReplace = 2;
	constexpr auto kComposePendingVisualInner = 3;

	if (!context.field || context.field->isHidden() || context.searchActive) {
		context.operation = 0;
		context.visualMode = 0;
		context.visualAnchor = -1;
		context.pending = 0;
		context.refreshCursor();
		return false;
	}
	if (FocusEmojiKey(e) || FocusChatKey(e) || EmojiPanelKey(e)) {
		return false;
	}
	const auto raw = context.field->rawTextEdit();
	const auto selectorOpen = context.panelOpen;
	if (selectorOpen) {
		context.operation = 0;
		context.visualMode = 0;
		context.visualAnchor = -1;
		context.pending = 0;
		context.refreshCursor();
		return false;
	}
	const auto fieldFocused = context.field->hasFocus() || raw->hasFocus();
	const auto stateActive = context.operation
		|| context.visualMode
		|| context.pending;
#ifndef TDESKTOP_DISABLE_SPELLCHECK
	if (!stateActive
		&& Core::VimKeymap::NormalMode()
		&& Core::VimKeymap::Bindings::SpellcheckKey(e)) {
		if (!e->isAutoRepeat()) {
			Spellchecker::ShowSuggestionsMenu(context.field);
		}
		return true;
	}
#endif // !TDESKTOP_DISABLE_SPELLCHECK
	if (!stateActive
		&& Core::VimKeymap::NormalMode()
		&& (Core::VimKeymap::Bindings::IsMessageReaction(e)
			|| Core::VimKeymap::Bindings::IsMessageSelection(e)
			|| Core::VimKeymap::Bindings::IsMessageSelection(e, true))) {
		return false;
	}
	const auto messageAction = Core::VimKeymap::ActionKey(e).has_value();
	if (Core::VimKeymap::EmptyComposeDefersToMessageAction(
			stateActive,
			context.field->empty(),
			messageAction)) {
		context.refreshCursor();
		return false;
	}
	if (!stateActive && Core::VimKeymap::NavigationKey(e)) {
		context.refreshCursor();
		return false;
	}
	const auto optionLeft = VimKeymapOptionTextKey(
		e,
		Qt::Key_H,
		u"h"_q,
		u"\u0440"_q);
	const auto optionRight = VimKeymapOptionTextKey(
		e,
		Qt::Key_L,
		u"l"_q,
		u"\u0434"_q);
	const auto inputCursorMove = optionLeft || optionRight;
	const auto plainLeft = VimKeymapPlainTextKey(
		e,
		Qt::Key_H,
		u"h"_q,
		u"\u0440"_q);
	const auto plainRight = VimKeymapPlainTextKey(
		e,
		Qt::Key_L,
		u"l"_q,
		u"\u0434"_q);
	const auto composeHorizontalMove = plainLeft || plainRight;
	const auto escape = !e->isAutoRepeat()
		&& VimKeymapCleanModifiers(e) == Qt::NoModifier
		&& e->key() == Qt::Key_Escape;
	if (escape && fieldFocused) {
		context.operation = 0;
		context.pending = 0;
		if (context.visualMode) {
			auto cursor = context.field->textCursor();
			cursor.clearSelection();
			context.field->setTextCursor(cursor);
			context.visualMode = 0;
			context.visualAnchor = -1;
			Core::VimKeymap::TraceKey(e, u"compose visual off"_q);
		} else {
			Core::VimKeymap::SetNormalMode(true);
			Core::VimKeymap::TraceKey(e, u"compose normal mode"_q);
		}
		context.field->setFocusFast();
		context.refreshCursor();
		return true;
	}
	if (!Core::VimKeymap::NormalMode() && !inputCursorMove) {
		context.operation = 0;
		context.visualMode = 0;
		context.visualAnchor = -1;
		context.pending = 0;
		context.refreshCursor();
		return false;
	} else if (inputCursorMove
		&& !fieldFocused) {
		return false;
	} else if (Core::VimKeymap::NormalMode()
		&& !fieldFocused
		&& context.field->empty()
		&& !stateActive
		&& !inputCursorMove
		&& !composeHorizontalMove) {
		context.refreshCursor();
		return false;
	}

	const auto focusField = [&] {
		context.field->setFocusFast();
		context.field->ensureCursorVisible();
		context.refreshCursor();
	};
	const auto clearVisual = [&] {
		if (!context.visualMode) {
			return;
		}
		auto cursor = context.field->textCursor();
		cursor.clearSelection();
		context.field->setTextCursor(cursor);
		context.visualMode = 0;
		context.visualAnchor = -1;
		context.refreshCursor();
	};
	const auto clearPending = [&] {
		context.pending = 0;
	};
	const auto moveMode = [&] {
		return (context.visualMode || context.operation)
			? QTextCursor::KeepAnchor
			: QTextCursor::MoveAnchor;
	};
	const auto traceMotion = [&](bool changed, const QString &status) {
		Core::VimKeymap::TraceKey(
			e,
			context.visualMode
				? (changed ? u"compose visual move"_q : u"compose visual edge"_q)
				: (changed ? status : status + u" edge"_q));
	};
	const auto copyCursorSelection = [&](QTextCursor cursor) {
		if (!cursor.hasSelection()) {
			return false;
		}
		context.field->setTextCursor(cursor);
		raw->copy();
		return true;
	};
	const auto copySelection = [&] {
		return copyCursorSelection(context.field->textCursor());
	};
	const auto finishOperatorWithSelection = [&](QTextCursor cursor) {
		const auto op = context.operation;
		context.operation = 0;
		context.visualMode = 0;
		context.visualAnchor = -1;
		context.pending = 0;
		if (!cursor.hasSelection()) {
			context.field->setTextCursor(cursor);
			focusField();
			Core::VimKeymap::TraceKey(e, u"compose operator empty"_q);
			return true;
		}
		if (op == kComposeOpYank) {
			copyCursorSelection(cursor);
			cursor = context.field->textCursor();
			cursor.clearSelection();
			context.field->setTextCursor(cursor);
			focusField();
			Core::VimKeymap::TraceKey(e, u"compose yank"_q);
			return true;
		}
		const auto before = context.field->getTextWithTags();
		copyCursorSelection(cursor);
		cursor = context.field->textCursor();
		cursor.removeSelectedText();
		context.field->setTextCursor(cursor);
		if (context.field->getTextWithTags() != before) {
			context.saveDraft();
		}
		const auto change = (op == kComposeOpChange
			|| op == kComposeOpChangeInner);
		Core::VimKeymap::SetNormalMode(!change);
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			change ? u"compose change motion"_q : u"compose delete motion"_q);
		return true;
	};
	const auto maximumCursorPosition = [&] {
		return std::max(0, raw->document()->characterCount() - 1);
	};
	const auto visualAnchor = [&] {
		const auto maximum = maximumCursorPosition();
		if (context.visualAnchor < 0
			|| context.visualAnchor > maximum) {
			const auto cursor = context.field->textCursor();
			context.visualAnchor = std::clamp(
				cursor.hasSelection()
					? cursor.selectionStart()
					: cursor.position(),
				0,
				maximum);
		}
		return context.visualAnchor;
	};
	const auto setVisualSelection = [&](int position) {
		const auto maximum = maximumCursorPosition();
		const auto anchor = std::clamp(visualAnchor(), 0, maximum);
		const auto endpoint = std::clamp(position, 0, maximum);
		context.visualAnchor = anchor;
		auto cursor = context.field->textCursor();
		if (endpoint >= anchor) {
			cursor.setPosition(anchor);
			cursor.setPosition(
				std::min(maximum, std::max(endpoint, anchor + 1)),
				QTextCursor::KeepAnchor);
		} else {
			cursor.setPosition(std::min(maximum, anchor + 1));
			cursor.setPosition(endpoint, QTextCursor::KeepAnchor);
		}
		context.field->setTextCursor(cursor);
	};
	const auto visualCharacterPosition = [&] {
		const auto cursor = context.field->textCursor();
		if (cursor.hasSelection()
			&& cursor.position() > cursor.anchor()) {
			return std::max(0, cursor.position() - 1);
		}
		return cursor.position();
	};
	const auto setVisualSelectionCharacter = [&](int position) {
		const auto maximum = maximumCursorPosition();
		const auto anchor = std::clamp(visualAnchor(), 0, maximum);
		const auto point = std::clamp(
			position,
			0,
			std::max(0, maximum - 1));
		setVisualSelection((point >= anchor)
			? std::min(maximum, point + 1)
			: point);
	};
	const auto lineLastCharacterPosition = [](QTextCursor cursor) {
		const auto line = VimKeymapComposeLineAt(cursor);
		return std::max(line.start, line.end - 1);
	};
	const auto refreshMovedCursor = [&] {
		context.field->ensureCursorVisible();
		context.refreshCursor();
	};
	const auto moveCursor = [&](
			QTextCursor::MoveOperation operation,
			bool allowPastLineEnd = false) {
		auto cursor = context.field->textCursor();
		if (context.visualMode == 1) {
			if (operation == QTextCursor::Left
				|| operation == QTextCursor::Right) {
				auto positionCursor = context.field->textCursor();
				positionCursor.setPosition(visualCharacterPosition());
				const auto first = VimKeymapComposeLineAt(positionCursor).start;
				const auto last = lineLastCharacterPosition(positionCursor);
				const auto step = (operation == QTextCursor::Right) ? 1 : -1;
				const auto position = std::clamp(
					positionCursor.position() + step,
					first,
					last);
				const auto changed = (position != positionCursor.position());
				setVisualSelectionCharacter(position);
				refreshMovedCursor();
				return changed;
			}
			cursor.setPosition(visualCharacterPosition());
			const auto changed = cursor.movePosition(
				operation,
				QTextCursor::MoveAnchor);
			setVisualSelectionCharacter(cursor.position());
			refreshMovedCursor();
			return changed;
		}
		if (Core::VimKeymap::NormalMode()
			&& !context.operation
			&& !allowPastLineEnd) {
			VimKeymapNormalizeComposeCursor(cursor);
			if (operation == QTextCursor::Left
				|| operation == QTextCursor::Right) {
				const auto was = cursor.position();
				const auto first = VimKeymapComposeLineAt(cursor).start;
				const auto last = lineLastCharacterPosition(cursor);
				const auto step = (operation == QTextCursor::Right) ? 1 : -1;
				const auto position = std::clamp(
					cursor.position() + step,
					first,
					last);
				cursor.setPosition(position);
				const auto changed = (cursor.position() != was);
				context.field->setTextCursor(cursor);
				refreshMovedCursor();
				return changed;
			}
		}
		const auto changed = cursor.movePosition(operation, moveMode());
		if (context.operation) {
			return finishOperatorWithSelection(cursor);
		}
		context.field->setTextCursor(cursor);
		focusField();
		return changed;
	};
	const auto moveCursorTo = [&](int position, const QString &status) {
		auto cursor = context.field->textCursor();
		position = std::clamp(
			position,
			0,
			std::max(0, raw->document()->characterCount() - 1));
		const auto changed = (cursor.position() != position);
		if (context.visualMode == 1) {
			setVisualSelection(position);
			refreshMovedCursor();
			traceMotion(changed, status);
			return true;
		}
		cursor.setPosition(position, moveMode());
		if (context.operation) {
			return finishOperatorWithSelection(cursor);
		}
		context.field->setTextCursor(cursor);
		focusField();
		traceMotion(changed, status);
		return true;
	};
	const auto undoRedo = [&](bool redo) {
		clearPending();
		const auto document = raw->document();
		const auto available = redo
			? (document->availableRedoSteps() > 0 || context.field->isRedoAvailable())
			: (document->availableUndoSteps() > 0 || context.field->isUndoAvailable());
		if (available) {
			if (redo) {
				context.field->redo();
			} else {
				context.field->undo();
			}
			context.saveDraft();
		}
		Core::VimKeymap::SetNormalMode(true);
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			available
				? (redo ? u"redo compose text"_q : u"undo compose text"_q)
				: (redo ? u"redo compose empty"_q : u"undo compose empty"_q));
		return true;
	};
	const auto deleteSelection = [&](QTextCursor cursor) {
		const auto before = context.field->getTextWithTags();
		copyCursorSelection(cursor);
		cursor = context.field->textCursor();
		cursor.removeSelectedText();
		context.field->setTextCursor(cursor);
		if (context.field->getTextWithTags() != before) {
			context.saveDraft();
			focusField();
			return true;
		}
		focusField();
		return false;
	};
	const auto selectCurrentLine = [&] {
		auto cursor = context.field->textCursor();
		const auto line = VimKeymapComposeLineAt(cursor);
		const auto end = std::min(line.end + 1, maximumCursorPosition());
		cursor.setPosition(line.start);
		cursor.setPosition(end, QTextCursor::KeepAnchor);
		return cursor;
	};
	const auto deleteCurrentLine = [&] {
		return deleteSelection(selectCurrentLine());
	};
	const auto selectInnerWordAt = [&](int position) {
		auto cursor = context.field->textCursor();
		cursor.clearSelection();
		cursor.setPosition(std::clamp(position, 0, maximumCursorPosition()));
		cursor.select(QTextCursor::WordUnderCursor);
		if (!cursor.hasSelection() || cursor.selectedText().isEmpty()) {
			cursor = context.field->textCursor();
			cursor.clearSelection();
			cursor.setPosition(std::clamp(position, 0, maximumCursorPosition()));
			cursor.movePosition(QTextCursor::NextWord);
			cursor.select(QTextCursor::WordUnderCursor);
		}
		return cursor;
	};
	const auto selectInnerWord = [&] {
		return selectInnerWordAt(context.visualMode
			? visualCharacterPosition()
			: context.field->textCursor().position());
	};
	const auto selectVisualInnerWord = [&] {
		const auto cursor = selectInnerWordAt(visualCharacterPosition());
		const auto selected = cursor.hasSelection()
			&& !cursor.selectedText().isEmpty();
		if (selected) {
			context.visualMode = 1;
			context.visualAnchor = cursor.selectionStart();
			context.field->setTextCursor(cursor);
		}
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			selected ? u"compose visual inner word"_q : u"compose word empty"_q);
		return true;
	};
	const auto deleteInnerWord = [&] {
		const auto cursor = selectInnerWord();
		return cursor.hasSelection()
			&& !cursor.selectedText().isEmpty()
			&& deleteSelection(cursor);
	};
	const auto copyInnerWord = [&] {
		const auto cursor = selectInnerWord();
		const auto copied = cursor.hasSelection()
			&& !cursor.selectedText().isEmpty()
			&& copyCursorSelection(cursor);
		auto restore = context.field->textCursor();
		restore.clearSelection();
		context.field->setTextCursor(restore);
		focusField();
		return copied;
	};
	const auto changeAfterDelete = [&](bool changed, const QString &status) {
		Core::VimKeymap::SetNormalMode(false);
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			changed ? status : status + u" empty"_q);
		return true;
	};
	const auto deleteNextChar = [&](bool inputAfter) {
		const auto before = context.field->getTextWithTags();
		auto cursor = context.field->textCursor();
		const auto hasSelection = cursor.movePosition(
			QTextCursor::Right,
			QTextCursor::KeepAnchor);
		if (hasSelection) {
			deleteSelection(cursor);
		}
		const auto changed = (context.field->getTextWithTags() != before);
		if (changed) {
			context.saveDraft();
		}
		if (inputAfter) {
			return changeAfterDelete(changed, u"compose substitute"_q);
		}
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			changed ? u"compose delete char"_q : u"compose delete char edge"_q);
		return true;
	};
	const auto deletePreviousChar = [&] {
		const auto before = context.field->getTextWithTags();
		auto cursor = context.field->textCursor();
		const auto hasSelection = cursor.movePosition(
			QTextCursor::Left,
			QTextCursor::KeepAnchor);
		if (hasSelection) {
			deleteSelection(cursor);
		}
		const auto changed = (context.field->getTextWithTags() != before);
		if (changed) {
			context.saveDraft();
		}
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			changed ? u"compose delete previous char"_q : u"compose delete previous edge"_q);
		return true;
	};
	const auto copyCurrentLine = [&] {
		const auto selected = selectCurrentLine();
		copyCursorSelection(selected);
		auto cursor = context.field->textCursor();
		cursor.clearSelection();
		context.field->setTextCursor(cursor);
		focusField();
		Core::VimKeymap::TraceKey(e, u"compose yank line"_q);
		return true;
	};
	const auto replaceNextChar = [&](const QString &text) {
		context.pending = 0;
		if (text.isEmpty()) {
			focusField();
			Core::VimKeymap::TraceKey(e, u"compose replace cancelled"_q);
			return true;
		}
		const auto before = context.field->getTextWithTags();
		auto cursor = context.field->textCursor();
		if (cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor)) {
			cursor.insertText(text.left(1));
			context.field->setTextCursor(cursor);
		}
		const auto changed = (context.field->getTextWithTags() != before);
		if (changed) {
			context.saveDraft();
		}
		Core::VimKeymap::SetNormalMode(true);
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			changed ? u"compose replace char"_q : u"compose replace edge"_q);
		return true;
	};
	const auto toggleCase = [&] {
		const auto before = context.field->getTextWithTags();
		auto cursor = context.field->textCursor();
		cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
		const auto selected = cursor.selectedText();
		if (!selected.isEmpty()) {
			const auto ch = selected.front();
			cursor.insertText(ch.isUpper()
				? QString(ch.toLower())
				: QString(ch.toUpper()));
			context.field->setTextCursor(cursor);
		}
		const auto changed = (context.field->getTextWithTags() != before);
		if (changed) {
			context.saveDraft();
		}
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			changed ? u"compose toggle case"_q : u"compose toggle case edge"_q);
		return true;
	};
	const auto joinLine = [&] {
		const auto before = context.field->getTextWithTags();
		auto cursor = context.field->textCursor();
		cursor.movePosition(QTextCursor::EndOfLine);
		cursor.deleteChar();
		if (context.field->getTextWithTags() != before) {
			cursor.insertText(u" "_q);
			context.field->setTextCursor(cursor);
		}
		const auto changed = (context.field->getTextWithTags() != before);
		if (changed) {
			context.saveDraft();
		}
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			changed ? u"compose join line"_q : u"compose join line edge"_q);
		return true;
	};
	const auto currentLineStart = [&] {
		return VimKeymapComposeLineAt(context.field->textCursor()).start;
	};
	const auto currentLineEnd = [&] {
		return VimKeymapComposeLineAt(context.field->textCursor()).end;
	};
	const auto currentLineFirstNonBlank = [&] {
		const auto cursor = context.field->textCursor();
		const auto line = VimKeymapComposeLineAt(cursor);
		const auto block = cursor.block();
		const auto text = block.text();
		auto position = line.start;
		while (position < line.end
			&& text[position - block.position()].isSpace()) {
			++position;
		}
		return position;
	};
	const auto setInputCursorPosition = [&](int position) {
		auto cursor = context.field->textCursor();
		cursor.clearSelection();
		cursor.setPosition(std::clamp(position, 0, maximumCursorPosition()));
		context.field->setTextCursor(cursor);
		context.field->ensureCursorVisible();
	};
	const auto setInputCursorAfterCurrentCharacter = [&] {
		auto cursor = context.field->textCursor();
		cursor.clearSelection();
		VimKeymapNormalizeComposeCursor(cursor);
		if (!cursor.block().text().isEmpty()) {
			cursor.movePosition(QTextCursor::Right);
		}
		context.field->setTextCursor(cursor);
		context.field->ensureCursorVisible();
	};
	const auto deleteToLineEnd = [&](bool inputAfter) {
		auto cursor = context.field->textCursor();
		cursor.setPosition(currentLineEnd(), QTextCursor::KeepAnchor);
		const auto changed = deleteSelection(cursor);
		if (inputAfter) {
			return changeAfterDelete(changed, u"compose change line end"_q);
		}
		Core::VimKeymap::TraceKey(
			e,
			changed ? u"compose delete to line end"_q : u"compose line end empty"_q);
		return true;
	};
	const auto enterInputMode = [&](const QString &status) {
		context.operation = 0;
		context.visualMode = 0;
		context.visualAnchor = -1;
		clearPending();
		Core::VimKeymap::SetNormalMode(false);
		focusField();
		Core::VimKeymap::TraceKey(e, status);
		return true;
	};
	const auto startVisualMode = [&](bool line) {
		context.operation = 0;
		context.visualMode = line ? 2 : 1;
		clearPending();
		auto cursor = line ? selectCurrentLine() : context.field->textCursor();
		if (!line) {
			VimKeymapNormalizeComposeCursor(cursor);
		}
		context.visualAnchor = cursor.hasSelection()
			? cursor.selectionStart()
			: cursor.position();
		if (line) {
			context.field->setTextCursor(cursor);
		} else {
			setVisualSelection(context.visualAnchor);
		}
		focusField();
		Core::VimKeymap::TraceKey(
			e,
			line ? u"compose visual line"_q : u"compose visual"_q);
		return true;
	};
	const auto linePaste = [&](bool before) {
		clearPending();
		auto cursor = context.field->textCursor();
		if (before) {
			cursor.movePosition(QTextCursor::StartOfLine);
		} else {
			cursor.movePosition(QTextCursor::EndOfLine);
			cursor.insertText(u"\n"_q);
		}
		context.field->setTextCursor(cursor);
		raw->paste();
		context.saveDraft();
		focusField();
		Core::VimKeymap::TraceKey(e, before ? u"compose paste before"_q : u"compose paste"_q);
		return true;
	};

	if (context.pending == kComposePendingVisualInner) {
		clearPending();
		if (VimKeymapPlainOrShiftTextKey(e, Qt::Key_W, u"w"_q, u"\u0446"_q)) {
			return selectVisualInnerWord();
		}
		focusField();
		Core::VimKeymap::TraceKey(e, u"compose visual inner cancelled"_q);
		return true;
	}

	if (context.visualMode) {
		if (Core::VimKeymap::UndoKey(e)) {
			clearVisual();
			return undoRedo(false);
		} else if (Core::VimKeymap::RedoKey(e)) {
			clearVisual();
			return undoRedo(true);
		} else if (plainLeft || plainRight) {
			clearPending();
			const auto changed = moveCursor(
				plainLeft ? QTextCursor::Left : QTextCursor::Right);
			traceMotion(
				changed,
				plainLeft
					? u"compose cursor left"_q
					: u"compose cursor right"_q);
			return true;
		} else if (VimKeymapPlainTextKey(e, Qt::Key_Y, u"y"_q, u"\u043D"_q)) {
			const auto copied = copySelection();
			clearVisual();
			focusField();
			Core::VimKeymap::TraceKey(
				e,
				copied ? u"compose visual yank"_q : u"compose visual empty"_q);
			return true;
		} else if (VimKeymapPlainTextKey(e, Qt::Key_D, u"d"_q, u"\u0432"_q)
			|| VimKeymapPlainTextKey(e, Qt::Key_X, u"x"_q, u"\u0447"_q)) {
			const auto deleted = deleteSelection(context.field->textCursor());
			context.visualMode = 0;
			context.visualAnchor = -1;
			context.refreshCursor();
			Core::VimKeymap::TraceKey(
				e,
				deleted ? u"compose visual delete"_q : u"compose visual empty"_q);
			return true;
		} else if (VimKeymapPlainTextKey(e, Qt::Key_I, u"i"_q, u"\u0448"_q)) {
			context.pending = kComposePendingVisualInner;
			focusField();
			Core::VimKeymap::TraceKey(e, u"compose visual inner pending"_q);
			return true;
		}
	}

	if (context.pending == kComposePendingReplace) {
		const auto modifiers = VimKeymapCleanModifiers(e);
		if (modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier) {
			return replaceNextChar(e->text());
		}
		clearPending();
		focusField();
		Core::VimKeymap::TraceKey(e, u"compose replace cancelled"_q);
		return true;
	}

	if (Core::VimKeymap::UndoKey(e)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return undoRedo(false);
	} else if (Core::VimKeymap::RedoKey(e)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return undoRedo(true);
	} else if (inputCursorMove) {
		context.operation = 0;
		clearVisual();
		clearPending();
		const auto changed = moveCursor(
			optionLeft ? QTextCursor::Left : QTextCursor::Right);
		Core::VimKeymap::TraceKey(
			e,
			changed ? u"compose cursor"_q : u"compose cursor edge"_q);
		return true;
	}

	if ((context.operation == kComposeOpDelete
			|| context.operation == kComposeOpChange
			|| context.operation == kComposeOpYank)
		&& VimKeymapPlainTextKey(e, Qt::Key_I, u"i"_q, u"\u0448"_q)) {
		context.operation
			= (context.operation == kComposeOpDelete)
				? kComposeOpDeleteInner
				: (context.operation == kComposeOpChange)
				? kComposeOpChangeInner
				: kComposeOpYankInner;
		clearPending();
		focusField();
		Core::VimKeymap::TraceKey(e, u"compose inner pending"_q);
		return true;
	} else if (VimKeymapPlainTextKey(e, Qt::Key_I, u"i"_q, u"\u0448"_q)) {
		return enterInputMode(u"compose insert mode"_q);
	} else if (VimKeymapShiftTextKey(e, Qt::Key_I, u"i"_q, u"\u0448"_q)) {
		clearPending();
		setInputCursorPosition(currentLineFirstNonBlank());
		return enterInputMode(u"compose insert line"_q);
	} else if (VimKeymapPlainTextKey(e, Qt::Key_A, u"a"_q, u"\u0444"_q)) {
		clearPending();
		setInputCursorAfterCurrentCharacter();
		return enterInputMode(u"compose append mode"_q);
	} else if (VimKeymapShiftTextKey(e, Qt::Key_A, u"a"_q, u"\u0444"_q)) {
		clearPending();
		setInputCursorPosition(currentLineEnd());
		return enterInputMode(u"compose append line"_q);
	} else if (VimKeymapPlainTextKey(e, Qt::Key_O, u"o"_q, u"\u0449"_q)) {
		clearPending();
		setInputCursorPosition(currentLineEnd());
		auto cursor = context.field->textCursor();
		cursor.insertText(u"\n"_q);
		context.field->setTextCursor(cursor);
		context.saveDraft();
		return enterInputMode(u"compose open line below"_q);
	} else if (VimKeymapShiftTextKey(e, Qt::Key_O, u"o"_q, u"\u0449"_q)) {
		clearPending();
		moveCursorTo(currentLineStart(), u"compose line start"_q);
		auto cursor = context.field->textCursor();
		cursor.insertText(u"\n"_q);
		context.field->setTextCursor(cursor);
		moveCursor(QTextCursor::Left);
		context.saveDraft();
		return enterInputMode(u"compose open line above"_q);
	} else if (VimKeymapPlainTextKey(e, Qt::Key_V, u"v"_q, u"\u043C"_q)) {
		return startVisualMode(false);
	} else if (VimKeymapShiftTextKey(e, Qt::Key_V, u"v"_q, u"\u043C"_q)) {
		return startVisualMode(true);
	} else if (VimKeymapPlainTextKey(e, Qt::Key_G, u"g"_q, u"\u043F"_q)) {
		if (context.pending == kComposePendingGoto) {
			clearPending();
			return moveCursorTo(0, u"compose start"_q);
		}
		context.pending = kComposePendingGoto;
		focusField();
		Core::VimKeymap::TraceKey(e, u"compose g pending"_q);
		return true;
	} else if (VimKeymapShiftTextKey(e, Qt::Key_G, u"g"_q, u"\u043F"_q)) {
		clearPending();
		return moveCursorTo(
			std::max(0, raw->document()->characterCount() - 1),
			u"compose end"_q);
	} else if (plainLeft) {
		clearPending();
		const auto changed = moveCursor(QTextCursor::Left);
		traceMotion(changed, u"compose cursor left"_q);
		return true;
	} else if (VimKeymapPlainTextKey(e, Qt::Key_J, u"j"_q, u"\u043E"_q)) {
		clearPending();
		const auto changed = moveCursor(QTextCursor::Down);
		traceMotion(changed, u"compose cursor down"_q);
		return true;
	} else if (VimKeymapPlainTextKey(e, Qt::Key_K, u"k"_q, u"\u043B"_q)) {
		clearPending();
		const auto changed = moveCursor(QTextCursor::Up);
		traceMotion(changed, u"compose cursor up"_q);
		return true;
	} else if (plainRight) {
		clearPending();
		const auto changed = moveCursor(QTextCursor::Right);
		traceMotion(changed, u"compose cursor right"_q);
		return true;
	} else if (VimKeymapPlainOrShiftTextKey(e, Qt::Key_W, u"w"_q, u"\u0446"_q)) {
		if (context.operation == kComposeOpDeleteInner
			|| context.operation == kComposeOpChangeInner
			|| context.operation == kComposeOpYankInner) {
			const auto change
				= (context.operation == kComposeOpChangeInner);
			const auto yank
				= (context.operation == kComposeOpYankInner);
			context.operation = 0;
			clearPending();
			if (yank) {
				const auto copied = copyInnerWord();
				Core::VimKeymap::TraceKey(
					e,
					copied ? u"compose yank inner word"_q : u"compose word empty"_q);
				return true;
			}
			const auto deleted = deleteInnerWord();
			if (change) {
				return changeAfterDelete(
					deleted,
					u"compose change inner word"_q);
			}
			Core::VimKeymap::TraceKey(
				e,
				deleted ? u"compose delete inner word"_q : u"compose word empty"_q);
			return true;
		}
		clearPending();
		const auto changed = moveCursor(QTextCursor::NextWord);
		traceMotion(changed, u"compose next word"_q);
		return true;
	} else if (VimKeymapPlainOrShiftTextKey(e, Qt::Key_B, u"b"_q, u"\u0438"_q)) {
		clearPending();
		const auto changed = moveCursor(QTextCursor::PreviousWord);
		traceMotion(changed, u"compose previous word"_q);
		return true;
	} else if (VimKeymapPlainOrShiftTextKey(e, Qt::Key_E, u"e"_q, u"\u0443"_q)) {
		clearPending();
		const auto changed = moveCursor(QTextCursor::EndOfWord);
		traceMotion(changed, u"compose word end"_q);
		return true;
	} else if (VimKeymapTypedText(e, u"^"_q)) {
		clearPending();
		return moveCursorTo(currentLineFirstNonBlank(), u"compose first nonblank"_q);
	} else if (Core::VimKeymap::Bindings::IsLineStart(e)) {
		clearPending();
		return moveCursorTo(currentLineStart(), u"compose line start"_q);
	} else if (Core::VimKeymap::Bindings::IsLineEnd(e)) {
		clearPending();
		return moveCursorTo(currentLineEnd(), u"compose line end"_q);
	} else if (VimKeymapTypedText(e, u"{"_q)) {
		clearPending();
		const auto changed = moveCursor(QTextCursor::PreviousBlock);
		traceMotion(changed, u"compose paragraph up"_q);
		return true;
	} else if (VimKeymapTypedText(e, u"}"_q)) {
		clearPending();
		const auto changed = moveCursor(QTextCursor::NextBlock);
		traceMotion(changed, u"compose paragraph down"_q);
		return true;
	} else if (VimKeymapTypedText(e, u"~"_q)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return toggleCase();
	} else if (VimKeymapPlainTextKey(e, Qt::Key_X, u"x"_q, u"\u0447"_q)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return deleteNextChar(false);
	} else if (VimKeymapShiftTextKey(e, Qt::Key_X, u"x"_q, u"\u0447"_q)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return deletePreviousChar();
	} else if (VimKeymapShiftTextKey(e, Qt::Key_D, u"d"_q, u"\u0432"_q)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return deleteToLineEnd(false);
	} else if (VimKeymapShiftTextKey(e, Qt::Key_C, u"c"_q, u"\u0441"_q)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return deleteToLineEnd(true);
	} else if (VimKeymapPlainTextKey(e, Qt::Key_C, u"c"_q, u"\u0441"_q)) {
		clearPending();
		if (!context.field->hasText()) {
			context.operation = 0;
			return false;
		} else if (context.operation == kComposeOpChange) {
			context.operation = 0;
			const auto changed = deleteCurrentLine();
			return changeAfterDelete(changed, u"compose change line"_q);
		}
		context.operation = kComposeOpChange;
		focusField();
		Core::VimKeymap::TraceKey(e, u"compose change pending"_q);
		return true;
	} else if (VimKeymapPlainTextKey(e, Qt::Key_D, u"d"_q, u"\u0432"_q)) {
		if (e->isAutoRepeat()) {
			return true;
		}
		clearPending();
		if (!context.field->hasText()) {
			context.operation = 0;
			return false;
		} else if (context.operation == kComposeOpDelete) {
			context.operation = 0;
			const auto deleted = deleteCurrentLine();
			Core::VimKeymap::TraceKey(
				e,
				deleted ? u"compose delete line"_q : u"compose line empty"_q);
			return true;
		}
		context.operation = kComposeOpDelete;
		focusField();
		if (Core::VimKeymap::ActionKey(e)
			== Core::VimKeymap::Action::DeleteMessage) {
			const auto revision = raw->document()->revision();
			context.deleteTimer.setCallback([=] {
				if (context.operation != kComposeOpDelete) {
					return;
				}
				context.operation = 0;
				if (!Core::VimKeymap::NormalMode()
					|| !context.canShowDeleteHints()
					|| raw->document()->revision() != revision) {
					return;
				}
				const auto handled = context.showDeleteHints();
				Core::VimKeymap::TraceCommand(u"d"_q,
					handled ? u"message delete hints"_q : u"no deletable messages"_q);
			});
			context.deleteTimer.callOnce(
				kVimKeymapDeleteTimeout,
				Qt::PreciseTimer);
		}
		Core::VimKeymap::TraceKey(e, u"compose delete pending"_q);
		return true;
	} else if (VimKeymapPlainTextKey(e, Qt::Key_S, u"s"_q, u"\u044B"_q)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return deleteNextChar(true);
	} else if (VimKeymapShiftTextKey(e, Qt::Key_S, u"s"_q, u"\u044B"_q)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		const auto changed = deleteCurrentLine();
		return changeAfterDelete(changed, u"compose substitute line"_q);
	} else if (VimKeymapPlainTextKey(e, Qt::Key_R, u"r"_q, u"\u043A"_q)) {
		context.operation = 0;
		clearVisual();
		context.pending = kComposePendingReplace;
		focusField();
		Core::VimKeymap::TraceKey(e, u"compose replace pending"_q);
		return true;
	} else if (VimKeymapShiftTextKey(e, Qt::Key_R, u"r"_q, u"\u043A"_q)) {
		return enterInputMode(u"compose replace input"_q);
	} else if (VimKeymapShiftTextKey(e, Qt::Key_J, u"j"_q, u"\u043E"_q)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return joinLine();
	} else if (VimKeymapPlainTextKey(e, Qt::Key_Y, u"y"_q, u"\u043D"_q)) {
		clearPending();
		if (!context.field->hasText()) {
			context.operation = 0;
			return false;
		} else if (context.operation == kComposeOpYank) {
			context.operation = 0;
			return copyCurrentLine();
		}
		context.operation = kComposeOpYank;
		focusField();
		Core::VimKeymap::TraceKey(e, u"compose yank pending"_q);
		return true;
	} else if (VimKeymapShiftTextKey(e, Qt::Key_Y, u"y"_q, u"\u043D"_q)) {
		context.operation = 0;
		clearVisual();
		clearPending();
		return copyCurrentLine();
	} else if (VimKeymapPlainTextKey(e, Qt::Key_P, u"p"_q, u"\u0437"_q)) {
		context.operation = 0;
		clearVisual();
		return linePaste(false);
	} else if (VimKeymapShiftTextKey(e, Qt::Key_P, u"p"_q, u"\u0437"_q)) {
		context.operation = 0;
		clearVisual();
		return linePaste(true);
	} else if (context.pending) {
		clearPending();
		Core::VimKeymap::TraceKey(e, u"compose pending cancelled"_q);
		return true;
	} else if (context.operation) {
		context.operation = 0;
		clearPending();
		Core::VimKeymap::TraceKey(e, u"compose operator cancelled"_q);
		return true;
	}
	return false;
}

} // namespace Core::VimKeymap
