/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/business/settings_shortcut_messages.h"

#include "api/api_editing.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "boxes/delete_messages_box.h"
#include "boxes/premium_limits_box.h"
#include "boxes/premium_preview_box.h"
#include "boxes/send_files_box.h"
#include "chat_helpers/tabbed_selector.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/business/data_shortcut_messages.h"
#include "data/components/recent_inline_bots.h"
#include "data/data_chat_participant_status.h"
#include "data/data_message_reaction_id.h"
#include "data/data_premium_limits.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/local_quick_replies.h"
#include "data/local_quick_reply_media.h"
#include "data/local_quick_reply_view.h"
#include "history/view/controls/compose_controls_common.h"
#include "history/view/controls/history_view_compose_controls.h"
#include "history/view/history_view_corner_buttons.h"
#include "history/view/history_view_empty_list_bubble.h"
#include "history/view/history_view_list_widget.h"
#include "history/view/history_view_service_message.h"
#include "history/view/history_view_sticker_toast.h"
#include "history/history.h"
#include "history/history_item.h"
#include "info/info_wrap_widget.h"
#include "inline_bots/inline_bot_result.h"
#include "lang/lang_keys.h"
#include "lang/lang_numbers_animation.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "menu/menu_send.h"
#include "settings/business/settings_quick_replies.h"
#include "settings/business/settings_recipients_helper.h"
#include "storage/localimageloader.h"
#include "storage/storage_account.h"
#include "storage/storage_media_prepare.h"
#include "storage/storage_shared_media.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "ui/chat/attach/attach_send_files_way.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/controls/jump_down_button.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/scroll_area.h"
#include "ui/painter.h"
#include "ui/screen_reader_mode.h"
#include "window/themes/window_theme.h"
#include "window/section_widget.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_chat.h"
#include "styles/style_menu_icons.h"
#include "styles/style_layers.h"

#include <QtCore/QMimeData>

#include <variant>

namespace Settings {
namespace {

using namespace HistoryView;

class ShortcutMessages
	: public AbstractSection
	, private WindowListDelegate
	, private CornerButtonsDelegate {
public:
	ShortcutMessages(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<Ui::ScrollArea*> scroll,
		rpl::producer<Container> containerValue,
		std::variant<BusinessShortcutId, QString> context);
	~ShortcutMessages();

	[[nodiscard]] static Type Id(BusinessShortcutId shortcutId);
	[[nodiscard]] static Type Id(const QString &replyId);

	[[nodiscard]] Type id() const final override {
		return _localQuickReplyRef.isEmpty()
			? Id(_shortcutId.current())
			: Id(_localQuickReplyRef);
	}

	[[nodiscard]] rpl::producer<QString> title() override;
	[[nodiscard]] rpl::producer<> sectionShowBack() override;
	bool processChosenSticker(ChatHelpers::FileChosen &&chosen) override;
	void setInnerFocus() override;

	rpl::producer<Info::SelectedItems> selectedListValue() override;
	void selectionAction(Info::SelectionAction action) override;
	void fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) override;

	bool paintOuter(
		not_null<QWidget*> outer,
		int maxVisibleHeight,
		QRect clip) override;

private:
	void outerResized();
	void updateComposeControlsPosition();

	// ListDelegate interface.
	Context listContext() override;
	bool listScrollTo(int top, bool syntetic = true) override;
	void listCancelRequest() override;
	void listDeleteRequest() override;
	void listTryProcessKeyInput(not_null<QKeyEvent*> e) override;
	rpl::producer<Data::MessagesSlice> listSource(
		Data::MessagePosition aroundId,
		int limitBefore,
		int limitAfter) override;
	bool listAllowsMultiSelect() override;
	bool listIsItemGoodForSelection(not_null<HistoryItem*> item) override;
	bool listIsLessInOrder(
		not_null<HistoryItem*> first,
		not_null<HistoryItem*> second) override;
	void listSelectionChanged(SelectedItems &&items) override;
	void listMarkReadTill(not_null<HistoryItem*> item) override;
	void listMarkContentsRead(
		const base::flat_set<not_null<HistoryItem*>> &items) override;
	MessagesBarData listMessagesBar(
		const std::vector<not_null<Element*>> &elements,
		bool markLastAsRead) override;
	void listContentRefreshed() override;
	void listUpdateDateLink(
		ClickHandlerPtr &link,
		not_null<Element*> view) override;
	bool listElementHideReply(not_null<const Element*> view) override;
	bool listElementShownUnread(not_null<const Element*> view) override;
	bool listIsGoodForAroundPosition(
		not_null<const Element *> view) override;
	void listSendBotCommand(
		const QString &command,
		const FullMsgId &context) override;
	void listSearch(
		const QString &query,
		const FullMsgId &context) override;
	void listHandleViaClick(not_null<UserData*> bot) override;
	not_null<Ui::ChatTheme*> listChatTheme() override;
	CopyRestrictionType listCopyRestrictionType(HistoryItem *item) override;
	CopyRestrictionType listCopyMediaRestrictionType(
		not_null<HistoryItem*> item) override;
	CopyRestrictionType listSelectRestrictionType() override;
	auto listAllowedReactionsValue()
		-> rpl::producer<Data::AllowedReactions> override;
	void listShowPremiumToast(not_null<DocumentData*> document) override;
	void listOpenPhoto(
		not_null<PhotoData*> photo,
		FullMsgId context) override;
	void listOpenDocument(
		not_null<DocumentData*> document,
		FullMsgId context,
		bool showInMediaView) override;
	void listPaintEmpty(
		Painter &p,
		const Ui::ChatPaintContext &context) override;
	QString listElementAuthorRank(not_null<const Element*> view) override;
	bool listElementHideTopicButton(not_null<const Element*> view) override;
	History *listTranslateHistory() override;
	void listAddTranslatedItems(
		not_null<TranslateTracker*> tracker) override;
	bool listIgnorePaintEvent(QWidget *w, QPaintEvent *e) override;

	// CornerButtonsDelegate delegate.
	void cornerButtonsShowAtPosition(
		Data::MessagePosition position) override;
	Data::Thread *cornerButtonsThread() override;
	FullMsgId cornerButtonsCurrentId() override;
	bool cornerButtonsIgnoreVisibility() override;
	std::optional<bool> cornerButtonsDownShown() override;
	bool cornerButtonsUnreadMayBeShown() override;
	bool cornerButtonsHas(CornerButtonType type) override;

	base::weak_qptr<Ui::RpWidget> createPinnedToBottom(
		not_null<Ui::RpWidget*> parent) override;
	void setupComposeControls();
	void processScroll();
	void updateInnerVisibleArea();

	void checkReplyReturns();
	void confirmDeleteSelected();
	void clearSelected();

	void uploadFile(const QByteArray &fileContent, SendMediaType type);
	bool confirmSendingFiles(
		QImage &&image,
		QByteArray &&content,
		std::optional<bool> overrideSendImagesAsPhotos = std::nullopt,
		const QString &insertTextOnCancel = QString());
	bool confirmSendingFiles(
		Ui::PreparedList &&list,
		const QString &insertTextOnCancel = QString());
	bool confirmSendingFiles(
		not_null<const QMimeData*> data,
		std::optional<bool> overrideSendImagesAsPhotos,
		const QString &insertTextOnCancel = QString());
	bool showSendingFilesError(const Ui::PreparedList &list) const;
	bool showSendingFilesError(const Ui::PreparedBundle &bundle) const;

	void sendingFilesConfirmed(
		std::shared_ptr<Ui::PreparedBundle> bundle,
		Api::SendOptions options);

	bool sendExistingDocument(
		not_null<DocumentData*> document,
		Api::SendOptions options,
		std::optional<MsgId> localId);
	void sendExistingPhoto(not_null<PhotoData*> photo);
	bool sendExistingPhoto(
		not_null<PhotoData*> photo,
		Api::SendOptions options);
	void sendInlineResult(
		std::shared_ptr<InlineBots::Result> result,
		not_null<UserData*> bot);
	void sendInlineResult(
		std::shared_ptr<InlineBots::Result> result,
		not_null<UserData*> bot,
		Api::SendOptions options,
		std::optional<MsgId> localMessageId);

