/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/bot_keyboard.h"

#include "api/api_bot.h"
#include "core/click_handler_types.h"
#include "core/vim_keymap.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "ui/cached_round_corners.h"
#include "ui/chat/chat_style_radius.h"
#include "ui/painter.h"
#include "ui/round_rect.h"
#include "ui/ui_utility.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_widgets.h"

#include <QtGui/QKeyEvent>

#include <algorithm>

namespace {

const auto kBotKeyboardRounding = Ui::BubbleRounding{
	Ui::BubbleCornerRounding::Large,
	Ui::BubbleCornerRounding::Large,
	Ui::BubbleCornerRounding::Large,
	Ui::BubbleCornerRounding::Large,
};

class Style : public ReplyKeyboard::Style {
public:
	Style(
		not_null<BotKeyboard*> parent,
		const style::BotKeyboardButton &st);

	Images::CornersMaskRef buttonRounding(
		Ui::BubbleRounding outer,
		RectParts sides) const override;

	const style::TextStyle &textStyle() const override;
	void repaint(not_null<const HistoryItem*> item) const override;

protected:
	void paintButtonBg(
		QPainter &p,
		const Ui::ChatStyle *st,
		const QRect &rect,
		HistoryMessageMarkupButton::Color color,
		Ui::BubbleRounding rounding,
		float64 howMuchOver) const override;
	void paintButtonStart(
		QPainter &p,
		const Ui::ChatStyle *st,
		HistoryMessageMarkupButton::Color color) const override;
	void paintButtonIcon(
		QPainter &p,
		const Ui::ChatStyle *st,
		const QRect &rect,
		int outerWidth,
		HistoryMessageMarkupButton::Type type) const override;
	void paintButtonLoading(
		QPainter &p,
		const Ui::ChatStyle *st,
		const QRect &rect,
		HistoryMessageMarkupButton::Color color,
		int outerWidth,
		Ui::BubbleRounding rounding) const override;
	int minButtonWidth(HistoryMessageMarkupButton::Type type) const override;

private:
	not_null<BotKeyboard*> _parent;

};

Style::Style(
	not_null<BotKeyboard*> parent,
	const style::BotKeyboardButton &st)
: ReplyKeyboard::Style(st), _parent(parent) {
}

void Style::paintButtonStart(
		QPainter &p,
		const Ui::ChatStyle *st,
		HistoryMessageMarkupButton::Color color) const {
	using Color = HistoryMessageMarkupButton::Color;
	p.setPen((color == Color::Normal) ? st::botKbColor : st::white);
	p.setFont(st::botKbStyle.font);
}

const style::TextStyle &Style::textStyle() const {
	return st::botKbStyle;
}

void Style::repaint(not_null<const HistoryItem*> item) const {
	_parent->update();
}

Images::CornersMaskRef Style::buttonRounding(
		Ui::BubbleRounding outer,
		RectParts sides) const {
	using namespace Images;
	using namespace Ui;
	using Radius = CachedCornerRadius;
	using Corner = BubbleCornerRounding;
	auto result = CornersMaskRef(CachedCornersMasks(Radius::BubbleSmall));
	const auto &large = CachedCornersMasks(Radius::BubbleLarge);
	const auto round = [&](
			RectPart vertSide,
			RectPart horizSide,
			int index) {
		if ((sides & vertSide)
			&& (sides & horizSide)
			&& (outer[index] == Corner::Large)) {
			result.p[index] = &large[index];
		}
	};
	round(RectPart::Top, RectPart::Left, kTopLeft);
	round(RectPart::Top, RectPart::Right, kTopRight);
	round(RectPart::Bottom, RectPart::Left, kBottomLeft);
	round(RectPart::Bottom, RectPart::Right, kBottomRight);
	return result;
}

void Style::paintButtonBg(
		QPainter &p,
		const Ui::ChatStyle *st,
		const QRect &rect,
		HistoryMessageMarkupButton::Color color,
		Ui::BubbleRounding rounding,
		float64 howMuchOver) const {
	using Color = HistoryMessageMarkupButton::Color;
	using Corner = Ui::BubbleCornerRounding;
	const auto bg = (color == Color::Normal)
		? st::botKbBg->c
		: (color == Color::Primary)
		? st::botKbPrimaryBg->c
		: (color == Color::Danger)
		? st::botKbDangerBg->c
		: st::botKbSuccessBg->c;
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(bg);
	const auto large = Ui::BubbleRadiusLarge();
	const auto small = Ui::BubbleRadiusSmall();
	const auto radius = [&](int index) {
		return (rounding[index] == Corner::Large) ? large : small;
	};
	const auto tl = radius(0);
	const auto tr = radius(1);
	const auto bl = radius(2);
	const auto br = radius(3);
	if ((tl == tr) && (tl == bl) && (tl == br)) {
		p.drawRoundedRect(rect, tl, tl);
	} else {
		p.drawPath(Ui::ComplexRoundedRectPath(rect, tl, tr, bl, br));
	}
}

void Style::paintButtonIcon(
		QPainter &p,
		const Ui::ChatStyle *st,
		const QRect &rect,
		int outerWidth,
		HistoryMessageMarkupButton::Type type) const {
	// Buttons with icons should not appear here.
}

void Style::paintButtonLoading(
		QPainter &p,
		const Ui::ChatStyle *st,
		const QRect &rect,
		HistoryMessageMarkupButton::Color color,
		int outerWidth,
		Ui::BubbleRounding rounding) const {
	// Buttons with loading progress should not appear here.
}

int Style::minButtonWidth(HistoryMessageMarkupButton::Type type) const {
	int result = 2 * buttonPadding();
	return result;
}

} // namespace

