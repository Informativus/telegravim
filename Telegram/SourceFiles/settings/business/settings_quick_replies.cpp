/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/business/settings_quick_replies.h"

#include "chat_helpers/compose/compose_show.h"
#include "chat_helpers/message_field.h"
#include "data/data_messages.h"
#include "data/data_session.h"
#include "data/local_quick_replies.h"
#include "data/local_quick_reply_view.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/business/settings_shortcut_messages.h"
#include "settings/settings_common.h"
#include "settings/settings_common_session.h"
#include "storage/local_quick_replies_format.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"

#include <algorithm>
#include <QtCore/QUuid>
#include <QtGui/QFocusEvent>
#include <QtGui/QPolygon>

#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

constexpr auto kShortcutLimit = 32;

class QuickReplyHelpButton final : public Ui::IconButton {
public:
	QuickReplyHelpButton(QWidget *parent);

protected:
	void focusInEvent(QFocusEvent *event) override;
	void focusOutEvent(QFocusEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

};

class QuickReplyField final : public Ui::RpWidget {
public:
	QuickReplyField(
		QWidget *parent,
		const style::InputField &fieldStyle,
		rpl::producer<QString> placeholder,
		const QString &value,
		QString helpName,
		QString helpDescription,
		Fn<void()> showHelp);

	[[nodiscard]] not_null<Ui::InputField*> field() const;

protected:
	int resizeGetHeight(int width) override;

private:
	object_ptr<Ui::InputField> _field;
	object_ptr<QuickReplyHelpButton> _help;

};

class QuickReplyGuideButton final : public Ui::RpWidget {
public:
	QuickReplyGuideButton(QWidget *parent, Fn<void()> callback);

protected:
	int resizeGetHeight(int width) override;

private:
	object_ptr<QuickReplyHelpButton> _button;

};

enum class QuickReplyGuideSampleKind {
	Field,
	Message,
	Composer,
};

class QuickReplyGuideSample final : public Ui::RpWidget {
public:
	QuickReplyGuideSample(
		QWidget *parent,
		rpl::producer<QString> label,
		rpl::producer<TextWithEntities> value,
		rpl::producer<QString> action = nullptr,
		QuickReplyGuideSampleKind kind = QuickReplyGuideSampleKind::Field);

protected:
	int resizeGetHeight(int width) override;
	void paintEvent(QPaintEvent *event) override;

private:
	object_ptr<Ui::FlatLabel> _label;
	object_ptr<Ui::FlatLabel> _value;
	object_ptr<Ui::FlatLabel> _action = { nullptr };
	QRect _fieldRect;
	QRect _actionRect;
	QRect _sendIconRect;
	const QuickReplyGuideSampleKind _kind;

};

class QuickReplyGuidePanel final : public Ui::RpWidget {
public:
	QuickReplyGuidePanel(
		QWidget *parent,
		rpl::producer<QString> caption);

	[[nodiscard]] not_null<Ui::VerticalLayout*> content() const;

protected:
	int resizeGetHeight(int width) override;
	void paintEvent(QPaintEvent *event) override;

private:
	object_ptr<Ui::VerticalLayout> _content;

};

QuickReplyHelpButton::QuickReplyHelpButton(QWidget *parent)
: IconButton(parent, st::settingsQuickReplyHelpButton) {
	setFocusPolicy(Qt::StrongFocus);
}

void QuickReplyHelpButton::focusInEvent(QFocusEvent *event) {
	IconButton::focusInEvent(event);
	update();
}

void QuickReplyHelpButton::focusOutEvent(QFocusEvent *event) {
	IconButton::focusOutEvent(event);
	update();
}

void QuickReplyHelpButton::paintEvent(QPaintEvent *event) {
	IconButton::paintEvent(event);
	if (!hasFocus()) {
		return;
	}
	const auto inset = st::settingsQuickReplyHelpFocusInset;
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(st::windowActiveTextFg->c, st::lineWidth));
	p.drawEllipse(rect().adjusted(inset, inset, -inset, -inset));
}

QuickReplyField::QuickReplyField(
	QWidget *parent,
	const style::InputField &fieldStyle,
	rpl::producer<QString> placeholder,
	const QString &value,
	QString helpName,
	QString helpDescription,
	Fn<void()> showHelp)
: RpWidget(parent)
, _field(this, fieldStyle, std::move(placeholder), value)
, _help(this) {
	_field->setAccessibleDescription(helpDescription);
	_help->setAccessibleName(std::move(helpName));
	_help->setClickedCallback(std::move(showHelp));
	_field->show();
	_help->show();
}

