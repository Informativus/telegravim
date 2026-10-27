/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_local_quick_reply_sending.h"

#include "api/api_sending.h"
#include "apiwrap.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "chat_helpers/message_field.h"
#include "data/components/ephemeral_messages.h"
#include "data/components/scheduled_messages.h"
#include "data/stickers/data_stickers.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_histories.h"
#include "data/data_messages.h"
#include "data/data_photo.h"
#include "data/data_premium_limits.h"
#include "data/data_session.h"
#include "data/local_quick_replies.h"
#include "data/local_quick_reply_media.h"
#include "data/local_quick_reply_rich_codec.h"
#include "data/local_quick_reply_view.h"
#include "history/history.h"
#include "history/history_item_helpers.h"
#include "iv/editor/iv_local_quick_reply_send.h"
#include "iv/iv_rich_message_serializer.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/sender.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"

#include <crl/crl_on_main.h>
#include <crl/crl_queue.h>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtGui/QImageReader>

#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace Api {
namespace {

constexpr auto kImageMemoryLimit = int64(128) * 1024 * 1024;
constexpr auto kBatchMemoryLimit = int64(256) * 1024 * 1024;
constexpr auto kMaxStickerReferences = 1024;
constexpr auto kMaxStickerSets = 64;
constexpr auto kMaxResolvedStickerDocuments = 4096;

[[nodiscard]] crl::queue &PreparationQueue() {
	static auto result = crl::queue();
	return result;
}

[[nodiscard]] int AssetIndex(const QJsonObject &media, const QString &key) {
	const auto value = media.value(key);
	const auto index = value.toInt(-1);
	return value.isDouble() && index >= 0 && value.toDouble() == index
		? index : -1;
}

[[nodiscard]] bool DocumentKind(const QString &kind) {
	return kind == u"file"_q || kind == u"video"_q
		|| kind == u"round_video"_q || kind == u"voice"_q
		|| kind == u"audio"_q || kind == u"animation"_q
		|| kind == u"sticker"_q;
}

[[nodiscard]] TextWithTags WithTags(const TextWithEntities &text) {
	return { text.text, TextUtilities::ConvertEntitiesToTextTags(text.entities) };
}

[[nodiscard]] QString MetadataError(const QJsonObject &media, int depth = 0) {
	if (depth > 2) return u"The quick reply media is too deeply nested."_q;
	const auto invalid = u"The quick reply has invalid media metadata."_q;
	for (const auto &key : {
			u"spoiler"_q, u"invert_caption"_q, u"force_file"_q,
			u"supports_streaming"_q, u"nosound"_q, u"mask"_q,
			u"no_webpage"_q, u"requires_server_replay"_q, u"custom_emoji"_q,
			u"sticker_association_requires_server"_q }) {
		if (media.contains(key) && !media.value(key).isBool()) return invalid;
	}
	for (const auto &key : {
			u"kind"_q, u"title"_q, u"performer"_q, u"waveform"_q,
			u"alt"_q, u"video_codec"_q, u"first_name"_q, u"last_name"_q,
			u"phone_number"_q, u"vcard"_q, u"address"_q, u"provider"_q,
			u"venue_id"_q, u"venue_type"_q }) {
		if (media.contains(key) && (!media.value(key).isString()
			|| media.value(key).toString().size() > 65536)) return invalid;
	}
	for (const auto &key : {
			u"width"_q, u"height"_q, u"duration"_q,
			u"preload_prefix_size"_q, u"ttl_seconds"_q,
			u"video_start_ms"_q, u"video_timestamp"_q }) {
		if (!media.contains(key)) continue;
		const auto value = media.value(key);
		const auto number = value.toDouble(-1.);
		const auto fractional = key == u"duration"_q
			|| key == u"video_start_ms"_q || key == u"video_timestamp"_q;
		const auto maximum = double(std::numeric_limits<int>::max())
			* (key == u"video_start_ms"_q || key == u"video_timestamp"_q ? 1000. : 1.);
		if (!value.isDouble() || !std::isfinite(number) || number < 0.
			|| number > maximum || (!fractional && std::floor(number) != number)) {
			return invalid;
		}
	}
	for (const auto &key : {
			u"point"_q, u"sticker_set"_q, u"mask_coords"_q,
			u"webpage"_q, u"reply_markup"_q, u"rich"_q, u"cover_asset_media"_q }) {
		if (media.contains(key) && !media.value(key).isObject()) return invalid;
	}
	for (const auto &key : { u"rich_assets"_q, u"attached_stickers"_q }) {
		if (media.contains(key) && !media.value(key).isArray()) return invalid;
	}
	const auto webpage = media.value(u"webpage"_q).toObject();
	if (webpage.contains(u"url"_q) && !webpage.value(u"url"_q).isString()) return invalid;
	for (const auto &key : {
			u"force_large_media"_q, u"force_small_media"_q, u"manual"_q, u"safe"_q }) {
		if (webpage.contains(key) && !webpage.value(key).isBool()) return invalid;
	}
	const auto mask = media.value(u"mask_coords"_q).toObject();
	if (!mask.isEmpty()) {
		for (const auto &key : { u"n"_q, u"x"_q, u"y"_q, u"zoom"_q }) {
			const auto number = mask.value(key).toDouble();
			if (!mask.value(key).isDouble() || !std::isfinite(number)) return invalid;
		}
		const auto n = mask.value(u"n"_q).toDouble();
		if (n < 0. || n > 3. || std::floor(n) != n
			|| mask.value(u"zoom"_q).toDouble() <= 0.) return invalid;
	}
	const auto point = media.value(u"point"_q).toObject();
	if (point.contains(u"accuracy_radius"_q)) {
		const auto value = point.value(u"accuracy_radius"_q);
		const auto number = value.toDouble(-1.);
		if (!value.isDouble() || number < 0. || number > 1500.
			|| std::floor(number) != number) return invalid;
	}
	if (media.contains(u"cover_asset_media"_q)) {
		return MetadataError(media.value(u"cover_asset_media"_q).toObject(), depth + 1);
	}
	return {};
}

[[nodiscard]] QString ReplayError(const QJsonObject &media) {
	const auto kind = media.value(u"kind"_q).toString();
	if (media.value(u"requires_server_replay"_q).toBool()
		|| kind == u"game"_q || kind == u"invoice"_q
		|| kind == u"live_geo"_q
		|| !media.value(u"reply_markup"_q).toObject().isEmpty()
		|| media.value(u"custom_emoji"_q).toBool()
		|| media.contains(u"video_asset"_q)
		|| media.contains(u"video_thumbnail_asset"_q)) {
		const auto detail = media.value(u"replay_limitation"_q).toString();
		return detail.isEmpty()
			? u"This quick reply needs server-dependent media or actions that cannot be replayed safely; the original is preserved."_q
			: detail;
	}
	const auto set = media.value(u"sticker_set"_q).toObject();
	if (set.contains(u"constructor"_q)) {
		const auto constructor = set.value(u"constructor"_q).toString();
		if (constructor != QString::number(mtpc_inputStickerSetEmpty)
			&& constructor != QString::number(mtpc_inputStickerSetID)
			&& constructor != QString::number(mtpc_inputStickerSetShortName)) {
			return u"This special sticker set cannot be replayed as an ordinary sticker; the original is preserved."_q;
		}
	}
	if (media.value(u"sticker_association_requires_server"_q).toBool()
		&& media.value(u"attached_stickers"_q).toArray().isEmpty()) {
		return u"The attached sticker identities are missing; the original is preserved."_q;
	}
	if (media.contains(u"cover_asset_media"_q)) {
		return ReplayError(media.value(u"cover_asset_media"_q).toObject());
	}
	return {};
}

[[nodiscard]] QVector<MTPDocumentAttribute> Attributes(
		const QJsonObject &media,
		const QString &filename,
		std::optional<MTPInputStickerSet> resolvedSet = std::nullopt) {
	const auto kind = media.value(u"kind"_q).toString();
	auto result = QVector<MTPDocumentAttribute>{
		MTP_documentAttributeFilename(MTP_string(filename)) };
	const auto width = media.value(u"width"_q).toInt();
	const auto height = media.value(u"height"_q).toInt();
	if (kind == u"video"_q || kind == u"round_video"_q
		|| (kind == u"animation"_q && media.contains(u"duration"_q))) {
		using Flag = MTPDdocumentAttributeVideo::Flag;
		auto flags = MTPDdocumentAttributeVideo::Flags();
		if (kind == u"round_video"_q) flags |= Flag::f_round_message;
		if (media.value(u"supports_streaming"_q).toBool()) flags |= Flag::f_supports_streaming;
		if (media.value(u"nosound"_q).toBool()) flags |= Flag::f_nosound;
		if (media.contains(u"preload_prefix_size"_q)) flags |= Flag::f_preload_prefix_size;
		if (media.contains(u"video_start_ms"_q)) flags |= Flag::f_video_start_ts;
		if (media.contains(u"video_codec"_q)) flags |= Flag::f_video_codec;
		result.push_back(MTP_documentAttributeVideo(
			MTP_flags(flags),
			MTP_double(media.value(u"duration"_q).toDouble()),
			MTP_int(width),
			MTP_int(height),
			MTP_int(media.value(u"preload_prefix_size"_q).toInt()),
			MTP_double(media.value(u"video_start_ms"_q).toDouble() / 1000.),
			MTP_string(media.value(u"video_codec"_q).toString())));
	} else if (kind == u"voice"_q || kind == u"audio"_q) {
		using Flag = MTPDdocumentAttributeAudio::Flag;
		result.push_back(MTP_documentAttributeAudio(
			MTP_flags(kind == u"voice"_q
				? Flag::f_voice | Flag::f_waveform
				: Flag::f_title | Flag::f_performer),
			MTP_int(media.value(u"duration"_q).toInt()),
			MTP_string(media.value(u"title"_q).toString()),
			MTP_string(media.value(u"performer"_q).toString()),
			MTP_bytes(QByteArray::fromBase64(
				media.value(u"waveform"_q).toString().toLatin1()))));
	} else if (width > 0 && height > 0) {
		result.push_back(MTP_documentAttributeImageSize(
			MTP_int(width), MTP_int(height)));
	}
	if (kind == u"sticker"_q) {
		const auto name = media.value(u"sticker_set"_q).toObject()
			.value(u"short_name"_q).toString();
		const auto coords = media.value(u"mask_coords"_q).toObject();
		using Flag = MTPDdocumentAttributeSticker::Flag;
		auto flags = MTPDdocumentAttributeSticker::Flags();
		if (media.value(u"mask"_q).toBool()) flags |= Flag::f_mask;
		if (!coords.isEmpty()) flags |= Flag::f_mask_coords;
		result.push_back(MTP_documentAttributeSticker(
			MTP_flags(flags),
			MTP_string(media.value(u"alt"_q).toString()),
			resolvedSet.value_or(name.isEmpty()
				? MTPInputStickerSet(MTP_inputStickerSetEmpty())
				: MTP_inputStickerSetShortName(MTP_string(name))),
			(coords.isEmpty() ? MTPMaskCoords() : MTP_maskCoords(
				MTP_int(coords.value(u"n"_q).toInt()),
				MTP_double(coords.value(u"x"_q).toDouble()),
				MTP_double(coords.value(u"y"_q).toDouble()),
				MTP_double(coords.value(u"zoom"_q).toDouble())))));
	}
	if (kind == u"animation"_q) result.push_back(MTP_documentAttributeAnimated());
	return result;
}

struct PreparedAsset {
	QJsonObject media;
	QJsonObject descriptor;
	QString path;
	QString thumbnail;
	QString goodThumbnail;
	std::shared_ptr<FileLoadTask> task;
	std::shared_ptr<FilePrepareResult> file;
	std::shared_ptr<const Data::LocalQuickReplyAssetLease> lease;
	std::shared_ptr<PreparedAsset> cover;
	QString stickerSetKey;
	std::vector<DocumentId> attachedStickerIds;
};

struct PreparedMessage {
	Data::LocalQuickReplyMessage source;
	Data::LocalQuickReplyMaterialized materialized;
	std::shared_ptr<PreparedAsset> asset;
	std::shared_ptr<const Iv::RichPage> rich;
	std::map<int, std::shared_ptr<PreparedAsset>> richAssets;
	Iv::LocalRichSendFiles richFiles;
	std::optional<MTPInputRichMessage> serialized;
	std::optional<MTPInputMedia> input;
	std::optional<MTPMessageMedia> local;
	std::shared_ptr<SendingAlbum> album;
};

[[nodiscard]] QString ReadImage(
		const QString &path,
		QImage &image,
		QByteArray &bytes) {
	auto input = QFile(path);
	auto reader = QImageReader(path);
	const auto size = reader.size();
	if (size.isEmpty() || int64(size.width()) * size.height() * 4 > kImageMemoryLimit
		|| !input.open(QIODevice::ReadOnly) || input.size() > kImageMemoryLimit) {
		return u"The quick reply image exceeds native preparation limits."_q;
	}
	bytes = input.read(kImageMemoryLimit + 1);
	image = reader.read();
	return image.isNull() || bytes.size() != input.size()
		? u"Could not read the quick reply image."_q : QString();
}

[[nodiscard]] QString ReserveImages(
		const std::shared_ptr<PreparedAsset> &asset,
		int64 &remaining) {
	auto paths = QStringList{ asset->thumbnail, asset->goodThumbnail };
	if (asset->media.value(u"kind"_q).toString() == u"photo"_q) {
		paths.push_back(asset->path);
	}
	for (const auto &path : paths) {
		if (path.isEmpty()) continue;
		const auto bytes = QFileInfo(path).size();
		const auto size = QImageReader(path).size();
		const auto pixels = int64(size.width()) * size.height();
		if (size.isEmpty() || bytes <= 0 || bytes > kImageMemoryLimit
			|| pixels > kImageMemoryLimit / 4) {
			return u"The quick reply image exceeds the preparation memory limit."_q;
		}
		const auto reserved = pixels * 12 + bytes * 3;
		if (reserved > remaining) {
			return u"This quick reply selection exceeds the preparation memory budget; send a smaller selection."_q;
		}
		remaining -= reserved;
	}
	return asset->cover ? ReserveImages(asset->cover, remaining) : QString();
}

[[nodiscard]] QString ProcessAsset(
		const std::shared_ptr<PreparedAsset> &asset,
		int64 limit) {
	const auto info = QFileInfo(asset->path);
	if (!info.isFile() || info.size() <= 0 || info.size() > limit) {
		return u"The quick reply attachment is missing or exceeds the account upload limit."_q;
	}
	if (asset->media.value(u"kind"_q).toString() == u"photo"_q) {
		auto reader = QImageReader(asset->path);
		const auto size = reader.size();
		if (size.isEmpty()
			|| int64(size.width()) * size.height() * 4 > kImageMemoryLimit) {
			return u"The quick reply photo exceeds native preparation limits."_q;
		}
	}
	asset->task->process({ .generateGoodThumbnail = false });
	asset->file = asset->task->peekResult();
	const auto file = asset->file;
	const auto kind = asset->media.value(u"kind"_q).toString();
	if (!file || file->filesize <= 0 || file->filesize > limit
		|| (kind == u"photo"_q && file->type != SendMediaType::Photo)
		|| file->videoSource || file->animationJob || file->archive) {
		return u"Could not prepare the complete quick reply attachment."_q;
	}
	if (!asset->thumbnail.isEmpty()) {
		auto bytes = QByteArray();
		if (const auto error = ReadImage(asset->thumbnail, file->thumb, bytes)
			; !error.isEmpty()) return error;
		file->thumbId = base::RandomValue<uint64>();
		file->thumbname = bytes.startsWith("RIFF") ? u"thumb.webp"_q : u"thumb.jpg"_q;
		file->thumbparts.clear();
		file->setThumbData(bytes);
	}
	if (!asset->goodThumbnail.isEmpty()) {
		const auto error = ReadImage(
			asset->goodThumbnail, file->goodThumbnail, file->goodThumbnailBytes);
		if (!error.isEmpty()) return error;
	}
	if (DocumentKind(kind)) {
		file->filename = asset->descriptor.value(u"filename"_q).toString();
		file->filemime = asset->descriptor.value(u"mime"_q).toString();
		const auto &original = file->document.c_document();
		file->document = MTP_document(
			MTP_flags(original.vflags().v
				| (!file->thumb.isNull()
					? MTPDdocument::Flag::f_thumbs : MTPDdocument::Flag())),
			original.vid(),
			MTP_long(0),
			MTP_bytes(),
			original.vdate(),
			MTP_string(file->filemime),
			MTP_long(file->filesize),
			MTP_vector<MTPPhotoSize>(file->thumb.isNull()
				? original.vthumbs().value_or_empty()
				: QVector<MTPPhotoSize>{ MTP_photoSize(
					MTP_string("m"),
					MTP_int(file->thumb.width()),
					MTP_int(file->thumb.height()),
					MTP_int(file->thumbbytes.size())) }),
			MTPVector<MTPVideoSize>(),
			original.vdc_id(),
			MTP_vector<MTPDocumentAttribute>(Attributes(asset->media, file->filename)));
		file->type = kind == u"voice"_q ? SendMediaType::Audio
			: kind == u"round_video"_q ? SendMediaType::Round : SendMediaType::File;
	}
	if (asset->cover) {
		const auto error = ProcessAsset(asset->cover, limit);
		if (!error.isEmpty()) return error;
		file->videoCover = std::shared_ptr<FilePrepareResult>(
			asset->cover, asset->cover->file.get());
	}
	file->videoCoverOffset = crl::time(asset->media.value(u"video_start_ms"_q).toDouble());
	return {};
}

[[nodiscard]] std::optional<uint64> PortableStickerId(const QJsonValue &value) {
	if (!value.isString()) return std::nullopt;
	const auto text = value.toString();
	auto ok = false;
	const auto id = text.toULongLong(&ok);
	return ok && id && QString::number(id) == text
		? std::optional<uint64>(id) : std::nullopt;
}

[[nodiscard]] bool StickerShortName(const QString &name) {
	if (name.isEmpty() || name.size() > 64) return false;
	return ranges::all_of(name, [](QChar ch) {
		return (ch >= u'A' && ch <= u'Z') || (ch >= u'a' && ch <= u'z')
			|| (ch >= u'0' && ch <= u'9') || ch == u'_';
	});
}

[[nodiscard]] bool RemoteStickerReady(not_null<DocumentData*> document) {
	return document->sticker() && document->hasRemoteLocation()
		&& !document->fileReference().isEmpty();
}

void ReplaceStickerSet(
		const std::shared_ptr<PreparedAsset> &asset,
		const MTPInputStickerSet &set) {
	const auto file = asset->file;
	const auto &data = file->document.c_document();
	file->document = MTP_document(
		MTP_flags(data.vflags().v),
		data.vid(),
		data.vaccess_hash(),
		data.vfile_reference(),
		data.vdate(),
		data.vmime_type(),
		data.vsize(),
		MTP_vector<MTPPhotoSize>(data.vthumbs().value_or_empty()),
		MTP_vector<MTPVideoSize>(data.vvideo_thumbs().value_or_empty()),
		data.vdc_id(),
		MTP_vector<MTPDocumentAttribute>(Attributes(asset->media, file->filename, set)));
}

struct StickerSetResolution {
	StickerSetIdentifier identity;
	std::optional<MTPInputStickerSet> input;
	std::optional<MTPInputStickerSet> resolved;
};

struct AttachedStickerResolution {
	QJsonObject hint;
	QString setKey;
	std::optional<MTPInputDocument> input;
};

struct SendState final : std::enable_shared_from_this<SendState> {
	SendState(std::shared_ptr<const Data::LocalQuickReplyStage> stage,
		QStringList selectedMessageRefs, Data::ForwardOptions forwardOptions,
		SendAction action, Fn<bool()> prepared, Fn<void(QString)> failed)
	: stage(std::move(stage))
	, selectedMessageRefs(std::move(selectedMessageRefs))
	, forwardOptions(forwardOptions)
	, action(action)
	, session(base::make_weak(&action.history->session()))
	, prepared(std::move(prepared))
	, failed(std::move(failed)) {
	}