BotKeyboard::BotKeyboard(
	not_null<Window::SessionController*> controller,
	QWidget *parent)
: RpWidget(parent)
, _controller(controller)
, _st(&st::botKbButton) {
	setGeometry(0, 0, _st->margin, st::botKbScroll.deltat);
	_height = st::botKbScroll.deltat;
	setMouseTracking(true);
}

void BotKeyboard::paintEvent(QPaintEvent *e) {
	Painter p(this);

	auto clip = e->rect();
	p.fillRect(clip, st::historyComposeAreaBg);

	if (_impl) {
		int x = rtl() ? st::botKbScroll.width : _st->margin;
		p.translate(x, st::botKbScroll.deltat);
		_impl->paint(
			p,
			nullptr,
			kBotKeyboardRounding,
			width(),
			clip.translated(-x, -st::botKbScroll.deltat),
			_controller->isGifPausedAtLeastFor(Window::GifPauseReason::Any));
		vimKeymapPaintHints(p);
	}
}

void BotKeyboard::mousePressEvent(QMouseEvent *e) {
	_lastMousePos = e->globalPos();
	updateSelected();

	ClickHandler::pressed();
}

void BotKeyboard::mouseMoveEvent(QMouseEvent *e) {
	_lastMousePos = e->globalPos();
	updateSelected();
}

void BotKeyboard::mouseReleaseEvent(QMouseEvent *e) {
	_lastMousePos = e->globalPos();
	updateSelected();

	if (ClickHandlerPtr activated = ClickHandler::unpressed()) {
		ActivateClickHandler(window(), activated, {
			e->button(),
			QVariant::fromValue(ClickHandlerContext{
				.sessionWindow = base::make_weak(_controller),
			})
		});
	}
}

void BotKeyboard::enterEventHook(QEnterEvent *e) {
	_lastMousePos = QCursor::pos();
	updateSelected();
}

void BotKeyboard::leaveEventHook(QEvent *e) {
	clearSelection();
}

bool BotKeyboard::moderateKeyActivate(
		int key,
		Fn<ClickContext(FullMsgId)> context) {
	const auto &data = _controller->session().data();

	const auto botCommand = [](int key) {
		if (key == Qt::Key_Q || key == Qt::Key_6) {
			return u"/translate"_q;
		} else if (key == Qt::Key_W || key == Qt::Key_5) {
			return u"/eng"_q;
		} else if (key == Qt::Key_3) {
			return u"/pattern"_q;
		} else if (key == Qt::Key_4) {
			return u"/abuse"_q;
		} else if (key == Qt::Key_0 || key == Qt::Key_E || key == Qt::Key_9) {
			return u"/undo"_q;
		} else if (key == Qt::Key_Plus
				|| key == Qt::Key_QuoteLeft
				|| key == Qt::Key_7) {
			return u"/next"_q;
		} else if (key == Qt::Key_Period
				|| key == Qt::Key_S
				|| key == Qt::Key_8) {
			return u"/stats"_q;
		}
		return QString();
	};

	if (const auto item = data.message(_wasForMsgId)) {
		if (const auto markup = item->Get<HistoryMessageReplyMarkup>()) {
			if (key >= Qt::Key_1 && key <= Qt::Key_2) {
				const auto index = int(key - Qt::Key_1);
				if (!markup->data.rows.empty()
					&& index >= 0
					&& index < int(markup->data.rows.front().size())) {
					Api::ActivateBotCommand(
						context(
							_wasForMsgId).other.value<ClickHandlerContext>(),
						0,
						index);
					return true;
				}
			} else if (const auto user = item->history()->peer->asUser()) {
				if (user->isBot() && item->from() == user) {
					const auto command = botCommand(key);
					if (!command.isEmpty()) {
						_sendCommandRequests.fire({
							.peer = user,
							.command = command,
							.context = item->fullId(),
						});
					}
					return true;
				}
			}
		}
	}
	return false;
}