not_null<Ui::InputField*> QuickReplyField::field() const {
	return _field;
}

int QuickReplyField::resizeGetHeight(int width) {
	const auto fieldWidth = std::max(
		width - _help->width() - st::settingsQuickReplyHelpSkip,
		0);
	_field->resizeToWidth(fieldWidth);
	const auto height = std::max(_field->height(), _help->height());
	_field->moveToLeft(0, 0, width);
	_help->moveToRight(0, (height - _help->height()) / 2, width);
	return height;
}

QuickReplyGuideButton::QuickReplyGuideButton(
	QWidget *parent,
	Fn<void()> callback)
: RpWidget(parent)
, _button(this) {
	_button->setAccessibleName(tr::lng_local_replies_how_it_works(tr::now));
	_button->setClickedCallback(std::move(callback));
	_button->show();
}

int QuickReplyGuideButton::resizeGetHeight(int width) {
	_button->moveToRight(0, 0, width);
	return _button->height();
}

QuickReplyGuideSample::QuickReplyGuideSample(
	QWidget *parent,
	rpl::producer<QString> label,
	rpl::producer<TextWithEntities> value,
	rpl::producer<QString> action,
	QuickReplyGuideSampleKind kind)
: RpWidget(parent)
, _label(this, std::move(label), st::settingsQuickReplyGuideLabel)
, _value(this, std::move(value), (kind == QuickReplyGuideSampleKind::Message)
	? st::settingsQuickReplyGuideBubbleValue
	: st::settingsQuickReplyGuideValue)
, _kind(kind) {
	if (action) {
		_action.create(
			this,
			std::move(action),
			st::settingsQuickReplyGuideAction);
		_action->show();
	}
	_label->show();
	_value->show();
}

int QuickReplyGuideSample::resizeGetHeight(int width) {
	const auto padding = st::settingsQuickReplyGuideInputPadding;
	const auto skip = st::settingsQuickReplyGuideLabelSkip;
	const auto message = (_kind == QuickReplyGuideSampleKind::Message);
	const auto composer = (_kind == QuickReplyGuideSampleKind::Composer);
	const auto stacked = message
		|| composer
		|| width < st::settingsQuickReplyGuideStackWidth;
	const auto labelWidth = stacked
		? width
		: std::min(st::settingsQuickReplyGuideLabelWidth, width / 3);
	_label->resizeToWidth(labelWidth);
	const auto left = message
		? st::settingsQuickReplyGuideBubbleTail
		: stacked ? 0 : labelWidth + skip;
	const auto top = stacked ? _label->height() + skip : 0;
	const auto fieldWidth = message
		? std::min(
			std::max(width - left, 0),
			_value->textMaxWidth() + 2 * padding)
		: std::max(width - left, 0);
	const auto innerWidth = std::max(fieldWidth - 2 * padding, 0);
	const auto &sendIcon = st::settingsQuickReplyGuideSendIcon;
	const auto actionWidth = _action
		? std::min(_action->textMaxWidth(), innerWidth / 3)
		: 0;
	const auto buttonWidth = composer
		? actionWidth + sendIcon.width() + skip + 2 * padding
		: 0;
	_value->resizeToWidth(std::max(innerWidth - buttonWidth, 0));
	auto fieldHeight = _value->height() + 2 * padding;
	if (_action) {
		_action->resizeToWidth(actionWidth);
		const auto actionHeight = (composer
			? std::max(_action->height(), sendIcon.height())
			: _action->height()) + 2 * padding;
		if (composer) {
			fieldHeight = std::max(fieldHeight, actionHeight);
			_actionRect = style::rtlrect(
				width - buttonWidth,
				top,
				buttonWidth,
				fieldHeight,
				width);
			_sendIconRect = style::rtlrect(
				width - padding - sendIcon.width(),
				top + (fieldHeight - sendIcon.height()) / 2,
				sendIcon.width(),
				sendIcon.height(),
				width);
			_action->moveToRight(
				padding + sendIcon.width() + skip,
				top + (fieldHeight - _action->height()) / 2,
				width);
		} else {
			_actionRect = style::rtlrect(
				width - actionWidth - 3 * padding,
				top + fieldHeight,
				actionWidth + 2 * padding,
				actionHeight,
				width);
			_action->moveToRight(
				2 * padding,
				_actionRect.y() + padding,
				width);
			fieldHeight += actionHeight + padding;
		}
	}
	const auto height = std::max(top + fieldHeight, _label->height());
	_fieldRect = style::rtlrect(left, top, fieldWidth, fieldHeight, width);
	_label->moveToLeft(
		0,
		stacked ? 0 : (height - _label->height()) / 2,
		width);
	_value->moveToLeft(left + padding, top + padding, width);
	return height;
}