	[[nodiscard]] Api::SendAction prepareSendAction(
		Api::SendOptions options) const;
	void send();
	void send(Api::SendOptions options);
	void sendVoice(Controls::VoiceToSend &&data);
	void edit(
		not_null<HistoryItem*> item,
		Api::SendOptions options,
		mtpRequestId *const saveEditMsgRequestId,
		bool spoilered,
		Api::VideoCoverEdit videoCover);
	void chooseAttach(std::optional<bool> overrideSendImagesAsPhotos);
	[[nodiscard]] FullReplyTo replyTo() const;
	void doSetInnerFocus();
	void showAtPosition(
		Data::MessagePosition position,
		FullMsgId originItemId = {});
	void showAtPosition(
		Data::MessagePosition position,
		FullMsgId originItemId,
		const Window::SectionShow &params);
	void showAtEnd();
	void finishSending();
	void refreshEmptyText();
	bool showPremiumRequired() const;
	[[nodiscard]] bool isLocal() const;
	[[nodiscard]] int nextLocalPosition() const;
	void prepareLocalMedia(
		Data::LocalQuickReplyMediaRequest request,
		Fn<void()> saved = {},
		bool completionCaptured = false);
	void saveLocalMessages(
		std::vector<Data::LocalQuickReplyMessage> messages,
		std::shared_ptr<const Data::LocalQuickReplyMediaAssets> assets = {},
		Fn<void()> saved = {},
		uint64 operation = 0);
	[[nodiscard]] Data::LocalQuickReplyMediaTarget localMediaTarget() const;
	void saveLocalText();
	void saveLocalEdit(
		not_null<HistoryItem*> item,
		const TextWithTags &text,
		std::optional<bool> spoilered = std::nullopt,
		std::optional<bool> invertCaption = std::nullopt);
	[[nodiscard]] Data::LocalQuickReplyMessage localMessage(
		const QString &mediaKind,
		QString path,
		TextWithEntities text = {});
	void showLocalError(const QString &text);
	void showLocalRetry(const QString &error, Fn<void()> retry);
	[[nodiscard]] Fn<void()> localCompletion(Fn<void()> saved);

	const not_null<Window::SessionController*> _controller;
	const not_null<Main::Session*> _session;
	const not_null<Ui::ScrollArea*> _scroll;
	const not_null<History*> _history;
	rpl::variable<BusinessShortcutId> _shortcutId;
	const QString _localQuickReplyRef;
	rpl::variable<FullMsgId> _editingId;
	rpl::variable<QString> _shortcut;
	rpl::variable<Container> _container;
	rpl::variable<int> _count;
	std::shared_ptr<Ui::ChatStyle> _style;
	std::shared_ptr<Ui::ChatTheme> _theme;
	QPointer<ListWidget> _inner;
	std::unique_ptr<Ui::RpWidget> _controlsWrap;
	std::unique_ptr<ComposeControls> _composeControls;
	rpl::event_stream<> _showBackRequests;
	bool _skipScrollEvent = false;

	QSize _inOuterResize;
	QSize _pendingOuterResize;

	const style::icon *_emptyIcon = nullptr;
	Ui::Text::String _emptyText;
	int _emptyTextWidth = 0;
	int _emptyTextHeight = 0;

	rpl::variable<Info::SelectedItems> _selectedItems
		= Info::SelectedItems(Storage::SharedMediaType::kCount);

	std::unique_ptr<StickerToast> _stickerToast;

	FullMsgId _lastShownAt;
	CornerButtons _cornerButtons;

	Data::MessagesSlice _lastSlice;
	std::unique_ptr<Data::LocalQuickReplyMedia> _localMedia;
	std::unique_ptr<Data::LocalQuickReplyMediaOperation> _localMediaOperation;
	uint64 _localOperation = 0;
	bool _localSaving = false;
	bool _choosingAttach = false;

};

using ShortcutContext = std::variant<BusinessShortcutId, QString>;

struct Factory final : AbstractSectionFactory {
	explicit Factory(ShortcutContext context)
	: context(std::move(context)) {
	}

	object_ptr<AbstractSection> create(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<Ui::ScrollArea*> scroll,
		rpl::producer<Container> containerValue
	) const final override {
		return object_ptr<ShortcutMessages>(
			parent,
			controller,
			scroll,
			std::move(containerValue),
			context);
	}