	std::shared_ptr<const Data::LocalQuickReplyStage> stage;
	QStringList selectedMessageRefs;
	Data::ForwardOptions forwardOptions;
	SendAction action;
	std::shared_ptr<Iv::LocalRichSendPreparation> richOperation;
	std::unique_ptr<MTP::Sender> stickerResolver;
	std::vector<std::shared_ptr<PreparedAsset>> stickerAssets;
	std::map<QString, StickerSetResolution> stickerSets;
	std::map<DocumentId, AttachedStickerResolution> attachedStickers;
	bool recentAttachedRequested = false;
	base::weak_ptr<Main::Session> session;
	Fn<bool()> prepared;
	Fn<void(QString)> failed;
	std::vector<PreparedMessage> messages;
	std::map<QString, FullMsgId> sent;
	std::vector<FullMsgId> current;
	std::set<FullMsgId> scheduledBefore;
	std::map<FullMsgId, QString> waiting;
	std::shared_ptr<SendState> keepAlive;
	std::atomic<bool> cancelled = false;
	rpl::lifetime lifetime;
	int offset = 0;
	int till = 0;
	bool collecting = false;

	void stop(QString error = {}, bool notify = false) {
		const auto keep = shared_from_this();
		if (cancelled.exchange(true)) return;
		lifetime.destroy();
		stickerResolver = nullptr;
		stickerAssets.clear();
		stickerSets.clear();
		attachedStickers.clear();
		if (richOperation) richOperation->cancel();
		richOperation = nullptr;
		if ((!error.isEmpty() || notify) && failed) failed(std::move(error));
		prepared = nullptr;
		failed = nullptr;
		waiting.clear();
		current.clear();
		messages.clear();
		stage = nullptr;
		keepAlive = nullptr;
	}