void QuickReplyGuideSample::paintEvent(QPaintEvent *) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto message = (_kind == QuickReplyGuideSampleKind::Message);
	const auto radius = message
		? st::settingsQuickReplyGuideBubbleRadius
		: st::settingsQuickReplyGuideInputRadius;
	p.setPen(Qt::NoPen);
	p.setBrush(message ? st::msgInBg : st::windowBg);
	p.drawRoundedRect(_fieldRect, radius, radius);
	if (message) {
		const auto tail = st::settingsQuickReplyGuideBubbleTail;
		const auto direction = style::RightToLeft() ? -1 : 1;
		const auto x = style::RightToLeft()
			? _fieldRect.x() + _fieldRect.width()
			: _fieldRect.x();
		const auto bottom = _fieldRect.y() + _fieldRect.height();
		p.drawPolygon(QPolygon({
			QPoint(x, bottom - tail),
			QPoint(x - direction * tail, bottom),
			QPoint(x + direction * radius, bottom),
		}));
	}
	if (_action) {
		p.setBrush(st::activeButtonBg);
		p.drawRoundedRect(_actionRect, radius, radius);
	}
	if (_kind == QuickReplyGuideSampleKind::Composer) {
		st::settingsQuickReplyGuideSendIcon.paintInCenter(p, _sendIconRect);
	}
}

QuickReplyGuidePanel::QuickReplyGuidePanel(
	QWidget *parent,
	rpl::producer<QString> caption)
: RpWidget(parent)
, _content(this) {
	_content->add(
		object_ptr<Ui::FlatLabel>(
			_content,
			std::move(caption),
			st::settingsQuickReplyGuideCaption),
		st::settingsQuickReplyGuideCaptionPadding);
	_content->show();
	_content->heightValue() | rpl::on_next([=](int height) {
		resize(width(), height + 2 * st::settingsQuickReplyGuidePanelPadding);
	}, lifetime());
}

not_null<Ui::VerticalLayout*> QuickReplyGuidePanel::content() const {
	return _content;
}

int QuickReplyGuidePanel::resizeGetHeight(int width) {
	const auto padding = st::settingsQuickReplyGuidePanelPadding;
	_content->resizeToWidth(std::max(width - 2 * padding, 0));
	_content->moveToLeft(padding, padding, width);
	return _content->height() + 2 * padding;
}

void QuickReplyGuidePanel::paintEvent(QPaintEvent *) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = st::settingsQuickReplyGuidePanelRadius;
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgOver);
	p.drawRoundedRect(rect(), radius, radius);
}

void ShowQuickRepliesHelp(
		not_null<Window::SessionController*> controller,
		rpl::producer<QString> title,
		v::text::data text) {
	controller->show(Ui::MakeInformBox({
		.text = std::move(text),
		.title = std::move(title),
	}), Ui::LayerOption::KeepOther);
}

void ShowQuickRepliesHelp(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> title,
		v::text::data text) {
	box->uiShow()->show(Ui::MakeInformBox({
		.text = std::move(text),
		.title = std::move(title),
	}), Ui::LayerOption::KeepOther);
}

void BuildQuickRepliesGuide(not_null<Ui::GenericBox*> box) {
	box->setWidth(st::boxWideWidth);
	box->setTitle(tr::lng_local_replies_how_it_works());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_local_replies_guide_intro(),
		st::boxLabel));
	const auto panel = [&](rpl::producer<QString> caption) {
		return box->addRow(
			object_ptr<QuickReplyGuidePanel>(box, std::move(caption)),
			st::settingsQuickReplyGuidePanelMargin)->content();
	};
	const auto sample = [](
			not_null<Ui::VerticalLayout*> content,
			rpl::producer<QString> label,
			rpl::producer<TextWithEntities> value,
			rpl::producer<QString> action = nullptr,
			QuickReplyGuideSampleKind kind = QuickReplyGuideSampleKind::Field) {
		content->add(
			object_ptr<QuickReplyGuideSample>(
				content,
				std::move(label),
				std::move(value),
				std::move(action),
				kind),
			st::settingsQuickReplyGuideSampleMargin);
	};
	const auto metadata = panel(tr::lng_local_replies_guide_create());
	sample(
		metadata,
		tr::lng_local_replies_title_help(),
		tr::lng_local_replies_guide_title(tr::marked));
	sample(
		metadata,
		tr::lng_local_replies_shortcut_help(),
		rpl::single(tr::marked(u"hello"_q)));
	sample(
		metadata,
		tr::lng_local_replies_category_help(),
		tr::lng_local_replies_guide_category(tr::marked));

	auto variable = tr::marked(u"{{name}}"_q);
	variable.entities.push_back({ EntityType::Code, 0, int(variable.text.size()) });
	sample(
		panel(tr::lng_local_replies_guide_compose()),
		tr::lng_local_replies_guide_message(),
		tr::lng_local_replies_guide_example(
			lt_text,
			rpl::single(std::move(variable)),
			tr::marked),
		nullptr,
		QuickReplyGuideSampleKind::Message);

	const auto insert = panel(tr::lng_local_replies_guide_insert());
	sample(
		insert,
		tr::lng_local_replies_shortcut_help(),
		rpl::single(tr::marked(u"/hello"_q)));
	sample(
		insert,
		rpl::single(u"name"_q),
		tr::lng_local_replies_guide_person(tr::marked),
		tr::lng_local_replies_insert());
	sample(
		panel(tr::lng_local_replies_guide_send()),
		tr::lng_local_replies_guide_draft(),
		tr::lng_local_replies_guide_example(
			lt_text,
			tr::lng_local_replies_guide_person(tr::marked),
			tr::marked),
		tr::lng_send_button(),
		QuickReplyGuideSampleKind::Composer);
	box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
}