	const ShortcutContext context;
};

[[nodiscard]] bool IsAway(const QString &shortcut) {
	return (shortcut == u"away"_q);
}

[[nodiscard]] bool IsGreeting(const QString &shortcut) {
	return (shortcut == u"hello"_q);
}

ShortcutMessages::ShortcutMessages(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<Ui::ScrollArea*> scroll,
	rpl::producer<Container> containerValue,
	ShortcutContext context)
: AbstractSection(parent, controller)
, WindowListDelegate(controller)
, _controller(controller)
, _session(&controller->session())
, _scroll(scroll)
, _history(_session->data().history(_session->user()->id))
, _shortcutId(std::holds_alternative<BusinessShortcutId>(context)
	? std::get<BusinessShortcutId>(context)
	: BusinessShortcutId())
, _localQuickReplyRef(std::holds_alternative<QString>(context)
	? std::get<QString>(context)
	: QString())
, _shortcut(_localQuickReplyRef.isEmpty()
	? _session->data().shortcutMessages().lookupShortcut(
		_shortcutId.current()).name
	: _session->data().localQuickReplies().find(_localQuickReplyRef)
		.value_or(Data::LocalQuickReply()).shortcut)
, _container(std::move(containerValue))
, _cornerButtons(
		_scroll,
		controller->chatStyle(),
		static_cast<HistoryView::CornerButtonsDelegate*>(this)) {
	const auto messages = &_session->data().shortcutMessages();

	if (isLocal()) {
		_localMedia = std::make_unique<Data::LocalQuickReplyMedia>(_session);
		_session->data().localQuickReplies().changes() | rpl::on_next([=] {
			if (const auto reply = _session->data().localQuickReplies().find(
				_localQuickReplyRef)) {
				_shortcut = reply->shortcut;
			} else {
				_showBackRequests.fire({});
			}
		}, lifetime());
	} else {
		messages->shortcutIdChanged(
		) | rpl::on_next([=](Data::ShortcutIdChange change) {
			if (change.oldId == _shortcutId.current()) {
				if (change.newId) {
					_shortcutId = change.newId;
				} else {
					_showBackRequests.fire({});
				}
			}
		}, lifetime());
		messages->shortcutsChanged(
		) | rpl::on_next([=] {
			_shortcut = messages->lookupShortcut(_shortcutId.current()).name;
		}, lifetime());
	}

	controller->chatStyle()->paletteChanged(
	) | rpl::on_next([=] {
		_scroll->updateBars();
	}, _scroll->lifetime());

	_style = std::make_shared<Ui::ChatStyle>(_session->colorIndicesValue());
	_theme = std::shared_ptr<Ui::ChatTheme>(
		Window::Theme::DefaultChatThemeOn(lifetime()));

	_inner = Ui::CreateChild<ListWidget>(
		this,
		&controller->session(),
		static_cast<ListDelegate*>(this));
	_inner->overrideChatMode(ElementChatMode::Default);

	_scroll->sizeValue() | rpl::filter([](QSize size) {
		return !size.isEmpty();
	}) | rpl::on_next([=] {
		outerResized();
	}, lifetime());

	_scroll->scrolls(
	) | rpl::on_next([=] {
		processScroll();
	}, lifetime());

	_shortcut.value() | rpl::on_next([=] {
		refreshEmptyText();
		_inner->update();
	}, lifetime());

	_inner->editMessageRequested(
	) | rpl::on_next([=](auto fullId) {
		if (const auto item = _session->data().message(fullId)) {
			const auto media = item->media();
			if (item->isLocalQuickReply()
				|| !media
				|| media->webpage()
				|| media->allowsEditCaption()) {
				_composeControls->editMessage(
					fullId,
					_inner->getSelectedTextRange(item));
			} else if (media->todolist()) {
				Window::PeerMenuEditTodoList(_controller, item);
			}
		}
	}, _inner->lifetime());

	_inner->heightValue() | rpl::on_next([=](int height) {
		resize(width(), height);
	}, lifetime());
}

ShortcutMessages::~ShortcutMessages() = default;

void ShortcutMessages::refreshEmptyText() {
	const auto &shortcut = _shortcut.current();
	const auto away = !isLocal() && IsAway(shortcut);
	const auto greeting = !isLocal() && IsGreeting(shortcut);
	auto text = away
		? tr::lng_away_empty_title(
			tr::now,
			tr::bold
		).append("\n\n").append(tr::lng_away_empty_about(tr::now))
		: greeting
		? tr::lng_greeting_empty_title(
			tr::now,
			tr::bold
		).append("\n\n").append(tr::lng_greeting_empty_about(tr::now))
		: tr::lng_replies_empty_title(
			tr::now,
			tr::bold
		).append("\n\n").append(tr::lng_replies_empty_about(
			tr::now,
			lt_shortcut,
			tr::bold('/' + shortcut),
			tr::marked));
	_emptyIcon = away
		? &st::awayEmptyIcon
		: greeting
		? &st::greetingEmptyIcon
		: &st::repliesEmptyIcon;
	const auto padding = st::repliesEmptyPadding;
	const auto minWidth = st::repliesEmptyWidth / 4;
	const auto maxWidth = std::max(
		minWidth + 1,
		st::repliesEmptyWidth - padding.left() - padding.right());
	_emptyText = Ui::Text::String(
		st::messageTextStyle,
		text,
		kMarkupTextOptions,
		minWidth);
	const auto countHeight = [&](int width) {
		return _emptyText.countHeight(width);
	};
	_emptyTextWidth = Ui::FindNiceTooltipWidth(
		minWidth,
		maxWidth,
		countHeight);
	_emptyTextHeight = countHeight(_emptyTextWidth);
}

Type ShortcutMessages::Id(BusinessShortcutId shortcutId) {
	return std::make_shared<Factory>(shortcutId);
}

Type ShortcutMessages::Id(const QString &replyId) {
	return std::make_shared<Factory>(replyId);
}

rpl::producer<QString> ShortcutMessages::title() {
	return _shortcut.value() | rpl::map([=](const QString &shortcut) {
		return isLocal()
			? rpl::single('/' + shortcut)
			: IsAway(shortcut)
			? tr::lng_away_title()
			: IsGreeting(shortcut)
			? tr::lng_greeting_title()
			: rpl::single('/' + shortcut);
	}) | rpl::flatten_latest();
}

void ShortcutMessages::processScroll() {
	if (_skipScrollEvent) {
		return;
	}
	updateInnerVisibleArea();
}

void ShortcutMessages::updateInnerVisibleArea() {
	if (!_inner->animatedScrolling()) {
		checkReplyReturns();
	}
	const auto scrollTop = _scroll->scrollTop();
	_inner->setVisibleTopBottom(scrollTop, scrollTop + _scroll->height());
	_cornerButtons.updateJumpDownVisibility();
	_cornerButtons.updateUnreadThingsVisibility();
}

rpl::producer<> ShortcutMessages::sectionShowBack() {
	return _showBackRequests.events();
}

void ShortcutMessages::setInnerFocus() {
	_composeControls->focus();
}

rpl::producer<Info::SelectedItems> ShortcutMessages::selectedListValue() {
	return _selectedItems.value();
}

void ShortcutMessages::selectionAction(Info::SelectionAction action) {
	switch (action) {
	case Info::SelectionAction::Clear: clearSelected(); return;
	case Info::SelectionAction::Delete: confirmDeleteSelected(); return;
	}
	Unexpected("Action in ShortcutMessages::selectionAction.");
}

void ShortcutMessages::fillTopBarMenu(
		const Ui::Menu::MenuCallback &addAction) {
	if (isLocal()) {
		const auto replies = &_session->data().localQuickReplies();
		addAction(tr::lng_context_edit_shortcut(tr::now), [=] {
			if (const auto reply = replies->find(_localQuickReplyRef)) {
				ShowLocalQuickReplyMetadata(_controller, *reply);
			}
		}, &st::menuIconEdit);
		addAction({
			.text = tr::lng_context_delete_shortcut(tr::now),
			.handler = crl::guard(this, [=] {
				const auto remove = [=] {
					replies->remove(_localQuickReplyRef, crl::guard(this,
						[=](bool ok) {
							if (!ok) _controller->showToast(replies->error());
							else _showBackRequests.fire({});
						}));
				};
				if (_count.current() == 0) {
					remove();
				} else {
					_controller->show(Ui::MakeConfirmBox({
						.text = { tr::lng_replies_delete_sure() },
						.confirmed = [=](Fn<void()> close) {
							remove();
							close();
						},
						.confirmText = tr::lng_box_delete(),
						.confirmStyle = &st::attentionBoxButton,
					}));
				}
			}),
			.icon = &st::menuIconDeleteAttention,
			.isAttention = true,
		});
		return;
	}
	const auto owner = &_controller->session().data();
	const auto messages = &owner->shortcutMessages();

	addAction(tr::lng_context_edit_shortcut(tr::now), [=] {
		if (!_controller->session().premium()) {
			ShowPremiumPreviewToBuy(
				_controller,
				PremiumFeature::QuickReplies);
			return;
		}
		const auto submit = [=](QString name, Fn<void()> close) {
			const auto id = _shortcutId.current();
			const auto error = [=](QString text) {
				if (!text.isEmpty()) {
					_controller->showToast((text == u"SHORTCUT_OCCUPIED"_q)
						? tr::lng_replies_error_occupied(tr::now)
						: text);
				}
			};
			messages->editShortcut(id, name, close, crl::guard(this, error));
		};
		const auto name = _shortcut.current();
		_controller->show(
			Box(EditShortcutNameBox, name, crl::guard(this, submit)));
	}, &st::menuIconEdit);

	const auto justDelete = crl::guard(this, [=] {
		messages->removeShortcut(_shortcutId.current());
	});
	const auto confirmDeleteShortcut = [=] {
		const auto slice = messages->list(_shortcutId.current());
		if (slice.fullCount == 0) {
			justDelete();
		} else {
			const auto confirmed = [=](Fn<void()> close) {
				justDelete();
				close();
			};
			_controller->show(Ui::MakeConfirmBox({
				.text = { tr::lng_replies_delete_sure() },
				.confirmed = confirmed,
				.confirmText = tr::lng_box_delete(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}
	};
	addAction({
		.text = tr::lng_context_delete_shortcut(tr::now),
		.handler = crl::guard(this, confirmDeleteShortcut),
		.icon = &st::menuIconDeleteAttention,
		.isAttention = true,
	});
}

bool ShortcutMessages::paintOuter(
		not_null<QWidget*> outer,
		int maxVisibleHeight,
		QRect clip) {
	Window::SectionWidget::PaintBackground(
		_theme.get(),
		outer,
		std::max(outer->height(), maxVisibleHeight),
		0,
		clip);
	return true;
}

void ShortcutMessages::outerResized() {
	const auto outer = _scroll->size();
	if (!_inOuterResize.isEmpty()) {
		_pendingOuterResize = (_inOuterResize != outer)
			? outer
			: QSize();
		return;
	}
	_inOuterResize = outer;

	do {
		const auto newScrollTop = _scroll->isHidden()
			? std::nullopt
			: _scroll->scrollTop()
			? base::make_optional(_scroll->scrollTop())
			: 0;
		_skipScrollEvent = true;
		const auto minHeight = (_container.current() == Container::Layer)
			? st::boxWidth
			: _inOuterResize.height();
		_inner->resizeToWidth(_inOuterResize.width(), minHeight);
		_skipScrollEvent = false;

		if (!_scroll->isHidden() && newScrollTop) {
			_scroll->scrollToY(*newScrollTop);
		}
		_inOuterResize = base::take(_pendingOuterResize);
	} while (!_inOuterResize.isEmpty());

	if (!_scroll->isHidden()) {
		updateInnerVisibleArea();
	}
	updateComposeControlsPosition();
	_cornerButtons.updatePositions();
}

void ShortcutMessages::updateComposeControlsPosition() {
	const auto bottom = _scroll->parentWidget()->height();
	const auto controlsHeight = _composeControls->heightCurrent();
	_composeControls->move(0, bottom - controlsHeight + st::boxRadius);
	_composeControls->setAutocompleteBoundingRect(_scroll->geometry());
}

void ShortcutMessages::setupComposeControls() {
	if (!isLocal()) {
		_shortcutId.value() | rpl::on_next([=](BusinessShortcutId id) {
			_composeControls->updateShortcutId(id);
		}, lifetime());
	}

	const auto state = Dialogs::EntryState{
		.key = Dialogs::Key{ _history },
		.section = Dialogs::EntryState::Section::ShortcutMessages,
		.currentReplyTo = replyTo(),
		.currentSuggest = SuggestOptions(),
	};
	_composeControls->setCurrentDialogsEntryState(state);

	auto writeRestriction = rpl::producer<Controls::WriteRestriction>(
		rpl::single(Controls::WriteRestriction()));
	if (!isLocal()) {
		writeRestriction = rpl::combine(
			_count.value(),
			ShortcutMessagesLimitValue(_session)
		) | rpl::map([=](int count, int limit) {
			return (count >= limit)
				? Controls::WriteRestriction{
					.text = tr::lng_business_limit_reached(
						tr::now,
						lt_count,
						limit),
					.type = Controls::WriteRestrictionType::Rights,
				} : Controls::WriteRestriction();
		});
	}
	_composeControls->setHistory({
		.history = _history.get(),
		.sendActionFactory = [=] { return prepareSendAction({}); },
		.writeRestriction = std::move(writeRestriction),
	});

	_composeControls->cancelRequests(
	) | rpl::on_next([=] {
		listCancelRequest();
	}, lifetime());

	_composeControls->sendRequests(
	) | rpl::on_next([=] {
		send();
	}, lifetime());

	_composeControls->sendVoiceRequests(
	) | rpl::on_next([=](ComposeControls::VoiceToSend &&data) {
		sendVoice(std::move(data));
	}, lifetime());

	_composeControls->sendCommandRequests(
	) | rpl::on_next([=](const QString &command) {
		listSendBotCommand(command, FullMsgId());
	}, lifetime());

	_composeControls->editMsgIdValue() | rpl::on_next([=](FullMsgId id) {
		_editingId = id;
	}, lifetime());

	const auto saveEditMsgRequestId = lifetime().make_state<mtpRequestId>(0);
	_composeControls->editRequests(
	) | rpl::on_next([=](auto data) {
		if (const auto item = _session->data().message(data.fullId)) {
			if (item->isLocalQuickReply()) {
				if (data.videoCover) {
					const auto source = _composeControls->localQuickReplyEditSource();
					if (!source || source->replyRef != _localQuickReplyRef) {
						showLocalError(tr::lng_local_replies_edit_draft_missing(tr::now));
						return;
					}
					const auto error = Data::QuickReplyMessageEditError(*source);
					if (!error.isEmpty()) {
						showLocalError(error);
						return;
					}
					const auto cover = data.videoCover.localPrepared
						? Data::LocalQuickReplyCover(data.videoCover.localPrepared)
						: data.videoCover.photo
						? Data::LocalQuickReplyCover(
							Data::LocalQuickReplyPhoto{
								data.videoCover.photo,
								{}})
						: data.videoCover.cleared
						? Data::LocalQuickReplyCover(
							Data::LocalQuickReplyRemoveCover())
						: Data::LocalQuickReplyCover(
							Data::LocalQuickReplyKeepCover());
					const auto media = item->media();
					const auto dimensions = (media && media->document())
						? media->document()->dimensions
						: (media && media->photo())
						? QSize(
							media->photo()->width(),
							media->photo()->height())
						: QSize();
					prepareLocalMedia({
						.target = [&] {
							auto target = localMediaTarget();
							target.previous = *source;
							return target;
						}(),
						.caption = {
							data.textWithTags.text,
							TextUtilities::ConvertTextTagsToEntities(
								data.textWithTags.tags) },
						.options = {
							.spoiler = data.spoilered,
							.invertCaption = data.options.invertCaption,
						},
						.source = Data::LocalQuickReplyMediaPatch{
							.cover = cover,
							.videoDimensions = dimensions,
						},
					}, crl::guard(this, [=] {
						_composeControls->cancelEditMessage();
						finishSending();
					}));
				} else {
					saveLocalEdit(item, data.textWithTags, data.spoilered,
						data.options.invertCaption);
				}
			} else if (item->isBusinessShortcut()) {
				const auto spoiler = data.spoilered;
				edit(
					item,
					data.options,
					saveEditMsgRequestId,
					spoiler,
					data.videoCover);
			}
		}
	}, lifetime());

	_composeControls->attachRequests(
	) | rpl::filter([=] {
		return !_choosingAttach;
	}) | rpl::on_next([=](std::optional<bool> overrideCompress) {
		_choosingAttach = true;
		base::call_delayed(st::historyAttach.ripple.hideDuration, this, [=] {
			_choosingAttach = false;
			chooseAttach(overrideCompress);
		});
	}, lifetime());

	_composeControls->setSendAsFileConfirmed(crl::guard(this, [=](
			std::shared_ptr<Ui::PreparedBundle> bundle,
			Api::SendOptions options) {
		sendingFilesConfirmed(std::move(bundle), options);
	}));

	_composeControls->fileChosen(
	) | rpl::on_next([=](ChatHelpers::FileChosen data) {
		_controller->hideLayer(anim::type::normal);
		sendExistingDocument(data.document, {}, std::nullopt);
	}, lifetime());

	_composeControls->photoChosen(
	) | rpl::on_next([=](ChatHelpers::PhotoChosen chosen) {
		sendExistingPhoto(chosen.photo);
	}, lifetime());

	_composeControls->inlineResultChosen(
	) | rpl::on_next([=](ChatHelpers::InlineChosen chosen) {
		sendInlineResult(chosen.result, chosen.bot);
	}, lifetime());

	_composeControls->jumpToItemRequests(
	) | rpl::on_next([=](FullReplyTo to) {
		if (const auto item = _session->data().message(to.messageId)) {
			showAtPosition(item->position());
		}
	}, lifetime());

	rpl::merge(
		_composeControls->scrollKeyEvents(),
		_inner->scrollKeyEvents()
	) | rpl::on_next([=](not_null<QKeyEvent*> e) {
		_scroll->keyPressEvent(e);
	}, lifetime());

	_composeControls->editLastMessageRequests(
	) | rpl::on_next([=](not_null<QKeyEvent*> e) {
		if (!_inner->lastMessageEditRequestNotify()) {
			_scroll->keyPressEvent(e);
		}
	}, lifetime());

	_composeControls->setPasteToastParent(_scroll.get());
	_composeControls->setMimeDataHook([=](
			not_null<const QMimeData*> data,
			Ui::InputField::MimeAction action) {
		if (action == Ui::InputField::MimeAction::Check) {
			return Core::CanSendFiles(data);
		} else if (action == Ui::InputField::MimeAction::Insert) {
			return confirmSendingFiles(
				data,
				std::nullopt,
				Core::ReadMimeText(data));
		}
		Unexpected("action in MimeData hook.");
	});

	_composeControls->lockShowStarts(
	) | rpl::on_next([=] {
		_cornerButtons.updateJumpDownVisibility();
		_cornerButtons.updateUnreadThingsVisibility();
	}, lifetime());

	_composeControls->viewportEvents(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		_scroll->viewportEvent(e);
	}, lifetime());

	_controlsWrap->widthValue() | rpl::on_next([=](int width) {
		_composeControls->resizeToWidth(width);
	}, _controlsWrap->lifetime());

	_composeControls->height(
	) | rpl::on_next([=](int height) {
		const auto wasMax = (_scroll->scrollTop() >= _scroll->scrollTopMax());
		_controlsWrap->resize(width(), height - st::boxRadius);
		updateComposeControlsPosition();
		if (wasMax) {
			listScrollTo(_scroll->scrollTopMax());
		}
	}, lifetime());
}

base::weak_qptr<Ui::RpWidget> ShortcutMessages::createPinnedToBottom(
		not_null<Ui::RpWidget*> parent) {
	auto placeholder = rpl::deferred([=] {
		return _shortcutId.value();
	}) | rpl::map([=](BusinessShortcutId id) {
		return isLocal()
			? _shortcut.current()
			: _session->data().shortcutMessages().lookupShortcut(id).name;
	}) | rpl::map([=](const QString &shortcut) {
		return isLocal()
			? tr::lng_replies_message_placeholder()
			: (shortcut == u"away"_q)
			? tr::lng_away_message_placeholder()
			: (shortcut == u"hello"_q)
			? tr::lng_greeting_message_placeholder()
			: tr::lng_replies_message_placeholder();
	}) | rpl::flatten_latest();

	_controlsWrap = std::make_unique<Ui::RpWidget>(parent);
	_composeControls = std::make_unique<ComposeControls>(
		dynamic_cast<Ui::RpWidget*>(_scroll->parentWidget()),
		ComposeControlsDescriptor{
			.stOverride = &st::repliesComposeControls,
			.show = _controller->uiShow(),
			.unavailableEmojiPasted = [=](not_null<DocumentData*> emoji) {
				listShowPremiumToast(emoji);
			},
			.mode = HistoryView::ComposeControlsMode::Normal,
			.sendMenuDetails = [] { return SendMenu::Details(); },
			.regularWindow = _controller,
			.stickerOrEmojiChosen = _controller->stickerOrEmojiChosen(),
			.customPlaceholder = std::move(placeholder),
			.panelsLevel = Window::GifPauseReason::Layer,
			.voiceCustomCancelText = tr::lng_record_cancel_stories(tr::now),
			.voiceLockFromBottom = true,
			.localQuickReplyRef = _localQuickReplyRef,
			.features = {
				.sendAs = false,
				.ttlInfo = false,
				.botCommandSend = false,
				.silentBroadcastToggle = false,
				.attachBotsMenu = false,
				.megagroupSet = false,
				.commonTabbedPanel = false,
			},
		});

	setupComposeControls();

	showAtEnd();

	return _controlsWrap.get();
}

Context ShortcutMessages::listContext() {
	return Context::ShortcutMessages;
}

bool ShortcutMessages::listScrollTo(int top, bool syntetic) {
	top = std::clamp(top, 0, _scroll->scrollTopMax());
	if (_scroll->scrollTop() == top) {
		updateInnerVisibleArea();
		return false;
	}
	_scroll->scrollToY(top);
	return true;
}

void ShortcutMessages::listCancelRequest() {
	if (_inner && !_inner->getSelectedItems().empty()) {
		clearSelected();
		return;
	} else if (_composeControls->handleCancelRequest()) {
		return;
	}
	_showBackRequests.fire({});
}

void ShortcutMessages::listDeleteRequest() {
	confirmDeleteSelected();
}

void ShortcutMessages::listTryProcessKeyInput(not_null<QKeyEvent*> e) {
	_composeControls->tryProcessKeyInput(e);
}

rpl::producer<Data::MessagesSlice> ShortcutMessages::listSource(
		Data::MessagePosition aroundId,
		int limitBefore,
		int limitAfter) {
	if (isLocal()) {
		const auto view = &_session->data().localQuickReplyView();
		view->request(_localQuickReplyRef);
		return rpl::single(rpl::empty) | rpl::then(
			view->updates(_localQuickReplyRef)
		) | rpl::map([=] {
			return view->slice(_localQuickReplyRef);
		}) | rpl::after_next([=](const Data::MessagesSlice &slice) {
			_count = slice.fullCount.value_or(0);
			if (const auto error = view->error(_localQuickReplyRef);
				!error.isEmpty()) {
				showLocalError(error);
			}
		});
	}
	const auto messages = &_session->data().shortcutMessages();
	return _shortcutId.value(
	) | rpl::map([=](BusinessShortcutId shortcutId) {
		return rpl::single(rpl::empty) | rpl::then(
			messages->updates(shortcutId)
		) | rpl::map([=] {
			return messages->list(shortcutId);
		});
	}) | rpl::flatten_latest(
	) | rpl::after_next([=](const Data::MessagesSlice &slice) {
		_count = slice.fullCount.value_or(
			messages->count(_shortcutId.current()));
	});
}

bool ShortcutMessages::listAllowsMultiSelect() {
	return true;
}

bool ShortcutMessages::listIsItemGoodForSelection(
		not_null<HistoryItem*> item) {
	return !item->isSending() && !item->hasFailed();
}

bool ShortcutMessages::listIsLessInOrder(
		not_null<HistoryItem*> first,
		not_null<HistoryItem*> second) {
	return isLocal()
		? _session->data().localQuickReplyView().less(first, second)
		: first->position() < second->position();
}

void ShortcutMessages::listSelectionChanged(SelectedItems &&items) {
	auto value = Info::SelectedItems();
	value.title = [](int count) {
		return tr::lng_forum_messages(
			tr::now,
			lt_count,
			count,
			Ui::StringWithNumbers::FromString);
	};
	value.list = items | ranges::views::transform([](SelectedItem item) {
		auto result = Info::SelectedItem(GlobalMsgId{ item.msgId });
		result.canDelete = item.canDelete;
		return result;
	}) | ranges::to_vector;
	_selectedItems = std::move(value);

	if (items.empty()
		&& !(_inner->hasFocus() && Ui::ScreenReaderModeActive())) {
		doSetInnerFocus();
	}
}

void ShortcutMessages::listMarkReadTill(not_null<HistoryItem*> item) {
}

void ShortcutMessages::listMarkContentsRead(
	const base::flat_set<not_null<HistoryItem*>> &items) {
}

MessagesBarData ShortcutMessages::listMessagesBar(
		const std::vector<not_null<Element*>> &elements,
		bool markLastAsRead) {
	return {};
}

void ShortcutMessages::listContentRefreshed() {
}

void ShortcutMessages::listUpdateDateLink(
	ClickHandlerPtr &link,
	not_null<Element*> view) {
}

bool ShortcutMessages::listElementHideReply(not_null<const Element*> view) {
	return false;
}

bool ShortcutMessages::listElementShownUnread(not_null<const Element*> view) {
	return true;
}

bool ShortcutMessages::listIsGoodForAroundPosition(
		not_null<const Element*> view) {
	return true;
}

void ShortcutMessages::listSendBotCommand(
	const QString &command,
	const FullMsgId &context) {
}

void ShortcutMessages::listSearch(
		const QString &query,
		const FullMsgId &context) {
	const auto inChat = _history->peer->isUser()
		? Dialogs::Key()
		: Dialogs::Key(_history);
	_controller->searchMessages(query, inChat);
}

void ShortcutMessages::listHandleViaClick(not_null<UserData*> bot) {
	_composeControls->setText({ '@' + bot->username() + ' ' });
}

not_null<Ui::ChatTheme*> ShortcutMessages::listChatTheme() {
	return _theme.get();
}

CopyRestrictionType ShortcutMessages::listCopyRestrictionType(
		HistoryItem *item) {
	return CopyRestrictionType::None;
}

CopyRestrictionType ShortcutMessages::listCopyMediaRestrictionType(
		not_null<HistoryItem*> item) {
	if (const auto media = item->media()) {
		if (const auto invoice = media->invoice()) {
			if (!invoice->extendedMedia.empty()) {
				return CopyMediaRestrictionTypeFor(_history->peer, item);
			}
		}
	}
	return CopyRestrictionType::None;
}

CopyRestrictionType ShortcutMessages::listSelectRestrictionType() {
	return CopyRestrictionType::None;
}

auto ShortcutMessages::listAllowedReactionsValue()
-> rpl::producer<Data::AllowedReactions> {
	return rpl::single(Data::AllowedReactions());
}

void ShortcutMessages::listShowPremiumToast(
		not_null<DocumentData*> document) {
	if (!_stickerToast) {
		_stickerToast = std::make_unique<HistoryView::StickerToast>(
			_controller,
			this,
			[=] { _stickerToast = nullptr; });
	}
	_stickerToast->showFor(document);
}

void ShortcutMessages::listOpenPhoto(
		not_null<PhotoData*> photo,
		FullMsgId context) {
	_controller->openPhoto(photo, { context });
}

void ShortcutMessages::listOpenDocument(
		not_null<DocumentData*> document,
		FullMsgId context,
		bool showInMediaView) {
	_controller->openDocument(document, showInMediaView, { context });
}

void ShortcutMessages::listPaintEmpty(
		Painter &p,
		const Ui::ChatPaintContext &context) {
	Expects(_emptyIcon != nullptr);

	const auto width = st::repliesEmptyWidth;
	const auto padding = st::repliesEmptyPadding;
	const auto height = padding.top()
		+ _emptyIcon->height()
		+ st::repliesEmptySkip
		+ _emptyTextHeight
		+ padding.bottom();
	const auto r = QRect(
		(this->width() - width) / 2,
		(this->height() - height) / 3,
		width,
		height);
	HistoryView::ServiceMessagePainter::PaintBubble(p, context.st, r);

	_emptyIcon->paint(
		p,
		r.x() + (r.width() - _emptyIcon->width()) / 2,
		r.y() + padding.top(),
		this->width());
	p.setPen(st::msgServiceFg);
	_emptyText.draw(
		p,
		r.x() + (r.width() - _emptyTextWidth) / 2,
		r.y() + padding.top() + _emptyIcon->height() + st::repliesEmptySkip,
		_emptyTextWidth,
		style::al_top);
}

QString ShortcutMessages::listElementAuthorRank(
		not_null<const Element*> view) {
	return {};
}

bool ShortcutMessages::listElementHideTopicButton(
		not_null<const Element*> view) {
	return true;
}

History *ShortcutMessages::listTranslateHistory() {
	return nullptr;
}

void ShortcutMessages::listAddTranslatedItems(
	not_null<TranslateTracker*> tracker) {
}

bool ShortcutMessages::listIgnorePaintEvent(QWidget *w, QPaintEvent *e) {
	return false;
}

void ShortcutMessages::cornerButtonsShowAtPosition(
		Data::MessagePosition position) {
	showAtPosition(position);
}

Data::Thread *ShortcutMessages::cornerButtonsThread() {
	return _history;
}

FullMsgId ShortcutMessages::cornerButtonsCurrentId() {
	return _lastShownAt;
}

bool ShortcutMessages::cornerButtonsIgnoreVisibility() {
	return false;// animatingShow();
}

std::optional<bool> ShortcutMessages::cornerButtonsDownShown() {
	if (_composeControls->isLockPresent()
		|| _composeControls->isTTLButtonShown()) {
		return false;
	}
	const auto top = _scroll->scrollTop() + st::historyToDownShownAfter;
	if (top < _scroll->scrollTopMax() || _cornerButtons.replyReturn()) {
		return true;
	} else if (_inner->loadedAtBottomKnown()) {
		return !_inner->loadedAtBottom();
	}
	return std::nullopt;
}

bool ShortcutMessages::cornerButtonsUnreadMayBeShown() {
	return _inner->loadedAtBottomKnown()
		&& !_composeControls->isLockPresent()
		&& !_composeControls->isTTLButtonShown();
}

bool ShortcutMessages::cornerButtonsHas(CornerButtonType type) {
	return (type == CornerButtonType::Down);
}

void ShortcutMessages::checkReplyReturns() {
	const auto currentTop = _scroll->scrollTop();
	const auto shortcutId = _shortcutId.current();
	while (const auto replyReturn = _cornerButtons.replyReturn()) {
		const auto position = replyReturn->position();
		const auto scrollTop = _inner->scrollTopForPosition(position);
		const auto below = scrollTop
			? (currentTop >= std::min(*scrollTop, _scroll->scrollTopMax()))
			: _inner->isBelowPosition(position);
		if (replyReturn->shortcutId() != shortcutId || below) {
			_cornerButtons.calculateNextReplyReturn();
		} else {
			break;
		}
	}
}

void ShortcutMessages::confirmDeleteSelected() {
	if (isLocal()) {
		const auto selected = _inner->getSelectedItems();
		auto ids = QStringList();
		for (const auto &selectedItem : selected) {
			const auto item = _session->data().message(selectedItem.msgId);
			const auto source = item
				? _session->data().localQuickReplyView().message(item)
				: std::nullopt;
			if (!source || source->replyRef != _localQuickReplyRef) {
				showLocalError(tr::lng_local_replies_edit_draft_missing(tr::now));
				return;
			}
			ids.push_back(source->id);
		}
		if (ids.isEmpty()) return;
		const auto replies = &_session->data().localQuickReplies();
		_controller->show(Ui::MakeConfirmBox({
			.text = { tr::lng_replies_delete_sure() },
			.confirmed = [=](Fn<void()> close) {
				replies->removeMessages(_localQuickReplyRef, ids,
					crl::guard(this, [=](bool ok) {
						if (!ok) showLocalError(replies->error());
						else _inner->cancelSelection();
						close();
					}));
			},
			.confirmText = tr::lng_box_delete(),
			.confirmStyle = &st::attentionBoxButton,
		}));
		return;
	}
	ConfirmDeleteSelectedItems(_inner);
}

void ShortcutMessages::clearSelected() {
	_inner->cancelSelection();
}

bool ShortcutMessages::isLocal() const {
	return !_localQuickReplyRef.isEmpty();
}

int ShortcutMessages::nextLocalPosition() const {
	const auto reply = _session->data().localQuickReplies().find(
		_localQuickReplyRef);
	if (!reply || reply->messages.empty()) return 0;
	const auto last = ranges::max_element(
		reply->messages,
		[](const auto &a, const auto &b) {
			return std::tie(a.position, a.id) < std::tie(b.position, b.id);
		});
	return (last->position < std::numeric_limits<int>::max())
		? last->position + 1
		: last->position;
}

Data::LocalQuickReplyMessage ShortcutMessages::localMessage(
		const QString &mediaKind,
		QString path,
		TextWithEntities text) {
	auto result = Data::LocalQuickReplyMessage{
		.replyRef = _localQuickReplyRef,
		.replyToRef = localMediaTarget().replyToRef,
		.message = std::move(text),
		.media = QJsonObject{ { u"kind"_q, mediaKind } },
		.position = nextLocalPosition(),
	};
	if (!path.isEmpty()) result.sourcePaths = { std::move(path) };
	return result;
}

Data::LocalQuickReplyMediaTarget ShortcutMessages::localMediaTarget() const {
	QString replyToRef;
	const auto reply = replyTo();
	if (reply.messageId) {
		if (const auto item = _session->data().message(reply.messageId)) {
			if (const auto source = _session->data().localQuickReplyView().message(item)) {
				replyToRef = source->id;
			}
		}
	}
	return {
		.replyRef = _localQuickReplyRef,
		.replyToRef = std::move(replyToRef),
		.firstPosition = nextLocalPosition(),
	};
}

void ShortcutMessages::prepareLocalMedia(
		Data::LocalQuickReplyMediaRequest request,
		Fn<void()> saved,
		bool completionCaptured) {
	if (_localSaving) return;
	if (!_localMedia) {
		showLocalError(u"Local media preparation is unavailable."_q);
		return;
	}
	_localSaving = true;
	if (!completionCaptured) saved = localCompletion(std::move(saved));
	const auto pending = std::make_shared<Data::LocalQuickReplyMediaRequest>(
		std::move(request));
	const auto operation = ++_localOperation;
	_localMediaOperation = _localMedia->prepare(
		*pending,
		crl::guard(this, [=, saved = std::move(saved)](
				Data::LocalQuickReplyMediaResult result) mutable {
			if (operation != _localOperation) return;
			_localMediaOperation.reset();
			if (const auto error = std::get_if<Data::LocalQuickReplyMediaError>(
					&result)) {
				showLocalRetry(error->detail, [=] {
					prepareLocalMedia(*pending, saved, true);
				});
				return;
			}
			auto prepared = std::get<
				Data::PreparedLocalQuickReplyMedia>(std::move(result));
			saveLocalMessages(
				std::move(prepared.messages),
				std::move(prepared.assets),
				std::move(saved),
				operation);
		}));
}

void ShortcutMessages::saveLocalMessages(
		std::vector<Data::LocalQuickReplyMessage> messages,
		std::shared_ptr<const Data::LocalQuickReplyMediaAssets> assets,
		Fn<void()> saved,
		uint64 operation) {
	if (messages.empty()) {
		_localSaving = false;
		return;
	}
	if (!operation) {
		if (_localSaving) return;
		operation = ++_localOperation;
		saved = localCompletion(std::move(saved));
	}
	_localSaving = true;
	const auto pending = std::make_shared<Data::PreparedLocalQuickReplyMedia>(
		Data::PreparedLocalQuickReplyMedia{ std::move(messages), std::move(assets) });
	const auto replies = &_session->data().localQuickReplies();
	replies->saveMessages(
		_localQuickReplyRef,
		pending->messages,
		crl::guard(this, [=](bool ok) {
			if (operation != _localOperation) return;
			if (!ok) {
				showLocalRetry(replies->error(), [=] {
					saveLocalMessages(pending->messages, pending->assets, saved, operation);
				});
				return;
			}
			_localSaving = false;
			for (const auto &message : pending->messages) {
				_composeControls->localQuickReplyEditSaved(message);
			}
			if (saved) saved();
		}),
		pending->assets);
}

void ShortcutMessages::saveLocalText() {
	const auto text = _composeControls->getTextWithAppliedMarkdown();
	if (text.text.isEmpty()) return;
	if (_composeControls->isEditingMessage()) {
		const auto item = _session->data().message(_editingId.current());
		if (!item || !item->isLocalQuickReply()) {
			showLocalError(tr::lng_local_replies_edit_draft_missing(tr::now));
			return;
		}
		saveLocalEdit(item, text);
		return;
	}
	saveLocalMessages(
		{ localMessage(
			u"text"_q,
			QString(),
			{ text.text,
				TextUtilities::ConvertTextTagsToEntities(text.tags) }) },
		{},
		crl::guard(this, [=] { _composeControls->clear(); }));
}

void ShortcutMessages::saveLocalEdit(
		not_null<HistoryItem*> item,
		const TextWithTags &text,
		std::optional<bool> spoilered,
		std::optional<bool> invertCaption) {
	if (_localSaving) return;
	const auto source = (item->fullId() == _editingId.current())
		? _composeControls->localQuickReplyEditSource() : nullptr;
	if (!source || source->replyRef != _localQuickReplyRef) {
		showLocalError(tr::lng_local_replies_edit_draft_missing(tr::now));
		return;
	}
	if (const auto error = Data::QuickReplyMessageEditError(*source)
		; !error.isEmpty()) {
		showLocalError(error);
		return;
	}
	_localSaving = true;
	const auto operation = ++_localOperation;
	const auto editingId = item->fullId();
	auto changed = *source;
	changed.message = {
		text.text,
		TextUtilities::ConvertTextTagsToEntities(text.tags) };
	if (spoilered) changed.media.insert(u"spoiler"_q, *spoilered);
	if (invertCaption) changed.media.insert(u"invert_caption"_q, *invertCaption);
	const auto completed = localCompletion(crl::guard(this, [=] {
		_composeControls->cancelEditMessage();
	}));
	_session->data().localQuickReplies().saveMessages(
		_localQuickReplyRef,
		{ std::move(changed) },
		crl::guard(this, [=](bool ok) {
			if (operation == _localOperation) _localSaving = false;
			if (operation != _localOperation
				|| _editingId.current() != editingId) {
				return;
			}
			if (!ok) {
				showLocalError(_session->data().localQuickReplies().error());
				return;
			}
			_composeControls->localQuickReplyEditSaved(*source);
			completed();
		}));
}

Fn<void()> ShortcutMessages::localCompletion(Fn<void()> saved) {
	const auto text = _composeControls->getTextWithAppliedMarkdown();
	const auto editingId = _editingId.current();
	const auto reply = replyTo();
	return crl::guard(this, [=] {
		const auto current = _composeControls->getTextWithAppliedMarkdown();
		if (_editingId.current() != editingId
			|| replyTo() != reply
			|| current.text != text.text
			|| current.tags != text.tags) {
			return;
		}
		if (saved) saved();
		if (_composeControls->replyingToMessage() == reply) {
			_composeControls->cancelReplyMessage();
		}
		finishSending();
	});
}

void ShortcutMessages::showLocalRetry(const QString &error, Fn<void()> retry) {
	_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_local_replies_save_failed());
		box->setCloseByOutsideClick(false);
		box->setCloseByEscape(false);
		box->addRow(object_ptr<Ui::FlatLabel>(box, rpl::single(error), st::boxLabel));
		box->addButton(tr::lng_bot_download_retry(), crl::guard(this, [=] {
			_localSaving = false;
			box->closeBox();
			retry();
		}));
		box->addButton(tr::lng_record_lock_discard(), crl::guard(this, [=] {
			_localSaving = false;
			box->closeBox();
		}));
	}));
}