	[[nodiscard]] bool valid() const {
		return !cancelled.load() && session && stage && stage->valid();
	}

	[[nodiscard]] QString permissions() const {
		if (!valid()) return u"The quick reply account or insertion was closed."_q;
		auto items = HistoryItemsList();
		for (const auto &entry : messages) {
			const auto id = session->data().localQuickReplyView().resolve(
				stage->reference(), entry.source.id);
			const auto item = session->data().message(id);
			if (!item || session->data().localQuickReplyView().stageLease(item) != stage) {
				return u"The quick reply insertion is no longer available in this account."_q;
			}
			items.push_back(item);
		}
		const auto error = GetErrorForSending(action.history->peer, {
			.topicRootId = action.replyTo.topicRootId,
			.forward = &items,
			.messagesCount = int(items.size()),
		});
		return !error ? QString() : !error.text.isEmpty() ? error.text
			: u"Native permissions do not allow this quick reply to be sent."_q;
	}

	[[nodiscard]] QString validate() {
		if (!valid() || !prepared || IsLocalQuickReplyContext(action.options)
			|| action.options.shortcutId || action.options.welcomeTemplate
			|| action.replaceMediaOf || action.options.starsApproved < 0
			|| selectedMessageRefs.isEmpty()) {
			return u"This action is not an ordinary send of a local quick reply insertion."_q;
		}
		if (const auto reply = session->data().message(action.replyTo.messageId)) {
			if (reply->isLocalQuickReply()) {
				return u"A template message cannot be used as an outgoing reply target."_q;
			}
		}
		if (session->ephemeralMessages().isEphemeralBotReply(action.replyTo.messageId)) {
			return u"This quick reply cannot preserve an ephemeral bot reply; the insertion is unchanged."_q;
		}
		auto seen = std::set<QString>();
		auto completedGroups = std::set<QString>();
		auto group = QString();
		auto groupSize = 0;
		auto groupType = QString();
		auto groupReply = QString();
		auto invertCaption = false;
		const auto limits = Data::PremiumLimits(session.get());
		for (const auto &ref : selectedMessageRefs) {
			const auto found = ranges::find(stage->messages(), ref, &Data::LocalQuickReplyMessage::id);
			if (found == stage->messages().end()) {
				return u"A selected quick reply message is absent from its insertion snapshot."_q;
			}
			auto source = *found;
			if (source.replyRef != stage->replyRef() || source.id.isEmpty()
				|| seen.contains(source.id)
				|| (!source.replyToRef.isEmpty() && !seen.contains(source.replyToRef))) {
				return u"The quick reply order or reply references are invalid."_q;
			}
			if (action.options.scheduled && !source.replyToRef.isEmpty()) {
				return u"Scheduling quick replies with internal replies is not supported; the insertion is unchanged."_q;
			}
			if (session->ephemeralMessages().hasEphemeralCommand(
					action.history->peer, source.message.text)) {
				return u"Ephemeral commands cannot be replayed as ordinary quick reply messages; the insertion is unchanged."_q;
			}
			seen.emplace(source.id);
			const auto problem = Data::QuickReplyMessageEditError(source);
			if (!problem.isEmpty()) return problem;
			const auto &media = source.media;
			const auto kind = media.value(u"kind"_q).toString();
			const auto shape = MetadataError(media);
			if (!shape.isEmpty()) return shape;
			for (const auto &entity : source.message.entities) {
				if (entity.offset() < 0 || entity.length() <= 0
					|| entity.offset() > source.message.text.size()
					|| entity.length() > source.message.text.size() - entity.offset()) {
					return u"The quick reply contains an invalid text entity range."_q;
				}
			}
			const auto replay = ReplayError(media);
			if (!replay.isEmpty()) return replay;
			const auto mediaFile = kind == u"photo"_q || DocumentKind(kind);
			if (forwardOptions == Data::ForwardOptions::NoNamesAndCaptions
				&& kind != u"text"_q && kind != u"rich_message"_q) {
				source.message = {};
			}
			if (source.message.text.size() > (mediaFile
				? limits.captionLengthCurrent() : limits.messageLengthCurrent())) {
				return u"The quick reply exceeds the account text or caption limit."_q;
			}
			if (kind == u"text"_q && source.message.text.trimmed().isEmpty()) {
				return u"The quick reply contains an empty message."_q;
			}
			if (source.groupRef != group) {
				if (!group.isEmpty()) completedGroups.emplace(group);
				if (completedGroups.contains(source.groupRef)) {
					return u"The quick reply album is not contiguous."_q;
				}
				group = source.groupRef;
				groupSize = 0;
				groupType = kind == u"photo"_q || kind == u"video"_q
					? u"visual"_q : kind;
				groupReply = source.replyToRef;
				invertCaption = media.value(u"invert_caption"_q).toBool();
			}
			if (!group.isEmpty()) {
				const auto type = kind == u"photo"_q || kind == u"video"_q
					? u"visual"_q : kind;
				if (++groupSize > Ui::MaxAlbumItems()
					|| (type != u"visual"_q && type != u"file"_q && type != u"audio"_q)
					|| type != groupType || source.replyToRef != groupReply
					|| media.value(u"invert_caption"_q).toBool() != invertCaption
					|| media.value(u"ttl_seconds"_q).toInt()
					|| action.options.price || action.options.suggest
					|| action.options.scheduleRepeatPeriod) {
					return u"This quick reply album cannot preserve its native grouping and options."_q;
				}
				if (!source.replyToRef.isEmpty()) {
					for (const auto &entry : messages) {
						if (entry.source.id == source.replyToRef && entry.source.groupRef == group) {
						return u"An album item cannot reply to another item in the same album."_q;
						}
					}
				}
			}
			messages.push_back({ .source = source });
		}
		return permissions();
	}