void ShowQuickRepliesGuide(not_null<Ui::GenericBox*> parent) {
	parent->uiShow()->show(
		Box(BuildQuickRepliesGuide),
		Ui::LayerOption::KeepOther);
}

void ShowQuickRepliesGuide(
		not_null<Window::SessionController*> controller) {
	controller->show(Box(BuildQuickRepliesGuide), Ui::LayerOption::KeepOther);
}

void EditLocalQuickReply(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller,
	Data::LocalQuickReply reply,
	Fn<void(QString)> saved);

void PreviewLocalQuickReplyRecovery(
	not_null<Window::SessionController*> controller,
	Data::LocalQuickReply reply,
	Fn<void()> recover);

class QuickReplies : public Section<QuickReplies> {
public:
	QuickReplies(
		QWidget *parent,
		not_null<Window::SessionController*> controller);
	~QuickReplies();

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent(not_null<Window::SessionController*> controller);

};

QuickReplies::QuickReplies(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent(controller);
}

QuickReplies::~QuickReplies() = default;

rpl::producer<QString> QuickReplies::title() {
	return tr::lng_replies_title();
}

void QuickReplies::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	AddDividerTextWithLottie(content, {
		.lottie = u"writing"_q,
		.lottieSize = st::settingsCloudPasswordIconSize,
		.lottieMargins = st::peerAppearanceIconPadding,
		.showFinished = showFinishes(),
		.about = tr::lng_local_replies_about(tr::marked),
		.aboutMargins = st::peerAppearanceCoverLabelMargin,
	});
	Ui::AddSkip(content);
	const auto replies = &controller->session().data().localQuickReplies();
	const auto showHowItWorks = [=] {
		ShowQuickRepliesGuide(controller);
	};
	content->add(
		object_ptr<QuickReplyGuideButton>(content, showHowItWorks),
		st::settingsQuickReplyGuideButtonPadding);
	Ui::AddSkip(content);
	const auto add = AddButtonWithIcon(
		content,
		tr::lng_replies_add(),
		st::settingsButtonActive,
		{ &st::settingsIconAdd, IconType::Round, &st::windowBgActive });
	add->setClickedCallback([=] {
		auto reply = Data::LocalQuickReply();
		reply.position = replies->list().size();
		controller->show(Box(EditLocalQuickReply, controller, std::move(reply),
			crl::guard(this, [=](QString id) {
				showOther(ShortcutMessagesId(id));
			})));
	});
	const auto searchField = content->add(
		object_ptr<QuickReplyField>(
			content,
			st::defaultInputField,
			tr::lng_local_replies_search(),
			QString(),
			tr::lng_local_replies_search_help_button(tr::now),
			tr::lng_local_replies_search_help_text(tr::now),
			[=] {
				ShowQuickRepliesHelp(
					controller,
					tr::lng_local_replies_search_help(),
					tr::lng_local_replies_search_help_text());
			}),
		st::peerAppearanceDividerTextMargin);
	const auto search = searchField->field();
	Ui::AddSkip(content);
	const auto inner = content->add(object_ptr<Ui::VerticalLayout>(content));
	Ui::AddSkip(content);
	const auto refresh = [=] {
		while (inner->count()) delete inner->widgetAt(0);
		const auto query = search->getLastText().trimmed();
		auto shown = 0;
		for (const auto &reply : replies->list()) {
			if (!query.isEmpty()
				&& !reply.title.contains(query, Qt::CaseInsensitive)
				&& !reply.shortcut.contains(query, Qt::CaseInsensitive)
				&& !reply.category.contains(query, Qt::CaseInsensitive)
				&& !reply.message.text.contains(query, Qt::CaseInsensitive)
				&& !ranges::any_of(reply.messages, [&](const auto &message) {
					return message.message.text.contains(query, Qt::CaseInsensitive);
				})) continue;
			++shown;
			const auto label = reply.category.isEmpty()
				? reply.title
				: reply.category + u" · "_q + reply.title;
			AddButtonWithLabel(inner, rpl::single(u'/' + reply.shortcut),
				rpl::single(label), st::settingsButtonNoIcon
			)->setClickedCallback([=] {
				showOther(ShortcutMessagesId(reply.id));
			});
		}
		if (!shown) {
			auto text = [&]() -> rpl::producer<QString> {
				if (!replies->loaded()) return tr::lng_contacts_loading();
				if (!replies->error().isEmpty() && replies->list().empty()) {
					return rpl::single(replies->error());
				}
				if (query.isEmpty()) {
					return tr::lng_local_replies_empty();
				}
				return tr::lng_local_replies_no_results();
			}();
			inner->add(object_ptr<Ui::FlatLabel>(
				inner, std::move(text), st::settingsQuickRepliesNoResults),
				st::settingsQuickRepliesNoResultsPadding,
				style::al_justify);
		}
		if (!replies->recoveryList().empty()) {
			Ui::AddSkip(inner);
			Ui::AddDivider(inner);
			Ui::AddSkip(inner);
			Ui::AddSubsectionTitle(inner, tr::lng_local_replies_recovery_title());
		}
		for (const auto &reply : replies->recoveryList()) {
			AddButtonWithLabel(inner, tr::lng_local_replies_recovery(),
				rpl::single(reply.title), st::settingsButtonNoIcon
			)->setClickedCallback([=] {
				PreviewLocalQuickReplyRecovery(controller, reply,
					crl::guard(this, [=] {
						controller->show(Box(EditShortcutNameBox, reply.shortcut,
							[=](QString name, Fn<void()> close) {
								auto restored = reply;
								restored.shortcut = name;
								if (restored.title.isEmpty()) restored.title = name;
								replies->recover(restored, crl::guard(this, [=](bool ok) {
									if (ok) {
										close();
										showOther(ShortcutMessagesId(restored.id));
									} else {
										controller->showToast(replies->error());
									}
								}));
							}));
					}));
			});
			const auto remove = inner->add(object_ptr<Ui::SettingsButton>(inner,
				tr::lng_local_replies_remove_recovery(), st::settingsButtonNoIcon));
			remove->setClickedCallback([=] {
				controller->show(Ui::MakeConfirmBox({
					.text = { tr::lng_replies_delete_sure() },
					.confirmed = crl::guard(this, [=](Fn<void()> close) {
						replies->remove(reply.id, crl::guard(this, [=](bool ok) {
							if (ok) close();
							else controller->showToast(replies->error());
						}));
					}),
					.confirmText = tr::lng_box_delete(),
					.confirmStyle = &st::attentionBoxButton,
				}));
			});
		}
		if (const auto width = content->width()) content->resizeToWidth(width);
	};
	rpl::single(rpl::empty) | rpl::then(replies->changes())
		| rpl::on_next(refresh, content->lifetime());
	search->changes() | rpl::on_next(refresh, content->lifetime());
	Ui::ResizeFitChild(this, content);
}