void ShortcutMessages::showLocalError(const QString &text) {
	if (!text.isEmpty()) _controller->showToast(text);
}

void ShortcutMessages::uploadFile(
		const QByteArray &fileContent,
		SendMediaType type) {
	if (!isLocal()) {
		_session->api().sendFile(fileContent, type, prepareSendAction({}));
		return;
	}
	prepareLocalMedia({
		.target = localMediaTarget(),
		.caption = {
			_composeControls->getTextWithAppliedMarkdown().text,
			TextUtilities::ConvertTextTagsToEntities(
				_composeControls->getTextWithAppliedMarkdown().tags) },
		.source = Data::LocalQuickReplyBytes{ fileContent, type },
	}, crl::guard(this, [=] { _composeControls->clear(); }));
}

bool ShortcutMessages::showSendingFilesError(
		const Ui::PreparedList &list) const {
	if (!isLocal() && showPremiumRequired()) {
		return true;
	}
	const auto show = _controller->uiShow();
	const auto peer = _controller->session().user();
	return Data::ShowSendError(show, peer, list, std::nullopt, true, isLocal());
}

bool ShortcutMessages::showSendingFilesError(
		const Ui::PreparedBundle &bundle) const {
	if (!isLocal() && showPremiumRequired()) {
		return true;
	}
	const auto peer = _controller->session().user();
	return Data::ShowSendError(_controller->uiShow(), peer, bundle, true, isLocal());
}