	[[nodiscard]] QJsonObject descriptor(const PreparedMessage &entry, int index) const {
		if (index < 0) return {};
		if (index < entry.source.attachments.size()) {
			return entry.source.attachments.at(index).toObject();
		}
		index -= entry.source.attachments.size();
		const auto assets = stage->preparedAssets();
		if (!assets || index >= entry.source.sourcePaths.size()) return {};
		const auto &path = entry.source.sourcePaths[index];
		for (const auto &file : assets->files()) {
			if (file.path == path) return {
				{ u"filename"_q, file.filename }, { u"mime"_q, file.mime },
				{ u"size"_q, QString::number(file.size) },
			};
		}
		return {};
	}

	[[nodiscard]] std::shared_ptr<PreparedAsset> asset(
			const PreparedMessage &entry,
			const QJsonObject &media,
			QString &error,
			int depth = 0) {
		const auto index = AssetIndex(media, u"asset"_q);
		if (depth > 1 || index < 0 || index >= entry.materialized.paths.size()) {
			error = u"The quick reply attachment index is invalid."_q;
			return nullptr;
		}
		auto result = std::make_shared<PreparedAsset>();
		result->media = media;
		result->path = entry.materialized.paths[index];
		result->lease = entry.materialized.assets;
		result->descriptor = descriptor(entry, index);
		if (result->descriptor.isEmpty()) {
			error = u"The quick reply attachment metadata is missing."_q;
			return nullptr;
		}
		for (const auto &key : { u"thumbnail_asset"_q, u"good_thumbnail_asset"_q }) {
			if (!media.contains(key)) continue;
			const auto thumbnail = AssetIndex(media, key);
			if (thumbnail < 0 || thumbnail >= entry.materialized.paths.size()) {
				error = u"The quick reply thumbnail index is invalid."_q;
				return nullptr;
			}
			(key == u"thumbnail_asset"_q ? result->thumbnail : result->goodThumbnail)
				= entry.materialized.paths[thumbnail];
		}
		if (media.contains(u"cover_asset"_q)) {
			const auto cover = media.value(u"cover_asset_media"_q).toObject();
			if (AssetIndex(cover, u"asset"_q) != AssetIndex(media, u"cover_asset"_q)
				|| cover.value(u"kind"_q).toString() != u"photo"_q) {
				error = u"The quick reply cover index is invalid."_q;
				return nullptr;
			}
			result->cover = asset(entry, cover, error, depth + 1);
			if (!error.isEmpty()) return nullptr;
		}
		const auto photo = media.value(u"kind"_q).toString() == u"photo"_q;
		auto information = photo ? nullptr : std::make_unique<Ui::PreparedFileInformation>();
		if (information) information->filemime = result->descriptor.value(u"mime"_q).toString();
		result->task = std::make_shared<FileLoadTask>(FileLoadTask::Args{
			.session = session.get(),
			.filepath = result->path,
			.information = std::move(information),
			.type = photo ? SendMediaType::Photo : SendMediaType::File,
			.to = { action.history->peer->id, {}, {}, {} },
			.caption = WithTags(entry.source.message),
			.spoiler = media.value(u"spoiler"_q).toBool(),
			.forceFile = media.value(u"force_file"_q).toBool(),
			.sendLargePhotos = true,
			.displayName = result->descriptor.value(u"filename"_q).toString(),
		});
		return result;
	}