void BotKeyboard::clickHandlerActiveChanged(const ClickHandlerPtr &p, bool active) {
	if (!_impl) return;
	_impl->clickHandlerActiveChanged(p, active);
}

void BotKeyboard::clickHandlerPressedChanged(const ClickHandlerPtr &p, bool pressed) {
	if (!_impl) return;
	_impl->clickHandlerPressedChanged(p, pressed, kBotKeyboardRounding);
}

bool BotKeyboard::updateMarkup(HistoryItem *to, bool force) {
	if (!to || !to->definesReplyKeyboard()) {
		if (_wasForMsgId.msg) {
			vimKeymapClearHints();
			_maximizeSize = _singleUse = _forceReply = _persistent = false;
			_wasForMsgId = FullMsgId();
			_placeholder = QString();
			_impl = nullptr;
			return true;
		}
		return false;
	}

	const auto peerId = to->history()->peer->id;
	if (_wasForMsgId == FullMsgId(peerId, to->id) && !force) {
		return false;
	}

	_wasForMsgId = FullMsgId(peerId, to->id);
	vimKeymapClearHints();

	auto markupFlags = to->replyKeyboardFlags();
	const auto markup = to->Get<HistoryMessageReplyMarkup>();
	const auto hasVisibleRows = markup
		&& !markup->data.rows.empty()
		&& !(markupFlags & ReplyMarkupFlag::Inline);
	_forceReply = markupFlags & ReplyMarkupFlag::ForceReply;
	_maximizeSize = !(markupFlags & ReplyMarkupFlag::Resize);
	_singleUse = (_forceReply && !hasVisibleRows)
		|| (markupFlags & ReplyMarkupFlag::SingleUse);
	_persistent = (markupFlags & ReplyMarkupFlag::Persistent);

	_placeholder = markup ? markup->data.placeholder : QString();

	_impl = nullptr;
	if (hasVisibleRows) {
		_impl = std::make_unique<ReplyKeyboard>(
			to,
			std::make_unique<Style>(this, *_st));
	}

	resizeToWidth(width(), _maxOuterHeight);

	return true;
}

bool BotKeyboard::hasMarkup() const {
	return _impl != nullptr;
}

bool BotKeyboard::forceReply() const {
	return _forceReply;
}

bool BotKeyboard::vimKeymapBeginHints() {
	vimKeymapClearHints();
	if (!_impl || isHidden() || !isVisible()) {
		return false;
	}
	for (const auto &entry : _impl->linkRects()) {
		_vimKeymapHints.push_back({
			.badge = QRect(entry.rect.topLeft() + QPoint(6, 6), QSize()),
			.link = entry.link,
		});
	}
	if (_vimKeymapHints.empty()) {
		return false;
	}
	vimKeymapAssignHintLabels();
	update();
	return true;
}

bool BotKeyboard::vimKeymapHandleHintKey(not_null<QKeyEvent*> e) {
	if (_vimKeymapHints.empty()) {
		return false;
	} else if (e->key() == Qt::Key_Escape) {
		vimKeymapClearHints();
		return true;
	} else if (e->key() == Qt::Key_Backspace) {
		if (!_vimKeymapHintPrefix.isEmpty()) {
			_vimKeymapHintPrefix.chop(1);
			update();
		}
		return true;
	}
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	if (modifiers != Qt::NoModifier && modifiers != Qt::ShiftModifier) {
		return true;
	}
	const auto text = Core::VimKeymap::HintInput(e);
	if (text.isEmpty()) {
		return true;
	}
	_vimKeymapHintPrefix += text.front();
	auto exact = (const VimKeymapHint*)nullptr;
	auto hasPrefix = false;
	for (const auto &hint : _vimKeymapHints) {
		if (hint.label == _vimKeymapHintPrefix) {
			exact = &hint;
			break;
		} else if (hint.label.startsWith(_vimKeymapHintPrefix)) {
			hasPrefix = true;
		}
	}
	if (exact) {
		const auto chosen = *exact;
		return vimKeymapTriggerHint(chosen);
	} else if (!hasPrefix) {
		_vimKeymapHintPrefix.clear();
	}
	update();
	return true;
}

void BotKeyboard::vimKeymapClearHints() {
	if (_vimKeymapHints.empty() && _vimKeymapHintPrefix.isEmpty()) {
		return;
	}
	_vimKeymapHints.clear();
	_vimKeymapHintPrefix.clear();
	update();
}

void BotKeyboard::vimKeymapAssignHintLabels() {
	for (auto i = 0, count = int(_vimKeymapHints.size()); i != count; ++i) {
		_vimKeymapHints[i].label = Core::VimKeymap::HintLabel(i, count);
	}
}