Api::SendAction ShortcutMessages::prepareSendAction(
		Api::SendOptions options) const {
	auto result = Api::SendAction(_history, options);
	result.replyTo = replyTo();
	if (!isLocal()) {
		result.options.shortcutId = _shortcutId.current();
	}
	result.options.localQuickReplyRef = _localQuickReplyRef;
	result.options.sendAs = _composeControls->sendAsPeer();
	return result;
}

void ShortcutMessages::send() {
	if (isLocal()) {
		saveLocalText();
		return;
	}
	if (_composeControls->getTextWithAppliedMarkdown().text.isEmpty()) {
		return;
	}
	send({});
}

void ShortcutMessages::sendVoice(ComposeControls::VoiceToSend &&data) {
	if (isLocal()) {
		prepareLocalMedia({
			.target = localMediaTarget(),
			.caption = {
				_composeControls->getTextWithAppliedMarkdown().text,
				TextUtilities::ConvertTextTagsToEntities(
					_composeControls->getTextWithAppliedMarkdown().tags) },
			.source = Data::LocalQuickReplyRecording{
				.bytes = std::move(data.bytes),
				.waveform = std::move(data.waveform),
				.duration = data.duration,
				.roundVideo = data.video,
			},
		}, crl::guard(this, [=] {
			_composeControls->clearListenState();
			_composeControls->clear();
		}));
		return;
	}
	if (showPremiumRequired()) {
		return;
	}
	auto action = prepareSendAction(data.options);
	_session->api().sendVoiceMessage(
		data.bytes,
		data.waveform,
		data.duration,
		data.video,
		std::move(action));

	_composeControls->cancelReplyMessage();
	_composeControls->clearListenState();
	finishSending();
}