	void materialize(int index) {
		if (!valid()) { stop(); return; }
		if (index == int(messages.size())) { prepareFiles(); return; }
		const auto self = shared_from_this();
		session->data().localQuickReplies().materializeMessage(messages[index].source,
			[self, index](Data::LocalQuickReplyMaterialized files, QString error) {
				if (!self->valid()) { self->stop(); return; }
				if (!error.isEmpty()) { self->stop(std::move(error)); return; }
				self->messages[index].materialized = std::move(files);
				crl::on_main([self, index] { self->materialize(index + 1); });
			}, stage->preparedAssets());
	}

	void prepareFiles() {
		auto assets = std::vector<std::shared_ptr<PreparedAsset>>();
		for (auto &entry : messages) {
			const auto kind = entry.source.media.value(u"kind"_q).toString();
			if (kind == u"rich_message"_q) {
				for (const auto &value : entry.source.media.value(u"rich_assets"_q).toArray()) {
					const auto descriptor = value.toObject();
					const auto media = descriptor.value(u"media"_q).toObject();
					const auto index = AssetIndex(descriptor, u"asset"_q);
					if (index != AssetIndex(media, u"asset"_q)
						|| entry.richAssets.contains(index)) {
						stop(u"The rich attachment descriptor is invalid."_q);
						return;
					}
					auto error = MetadataError(media);
					if (!error.isEmpty()) { stop(error); return; }
					error = ReplayError(media);
					if (!error.isEmpty()) { stop(error); return; }
					auto prepared = asset(entry, media, error);
					if (!error.isEmpty()) { stop(error); return; }
					entry.richAssets.emplace(index, prepared);
					assets.push_back(std::move(prepared));
				}
			} else if (kind == u"photo"_q || DocumentKind(kind)) {
				auto error = QString();
				entry.asset = asset(entry, entry.source.media, error);
				if (!error.isEmpty()) { stop(std::move(error)); return; }
				assets.push_back(entry.asset);
			} else if (kind != u"text"_q) {
				const auto error = prepareSimple(entry);
				if (!error.isEmpty()) { stop(error); return; }
			}
		}
		const auto limit = session->premium() ? kFileSizePremiumLimit : kFileSizeLimit;
		const auto self = shared_from_this();
		PreparationQueue().async([self, assets = std::move(assets), limit] {
			auto error = QString();
			auto remaining = kBatchMemoryLimit;
			for (const auto &asset : assets) {
				if (self->cancelled.load()) return;
				error = ReserveImages(asset, remaining);
				if (!error.isEmpty()) break;
			}
			for (const auto &asset : assets) {
				if (!error.isEmpty()) break;
				if (self->cancelled.load()) return;
				error = ProcessAsset(asset, limit);
				if (!error.isEmpty()) break;
			}
			crl::on_main([self, error] {
				if (!self->valid()) { self->stop(); return; }
				if (!error.isEmpty()) { self->stop(error); return; }
				self->ready();
			});
		});
	}

	[[nodiscard]] QString prepareSimple(PreparedMessage &entry) {
		const auto &media = entry.source.media;
		const auto kind = media.value(u"kind"_q).toString();
		if (kind == u"contact"_q) {
			entry.input = MTP_inputMediaContact(
				MTP_string(media.value(u"phone_number"_q).toString()),
				MTP_string(media.value(u"first_name"_q).toString()),
				MTP_string(media.value(u"last_name"_q).toString()),
				MTP_string(media.value(u"vcard"_q).toString()));
			entry.local = MTP_messageMediaContact(
				MTP_string(media.value(u"phone_number"_q).toString()),
				MTP_string(media.value(u"first_name"_q).toString()),
				MTP_string(media.value(u"last_name"_q).toString()),
				MTP_string(media.value(u"vcard"_q).toString()),
				MTP_long(0));
		} else if (kind == u"geo"_q || kind == u"venue"_q) {
			const auto point = media.value(u"point"_q).toObject();
			const auto lat = point.value(u"latitude"_q);
			const auto lon = point.value(u"longitude"_q);
			if (!lat.isDouble() || !lon.isDouble()
				|| !std::isfinite(lat.toDouble()) || !std::isfinite(lon.toDouble())
				|| std::abs(lat.toDouble()) > 90. || std::abs(lon.toDouble()) > 180.) {
				return u"The quick reply location is invalid."_q;
			}
			const auto accuracy = point.value(u"accuracy_radius"_q).toInt();
			const auto input = MTP_inputGeoPoint(
				MTP_flags(accuracy ? MTPDinputGeoPoint::Flag::f_accuracy_radius : MTPDinputGeoPoint::Flag()),
				MTP_double(lat.toDouble()), MTP_double(lon.toDouble()), MTP_int(accuracy));
			const auto local = MTP_geoPoint(
				MTP_flags(accuracy ? MTPDgeoPoint::Flag::f_accuracy_radius : MTPDgeoPoint::Flag()),
				MTP_double(lon.toDouble()), MTP_double(lat.toDouble()), MTP_long(0), MTP_int(accuracy));
			if (kind == u"geo"_q) {
				entry.input = MTP_inputMediaGeoPoint(input);
				entry.local = MTP_messageMediaGeo(local);
			} else {
				const auto title = MTP_string(media.value(u"title"_q).toString());
				const auto address = MTP_string(media.value(u"address"_q).toString());
				const auto provider = MTP_string(media.value(u"provider"_q).toString());
				const auto id = MTP_string(media.value(u"venue_id"_q).toString());
				const auto type = MTP_string(media.value(u"venue_type"_q).toString());
				entry.input = MTP_inputMediaVenue(input, title, address, provider, id, type);
				entry.local = MTP_messageMediaVenue(local, title, address, provider, id, type);
			}
		} else {
			return u"This quick reply media cannot be sent without changing its content."_q;
		}
		return {};
	}