bool BotKeyboard::vimKeymapTriggerHint(const VimKeymapHint &hint) {
	const auto link = hint.link;
	vimKeymapClearHints();
	if (!link) {
		return false;
	}
	ActivateClickHandler(window(), link, {
		Qt::LeftButton,
		QVariant::fromValue(ClickHandlerContext{
			.sessionWindow = base::make_weak(_controller),
		}),
	});
	return true;
}

void BotKeyboard::vimKeymapPaintHints(Painter &p) const {
	if (_vimKeymapHints.empty()) {
		return;
	}
	p.save();
	const auto hintSize = Core::VimKeymap::HintSize();
	const auto font = QFont(u"Menlo"_q, hintSize, QFont::DemiBold);
	const auto metrics = QFontMetrics(font);
	const auto horizontalPadding = std::max(7, hintSize / 2);
	const auto verticalPadding = std::max(3, hintSize / 4);
	const auto implWidth = std::max(
		1,
		width() - _st->margin - st::botKbScroll.width);
	const auto implHeight = std::max(
		1,
		height() - st::botKbScroll.deltat - st::botKbScroll.deltab);
	p.setFont(font);
	p.setRenderHint(QPainter::Antialiasing, true);
	for (const auto &hint : _vimKeymapHints) {
		const auto remaining = hint.label.mid(_vimKeymapHintPrefix.size());
		const auto label = _vimKeymapHintPrefix.isEmpty()
			? hint.label
			: remaining.isEmpty()
			? hint.label
			: remaining;
		const auto textWidth = metrics.horizontalAdvance(label);
		auto rect = QRect(
			hint.badge.topLeft(),
			QSize(
				textWidth + 2 * horizontalPadding,
				metrics.height() + 2 * verticalPadding));
		const auto minLeft = 4;
		const auto maxLeft = std::max(minLeft, implWidth - rect.width() - 4);
		const auto minTop = 4;
		const auto maxTop = std::max(minTop, implHeight - rect.height() - 4);
		rect.moveLeft(std::clamp(rect.left(), minLeft, maxLeft));
		rect.moveTop(std::clamp(rect.top(), minTop, maxTop));
		const auto radius = rect.height() / 2;
		p.setPen(QColor(102, 78, 0, 105));
		p.setBrush(QColor(255, 218, 72, 246));
		p.drawRoundedRect(rect, radius, radius);
		p.setPen(QColor(28, 24, 14));
		p.drawText(rect, Qt::AlignCenter, label);
	}
	p.restore();
}

int BotKeyboard::resizeGetHeight(int newWidth) {
	updateStyle(newWidth);
	_height = st::botKbScroll.deltat + st::botKbScroll.deltab + (_impl ? _impl->naturalHeight() : 0);
	if (_maximizeSize) {
		accumulate_max(_height, _maxOuterHeight);
	}
	if (_impl) {
		int implWidth = newWidth - _st->margin - st::botKbScroll.width;
		int implHeight = _height - (st::botKbScroll.deltat + st::botKbScroll.deltab);
		_impl->resize(implWidth, implHeight);
	}
	return _height;
}

bool BotKeyboard::maximizeSize() const {
	return _maximizeSize;
}

bool BotKeyboard::singleUse() const {
	return _singleUse;
}

bool BotKeyboard::persistent() const {
	return _persistent;
}

void BotKeyboard::updateStyle(int newWidth) {
	if (!_impl) return;

	int implWidth = newWidth - st::botKbButton.margin - st::botKbScroll.width;
	_st = _impl->isEnoughSpace(implWidth, st::botKbButton) ? &st::botKbButton : &st::botKbTinyButton;

	_impl->setStyle(std::make_unique<Style>(this, *_st));
}

void BotKeyboard::clearSelection() {
	if (_impl) {
		if (ClickHandler::setActive(ClickHandlerPtr(), this)) {
			Ui::Tooltip::Hide();
			setCursor(style::cur_default);
		}
	}
}

QPoint BotKeyboard::tooltipPos() const {
	return _lastMousePos;
}

bool BotKeyboard::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

QString BotKeyboard::tooltipText() const {
	if (ClickHandlerPtr lnk = ClickHandler::getActive()) {
		return lnk->tooltip();
	}
	return QString();
}

void BotKeyboard::updateSelected() {
	Ui::Tooltip::Show(1000, this);

	if (!_impl) return;

	auto p = mapFromGlobal(_lastMousePos);
	auto x = rtl() ? st::botKbScroll.width : _st->margin;

	auto link = _impl->getLink(p - QPoint(x, _st->margin));
	if (ClickHandler::setActive(link, this)) {
		Ui::Tooltip::Hide();
		setCursor(link ? style::cur_pointer : style::cur_default);
	}
}

auto BotKeyboard::sendCommandRequests() const
-> rpl::producer<Bot::SendCommandRequest> {
	return _sendCommandRequests.events();
}

BotKeyboard::~BotKeyboard() = default;