void ShortcutMessages::send(Api::SendOptions options) {
	if (isLocal()) {
		saveLocalText();
		return;
	}
	if (showPremiumRequired()) {
		return;
	}
	_cornerButtons.clearReplyReturns();

	auto message = Api::MessageToSend(prepareSendAction(options));
	message.textWithTags = _composeControls->getTextWithAppliedMarkdown();
	message.webPage = _composeControls->webPageDraft();

	_session->api().sendMessage(std::move(message));

	_composeControls->clear();

	finishSending();
}

void ShortcutMessages::edit(
		not_null<HistoryItem*> item,
		Api::SendOptions options,
		mtpRequestId *const saveEditMsgRequestId,
		bool spoilered,
		Api::VideoCoverEdit videoCover) {
	if (isLocal()) {
		showLocalError(u"Local quick reply edits must use the native editor."_q);
		return;
	}
	if (*saveEditMsgRequestId) {
		return;
	}
	const auto webpage = _composeControls->webPageDraft();
	auto sending = TextWithEntities();
	auto left = _composeControls->prepareTextForEditMsg();

	const auto originalLeftSize = left.text.size();
	const auto hasMediaWithCaption = item
		&& item->media()
		&& item->media()->allowsEditCaption();
	const auto limits = Data::PremiumLimits(_session);
	const auto maxTextSize = hasMediaWithCaption
		? limits.captionLengthCurrent()
		: limits.messageLengthCurrent();
	if (!TextUtilities::CutPart(sending, left, maxTextSize)
		&& !hasMediaWithCaption) {
		if (item) {
			_controller->show(Box<DeleteMessagesBox>(item));
		} else {
			doSetInnerFocus();
		}
		return;
	} else if (!left.text.isEmpty()) {
		const auto remove = originalLeftSize - maxTextSize;
		_controller->showToast(
			tr::lng_edit_limit_reached(tr::now, lt_count, remove));
		return;
	}

	lifetime().add([=] {
		if (!*saveEditMsgRequestId) {
			return;
		}
		_session->api().request(base::take(*saveEditMsgRequestId)).cancel();
	});

	const auto done = [=](mtpRequestId requestId) {
		if (requestId == *saveEditMsgRequestId) {
			*saveEditMsgRequestId = 0;
			_composeControls->cancelEditMessage();
		}
	};

	const auto fail = [=](const QString &error, mtpRequestId requestId) {
		if (requestId == *saveEditMsgRequestId) {
			*saveEditMsgRequestId = 0;
		}

		if (ranges::contains(Api::kDefaultEditMessagesErrors, error)) {
			_controller->showToast(tr::lng_edit_error(tr::now));
		} else if (error == u"MESSAGE_NOT_MODIFIED"_q) {
			_composeControls->cancelEditMessage();
		} else if (error == u"MESSAGE_EMPTY"_q) {
			doSetInnerFocus();
		} else {
			_controller->showToast(tr::lng_edit_error(tr::now));
		}
		update();
		return true;
	};

	*saveEditMsgRequestId = Api::EditTextMessage(
		item,
		sending,
		webpage,
		options,
		crl::guard(this, done),
		crl::guard(this, fail),
		spoilered,
		videoCover);

	_composeControls->hidePanelsAnimated();
	doSetInnerFocus();
}