	void ready() {
		for (auto &entry : messages) {
			if (entry.source.media.value(u"kind"_q).toString() != u"rich_message"_q) continue;
			auto photos = std::map<int, PhotoData*>();
			auto documents = std::map<int, DocumentData*>();
			for (const auto &[index, asset] : entry.richAssets) {
				const auto file = std::shared_ptr<FilePrepareResult>(asset, asset->file.get());
				entry.richFiles.emplace(file->id, file);
				if (file->type == SendMediaType::Photo) {
					photos.emplace(index, session->data().processPhoto(file->photo, file->photoThumbs));
				} else {
					documents.emplace(index, session->data().processDocument(file->document));
				}
			}
			auto decoded = Data::DecodeLocalQuickReplyRich(
				entry.source.media.value(u"rich"_q).toObject(),
				[&](int index) -> PhotoData* {
					const auto i = photos.find(index);
					return i == photos.end() ? nullptr : i->second;
				}, [&](int index) -> DocumentData* {
					const auto i = documents.find(index);
					return i == documents.end() ? nullptr : i->second;
				});
			if (const auto error = std::get_if<QString>(&decoded)) { stop(*error); return; }
			entry.rich = std::get<std::shared_ptr<Iv::RichPage>>(std::move(decoded));
			const auto error = Iv::ValidateLocalRichForSend(session.get(), *entry.rich, entry.richFiles);
			if (!error.isEmpty()) { stop(error); return; }
		}
		prepareStickers();
	}

	[[nodiscard]] QString registerStickerSet(
			const QJsonObject &metadata,
			StickerSetIdentifier native,
			QString &error) {
		auto identity = StickerSetIdentifier();
		if (metadata.contains(u"id"_q)
			&& metadata.value(u"id"_q) != QJsonValue(u"0"_q)) {
			const auto id = PortableStickerId(metadata.value(u"id"_q));
			if (!id) { error = u"The sticker set identity is invalid."_q; return {}; }
			identity.id = *id;
		}
		if (metadata.contains(u"short_name"_q)) {
			const auto value = metadata.value(u"short_name"_q);
			identity.shortName = value.toString();
			if (!value.isString()
				|| (!identity.shortName.isEmpty() && !StickerShortName(identity.shortName))) {
				error = u"The sticker set name is invalid."_q;
				return {};
			}
		}
		const auto constructor = metadata.value(u"constructor"_q).toString();
		if ((constructor == QString::number(mtpc_inputStickerSetID) && !identity.id)
			|| (constructor == QString::number(mtpc_inputStickerSetShortName)
				&& identity.shortName.isEmpty())
			|| (constructor == QString::number(mtpc_inputStickerSetEmpty) && identity)
			|| metadata.contains(u"emoticon"_q)) {
			error = u"The sticker set metadata does not identify a supported ordinary set."_q;
			return {};
		}
		if (!identity) identity = native;
		if (!identity) return {};
		if (identity.id && identity.id == native.id) identity.accessHash = native.accessHash;
		const auto &sets = session->data().stickers().sets();
		const auto i = sets.find(identity.id);
		if (identity.id && i != sets.end()) {
			if (!identity.accessHash) identity.accessHash = i->second->accessHash;
			if (identity.shortName.isEmpty()) identity.shortName = i->second->shortName;
		}
		const auto key = identity.id ? u"id:"_q + QString::number(identity.id)
			: u"name:"_q + identity.shortName.toLower();
		if (!stickerSets.contains(key) && stickerSets.size() >= kMaxStickerSets) {
			error = u"This selection needs too many sticker set lookups; send a smaller selection."_q;
			return {};
		}
		auto &request = stickerSets[key];
		if (!request.identity) request.identity = identity;
		if (!request.input) {
			if (identity.id && identity.accessHash) {
				request.input = MTP_inputStickerSetID(
					MTP_long(identity.id),
					MTP_long(identity.accessHash));
			} else if (StickerShortName(identity.shortName)) {
				request.input = MTP_inputStickerSetShortName(MTP_string(identity.shortName));
			}
		}
		return key;
	}

	[[nodiscard]] QString registerAttachedSticker(DocumentId id) {
		auto &request = attachedStickers[id];
		const auto document = session->data().document(id);
		const auto sticker = document->sticker();
		auto error = QString();
		request.setKey = registerStickerSet(
			request.hint,
			sticker ? sticker->set : StickerSetIdentifier(),
			error);
		if (!error.isEmpty()) return error;
		if (request.setKey.isEmpty() && RemoteStickerReady(document)) {
			request.input = document->mtpInput();
		}
		return {};
	}

	[[nodiscard]] QString collectStickers(const std::shared_ptr<PreparedAsset> &asset) {
		stickerAssets.push_back(asset);
		const auto &media = asset->media;
		auto error = QString();
		if (media.value(u"kind"_q).toString() == u"sticker"_q) {
			asset->stickerSetKey = registerStickerSet(
				media.value(u"sticker_set"_q).toObject(),
				{},
				error);
			if (!error.isEmpty()) return error;
		}
		const auto values = media.value(u"attached_stickers"_q).toArray();
		if (values.size() > kMaxStickerReferences) {
			return u"This selection has too many attached stickers; send a smaller selection."_q;
		}
		const auto hintsValue = media.value(u"attached_sticker_sets"_q);
		if (!hintsValue.isUndefined() && !hintsValue.isObject()) {
			return u"The attached sticker set hints are invalid."_q;
		}
		const auto hints = hintsValue.toObject();
		auto ids = std::set<QString>();
		for (const auto &value : values) {
			const auto id = PortableStickerId(value);
			if (!id) return u"An attached sticker identity is invalid."_q;
			ids.emplace(value.toString());
			asset->attachedStickerIds.push_back(*id);
			if (!attachedStickers.contains(*id)
				&& attachedStickers.size() >= kMaxStickerReferences) {
				return u"This selection has too many attached stickers; send a smaller selection."_q;
			}
			auto &request = attachedStickers[*id];
			if (hints.contains(value.toString())) {
				const auto hintValue = hints.value(value.toString());
				if (!hintValue.isObject()) return u"An attached sticker set hint is invalid."_q;
				const auto hint = hintValue.toObject();
				for (auto i = hint.begin(); i != hint.end(); ++i) {
					if (i.key() == u"id"_q && PortableStickerId(i.value())) continue;
					if (i.key() == u"short_name"_q && i.value().isString()
						&& StickerShortName(i.value().toString())) continue;
					return u"An attached sticker set hint uses unsupported metadata."_q;
				}
				if (hint.isEmpty()) return u"An attached sticker set hint is empty."_q;
				for (auto i = hint.begin(); i != hint.end(); ++i) {
					const auto previous = request.hint.value(i.key());
					if (!previous.isUndefined() && previous != i.value()
						&& (i.key() != u"short_name"_q || previous.toString().compare(
							i.value().toString(), Qt::CaseInsensitive) != 0)) {
						return u"The attached sticker set hints conflict."_q;
					}
					request.hint.insert(i.key(), i.value());
				}
			}
		}
		for (auto i = hints.begin(); i != hints.end(); ++i) {
			if (!ids.contains(i.key())) return u"A sticker hint has no attached sticker identity."_q;
		}
		return asset->cover ? collectStickers(asset->cover) : QString();
	}

