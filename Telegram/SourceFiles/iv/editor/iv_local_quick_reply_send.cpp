/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/editor/iv_local_quick_reply_send.h"

#include "base/random.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/local_quick_reply_rich_codec.h"
#include "iv/markdown/iv_markdown_prepare.h"
#include "iv/markdown/iv_markdown_prepare_serialize.h"
#include "iv/iv_rich_message_serializer.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QFileInfo>

#include <algorithm>

namespace Iv {
namespace {

using Block = RichPage::Block;
using BlockKind = RichPage::BlockKind;

struct MediaReplacement {
	PhotoData *photo = nullptr;
	DocumentData *document = nullptr;
};

using Replacements = std::map<uint64, MediaReplacement>;

struct UploadSource {
	UploadSource(
		const FilePrepareResult &source,
		std::shared_ptr<FilePrepareResult> owner);

	std::shared_ptr<FilePrepareResult> leaseOwner;
	FilePrepareResult file;
};

struct Attachment {
	uint64 sourceId = 0;
	std::shared_ptr<FilePrepareResult> file;
	std::shared_ptr<FilePrepareResult> cover;
};

class Preparation final
	: public LocalRichSendPreparation
	, public std::enable_shared_from_this<Preparation> {
public:
	Preparation(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		std::shared_ptr<const RichPage> page,
		LocalRichSendFiles files,
		LocalRichSendDone done);
	~Preparation();

	void schedule();
	void cancel() override;

private:
	void start();
	void uploadNext();
	void uploaded(const Storage::UploadedMedia &media, bool photo);
	void finalized(const MTPMessageMedia &media);
	void finish(std::optional<QString> error = std::nullopt);
	void clearUploading();
	void stop();

	Main::Session *_session = nullptr;
	const not_null<PeerData*> _peer;
	std::shared_ptr<RichPage> _page;
	LocalRichSendFiles _files;
	LocalRichSendDone _done;
	std::unique_ptr<MTP::Sender> _sender;
	std::shared_ptr<FilePrepareResult> _activeFile;
	std::vector<Attachment> _attachments;
	Replacements _replacements;
	std::optional<MTPInputPhoto> _cover;
	FullMsgId _uploadId;
	size_t _index = 0;
	bool _uploadingCover = false;
	bool _finalizing = false;
	bool _stopped = false;
	rpl::lifetime _lifetime;

};

UploadSource::UploadSource(
		const FilePrepareResult &source,
		std::shared_ptr<FilePrepareResult> owner)
: leaseOwner(std::move(owner))
, file(source) {
	file.transcodedTempPath.clear();
}

[[nodiscard]] QString Error(QString detail) {
	return u"Local quick reply rich send: "_q + detail;
}

void ReplaceMedia(Block &block, const Replacements &replacements) {
	const auto photo = [&](PhotoData *&value, uint64 &id) {
		const auto i = replacements.find(value ? value->id : id);
		if (i != replacements.end() && i->second.photo) {
			value = i->second.photo;
			id = value->id;
		}
	};
	const auto document = [&](DocumentData *&value, uint64 &id) {
		const auto i = replacements.find(value ? value->id : id);
		if (i != replacements.end() && i->second.document) {
			value = i->second.document;
			id = value->id;
		}
	};
	photo(block.photo, block.photoId);
	document(block.document, block.documentId);
	for (auto &item : block.mediaItems) {
		photo(item.photo, item.photoId);
		document(item.document, item.documentId);
	}
	for (auto &article : block.relatedArticles) {
		photo(article.photo, article.photoId);
	}
	for (auto &child : block.blocks) {
		ReplaceMedia(child, replacements);
	}
	for (auto &item : block.listItems) {
		for (auto &child : item.blocks) {
			ReplaceMedia(child, replacements);
		}
	}
}

[[nodiscard]] bool RemoteDocumentReady(not_null<DocumentData*> document) {
	const auto input = document->mtpInput();
	return document->hasRemoteLocation()
		&& input.type() == mtpc_inputDocument
		&& input.c_inputDocument().vid().v
		&& input.c_inputDocument().vaccess_hash().v
		&& !document->fileReference().isEmpty();
}

[[nodiscard]] bool UserReady(not_null<Main::Session*> session, uint64 id) {
	if (id == session->userId().bare) {
		return true;
	}
	const auto user = session->data().user(UserId(id));
	return id && (user->isLoaded()
		|| user->owner().messageWithPeer(user->id));
}

[[nodiscard]] QString ValidateText(
	not_null<Main::Session*> session,
	const TextWithEntities &text,
	int depth = 0);

[[nodiscard]] QString ValidateButton(
		not_null<Main::Session*> session,
		HistoryMessageMarkupButton::Type type,
		const QByteArray &data) {
	using Type = HistoryMessageMarkupButton::Type;
	switch (type) {
	case Type::Url:
	case Type::CopyText:
	case Type::Disabled:
		return QString();
	case Type::UserProfile:
		return UserReady(session, data.toULongLong())
			? QString()
			: Error(u"button profile is unavailable in this session"_q);
	default:
		return Error(u"this button requires a server-side bot context"_q);
	}
}

QString ValidateText(
		not_null<Main::Session*> session,
		const TextWithEntities &text,
		int depth) {
	if (depth > 16) {
		return Error(u"inline object nesting is too deep"_q);
	}
	auto entities = text.entities;
	for (const auto &entity : entities) {
		if (entity.offset() < 0
			|| entity.length() <= 0
			|| entity.offset() > text.text.size()
			|| entity.length() > text.text.size() - entity.offset()) {
			return Error(u"invalid text entity range"_q);
		}
	}
	std::sort(entities.begin(), entities.end(), [](const auto &a, const auto &b) {
		return (a.offset() != b.offset())
			? (a.offset() < b.offset())
			: (a.length() > b.length());
	});
	auto ends = std::vector<int>();
	for (const auto &entity : entities) {
		while (!ends.empty() && entity.offset() >= ends.back()) {
			ends.pop_back();
		}
		const auto end = entity.offset() + entity.length();
		if (!ends.empty() && end > ends.back()) {
			return Error(u"overlapping text entities cannot be preserved"_q);
		}
		ends.push_back(end);
		switch (entity.type()) {
		case EntityType::Invalid:
		case EntityType::Semibold:
		case EntityType::MediaTimestamp:
		case EntityType::Colorized:
		case EntityType::Pre:
		case EntityType::Blockquote:
			return Error(u"text entity has no native rich-send representation"_q);
		case EntityType::MentionName: {
			const auto fields = TextUtilities::MentionNameDataToFields(
				entity.data());
			if (!fields.userId
				|| fields.selfId != session->userId().bare
				|| (!fields.accessHash && !UserReady(session, fields.userId))) {
				return Error(u"mentioned user is unavailable in this session"_q);
			}
		} break;
		case EntityType::CustomEmoji: {
			const auto parsed = Markdown::ParseInlineTextObjectEntity(
				entity.data());
			if (!parsed) {
				const auto id = ::Data::ParseCustomEmojiData(entity.data());
				if (!id || !RemoteDocumentReady(session->data().document(id))) {
					return Error(u"custom emoji requires an available server document"_q);
				}
			} else if (parsed->kind == Markdown::InlineTextObjectKind::IvImage) {
				return Error(u"inline IV images have no native rich-send representation"_q);
			} else if (const auto button = std::get_if<
					Markdown::InlineTextObjectButtonData>(&parsed->data)) {
				const auto error = ValidateButton(session, button->type, button->data);
				if (!error.isEmpty()) {
					return error;
				} else if (button->peerTypes || button->label.text.isEmpty()) {
					return Error(u"inline button context cannot be preserved"_q);
				}
				const auto labelError = ValidateText(session, button->label, depth + 1);
				if (!labelError.isEmpty()) {
					return labelError;
				}
			} else if (const auto formula = std::get_if<
					Markdown::InlineTextObjectFormulaData>(&parsed->data)) {
				if (formula->trimmedTex.isEmpty() && formula->copySource.isEmpty()) {
					return Error(u"inline formula is empty"_q);
				}
			}
		} break;
		case EntityType::FormattedDate:
			if (!DeserializeFormattedDateData(entity.data()).first) {
				return Error(u"formatted date is invalid"_q);
			}
			break;
		case EntityType::CustomUrl:
			if (const auto decoded = DecodeRichPageLinkUrl(entity.data())) {
				if (decoded->webpageId) {
					return Error(u"linked webpage context cannot be preserved"_q);
				}
			}
			break;
		default:
			break;
		}
	}
	return QString();
}

[[nodiscard]] QString ValidateBlock(
		not_null<Main::Session*> session,
		const Block &block) {
	const auto hasText = [](const RichPage::RichText &text) {
		return !text.text.empty()
			|| !text.anchorId.isEmpty()
			|| !text.anchorIds.empty();
	};
	const auto textKinds = {
		BlockKind::Heading,
		BlockKind::Paragraph,
		BlockKind::Footer,
		BlockKind::Thinking,
		BlockKind::AuthorDate,
		BlockKind::Code,
		BlockKind::Quote,
		BlockKind::Table,
		BlockKind::Details,
	};
	const auto captionKinds = {
		BlockKind::Quote,
		BlockKind::Photo,
		BlockKind::Video,
		BlockKind::Audio,
		BlockKind::File,
		BlockKind::GroupedMedia,
		BlockKind::Map,
	};
	if ((hasText(block.text)
			&& std::find(textKinds.begin(), textKinds.end(), block.kind)
				== textKinds.end())
		|| (hasText(block.caption)
			&& std::find(captionKinds.begin(), captionKinds.end(), block.kind)
				== captionKinds.end())) {
		return Error(u"text or caption would be omitted from this block"_q);
	}
	if ((!block.blocks.empty()
			&& block.kind != BlockKind::Details
			&& block.kind != BlockKind::Quote)
		|| (!block.listItems.empty() && block.kind != BlockKind::List)
		|| (!block.mediaItems.empty() && block.kind != BlockKind::GroupedMedia)
		|| (!block.tableRows.empty() && block.kind != BlockKind::Table)
		|| (!block.buttons.empty() && block.kind != BlockKind::ButtonRow)
		|| !block.relatedArticles.empty()) {
		return Error(u"nested payload has no native rich-send representation"_q);
	}
	if ((block.kind == BlockKind::Photo && !block.url.isEmpty())
		|| ((block.kind == BlockKind::Audio || block.kind == BlockKind::File)
			&& block.spoiler)
		|| (block.kind == BlockKind::Quote
			&& block.collapsed
			&& !block.blocks.empty())) {
		return Error(u"rich media or quote options cannot be preserved"_q);
	}
	for (const auto text : { &block.text, &block.caption }) {
		const auto error = ValidateText(session, text->text);
		if (!error.isEmpty()) {
			return error;
		}
	}
	for (const auto &button : block.buttons) {
		const auto error = ValidateButton(
			session,
			button.button.type,
			button.button.data);
		if (!error.isEmpty()) {
			return error;
		} else if (button.button.visual.iconId || button.button.peerTypes) {
			return Error(u"button icon or context cannot be preserved"_q);
		}
		const auto labelError = ValidateText(session, button.text.text);
		if (!labelError.isEmpty()) {
			return labelError;
		}
	}
	for (const auto &row : block.tableRows) {
		for (const auto &cell : row.cells) {
			const auto error = ValidateText(session, cell.text.text);
			if (!error.isEmpty()) {
				return error;
			}
		}
	}
	for (const auto &child : block.blocks) {
		const auto error = ValidateBlock(session, child);
		if (!error.isEmpty()) {
			return error;
		}
	}
	for (const auto &item : block.listItems) {
		const auto error = ValidateText(session, item.text.text);
		if (!error.isEmpty()) {
			return error;
		}
		for (const auto &child : item.blocks) {
			const auto error = ValidateBlock(session, child);
			if (!error.isEmpty()) {
				return error;
			}
		}
	}
	return QString();
}

[[nodiscard]] QString ValidateFile(const FilePrepareResult &file) {
	if (!file.id || file.filesize <= 0) {
		return Error(u"attachment is empty or invalid"_q);
	}
	if (file.videoSource || file.animationJob || file.archive || file.archiveEntries) {
		return Error(u"attachment preparation has not completed"_q);
	}
	for (const auto &sticker : file.attachedStickers) {
		if (sticker.type() != mtpc_inputDocument
			|| !sticker.c_inputDocument().vid().v
			|| !sticker.c_inputDocument().vaccess_hash().v
			|| sticker.c_inputDocument().vfile_reference().v.isEmpty()) {
			return Error(u"attached sticker requires an available server document"_q);
		}
	}
	if (file.type == SendMediaType::Photo) {
		if (file.photo.type() != mtpc_photo || file.fileparts.empty()) {
			return Error(u"photo is not prepared"_q);
		}
	} else if (file.type == SendMediaType::File
		|| file.type == SendMediaType::Audio
		|| file.type == SendMediaType::Round) {
		if (file.document.type() != mtpc_document || file.filemime.isEmpty()) {
			return Error(u"document is not prepared"_q);
		}
		if (file.content.isEmpty() && file.filepath.isEmpty()) {
			return Error(u"document source is unavailable"_q);
		}
	} else {
		return Error(u"unsupported attachment upload type"_q);
	}
	if (!file.filepath.isEmpty()) {
		const auto info = QFileInfo(file.filepath);
		if (!info.isFile() || !info.isReadable()
			|| (file.type != SendMediaType::Photo
				&& file.content.isEmpty()
				&& info.size() != file.filesize)) {
			return Error(u"attachment source is unavailable"_q);
		}
	}
	if (file.videoCover) {
		if (file.type == SendMediaType::Photo
			|| file.videoCover->type != SendMediaType::Photo
			|| file.videoCover->videoCover) {
			return Error(u"invalid video cover"_q);
		}
		return ValidateFile(*file.videoCover);
	}
	return QString();
}

[[nodiscard]] std::shared_ptr<FilePrepareResult> CopyForUpload(
		const std::shared_ptr<FilePrepareResult> &source,
		const std::shared_ptr<FilePrepareResult> &leaseOwner) {
	const auto owner = std::make_shared<UploadSource>(*source, leaseOwner);
	auto result = std::shared_ptr<FilePrepareResult>(owner, &owner->file);
	result->id = base::RandomValue<uint64>();
	result->thumbId = (result->type == SendMediaType::Photo)
		? result->id
		: base::RandomValue<uint64>();
	result->videoCover.reset();
	result->album.reset();
	result->to.replaceMediaOf = MsgId();
	if (result->type == SendMediaType::Photo) {
		const auto &photo = source->photo.c_photo();
		result->photo = MTP_photo(
			photo.vflags(),
			MTP_long(result->id),
			MTP_long(0),
			MTP_bytes(QByteArray()),
			photo.vdate(),
			photo.vsizes(),
			MTP_vector<MTPVideoSize>(photo.vvideo_sizes().value_or_empty()),
			photo.vdc_id());
	} else {
		const auto &document = source->document.c_document();
		result->document = MTP_document(
			document.vflags(),
			MTP_long(result->id),
			MTP_long(0),
			MTP_bytes(QByteArray()),
			document.vdate(),
			document.vmime_type(),
			document.vsize(),
			MTP_vector<MTPPhotoSize>(document.vthumbs().value_or_empty()),
			MTP_vector<MTPVideoSize>(document.vvideo_thumbs().value_or_empty()),
			document.vdc_id(),
			document.vattributes());
	}
	return result;
}

Preparation::Preparation(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		std::shared_ptr<const RichPage> page,
		LocalRichSendFiles files,
		LocalRichSendDone done)
: _session(session)
, _peer(peer)
, _page(page ? std::make_shared<RichPage>(*page) : nullptr)
, _files(std::move(files))
, _done(std::move(done))
, _sender(std::make_unique<MTP::Sender>(&session->mtp())) {
}

Preparation::~Preparation() {
	stop();
}

void Preparation::schedule() {
	const auto weak = weak_from_this();
	_session->account().sessionChanges(
	) | rpl::on_next([weak](Main::Session *session) {
		if (const auto strong = weak.lock()) {
			if (session != strong->_session) {
				strong->cancel();
			}
		}
	}, _lifetime);
	crl::on_main([weak] {
		if (const auto strong = weak.lock()) {
			strong->start();
		}
	});
}

void Preparation::start() {
	if (_stopped) {
		return;
	}
	if (!_page || &_peer->session() != _session) {
		finish(Error(u"invalid page or destination session"_q));
		return;
	}
	const auto error = ValidateLocalRichForSend(_session, *_page, _files);
	if (!error.isEmpty()) {
		finish(error);
		return;
	}
	for (const auto &[id, file] : _files) {
		_attachments.push_back({
			.sourceId = id,
			.file = CopyForUpload(file, file),
			.cover = file->videoCover
				? CopyForUpload(file->videoCover, file)
				: nullptr,
		});
	}
	const auto weak = weak_from_this();
	_session->uploader().photoReady(
	) | rpl::on_next([weak](const Storage::UploadedMedia &media) {
		if (const auto strong = weak.lock()) {
			strong->uploaded(media, true);
		}
	}, _lifetime);
	_session->uploader().documentReady(
	) | rpl::on_next([weak](const Storage::UploadedMedia &media) {
		if (const auto strong = weak.lock()) {
			strong->uploaded(media, false);
		}
	}, _lifetime);
	rpl::merge(
		_session->uploader().photoFailed(),
		_session->uploader().documentFailed()
	) | rpl::on_next([weak](FullMsgId id) {
		if (const auto strong = weak.lock()) {
			if (id == strong->_uploadId) {
				strong->finish(Error(u"attachment upload failed"_q));
			}
		}
	}, _lifetime);
	uploadNext();
}

void Preparation::uploadNext() {
	if (_stopped) {
		return;
	} else if (_index == _attachments.size()) {
		for (auto &block : _page->blocks) {
			ReplaceMedia(block, _replacements);
		}
		finish();
		return;
	}
	const auto &attachment = _attachments[_index];
	_uploadingCover = attachment.cover && !_cover;
	const auto file = _uploadingCover ? attachment.cover : attachment.file;
	_activeFile = file;
	_uploadId = FullMsgId(_peer->id, _session->data().nextLocalMessageId());
	_finalizing = false;
	_session->uploader().upload(_uploadId, file);
}

void Preparation::uploaded(const Storage::UploadedMedia &media, bool photo) {
	if (_stopped || media.fullId != _uploadId || _finalizing) {
		return;
	}
	const auto &attachment = _attachments[_index];
	const auto &file = _uploadingCover ? attachment.cover : attachment.file;
	if (photo != (file->type == SendMediaType::Photo)) {
		finish(Error(u"unexpected upload result"_q));
		return;
	}
	_finalizing = true;
	auto stickers = QVector<MTPInputDocument>();
	for (const auto &sticker : media.info.attachedStickers) {
		stickers.push_back(sticker);
	}
	auto input = MTPInputMedia();
	if (photo) {
		using Flag = MTPDinputMediaUploadedPhoto::Flag;
		auto flags = MTPDinputMediaUploadedPhoto::Flags();
		if (file->spoiler) {
			flags |= Flag::f_spoiler;
		}
		if (!stickers.empty()) {
			flags |= Flag::f_stickers;
		}
		input = MTP_inputMediaUploadedPhoto(
			MTP_flags(flags),
			media.info.file,
			MTP_vector<MTPInputDocument>(std::move(stickers)),
			MTP_int(0),
			MTPInputDocument());
	} else {
		using Flag = MTPDinputMediaUploadedDocument::Flag;
		auto flags = MTPDinputMediaUploadedDocument::Flags();
		if (file->forceFile) {
			flags |= Flag::f_force_file;
		}
		if (file->spoiler) {
			flags |= Flag::f_spoiler;
		}
		if (media.info.thumb) {
			flags |= Flag::f_thumb;
		}
		if (!stickers.empty()) {
			flags |= Flag::f_stickers;
		}
		if (_cover) {
			flags |= Flag::f_video_cover;
		}
		if (file->videoCoverOffset) {
			flags |= Flag::f_video_timestamp;
		}
		const auto attributes = file->document.c_document().vattributes();
		const auto document = _session->data().document(file->id);
		if (!file->forceFile
			&& document->type != AnimatedDocument
			&& document->isSilentVideo()) {
			flags |= Flag::f_nosound_video;
		}
		input = MTP_inputMediaUploadedDocument(
			MTP_flags(flags),
			media.info.file,
			media.info.thumb.value_or(MTPInputFile()),
			MTP_string(file->filemime),
			attributes,
			MTP_vector<MTPInputDocument>(std::move(stickers)),
			_cover.value_or(MTPInputPhoto()),
			MTP_int(file->videoCoverOffset / 1000),
			MTP_int(0));
	}
	const auto weak = weak_from_this();
	_sender->request(MTPmessages_UploadMedia(
		MTP_flags(0),
		MTPstring(),
		_peer->input(),
		std::move(input)
	)).done([weak](const MTPMessageMedia &result) {
		if (const auto strong = weak.lock()) {
			strong->finalized(result);
		}
	}).fail([weak](const MTP::Error &error) {
		if (const auto strong = weak.lock()) {
			strong->finish(MTP::IgnoreError(error)
				? QString()
				: Error(u"media finalization failed: "_q + error.type()));
		}
	}).handleAllErrors().send();
}

void Preparation::finalized(const MTPMessageMedia &media) {
	if (_stopped || !_finalizing) {
		return;
	}
	const auto &attachment = _attachments[_index];
	const auto file = _uploadingCover ? attachment.cover : attachment.file;
	auto replacement = MediaReplacement();
	if (file->type == SendMediaType::Photo
		&& media.type() == mtpc_messageMediaPhoto) {
		const auto photo = media.c_messageMediaPhoto().vphoto();
		if (photo && photo->type() == mtpc_photo
			&& photo->c_photo().vid().v
			&& photo->c_photo().vaccess_hash().v
			&& !photo->c_photo().vfile_reference().v.isEmpty()) {
			replacement.photo = _session->data().processPhoto(*photo);
		}
	} else if (file->type != SendMediaType::Photo
		&& media.type() == mtpc_messageMediaDocument) {
		const auto document = media.c_messageMediaDocument().vdocument();
		if (document && document->type() == mtpc_document) {
			const auto resolved = _session->data().processDocument(*document);
			if (RemoteDocumentReady(resolved)) {
				replacement.document = resolved;
			}
		}
	}
	if (!replacement.photo && !replacement.document) {
		finish(Error(u"server returned incomplete uploaded media"_q));
		return;
	}
	clearUploading();
	_uploadId = FullMsgId();
	_finalizing = false;
	if (_uploadingCover) {
		_cover = replacement.photo->mtpInput();
	} else {
		_replacements.emplace(attachment.sourceId, replacement);
		_cover.reset();
		++_index;
	}
	uploadNext();
}

void Preparation::finish(std::optional<QString> error) {
	if (_stopped) {
		return;
	}
	auto value = std::optional<MTPInputRichMessage>();
	if (!error) {
		auto result = SerializeInputRichMessage(
			_session,
			*_page,
			SerializeInputRichMessageMode::FinalSubmit);
		if (result.status == SerializeInputRichMessageStatus::Success) {
			value = std::move(result.value);
		} else {
			error = Error(u"uploaded rich content cannot be serialized"_q);
		}
	}
	auto done = std::move(_done);
	auto page = value ? _page : nullptr;
	stop();
	if (done) {
		done(std::move(page), std::move(value), error.value_or(QString()));
	}
}

void Preparation::clearUploading() {
	if (const auto file = base::take(_activeFile)) {
		if (file->type == SendMediaType::Photo) {
			_session->data().photo(file->id)->uploadingData.reset();
		} else {
			_session->data().document(file->id)->uploadingData.reset();
		}
	}
}

void Preparation::stop() {
	if (std::exchange(_stopped, true)) {
		return;
	}
	_lifetime.destroy();
	_sender.reset();
	if (_uploadId && _session) {
		_session->uploader().cancel(base::take(_uploadId));
	}
	clearUploading();
	_attachments.clear();
	_files.clear();
	_replacements.clear();
	_page.reset();
	_session = nullptr;
}

void Preparation::cancel() {
	_done = nullptr;
	stop();
}

} // namespace

QString ValidateLocalRichForSend(
		not_null<Main::Session*> session,
		const RichPage &page,
		const LocalRichSendFiles &files) {
	if (ValidateRichMessage(page, ResolveRichMessageLimits(session))) {
		return Error(u"rich message exceeds native limits"_q);
	}
	const auto collected = ::Data::CollectLocalQuickReplyRichMedia(page);
	if (const auto error = std::get_if<QString>(&collected)) {
		return *error;
	}
	const auto &media = std::get<::Data::LocalQuickReplyRichMedia>(collected);
	auto replacements = Replacements();
	auto photos = std::vector<std::unique_ptr<PhotoData>>();
	auto documents = std::vector<std::unique_ptr<DocumentData>>();
	for (const auto &[id, file] : files) {
		if (!file || id != file->id) {
			return Error(u"attachment identity mismatch"_q);
		}
		const auto error = ValidateFile(*file);
		if (!error.isEmpty()) {
			return error;
		}
	}
	// FinalSubmit needs remote references even for structural validation.
	// Detached objects let the native serializer validate every media block,
	// caption and nested structure before the first upload. They never enter
	// the session media registry, and the resulting wire value is discarded.
	// Only actual server results are used by the preparation operation.
	for (const auto source : media.photos) {
		const auto i = files.find(source->id);
		if (&source->session() != session
			|| i == files.end()
			|| i->second->type != SendMediaType::Photo) {
			return Error(u"photo has no matching prepared local asset"_q);
		}
		auto photo = std::make_unique<PhotoData>(&session->data(), source->id);
		photo->setRemoteLocation(1, 1, QByteArray("local-validation"));
		replacements[source->id].photo = photo.get();
		photos.push_back(std::move(photo));
	}
	for (const auto source : media.documents) {
		const auto i = files.find(source->id);
		if (&source->session() != session
			|| i == files.end()
			|| i->second->type == SendMediaType::Photo
			|| replacements.contains(source->id)) {
			return Error(u"document has no matching prepared local asset"_q);
		}
		auto document = std::make_unique<DocumentData>(
			&session->data(),
			source->id);
		document->setRemoteLocation(1, 1, QByteArray("local-validation"));
		document->setattributes(i->second->document.c_document().vattributes().v);
		replacements[source->id].document = document.get();
		documents.push_back(std::move(document));
	}
	if (replacements.size() != files.size()) {
		return Error(u"prepared assets do not match the complete rich page"_q);
	}
	auto validation = page;
	for (auto &block : validation.blocks) {
		const auto error = ValidateBlock(session, block);
		if (!error.isEmpty()) {
			return error;
		}
		ReplaceMedia(block, replacements);
	}
	const auto draft = SerializeInputRichMessage(
		session,
		validation,
		SerializeInputRichMessageMode::Draft);
	const auto final = SerializeInputRichMessage(
		session,
		validation,
		SerializeInputRichMessageMode::FinalSubmit);
	if (draft.status != SerializeInputRichMessageStatus::Success
		|| final.status != SerializeInputRichMessageStatus::Success
		|| !final.value) {
		return Error(u"rich content has no complete native send representation"_q);
	}
	const auto &serialized = final.value->c_inputRichMessage();
	for (const auto &photo : serialized.vphotos().value_or_empty()) {
		if (photo.type() == mtpc_inputPhoto) {
			const auto i = replacements.find(photo.c_inputPhoto().vid().v);
			if (i != replacements.end() && i->second.photo) {
				replacements.erase(i);
			}
		}
	}
	for (const auto &document : serialized.vdocuments().value_or_empty()) {
		if (document.type() == mtpc_inputDocument) {
			const auto i = replacements.find(document.c_inputDocument().vid().v);
			if (i != replacements.end() && i->second.document) {
				replacements.erase(i);
			}
		}
	}
	return replacements.empty()
		? QString()
		: Error(u"rich serialization would omit a local attachment"_q);
}

std::shared_ptr<LocalRichSendPreparation> PrepareLocalRichForSend(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		std::shared_ptr<const RichPage> page,
		LocalRichSendFiles files,
		LocalRichSendDone done) {
	const auto result = std::make_shared<Preparation>(
		session,
		peer,
		std::move(page),
		std::move(files),
		std::move(done));
	result->schedule();
	return result;
}

} // namespace Iv