bool ShortcutMessages::confirmSendingFiles(
		not_null<const QMimeData*> data,
		std::optional<bool> overrideSendImagesAsPhotos,
		const QString &insertTextOnCancel) {
	const auto hasImage = data->hasImage();
	const auto premium = _controller->session().user()->isPremium();

	if (const auto urls = Core::ReadMimeUrls(data); !urls.empty()) {
		auto list = Storage::PrepareMediaList(
			urls,
			st::sendMediaPreviewSize,
			premium);
		if (list.error != Ui::PreparedList::Error::NonLocalUrl) {
			if (list.error == Ui::PreparedList::Error::None
				|| !hasImage) {
				const auto emptyTextOnCancel = QString();
				list.overrideSendImagesAsPhotos = overrideSendImagesAsPhotos;
				confirmSendingFiles(std::move(list), emptyTextOnCancel);
				return true;
			}
		}
	}

	if (auto read = Core::ReadMimeImage(data)) {
		confirmSendingFiles(
			std::move(read.image),
			std::move(read.content),
			overrideSendImagesAsPhotos,
			insertTextOnCancel);
		return true;
	}
	return false;
}

bool ShortcutMessages::confirmSendingFiles(
		Ui::PreparedList &&list,
		const QString &insertTextOnCancel) {
	if (_composeControls->confirmMediaEdit(list)) {
		return true;
	} else if (showSendingFilesError(list)) {
		return false;
	}

	auto box = Box<SendFilesBox>(
		_controller,
		std::move(list),
		_composeControls->getTextWithAppliedMarkdown(),
		_history->peer,
		Api::SendType::Normal,
		SendMenu::Details());

	box->setConfirmedCallback(crl::guard(this, [=](
			std::shared_ptr<Ui::PreparedBundle> bundle,
			Api::SendOptions options,
			FullReplyTo) {
		sendingFilesConfirmed(std::move(bundle), options);
	}));
	box->setCancelledCallback(_composeControls->restoreTextCallback(
		insertTextOnCancel));
	box->takeTextWithTagsRequests() | rpl::on_next([=](TextWithTags &&text) {
		_composeControls->setText(std::move(text));
	}, box->lifetime());

	//ActivateWindow(_controller);
	_controller->show(std::move(box));

	return true;
}