	void prepareStickers() {
		for (const auto &entry : messages) {
			if (entry.asset) {
				const auto error = collectStickers(entry.asset);
				if (!error.isEmpty()) { stop(error); return; }
			}
			for (const auto &[index, asset] : entry.richAssets) {
				const auto error = collectStickers(asset);
				if (!error.isEmpty()) { stop(error); return; }
			}
		}
		for (const auto &[id, request] : attachedStickers) {
			const auto error = registerAttachedSticker(id);
			if (!error.isEmpty()) { stop(error); return; }
		}
		stickerResolver = std::make_unique<MTP::Sender>(&session->mtp());
		resolveStickerSets();
	}

	void resolveStickerSets() {
		if (!valid()) { stop(); return; }
		for (const auto &[key, request] : stickerSets) {
			if (request.resolved) continue;
			if (!request.input) {
				if (!recentAttachedRequested && !attachedStickers.empty()) {
					resolveRecentAttached();
				} else {
					stop(u"The sticker set is unavailable in this account and has no usable portable name; the insertion is unchanged."_q);
				}
				return;
			}
			const auto weak = weak_from_this();
			stickerResolver->request(MTPmessages_GetStickerSet(
				*request.input,
				MTP_int(0)
			)).done([weak, key](const MTPmessages_StickerSet &result) {
				const auto self = weak.lock();
				if (!self) return;
				if (!self->valid()) { self->stop(); return; }
				if (result.type() != mtpc_messages_stickerSet) {
					self->stop(u"The sticker set returned no usable media; the insertion is unchanged."_q);
					return;
				}
				const auto &data = result.c_messages_stickerSet();
				const auto &set = data.vset().data();
				auto &request = self->stickerSets.at(key);
				const auto id = uint64(set.vid().v);
				if (!id || !set.vaccess_hash().v
					|| (request.identity.id && request.identity.id != id)
					|| (!request.identity.id && qs(set.vshort_name()).compare(
						request.identity.shortName, Qt::CaseInsensitive) != 0)
					|| data.vdocuments().v.size() > kMaxResolvedStickerDocuments) {
					self->stop(u"The resolved sticker set does not match its portable identity."_q);
					return;
				}
				request.resolved = MTP_inputStickerSetID(set.vid(), set.vaccess_hash());
				for (const auto &value : data.vdocuments().v) {
					if (value.type() != mtpc_document) continue;
					const auto i = self->attachedStickers.find(value.c_document().vid().v);
					if (i == self->attachedStickers.end() || i->second.setKey != key) continue;
					const auto document = self->session->data().processDocument(value);
					if (RemoteStickerReady(document)) i->second.input = document->mtpInput();
				}
				crl::on_main([weak] {
					if (const auto self = weak.lock()) self->resolveStickerSets();
				});
			}).fail([weak](const MTP::Error &error) {
				if (const auto self = weak.lock()) self->stop(
					MTP::IgnoreError(error) ? QString()
						: u"Could not resolve the sticker set; the insertion is unchanged."_q, true);
			}).handleFloodErrors().send();
			return;
		}
		for (const auto &[id, request] : attachedStickers) {
			if (!request.input) {
				if (!recentAttachedRequested) resolveRecentAttached();
				else stop(u"An attached sticker is unavailable in this account; the insertion is unchanged."_q);
				return;
			}
		}
		for (const auto &asset : stickerAssets) {
			if (!asset->stickerSetKey.isEmpty()) {
				ReplaceStickerSet(asset, *stickerSets.at(asset->stickerSetKey).resolved);
			}
			asset->file->attachedStickers.clear();
			for (const auto id : asset->attachedStickerIds) {
				asset->file->attachedStickers.push_back(*attachedStickers.at(id).input);
			}
		}
		stickerResolver = nullptr;
		stickerAssets.clear();
		stickerSets.clear();
		attachedStickers.clear();
		uploadRich(0);
	}

	void resolveRecentAttached() {
		recentAttachedRequested = true;
		const auto weak = weak_from_this();
		stickerResolver->request(MTPmessages_GetRecentStickers(
			MTP_flags(MTPmessages_GetRecentStickers::Flag::f_attached),
			MTP_long(0)
		)).done([weak](const MTPmessages_RecentStickers &result) {
			const auto self = weak.lock();
			if (!self) return;
			if (!self->valid()) { self->stop(); return; }
			if (result.type() != mtpc_messages_recentStickers
				|| result.c_messages_recentStickers().vstickers().v.size() > kMaxResolvedStickerDocuments) {
				self->stop(u"No usable attached stickers were returned; the insertion is unchanged."_q);
				return;
			}
			for (const auto &value : result.c_messages_recentStickers().vstickers().v) {
				if (value.type() != mtpc_document) continue;
				const auto i = self->attachedStickers.find(value.c_document().vid().v);
				if (i == self->attachedStickers.end()) continue;
				const auto document = self->session->data().processDocument(value);
				const auto error = self->registerAttachedSticker(document->id);
				if (!error.isEmpty()) { self->stop(error); return; }
				if (RemoteStickerReady(document) && i->second.setKey.isEmpty()) {
					i->second.input = document->mtpInput();
				}
			}
			crl::on_main([weak] {
				if (const auto self = weak.lock()) self->resolveStickerSets();
			});
		}).fail([weak](const MTP::Error &error) {
			if (const auto self = weak.lock()) self->stop(
				MTP::IgnoreError(error) ? QString()
					: u"Could not resolve attached stickers; the insertion is unchanged."_q, true);
		}).handleFloodErrors().send();
	}

	void uploadRich(int index) {
		if (!valid()) { stop(); return; }
		while (index < int(messages.size()) && !messages[index].rich) ++index;
		if (index == int(messages.size())) { accept(); return; }
		const auto self = shared_from_this();
		const auto &entry = messages[index];
		richOperation = Iv::PrepareLocalRichForSend(
			session.get(),
			action.history->peer,
			entry.rich,
			entry.richFiles,
			[self, index](std::shared_ptr<const Iv::RichPage> page,
					std::optional<MTPInputRichMessage> serialized, QString error) {
				if (!self->valid()) { self->stop(); return; }
				if (!error.isEmpty() || !serialized || !page) {
					self->stop(std::move(error), true);
					return;
				}
				self->messages[index].rich = std::move(page);
				self->messages[index].serialized = std::move(serialized);
				crl::on_main([self, index] {
					self->richOperation = nullptr;
					self->uploadRich(index + 1);
				});
			});
	}

