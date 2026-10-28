/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/controls/history_view_compose_controls.h"

#include "chat_helpers/compose/compose_show.h"
#include "data/data_drafts.h"
#include "data/data_session.h"
#include "data/local_quick_replies.h"
#include "data/local_quick_reply_view.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>

namespace HistoryView {
namespace {

[[nodiscard]] bool ContainsDraftFields(
		const QJsonValue &current,
		const QJsonValue &submitted) {
	if (!submitted.isObject()) return current == submitted;
	if (!current.isObject()) return false;
	const auto actual = current.toObject();
	const auto expected = submitted.toObject();
	for (auto i = expected.begin(); i != expected.end(); ++i) {
		if (i.key() == u"source_version_vector"_q
			|| i.key() == u"source_legacy_parent_version_vector"_q) {
			const auto value = actual.value(i.key());
			if (i.value().isNull()) {
				if (!value.isNull() && !value.isUndefined()) return false;
			} else if (value != i.value()) {
				return false;
			}
		} else if (!actual.contains(i.key())
			|| !ContainsDraftFields(actual.value(i.key()), i.value())) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QJsonObject EncodeDraftText(const TextWithTags &text) {
	auto tags = QJsonArray();
	for (const auto &tag : text.tags) {
		tags.push_back(QJsonObject{
			{ u"offset"_q, tag.offset },
			{ u"length"_q, tag.length },
			{ u"id"_q, tag.id },
		});
	}
	return {
		{ u"text"_q, text.text },
		{ u"tags"_q, tags },
	};
}

[[nodiscard]] TextWithTags DecodeDraftText(const QJsonObject &value) {
	auto result = TextWithTags{ value.value(u"text"_q).toString() };
	for (const auto &entry : value.value(u"tags"_q).toArray()) {
		const auto tag = entry.toObject();
		result.tags.push_back({
			tag.value(u"offset"_q).toInt(),
			tag.value(u"length"_q).toInt(),
			tag.value(u"id"_q).toString(),
		});
	}
	return result;
}

[[nodiscard]] QJsonObject EncodeDraft(
		const Data::Draft &draft,
		const QString &messageRef,
		const Data::LocalQuickReplyMessage *source = nullptr) {
	auto result = EncodeDraftText(draft.textWithTags);
	result.insert(u"message_ref"_q, messageRef.isEmpty()
		? QJsonValue(QJsonValue::Null) : QJsonValue(messageRef));
	if (source && source->id == messageRef) {
		const auto revision = source->retained.value(u"version_vector"_q).toObject();
		if (!revision.isEmpty()) {
			result.insert(u"source_version_vector"_q, revision);
		} else if (!source->legacyParentRevision.isEmpty()) {
			result.insert(u"source_legacy_parent_version_vector"_q,
				source->legacyParentRevision);
		}
	}
	result.insert(u"quote"_q, EncodeDraftText({
		draft.reply.quote.text,
		TextUtilities::ConvertEntitiesToTextTags(draft.reply.quote.entities),
	}));
	result.insert(u"quote_offset"_q, draft.reply.quoteOffset);
	result.insert(u"cursor"_q, QJsonObject{
		{ u"position"_q, draft.cursor.position },
		{ u"anchor"_q, draft.cursor.anchor },
		{ u"scroll"_q, draft.cursor.scroll },
	});
	result.insert(u"webpage"_q, QJsonObject{
		{ u"url"_q, draft.webpage.url },
		{ u"large"_q, draft.webpage.forceLargeMedia },
		{ u"small"_q, draft.webpage.forceSmallMedia },
		{ u"invert"_q, draft.webpage.invert },
		{ u"manual"_q, draft.webpage.manual },
		{ u"removed"_q, draft.webpage.removed },
	});
	return result;
}

[[nodiscard]] bool MatchesEditSource(
		const QJsonObject &draft,
		const Data::LocalQuickReplyMessage &source) {
	const auto revision = draft.value(u"source_version_vector"_q).toObject();
	const auto legacy = draft.value(u"source_legacy_parent_version_vector"_q).toObject();
	if (!revision.isEmpty()) {
		return legacy.isEmpty()
			&& revision == source.retained.value(u"version_vector"_q).toObject();
	}
	return !legacy.isEmpty() && legacy == source.legacyParentRevision
		&& source.retained.value(u"version_vector"_q).toObject().isEmpty();
}

[[nodiscard]] std::unique_ptr<Data::Draft> DecodeDraft(
		const QJsonObject &value,
		FullMsgId messageId) {
	if (value.isEmpty()) return nullptr;
	const auto cursor = value.value(u"cursor"_q).toObject();
	const auto webpage = value.value(u"webpage"_q).toObject();
	const auto quote = DecodeDraftText(value.value(u"quote"_q).toObject());
	return std::make_unique<Data::Draft>(
		DecodeDraftText(value),
		FullReplyTo{
			.messageId = messageId,
			.quote = { quote.text, TextUtilities::ConvertTextTagsToEntities(quote.tags) },
			.quoteOffset = value.value(u"quote_offset"_q).toInt(),
		},
		SuggestOptions(),
		MessageCursor{
			cursor.value(u"position"_q).toInt(),
			cursor.value(u"anchor"_q).toInt(),
			cursor.value(u"scroll"_q).toInt(),
		},
		Data::WebPageDraft{
			.url = webpage.value(u"url"_q).toString(),
			.forceLargeMedia = webpage.value(u"large"_q).toBool(),
			.forceSmallMedia = webpage.value(u"small"_q).toBool(),
			.invert = webpage.value(u"invert"_q).toBool(),
			.manual = webpage.value(u"manual"_q).toBool(),
			.removed = webpage.value(u"removed"_q).toBool(),
		});
}

} // namespace

class LocalQuickReplyDraftWriter final
	: public std::enable_shared_from_this<LocalQuickReplyDraftWriter> {
public:
	LocalQuickReplyDraftWriter(
		not_null<Data::LocalQuickReplies*> model,
		QString replyRef,
		QJsonObject revision,
		QJsonObject content,
		Fn<void(QString)> error)
	: _model(model.get())
	, _replyRef(std::move(replyRef))
	, _revision(std::move(revision))
	, _confirmed(QJsonDocument(content).toJson(QJsonDocument::Compact))
	, _error(std::move(error)) {
	}

	[[nodiscard]] bool busy() const {
		return _active;
	}

	void request(QJsonObject content) {
		_pending = std::move(content);
		_pendingBytes = QJsonDocument(_pending).toJson(QJsonDocument::Compact);
		start();
	}

private:
	void start() {
		if (!_model || _active || _pendingBytes == _confirmed) return;
		const auto submitted = _pendingBytes;
		const auto submittedContent = _pending;
		_active = true;
		const auto writer = shared_from_this();
		_model->saveEditorDraft(_replyRef, _pending, _revision,
			[writer, submitted, submittedContent](bool saved) {
				writer->_active = false;
				if (!writer->_model) return;
				if (!saved) {
					writer->_error(writer->_model->error());
					return;
				}
				const auto current = writer->_model->editorDraft(writer->_replyRef);
				auto content = QJsonObject();
				if (!current.value(u"deleted"_q).toBool()) {
					for (const auto &key : { u"normal"_q, u"edit"_q }) {
						if (current.contains(key)) content.insert(key, current.value(key));
					}
				}
				if (!ContainsDraftFields(content, submittedContent)) {
					writer->_error(tr::lng_local_replies_draft_changed(tr::now));
					return;
				}
				writer->_confirmed = submitted;
				writer->_revision = current.value(u"version_vector"_q).toObject();
				writer->start();
			});
	}

	const QPointer<Data::LocalQuickReplies> _model;
	const QString _replyRef;
	QJsonObject _revision;
	QJsonObject _pending;
	QByteArray _pendingBytes;
	QByteArray _confirmed;
	Fn<void(QString)> _error;
	bool _active = false;

};

Data::Draft *ComposeControls::composeDraft(DraftType type) const {
	if (_localQuickReplyRef.isEmpty()) {
		return _history ? _history->draft(draftKey(type)) : nullptr;
	}
	return (type == DraftType::Edit)
		? _localQuickReplyEditDraft.get() : _localQuickReplyNormalDraft.get();
}

void ComposeControls::setComposeDraft(
		DraftType type,
		std::unique_ptr<Data::Draft> draft) {
	if (_localQuickReplyRef.isEmpty()) {
		_history->setDraft(draftKey(type), std::move(draft));
		return;
	}
	auto &reference = (type == DraftType::Edit)
		? _localQuickReplyEditMessageRef : _localQuickReplyNormalMessageRef;
	if (draft->reply.messageId) {
		if (const auto item = session().data().message(draft->reply.messageId)) {
			if (const auto source = session().data().localQuickReplyView().message(item)) {
				reference = source->id;
			}
		}
	} else {
		reference.clear();
	}
	((type == DraftType::Edit)
		? _localQuickReplyEditDraft : _localQuickReplyNormalDraft) = std::move(draft);
}

void ComposeControls::clearComposeDraft(DraftType type) {
	if (_localQuickReplyRef.isEmpty()) {
		_history->clearDraft(draftKey(type));
		return;
	}
	((type == DraftType::Edit)
		? _localQuickReplyEditDraft : _localQuickReplyNormalDraft).reset();
	((type == DraftType::Edit)
		? _localQuickReplyEditMessageRef : _localQuickReplyNormalMessageRef).clear();
}

void ComposeControls::loadLocalQuickReplyDrafts() {
	_localQuickReplyDraftsLoaded = false;
	session().data().localQuickReplyView().updates(
		_localQuickReplyRef
	) | rpl::on_next([=] {
		restoreLocalQuickReplyDrafts();
	}, _historyLifetime);
	session().data().localQuickReplies().changes() | rpl::on_next([=] {
		restoreLocalQuickReplyDrafts();
	}, _historyLifetime);
	session().data().localQuickReplyView().request(_localQuickReplyRef);
	restoreLocalQuickReplyDrafts();
}

void ComposeControls::restoreLocalQuickReplyDrafts() {
	if (_localQuickReplyDraftsLoaded || !session().data().localQuickReplies().loaded()) {
		return;
	}
	const auto record = session().data().localQuickReplies().editorDraft(_localQuickReplyRef);
	const auto draftVersion = record.value(u"version_vector"_q).toObject();
	const auto normal = record.value(u"deleted"_q).toBool()
		? QJsonObject() : record.value(u"normal"_q).toObject();
	const auto edit = record.value(u"deleted"_q).toBool()
		? QJsonObject() : record.value(u"edit"_q).toObject();
	const auto normalRef = normal.value(u"message_ref"_q).toString();
	const auto editRef = edit.value(u"message_ref"_q).toString();
	const auto &view = session().data().localQuickReplyView();
	const auto normalId = view.resolve(_localQuickReplyRef, normalRef);
	const auto editId = view.resolve(_localQuickReplyRef, editRef);
	const auto reply = session().data().localQuickReplies().find(_localQuickReplyRef);
	if (!reply) return;
	const auto waitingFor = [&](const QString &reference, FullMsgId id) {
		return !reference.isEmpty() && !id
			&& ranges::find(reply->messages, reference,
				&Data::LocalQuickReplyMessage::id) != reply->messages.end();
	};
	const auto source = ranges::find(reply->messages, editRef,
		&Data::LocalQuickReplyMessage::id);
	const auto matched = source != reply->messages.end()
		&& MatchesEditSource(edit, *source)
		&& Data::QuickReplyMessageEditError(*source).isEmpty();
	if (waitingFor(normalRef, normalId)
		|| (matched && waitingFor(editRef, editId))) return;
	const auto recoverEdit = !edit.isEmpty() && (!matched || !editId);
	_localQuickReplyDeferredEditDraft = recoverEdit ? edit : QJsonObject();
	_localQuickReplyNormalDraft = DecodeDraft(normal, normalId);
	_localQuickReplyEditDraft = recoverEdit ? nullptr : DecodeDraft(edit, editId);
	_localEditSource = matched && editId
		? std::make_shared<Data::LocalQuickReplyMessage>(*source) : nullptr;
	_localQuickReplyNormalMessageRef = normalId ? normalRef : QString();
	_localQuickReplyEditMessageRef = editRef;
	_localQuickReplyDraftsLoaded = true;
	auto content = QJsonObject();
	if (!normal.isEmpty()) content.insert(u"normal"_q, normal);
	if (!edit.isEmpty()) content.insert(u"edit"_q, edit);
	_localQuickReplyDraftWriter = std::make_shared<LocalQuickReplyDraftWriter>(
		&session().data().localQuickReplies(),
		_localQuickReplyRef,
		draftVersion,
		content,
		crl::guard(_wrap.get(), [show = _show](QString error) {
			show->showToast(error);
		}));
	applyDraft();
	if (!normalRef.isEmpty() && !normalId) {
		_show->showToast(tr::lng_local_replies_reply_draft_missing(tr::now));
	}
	if (recoverEdit) showLocalQuickReplyDraftRecovery();
}

void ComposeControls::retainLocalQuickReplyEditDraft() {
	if (_localQuickReplyRef.isEmpty()) {
		cancelEditMessage();
		return;
	}
	saveFieldToHistoryLocalDraft(false);
	if (const auto draft = composeDraft(DraftType::Edit)) {
		_localQuickReplyDeferredEditDraft = EncodeDraft(
			*draft, _localQuickReplyEditMessageRef, _localEditSource.get());
	}
	cancelEditMessage();
	showLocalQuickReplyDraftRecovery();
}

void ComposeControls::showLocalQuickReplyDraftRecovery() {
	const auto edit = _localQuickReplyDeferredEditDraft;
	if (edit.isEmpty()) return;
	const auto replyRef = _localQuickReplyRef;
	const auto writer = _localQuickReplyDraftWriter;
	const auto reply = session().data().localQuickReplies().find(replyRef);
	if (!reply) return;
	_show->show(Ui::MakeConfirmBox({
		.text = tr::lng_local_replies_edit_draft_missing(),
		.confirmed = crl::guard(_wrap.get(), [=](Fn<void()> close) {
			const auto current = session().data().localQuickReplies().editorDraft(replyRef);
			if (_localQuickReplyRef != replyRef || _localQuickReplyDraftWriter != writer
				|| !writer || writer->busy()
				|| !ContainsDraftFields(current.value(u"edit"_q), edit)
				|| _localQuickReplyDeferredEditDraft != edit) {
				_show->showToast(tr::lng_local_replies_draft_changed(tr::now));
				close();
				return;
			}
			const auto draftVersion = current.value(u"version_vector"_q).toObject();
			const auto text = DecodeDraftText(edit);
			auto message = Data::LocalQuickReplyMessage{
				.message = { text.text,
					TextUtilities::ConvertTextTagsToEntities(text.tags) },
				.media = QJsonObject{ { u"kind"_q, u"text"_q } },
				.position = reply->messages.empty()
					? 0 : std::min(reply->messages.back().position, 2147483646) + 1,
			};
			session().data().localQuickReplies().saveMessages(
				replyRef,
				{ std::move(message) },
				crl::guard(_wrap.get(), [=](bool saved) {
					if (_localQuickReplyRef != replyRef || _localQuickReplyDraftWriter != writer) {
						return;
					}
					const auto current = session().data().localQuickReplies().editorDraft(replyRef);
					if (saved && (current.value(u"version_vector"_q).toObject() != draftVersion
						|| _localQuickReplyDeferredEditDraft != edit)) {
						_show->showToast(tr::lng_local_replies_draft_changed(tr::now));
					} else if (saved) {
						_localQuickReplyDeferredEditDraft = {};
						writeLocalQuickReplyDrafts();
					} else {
						_show->showToast(session().data().localQuickReplies().error());
					}
				}));
			close();
		}),
		.confirmText = tr::lng_local_replies_save_draft_text(),
	}));
}

void ComposeControls::writeLocalQuickReplyDrafts() {
	if (!_localQuickReplyDraftsLoaded) return;
	saveFieldToHistoryLocalDraft(false);
	auto content = QJsonObject();
	if (const auto draft = composeDraft(DraftType::Normal)) {
		if (!draft->reply.messageId) _localQuickReplyNormalMessageRef.clear();
		content.insert(u"normal"_q, EncodeDraft(*draft, _localQuickReplyNormalMessageRef));
	}
	if (!_localQuickReplyDeferredEditDraft.isEmpty()) {
		content.insert(u"edit"_q, _localQuickReplyDeferredEditDraft);
	} else if (const auto draft = composeDraft(DraftType::Edit)) {
		content.insert(u"edit"_q, EncodeDraft(
			*draft, _localQuickReplyEditMessageRef, _localEditSource.get()));
	}
	_localQuickReplyDraftWriter->request(std::move(content));
}

} // namespace HistoryView