bool ShortcutMessages::confirmSendingFiles(
		QImage &&image,
		QByteArray &&content,
		std::optional<bool> overrideSendImagesAsPhotos,
		const QString &insertTextOnCancel) {
	if (image.isNull()) {
		return false;
	}

	auto list = Storage::PrepareMediaFromImage(
		std::move(image),
		std::move(content),
		st::sendMediaPreviewSize);
	list.overrideSendImagesAsPhotos = overrideSendImagesAsPhotos;
	return confirmSendingFiles(std::move(list), insertTextOnCancel);
}

void ShortcutMessages::sendingFilesConfirmed(
		std::shared_ptr<Ui::PreparedBundle> bundle,
		Api::SendOptions options) {
	if (showSendingFilesError(*bundle)) {
		return;
	}
	if (isLocal()) {
		prepareLocalMedia({
			.target = localMediaTarget(),
			.caption = {
				_composeControls->getTextWithAppliedMarkdown().text,
				TextUtilities::ConvertTextTagsToEntities(
					_composeControls->getTextWithAppliedMarkdown().tags) },
			.source = Data::LocalQuickReplyFileBundle{ std::move(bundle) },
		}, crl::guard(this, [=] { _composeControls->clear(); }));
		return;
	}
	const auto compress = bundle->way.sendImagesAsPhotos();
	const auto type = compress ? SendMediaType::Photo : SendMediaType::File;
	auto action = prepareSendAction(options);
	action.clearDraft = false;
	auto &api = _session->api();
	for (auto &group : bundle->groups) {
		const auto album = (group.type != Ui::AlbumType::None)
			? std::make_shared<SendingAlbum>()
			: nullptr;
		api.sendFiles(std::move(group.list), type, album, action);
	}
	if (_composeControls->replyingToMessage() == action.replyTo) {
		_composeControls->cancelReplyMessage();
	}
	finishSending();
}

void ShortcutMessages::chooseAttach(
		std::optional<bool> overrideSendImagesAsPhotos) {
	if (showPremiumRequired()) {
		return;
	}
	_choosingAttach = false;

	const auto filter = (overrideSendImagesAsPhotos == true)
		? FileDialog::PhotoVideoFilesFilter()
		: FileDialog::AllOrImagesFilter();
	FileDialog::GetOpenPaths(this, tr::lng_choose_files(tr::now), filter, crl::guard(this, [=](
			FileDialog::OpenResult &&result) {
		if (result.paths.isEmpty() && result.remoteContent.isEmpty()) {
			return;
		}

		if (!result.remoteContent.isEmpty()) {
			auto read = Images::Read({
				.content = result.remoteContent,
			});
			if (!read.image.isNull() && !read.animated) {
				confirmSendingFiles(
					std::move(read.image),
					std::move(result.remoteContent),
					overrideSendImagesAsPhotos);
			} else {
				uploadFile(result.remoteContent, SendMediaType::File);
			}
		} else {
			const auto premium = _controller->session().user()->isPremium();
			auto list = Storage::PrepareMediaList(
				result.paths,
				st::sendMediaPreviewSize,
				premium);
			list.overrideSendImagesAsPhotos = overrideSendImagesAsPhotos;
			confirmSendingFiles(std::move(list));
		}
	}), nullptr);
}

void ShortcutMessages::finishSending() {
	_composeControls->hidePanelsAnimated();
	//if (_previewData && _previewData->pendingTill) previewCancel();
	doSetInnerFocus();
	showAtEnd();
}

bool ShortcutMessages::processChosenSticker(ChatHelpers::FileChosen &&chosen) {
	if (!_composeControls) {
		return false;
	}
	_composeControls->processChosenSticker(std::move(chosen));
	return true;
}

void ShortcutMessages::showAtEnd() {
	showAtPosition(Data::MaxMessagePosition);
}

void ShortcutMessages::doSetInnerFocus() {
	if (!_inner->getSelectedText().rich.text.isEmpty()
		|| !_inner->getSelectedItems().empty()
		|| !_composeControls->focus()) {
		_inner->setFocus();
	}
}

bool ShortcutMessages::sendExistingDocument(
		not_null<DocumentData*> document,
		Api::SendOptions options,
		std::optional<MsgId> localId) {
	if (isLocal()) {
		prepareLocalMedia({
			.target = localMediaTarget(),
			.caption = {
				_composeControls->getTextWithAppliedMarkdown().text,
				TextUtilities::ConvertTextTagsToEntities(
					_composeControls->getTextWithAppliedMarkdown().tags) },
			.source = Data::LocalQuickReplyDocument{ document, {} },
		}, crl::guard(this, [=] {
			_composeControls->clearFieldAfterStickerSend();
			_composeControls->clear();
		}));
		return true;
	} else if (showPremiumRequired()) {
		return false;
	}

	Api::SendExistingDocument(
		Api::MessageToSend(prepareSendAction(options)),
		document,
		localId);

	_composeControls->clearFieldAfterStickerSend();
	_composeControls->cancelReplyMessage();
	finishSending();
	return true;
}

void ShortcutMessages::sendExistingPhoto(not_null<PhotoData*> photo) {
	sendExistingPhoto(photo, {});
}

bool ShortcutMessages::sendExistingPhoto(
		not_null<PhotoData*> photo,
		Api::SendOptions options) {
	if (isLocal()) {
		prepareLocalMedia({
			.target = localMediaTarget(),
			.caption = {
				_composeControls->getTextWithAppliedMarkdown().text,
				TextUtilities::ConvertTextTagsToEntities(
					_composeControls->getTextWithAppliedMarkdown().tags) },
			.source = Data::LocalQuickReplyPhoto{ photo, {} },
		}, crl::guard(this, [=] {
			_composeControls->clear();
		}));
		return true;
	} else if (showPremiumRequired()) {
		return false;
	}
	Api::SendExistingPhoto(
		Api::MessageToSend(prepareSendAction(options)),
		photo);

	_composeControls->cancelReplyMessage();
	finishSending();
	return true;
}

void ShortcutMessages::sendInlineResult(
		std::shared_ptr<InlineBots::Result> result,
		not_null<UserData*> bot) {
	if (!isLocal() && showPremiumRequired()) {
		return;
	} else if (const auto error = isLocal()
		? Data::SendError() : result->getErrorOnSend(_history)) {
		Data::ShowSendErrorToast(_controller, _history->peer, error);
		return;
	}
	sendInlineResult(std::move(result), bot, {}, std::nullopt);
}

void ShortcutMessages::sendInlineResult(
		std::shared_ptr<InlineBots::Result> result,
		not_null<UserData*> bot,
		Api::SendOptions options,
		std::optional<MsgId> localMessageId) {
	if (isLocal()) {
		prepareLocalMedia({
			.target = localMediaTarget(),
			.caption = {
				_composeControls->getTextWithAppliedMarkdown().text,
				TextUtilities::ConvertTextTagsToEntities(
					_composeControls->getTextWithAppliedMarkdown().tags) },
			.source = Data::LocalQuickReplyInline{ std::move(result), bot },
		}, crl::guard(this, [=] { _composeControls->clear(); }));
		return;
	} else if (showPremiumRequired()) {
		return;
	}
	auto action = prepareSendAction(options);
	action.generateLocal = true;
	_session->api().sendInlineResult(
		bot,
		result.get(),
		action,
		localMessageId);

	_composeControls->clear();
	//_saveDraftText = true;
	//_saveDraftStart = crl::now();
	//onDraftSave();

	bot->session().recentInlineBots().bump(bot);
	finishSending();
}

void ShortcutMessages::showAtPosition(
		Data::MessagePosition position,
		FullMsgId originItemId) {
	showAtPosition(position, originItemId, {});
}

void ShortcutMessages::showAtPosition(
		Data::MessagePosition position,
		FullMsgId originItemId,
		const Window::SectionShow &params) {
	_lastShownAt = position.fullId;
	_inner->showAtPosition(
		position,
		params,
		_cornerButtons.doneJumpFrom(position.fullId, originItemId, true));
}

FullReplyTo ShortcutMessages::replyTo() const {
	return _composeControls->replyingToMessage();
}

bool ShortcutMessages::showPremiumRequired() const {
	if (isLocal()) {
		return false;
	} else if (!_controller->session().premium()) {
		ShowPremiumPreviewToBuy(_controller, PremiumFeature::QuickReplies);
		return true;
	}
	return false;
}

} // namespace

Type ShortcutMessagesId(int shortcutId) {
	return ShortcutMessages::Id(shortcutId);
}

Type ShortcutMessagesId(QString replyId) {
	return ShortcutMessages::Id(replyId);
}

} // namespace Settings