	void accept() {
		const auto error = permissions();
		if (!error.isEmpty()) { stop(error); return; }
		if (!prepared()) { stop(); return; }
		if (!valid()) { stop(); return; }
		prepared = nullptr;
		const auto weak = weak_from_this();
		session->data().newItemAdded() | rpl::on_next([weak](not_null<HistoryItem*> item) {
			if (const auto self = weak.lock()) {
				if (self->collecting && item->history() == self->action.history
					&& item->isSending()) self->current.push_back(item->fullId());
			}
		}, lifetime);
		if (action.options.scheduled) {
			session->scheduledMessages().updates(action.history) | rpl::on_next([weak] {
				const auto self = weak.lock();
				if (!self || !self->valid() || !self->collecting) return;
				for (const auto id : self->session->scheduledMessages().list(self->action.history).ids) {
					if (self->scheduledBefore.contains(id)) continue;
					const auto item = self->session->data().message(id);
					if (item && item->isSending()) {
						self->scheduledBefore.emplace(id);
						self->current.push_back(id);
					}
				}
			}, lifetime);
		}
		session->data().itemIdChanged() | rpl::on_next([weak](const Data::Session::IdChange &change) {
			const auto self = weak.lock();
			if (!self || !self->valid()) return;
			const auto old = FullMsgId(change.newId.peer, change.oldId);
			const auto i = self->waiting.find(old);
			const auto scheduled = self->action.options.scheduled
				&& change.newId.msg > ServerMaxMsgId
				&& change.newId.msg < ScheduledMaxMsgId;
			if (i == self->waiting.end()
				|| (!IsServerMsgId(change.newId.msg) && !scheduled)) return;
			self->sent.emplace(i->second, change.newId);
			self->waiting.erase(i);
			if (self->waiting.empty()) crl::on_main([weak] {
				if (const auto self = weak.lock()) self->next();
			});
		}, lifetime);
		session->data().itemRemoved() | rpl::on_next([weak](not_null<const HistoryItem*> item) {
			if (const auto self = weak.lock()) {
				if (self->waiting.contains(item->fullId())) {
					self->stop(u"Quick reply sending stopped after an outgoing message was removed; remaining content was not sent."_q);
				}
			}
		}, lifetime);
		session->data().itemDataChanges() | rpl::on_next([weak](not_null<const HistoryItem*> item) {
			if (const auto self = weak.lock()) {
				if (self->waiting.contains(item->fullId()) && item->hasFailed()) {
					self->stop(u"Quick reply sending stopped after a native send failure; remaining content was not sent."_q);
				}
			}
		}, lifetime);
		next();
	}

	void next() {
		if (!valid()) { stop(); return; }
		if (!waiting.empty()) return;
		offset = till;
		if (offset == int(messages.size())) { stop(); return; }
		const auto error = permissions();
		if (!error.isEmpty()) { stop(error); return; }
		const auto &first = messages[offset].source;
		till = offset + 1;
		if (!first.groupRef.isEmpty()) {
			while (till < int(messages.size()) && messages[till].source.groupRef == first.groupRef) ++till;
		}
		auto send = action;
		send.clearDraft = false;
		send.generateLocal = true;
		send.options.localQuickReplyRef.clear();
		send.options.localQuickReplyOutgoingMedia = nullptr;
		send.options.invertCaption = first.media.value(u"invert_caption"_q).toBool();
		const auto price = action.history->peer->starsPerMessageChecked();
		const auto approved = std::min<int64>(
			int64(price) * (till - offset), action.options.starsApproved);
		send.options.starsApproved = int(approved);
		action.options.starsApproved -= int(approved);
		if (!first.replyToRef.isEmpty()) {
			const auto reply = sent.find(first.replyToRef);
			if (reply == sent.end()) { stop(u"The preceding quick reply message has no confirmed server ID."_q); return; }
			send.replyTo = {
				.messageId = reply->second,
				.topicRootId = send.replyTo.topicRootId,
				.monoforumPeerId = send.replyTo.monoforumPeerId,
			};
		}
		current.clear();
		scheduledBefore.clear();
		if (action.options.scheduled) {
			const auto ids = session->scheduledMessages().list(action.history).ids;
			scheduledBefore.insert(ids.begin(), ids.end());
		}
		collecting = true;
		if (messages[offset].asset) {
			auto files = std::vector<std::shared_ptr<FilePrepareResult>>();
			const auto album = till - offset > 1 ? std::make_shared<SendingAlbum>() : nullptr;
			for (auto i = offset; i != till; ++i) {
				auto &entry = messages[i];
				const auto file = std::shared_ptr<FilePrepareResult>(entry.asset, entry.asset->file.get());
				auto options = send.options;
				options.invertCaption = entry.source.media.value(u"invert_caption"_q).toBool();
				options.mediaSpoiler = entry.source.media.value(u"spoiler"_q).toBool();
				options.ttlSeconds = entry.source.media.value(u"ttl_seconds"_q).toInt();
				if (file->type != SendMediaType::Photo) {
					auto metadata = std::make_shared<LocalQuickReplyOutgoingMedia>();
					metadata->attributes = file->document.c_document().vattributes().v;
					metadata->videoTimestamp = TimeId(
						entry.source.media.value(u"video_timestamp"_q).toDouble() / 1000.);
					options.localQuickReplyOutgoingMedia = std::move(metadata);
				}
				options.starsApproved = std::min(price, send.options.starsApproved);
				send.options.starsApproved -= options.starsApproved;
				file->to = { action.history->peer->id, options, send.replyTo, {} };
				file->album = album;
				if (album) {
					album->options = options;
					album->options.starsApproved = int(approved);
					album->items.emplace_back(file->taskId);
				}
				files.push_back(file);
			}
			session->api().sendPreparedFiles(files);
		} else {
			auto &entry = messages[offset];
			auto message = MessageToSend(send);
			message.textWithTags = WithTags(entry.source.message);
			if (entry.rich && entry.serialized) {
				session->api().sendRichMessage(entry.rich, *entry.serialized, send);
			} else if (entry.input && entry.local) {
				SendPreparedMedia(std::move(message), *entry.input, *entry.local);
			} else {
				const auto webpage = entry.source.media.value(u"webpage"_q).toObject();
				message.webPage = {
					.id = webpage.isEmpty() ? WebPageId() : base::RandomValue<WebPageId>(),
					.url = webpage.value(u"url"_q).toString(),
					.forceLargeMedia = webpage.value(u"force_large_media"_q).toBool(),
					.forceSmallMedia = webpage.value(u"force_small_media"_q).toBool(),
					.invert = entry.source.media.value(u"invert_caption"_q).toBool(),
					.manual = webpage.value(u"manual"_q).toBool(),
					.removed = entry.source.media.value(u"no_webpage"_q).toBool(),
				};
				session->api().sendMessage(std::move(message));
			}
		}
		collecting = false;
		if (!valid()) return;
		if (current.size() != till - offset) {
			stop(u"The native send did not create the expected outgoing messages; remaining content was not sent and no retry was attempted."_q);
			return;
		}
		for (auto i = 0; i != int(current.size()); ++i) {
			waiting.emplace(current[i], messages[offset + i].source.id);
		}
	}
};

} // namespace

void SendLocalQuickReplyMessages(
		std::shared_ptr<const Data::LocalQuickReplyStage> stage,
		QStringList selectedMessageRefs,
		Data::ForwardOptions forwardOptions,
		SendAction action,
		Fn<bool()> prepared,
		Fn<void(QString)> failed) {
	const auto state = std::make_shared<SendState>(std::move(stage),
		std::move(selectedMessageRefs), forwardOptions, action,
		std::move(prepared), std::move(failed));
	state->keepAlive = state;
	const auto weak = std::weak_ptr<SendState>(state);
	action.history->session().account().sessionChanges(
	) | rpl::on_next([weak](Main::Session *session) {
		if (const auto state = weak.lock()) {
			if (session != state->session.get()) state->stop();
		}
	}, state->lifetime);
	const auto error = state->validate();
	if (!error.isEmpty()) { state->stop(error); return; }
	state->materialize(0);
}

} // namespace Api