void EditLocalQuickReply(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		Data::LocalQuickReply reply,
		Fn<void(QString)> saved) {
	box->setTitle(reply.id.isEmpty()
		? tr::lng_replies_add_title() : tr::lng_local_replies_edit());
	box->setWidth(st::boxWideWidth);
	const auto replies = &controller->session().data().localQuickReplies();
	const auto state = box->lifetime().make_state<Data::LocalQuickReply>(reply);
	const auto title = box->addRow(object_ptr<QuickReplyField>(
		box,
		st::defaultInputField,
		tr::lng_local_replies_name(),
		reply.title,
		tr::lng_local_replies_title_help_button(tr::now),
		tr::lng_local_replies_title_help_text(tr::now),
		[=] {
			ShowQuickRepliesHelp(
				box,
				tr::lng_local_replies_title_help(),
				tr::lng_local_replies_title_help_text());
		}));
	title->field()->setMaxLength(128);
	const auto shortcut = box->addRow(object_ptr<QuickReplyField>(
		box,
		st::settingsAddReplyField,
		tr::lng_replies_add_placeholder(),
		reply.shortcut,
		tr::lng_local_replies_shortcut_help_button(tr::now),
		tr::lng_local_replies_shortcut_help_text(tr::now),
		[=] {
			ShowQuickRepliesHelp(
				box,
				tr::lng_local_replies_shortcut_help(),
				tr::lng_local_replies_shortcut_help_text());
		}));
	shortcut->field()->setMaxLength(32);
	const auto category = box->addRow(object_ptr<QuickReplyField>(
		box,
		st::defaultInputField,
		tr::lng_local_replies_category(),
		reply.category,
		tr::lng_local_replies_category_help_button(tr::now),
		tr::lng_local_replies_category_help_text(tr::now),
		[=] {
			ShowQuickRepliesHelp(
				box,
				tr::lng_local_replies_category_help(),
				tr::lng_local_replies_category_help_text());
		}));
	category->field()->setMaxLength(128);
	const auto focused = box->lifetime().make_state<Ui::InputField*>(
		title->field());
	const auto rememberFocus = [=](not_null<Ui::InputField*> field) {
		field->focusedChanges()
			| rpl::filter([](bool isFocused) { return isFocused; })
			| rpl::on_next([=] { *focused = field.get(); }, box->lifetime());
	};
	rememberFocus(title->field());
	rememberFocus(shortcut->field());
	rememberFocus(category->field());
	const auto collect = [=] {
		auto result = *state;
		if (result.id.isEmpty()) {
			result.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
		}
		result.title = title->field()->getLastText().trimmed();
		result.shortcut = shortcut->field()->getLastText().trimmed();
		result.category = category->field()->getLastText().trimmed();
		return result;
	};
	box->addRow(
		object_ptr<QuickReplyGuideButton>(box, [=] {
			ShowQuickRepliesGuide(box);
		}),
		st::settingsQuickReplyGuideButtonPadding);
	if (!reply.id.isEmpty()) {
		for (const auto direction : { -1, 1 }) {
			const auto move = box->addRow(object_ptr<Ui::SettingsButton>(box,
				direction < 0 ? tr::lng_local_replies_up() : tr::lng_local_replies_down(),
				st::settingsButtonNoIcon));
			move->setClickedCallback([=] {
				replies->move(reply.id, direction);
				box->closeBox();
			});
		}

	}
	box->addButton(tr::lng_settings_save(), [=] {
		if (replies->busy()) return;
		const auto value = collect();
		if (value.title.isEmpty()) { title->field()->showError(); return; }
		if (!Storage::LocalData::QuickReplies::ValidShortcut(value.shortcut)) {
			shortcut->field()->showError(); return;
		}
		for (const auto &existing : replies->list()) {
			if (existing.id != value.id
				&& existing.shortcut.compare(value.shortcut, Qt::CaseInsensitive) == 0) {
				shortcut->field()->showError();
				controller->showToast(tr::lng_replies_add_exists(tr::now));
				return;
			}
		}
		replies->save(value, {}, crl::guard(box, [=](bool ok) {
			if (ok) {
				box->closeBox();
				if (saved) saved(value.id);
			} else {
				controller->showToast(replies->error());
			}
		}));
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->setFocusCallback([=] { (*focused)->setFocusFast(); });
}

[[nodiscard]] bool ValidShortcutName(const QString &name) {
	if (name.isEmpty() || name.size() > kShortcutLimit) {
		return false;
	}
	for (const auto &ch : name) {
		if (!ch.isLetterOrNumber()
			&& (ch != QChar('_'))
			&& (ch.unicode() != 0x200c)
			&& (ch.unicode() != 0x00b7)
			&& (ch.unicode() < 0x0d80 || ch.unicode() > 0x0dff)) {
			return false;
		}
	}
	return true;
}

} // namespace

namespace {

class LocalQuickReplyPreview final
: public Ui::RpWidget
, public HistoryView::SimpleElementDelegate {
public:
	LocalQuickReplyPreview(
		QWidget *parent,
		not_null<Window::SessionController*> controller)
	: Ui::RpWidget(parent)
	, SimpleElementDelegate(controller, [=] { update(); })
	, _style(std::make_unique<Ui::ChatStyle>(
		controller->session().colorIndicesValue())) {
		_style->apply(controller->defaultChatTheme().get());
		auto &data = controller->session().data();
		data.viewRepaintRequest() | rpl::on_next([=](auto) {
			update();
		}, lifetime());
		data.viewResizeRequest() | rpl::on_next([=](auto view) {
			if (ranges::any_of(_elements, [&](const auto &element) {
				return element.get() == view;
			})) {
				resizeToWidth(width());
			}
		}, lifetime());
	}

	~LocalQuickReplyPreview() {
		reset();
	}

	void setMessages(
			const QString &replyRef,
			std::vector<Data::LocalQuickReplyMessage> messages) {
		reset();
		if (messages.empty()) return;
		auto &view = controller()->session().data().localQuickReplyView();
		_stage = view.stage(replyRef,
			QUuid::createUuid().toString(QUuid::WithoutBraces),
			std::move(messages));
		if (!_stage) {
			_status = tr::lng_local_replies_edit_draft_missing(tr::now);
			return;
		}
		view.updates(_stage->reference()) | rpl::on_next([=] {
			rebuild();
		}, _stageLifetime);
		rebuild();
	}

	[[nodiscard]] rpl::producer<QString> status() const {
		return _status.value();
	}

	[[nodiscard]] bool ready() const {
		return _stage && _stage->valid() && _status.current().isEmpty();
	}

	HistoryView::Context elementContext() override {
		return HistoryView::Context::ContactPreview;
	}

protected:
	int resizeGetHeight(int width) override {
		auto result = st::msgMargin.top();
		for (const auto &element : _elements) {
			result += element->resizeGetHeight(width);
		}
		return result + st::msgMargin.bottom();
	}

	void paintEvent(QPaintEvent *event) override {
		auto p = Painter(this);
		p.fillRect(event->rect(), st::boxBg);
		auto context = controller()->defaultChatTheme()->preparePaintContext(
			_style.get(), rect(), rect(), event->rect(), elementAnimationsPaused());
		p.translate(0, st::msgMargin.top());
		context.clip.translate(0, -st::msgMargin.top());
		for (const auto &element : _elements) {
			context.outbg = element->hasOutLayout();
			element->draw(p, context);
			p.translate(0, element->height());
			context.clip.translate(0, -element->height());
		}
	}

private:
	void reset() {
		_stageLifetime.destroy();
		_elements.clear();
		if (_stage && _stage->valid()) {
			controller()->session().data().localQuickReplyView().clearStage(
				_stage->reference());
		}
		_stage = nullptr;
	}

	void rebuild() {
		_elements.clear();
		if (!_stage || !_stage->valid()) {
			_status = tr::lng_local_replies_edit_draft_missing(tr::now);
			resizeToWidth(width());
			update();
			return;
		}
		auto &data = controller()->session().data();
		auto &view = data.localQuickReplyView();
		const auto slice = view.slice(_stage->reference());
		for (const auto id : slice.ids) {
			if (const auto item = data.message(id)) {
				auto element = item->createView(this);
				element->initDimensions();
				_elements.push_back(std::move(element));
			}
		}
		const auto error = view.error(_stage->reference());
		_status = !error.isEmpty() ? error
			: slice.ids.size() != _stage->messages().size()
			? tr::lng_contacts_loading(tr::now) : QString();
		resizeToWidth(width());
		update();
	}

	std::unique_ptr<Ui::ChatStyle> _style;
	std::shared_ptr<const Data::LocalQuickReplyStage> _stage;
	std::vector<std::unique_ptr<HistoryView::Element>> _elements;
	rpl::variable<QString> _status;
	rpl::lifetime _stageLifetime;

};

void PreviewLocalQuickReplyRecovery(
		not_null<Window::SessionController*> controller,
		Data::LocalQuickReply reply,
		Fn<void()> recover) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_local_replies_recovery());
		box->setWidth(st::boxWideWidth);
		const auto preview = box->addRow(
			object_ptr<LocalQuickReplyPreview>(box, controller));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box, preview->status(), st::settingsAddReplyLabel));
		preview->setMessages(reply.id, reply.messages);
		box->addButton(tr::lng_local_replies_restore(), [=] {
			box->closeBox();
			recover();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void PreviewLocalQuickReplyInternal(
		std::shared_ptr<ChatHelpers::Show> show,
		Data::LocalQuickReply reply,
		Fn<void(std::vector<Data::LocalQuickReplyMessage>)> insert) {
	const auto controller = show->resolveWindow();
	if (!controller) return;
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(reply.title));
		box->setWidth(st::boxWideWidth);
		struct State {
			QMap<QString, Ui::InputField*> values;
			Ui::InputField *focused = nullptr;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto preview = box->addRow(
			object_ptr<LocalQuickReplyPreview>(box, controller));
		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box, preview->status(), st::settingsAddReplyLabel));
		QString variablesError;
		const auto variableNames = Data::QuickReplyVariables(reply, &variablesError);
		if (!variableNames) {
			status->setText(variablesError);
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
			return;
		}
		const auto expand = [=](QString *error = nullptr) {
			auto variables = QMap<QString, QString>();
			for (auto i = state->values.begin(); i != state->values.end(); ++i) {
				variables.insert(i.key(), i.value()->getLastText());
			}
			return Data::ExpandQuickReplyMessages(reply, variables, error);
		};
		const auto refresh = [=] {
			QString error;
			const auto expanded = expand(&error);
			if (!expanded) {
				preview->setMessages(reply.id, {});
				status->setText(error);
				return;
			}
			preview->setMessages(reply.id, *expanded);
		};
		for (const auto &name : *variableNames) {
			auto initial = QString();
			for (const auto &entry : reply.variables) {
				const auto variable = entry.toObject();
				if (variable.value(u"name"_q).toString() == name) {
					initial = variable.value(u"default"_q).toString();
					break;
				}
			}
			const auto row = box->addRow(object_ptr<QuickReplyField>(
				box,
				st::defaultInputField,
				rpl::single(name),
				initial,
				tr::lng_local_replies_variable_help_button(
					tr::now,
					lt_text,
					name),
				tr::lng_local_replies_variable_help_text(tr::now),
				[=] {
					ShowQuickRepliesHelp(
						box,
						tr::lng_local_replies_variable_help(),
						tr::lng_local_replies_variable_help_text());
				}));
			const auto field = row->field();
			field->setMaxLength(Storage::LocalData::QuickReplies::kMaximumText);
			state->values.insert(name, field);
			if (!state->focused) {
				state->focused = field;
			}
			field->focusedChanges()
				| rpl::filter([](bool focused) { return focused; })
				| rpl::on_next([=] {
					state->focused = field;
				}, box->lifetime());
			field->changes() | rpl::on_next(refresh, box->lifetime());
		}
		refresh();
		box->setFocusCallback([=] {
			if (state->focused) {
				state->focused->setFocusFast();
			}
		});
		box->addButton(tr::lng_local_replies_insert(), [=] {
			if (!preview->ready()) return;
			QString error;
			const auto expanded = expand(&error);
			if (!expanded) {
				show->showToast(error);
				return;
			}
			box->closeBox();
			insert(*expanded);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace

void PreviewLocalQuickReplyMessages(
		std::shared_ptr<ChatHelpers::Show> show,
		Data::LocalQuickReply reply,
		Fn<void(std::vector<Data::LocalQuickReplyMessage>)> insert) {
	PreviewLocalQuickReplyInternal(
		std::move(show),
		std::move(reply),
		std::move(insert));
}

void ShowLocalQuickReplyMetadata(
		not_null<Window::SessionController*> controller,
		Data::LocalQuickReply reply) {
	controller->show(Box(EditLocalQuickReply, controller, std::move(reply),
		Fn<void(QString)>()));
}

Type QuickRepliesId() {
	return QuickReplies::Id();
}

void EditShortcutNameBox(
		not_null<Ui::GenericBox*> box,
		QString name,
		Fn<void(QString, Fn<void()>)> submit) {
	name = name.trimmed();
	const auto editing = !name.isEmpty();
	box->setTitle(editing
		? tr::lng_replies_edit_title()
		: tr::lng_replies_add_title());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		(editing
			? tr::lng_replies_edit_about()
			: tr::lng_replies_add_shortcut()),
		st::settingsAddReplyLabel));
	const auto row = box->addRow(object_ptr<QuickReplyField>(
		box,
		st::settingsAddReplyField,
		tr::lng_replies_add_placeholder(),
		name,
		tr::lng_local_replies_shortcut_help_button(tr::now),
		tr::lng_local_replies_shortcut_help_text(tr::now),
		[=] {
			ShowQuickRepliesHelp(
				box,
				tr::lng_local_replies_shortcut_help(),
				tr::lng_local_replies_shortcut_help_text());
		}));
	const auto field = row->field();
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	field->selectAll();
	field->setMaxLength(kShortcutLimit * 2);

	Ui::AddLengthLimitLabel(field, kShortcutLimit);

	const auto callback = [=] {
		const auto name = field->getLastText().trimmed();
		if (!ValidShortcutName(name)) {
			field->showError();
		} else {
			submit(name, [weak = base::make_weak(box)] {
				if (const auto strong = weak.get()) {
					strong->closeBox();
				}
			});
		}
	};
	field->submits(
	) | rpl::on_next(callback, field->lifetime());
	box->addButton(tr::lng_settings_save(), callback);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

} // namespace Settings
