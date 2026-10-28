/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_quick_reply_media.h"

#include "api/api_text_entities.h"
#include "base/unixtime.h"
#include "base/base_file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_game.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_streaming.h"
#include "data/data_user.h"
#include "data/local_quick_reply_rich_codec.h"
#include "data/stickers/data_stickers.h"
#include "editor/photo_editor_common.h"
#include "editor/scene/scene.h"
#include "editor/scene/scene_item_sticker.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_reply_markup.h"
#include "inline_bots/inline_bot_result.h"
#include "iv/iv_rich_page.h"
#include "main/main_session.h"
#include "media/media_video_encode.h"
#include "storage/file_download.h"
#include "storage/storage_folder_archive.h"
#include "storage/streamed_file_downloader.h"
#include "ui/image/image.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>

namespace Data {
namespace {

using Error = LocalQuickReplyMediaError;
using ErrorCode = Error::Code;
using StickerSnapshots = std::map<uint64, std::vector<uint64>>;
using FrozenStickerSnapshots = std::map<
	const Ui::PreparedFileInformation*,
	std::vector<uint64>>;
constexpr auto kCopyChunk = 64 * 1024;
constexpr auto kMaxStickerReferences = 1024;
constexpr auto kDirectoryPermissions = QFileDevice::ReadOwner
	| QFileDevice::WriteOwner | QFileDevice::ExeOwner;
constexpr auto kFilePermissions = QFileDevice::ReadOwner
	| QFileDevice::WriteOwner;
std::timed_mutex EncodeMutex;

[[nodiscard]] QString NewId() {
	return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

[[nodiscard]] TextWithEntities Caption(const TextWithTags &text) {
	return { text.text, TextUtilities::ConvertTextTagsToEntities(text.tags) };
}

[[nodiscard]] QString SafeFilename(QString name, const QString &fallback) {
	name.replace('\\', '/');
	name = name.section('/', -1);
	name.remove(QChar(0));
	return (name.isEmpty() || name == u"."_q || name == u".."_q)
		? fallback
		: base::FileNameFromUserString(std::move(name));
}

[[nodiscard]] bool KnownMediaKind(const QString &kind) {
	return kind.isEmpty() || kind == u"text"_q || kind == u"photo"_q
		|| kind == u"file"_q || kind == u"audio"_q || kind == u"voice"_q
		|| kind == u"video"_q || kind == u"round_video"_q
		|| kind == u"animation"_q || kind == u"sticker"_q
		|| kind == u"contact"_q || kind == u"geo"_q || kind == u"live_geo"_q
		|| kind == u"venue"_q || kind == u"game"_q || kind == u"invoice"_q
		|| kind == u"rich_message"_q;
}

[[nodiscard]] QJsonObject StickerSetMetadata(const MTPInputStickerSet &set) {
	auto result = QJsonObject{
		{ u"constructor"_q, QString::number(set.type()) },
	};
	set.match([&](const MTPDinputStickerSetID &data) {
		result.insert(u"id"_q, QString::number(data.vid().v));
	}, [&](const MTPDinputStickerSetShortName &data) {
		result.insert(u"short_name"_q, qs(data.vshort_name()));
	}, [&](const MTPDinputStickerSetDice &data) {
		result.insert(u"emoticon"_q, qs(data.vemoticon()));
	}, [&](const auto &) {
	});
	return result;
}

[[nodiscard]] QJsonObject DocumentMetadata(not_null<DocumentData*> document) {
	const auto kind = document->isVoiceMessage() ? u"voice"_q
		: document->isVideoMessage() ? u"round_video"_q
		: document->sticker() ? u"sticker"_q
		: document->isAnimation() ? u"animation"_q
		: document->isVideoFile() ? u"video"_q
		: document->isSong() ? u"audio"_q
		: u"file"_q;
	auto result = QJsonObject{
		{ u"kind"_q, kind },
		{ u"width"_q, document->dimensions.width() },
		{ u"height"_q, document->dimensions.height() },
		{ u"duration"_q, document->duration() / 1000. },
		{ u"supports_streaming"_q, document->supportsStreaming() },
		{ u"nosound"_q, document->isSilentVideo() },
		{ u"preload_prefix_size"_q, document->videoPreloadPrefix() },
	};
	if (const auto song = document->song()) {
		result.insert(u"title"_q, song->title);
		result.insert(u"performer"_q, song->performer);
	}
	if (const auto voice = document->voice()) {
		result.insert(u"waveform"_q, QString::fromLatin1(
			documentWaveformEncode5bit(voice->waveform).toBase64()));
	}
	if (const auto video = document->video()) {
		result.insert(u"video_start_ms"_q, double(video->startTs));
		result.insert(u"video_codec"_q, video->codec);
	}
	if (const auto sticker = document->sticker()) {
		result.insert(u"alt"_q, sticker->alt);
		result.insert(u"sticker_set"_q, QJsonObject{
			{ u"id"_q, QString::number(sticker->set.id) },
			{ u"short_name"_q, sticker->set.shortName },
		});
		result.insert(u"custom_emoji"_q, sticker->setType == StickersType::Emoji);
		result.insert(u"mask"_q, sticker->setType == StickersType::Masks);
		if (sticker->setType == StickersType::Emoji) {
			result.insert(u"free"_q, !document->isPremiumEmoji());
		}
		result.insert(u"text_color"_q, document->emojiUsesTextColor());
		result.insert(u"sticker_type"_q, sticker->isLottie() ? u"tgs"_q
			: sticker->isWebm() ? u"webm"_q : u"webp"_q);
	}
	return result;
}

[[nodiscard]] QJsonObject PreparedMetadata(const FilePrepareResult &file) {
	auto result = QJsonObject{
		{ u"kind"_q, file.type == SendMediaType::Photo
			? u"photo"_q : u"file"_q },
		{ u"spoiler"_q, file.spoiler },
		{ u"force_file"_q, file.forceFile },
		{ u"video_start_ms"_q, double(file.videoCoverOffset) },
	};
	if (file.type == SendMediaType::Photo) {
		const auto i = file.photoThumbs.find('y');
		if (i != file.photoThumbs.end()) {
			result.insert(u"width"_q, i->second.image.width());
			result.insert(u"height"_q, i->second.image.height());
		}
	} else if (file.document.type() == mtpc_document) {
		auto animated = false;
		for (const auto &attribute : file.document.c_document().vattributes().v) {
			attribute.match([&](const MTPDdocumentAttributeImageSize &data) {
				result.insert(u"width"_q, data.vw().v);
				result.insert(u"height"_q, data.vh().v);
			}, [&](const MTPDdocumentAttributeVideo &data) {
				result.insert(u"kind"_q, data.is_round_message()
					? u"round_video"_q : u"video"_q);
				result.insert(u"width"_q, data.vw().v);
				result.insert(u"height"_q, data.vh().v);
				result.insert(u"duration"_q, data.vduration().v);
				result.insert(u"supports_streaming"_q, data.is_supports_streaming());
				result.insert(u"nosound"_q, data.is_nosound());
				if (const auto value = data.vpreload_prefix_size()) {
					result.insert(u"preload_prefix_size"_q, value->v);
				}
				if (const auto value = data.vvideo_start_ts()) {
					result.insert(u"video_start_ms"_q, value->v * 1000.);
				}
				if (const auto value = data.vvideo_codec()) {
					result.insert(u"video_codec"_q, qs(*value));
				}
			}, [&](const MTPDdocumentAttributeAudio &data) {
				result.insert(u"kind"_q, data.is_voice() ? u"voice"_q : u"audio"_q);
				result.insert(u"duration"_q, data.vduration().v);
				result.insert(u"title"_q, qs(data.vtitle().value_or_empty()));
				result.insert(u"performer"_q, qs(data.vperformer().value_or_empty()));
				result.insert(u"waveform"_q, QString::fromLatin1(
					data.vwaveform().value_or_empty().toBase64()));
			}, [&](const MTPDdocumentAttributeSticker &data) {
				result.insert(u"kind"_q, u"sticker"_q);
				result.insert(u"alt"_q, qs(data.valt()));
				result.insert(u"mask"_q, data.is_mask());
				result.insert(u"sticker_set"_q, StickerSetMetadata(data.vstickerset()));
				if (const auto coords = data.vmask_coords()) {
					const auto &value = coords->data();
					result.insert(u"mask_coords"_q, QJsonObject{
						{ u"n"_q, value.vn().v },
						{ u"x"_q, value.vx().v },
						{ u"y"_q, value.vy().v },
						{ u"zoom"_q, value.vzoom().v },
					});
				}
				result.insert(u"sticker_type"_q,
					file.filemime == u"application/x-tgsticker"_q ? u"tgs"_q
					: file.filemime == u"video/webm"_q ? u"webm"_q : u"webp"_q);
			}, [&](const MTPDdocumentAttributeCustomEmoji &data) {
				result.insert(u"kind"_q, u"sticker"_q);
				result.insert(u"alt"_q, qs(data.valt()));
				result.insert(u"custom_emoji"_q, true);
				result.insert(u"free"_q, data.is_free());
				result.insert(u"text_color"_q, data.is_text_color());
				result.insert(u"sticker_set"_q, StickerSetMetadata(data.vstickerset()));
			}, [&](const MTPDdocumentAttributeAnimated &) {
				animated = true;
			}, [&](const auto &) {
			});
		}
		const auto kind = result.value(u"kind"_q).toString();
		if (animated && kind != u"sticker"_q && kind != u"round_video"_q) {
			result.insert(u"kind"_q, u"animation"_q);
		}
	}
	return result;
}

[[nodiscard]] QString PreparedFilename(const FilePrepareResult &file) {
	if (file.document.type() == mtpc_document) {
		for (const auto &attribute : file.document.c_document().vattributes().v) {
			if (attribute.type() == mtpc_documentAttributeFilename) {
				return qs(attribute.c_documentAttributeFilename().vfile_name());
			}
		}
	}
	return file.filename;
}

[[nodiscard]] std::vector<uint64> SceneStickerIds(
		not_null<const Editor::Scene*> scene) {
	auto result = std::vector<uint64>();
	for (const auto &item : scene->items()) {
		if (item->isVisible() && item->type() == Editor::ItemSticker::Type) {
			result.push_back(static_cast<Editor::ItemSticker*>(
				item.get())->sticker()->id);
		}
	}
	return result;
}

[[nodiscard]] Ui::PreparedFile SnapshotNativeFile(
		const Ui::PreparedFile &source,
		FrozenStickerSnapshots &stickers,
		QString &error,
		bool alreadyModified,
		int depth = 0) {
	auto result = Ui::PreparedFile(source.path);
	if (depth > 16) {
		error = u"The selected media cover nesting is invalid."_q;
		return result;
	}
	result.id = source.id;
	result.displayName = source.displayName;
	result.caption = source.caption;
	result.content = source.content;
	result.size = source.size;
	result.preview = source.preview;
	result.shownDimensions = source.shownDimensions;
	result.originalDimensions = source.originalDimensions;
	result.type = source.type;
	result.ttlSeconds = source.ttlSeconds;
	result.spoiler = source.spoiler;
	result.sendLargePhotos = source.sendLargePhotos;
	if (source.animationJob) {
		result.animationJob = std::make_shared<::Media::Encode::Job>(
			*source.animationJob);
	}
	if (source.archive) {
		result.archive = std::make_shared<Ui::PreparedFileArchive>(
			*source.archive);
	}
	if (source.videoCover) {
		result.videoCover = std::make_unique<Ui::PreparedFile>(SnapshotNativeFile(
			*source.videoCover,
			stickers,
			error,
			alreadyModified,
			depth + 1));
	}
	if (source.information) {
		result.information = std::make_unique<Ui::PreparedFileInformation>(
			*source.information);
		if (const auto image = std::get_if<Ui::PreparedFileInformation::Image>(
				&result.information->media)) {
			if (const auto &scene = image->modifications.paint) {
				stickers.emplace(
					result.information.get(),
					SceneStickerIds(scene.get()));
				if (alreadyModified) {
					image->modifications = Editor::PhotoModifications();
				} else if (!image->data.isNull()) {
					auto paint = Editor::PhotoModifications();
					paint.paint = scene;
					image->data = Editor::ImageModified(
						std::move(image->data),
						paint);
					image->modifications.paint = nullptr;
				} else {
					error = u"The selected cover image is unavailable."_q;
				}
			}
		} else if (const auto video = std::get_if<Ui::PreparedFileInformation::Video>(
				&result.information->media)) {
			if (video->modifications.geometry.paint) {
				error = u"The native video encoder cannot preserve a painted video scene."_q;
			}
		}
	}
	return result;
}

[[nodiscard]] std::shared_ptr<Ui::PreparedBundle> SnapshotBundle(
		const Ui::PreparedBundle &source,
		FrozenStickerSnapshots &stickers,
		QString &error) {
	auto result = std::make_shared<Ui::PreparedBundle>();
	result->way = source.way;
	result->totalCount = source.totalCount;
	result->ctrlShiftEnter = source.ctrlShiftEnter;
	result->groups.reserve(source.groups.size());
	for (const auto &group : source.groups) {
		auto copy = Ui::PreparedGroup();
		copy.type = group.type;
		copy.list.error = group.list.error;
		copy.list.errorData = group.list.errorData;
		copy.list.overrideSendImagesAsPhotos
			= group.list.overrideSendImagesAsPhotos;
		copy.list.files.reserve(group.list.files.size());
		for (const auto &file : group.list.files) {
			copy.list.files.push_back(SnapshotNativeFile(
				file,
				stickers,
				error,
				true));
		}
		for (const auto &file : group.list.filesToProcess) {
			copy.list.filesToProcess.push_back(SnapshotNativeFile(
				file,
				stickers,
				error,
				true));
		}
		result->groups.push_back(std::move(copy));
	}
	return result;
}

[[nodiscard]] std::shared_ptr<FilePrepareResult> SnapshotPreparedFile(
		const std::shared_ptr<FilePrepareResult> &source,
		QString &error,
		int depth = 0) {
	if (!source) {
		return nullptr;
	} else if (depth > 16) {
		error = u"The prepared media cover nesting is invalid."_q;
		return nullptr;
	}
	auto result = std::make_shared<FilePrepareResult>(*source);
	result->transcodedTempPath.clear();
	result->album.reset();
	if (source->animationJob) {
		result->animationJob = std::make_shared<::Media::Encode::Job>(
			*source->animationJob);
	}
	if (source->videoSource) {
		result->videoSource = std::make_shared<::Media::Encode::VideoSource>(
			*source->videoSource);
	}
	if (source->archive) {
		result->archive = std::make_shared<Ui::PreparedFileArchive>(
			*source->archive);
	}
	if (source->archiveEntries) {
		result->archiveEntries = std::make_shared<Storage::ArchiveEntries>(
			*source->archiveEntries);
	}
	result->videoCover = SnapshotPreparedFile(
		source->videoCover,
		error,
		depth + 1);
	return result;
}

[[nodiscard]] QString SnapshotRequest(
		LocalQuickReplyMediaRequest &request,
		FrozenStickerSnapshots &stickers) {
	auto error = QString();
	std::visit([&](auto &source) {
		using Type = std::decay_t<decltype(source)>;
		if constexpr (std::is_same_v<Type, LocalQuickReplyFileBundle>) {
			if (source.bundle) {
				source.bundle = SnapshotBundle(*source.bundle, stickers, error);
			}
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyPrepared>) {
			source.file = SnapshotPreparedFile(source.file, error);
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyRich>) {
			if (source.page) {
				source.page = std::make_shared<Iv::RichPage>(*source.page);
			}
			for (auto &[id, media] : source.pendingMedia) {
				media.file = SnapshotPreparedFile(media.file, error);
			}
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyMediaPatch>) {
			if (const auto cover = std::get_if<std::shared_ptr<Ui::PreparedFile>>(
					&source.cover)) {
				if (*cover) {
					*cover = std::make_shared<Ui::PreparedFile>(SnapshotNativeFile(
						**cover,
						stickers,
						error,
						false));
				}
			}
		}
	}, request.source);
	return error;
}

[[nodiscard]] std::unique_ptr<FileLoadTask> FileTask(
		not_null<Main::Session*> session,
		Ui::PreparedFile &&file,
		SendMediaType type,
		bool grouped,
		bool force,
		StickerSnapshots &snapshots,
		const FrozenStickerSnapshots &frozen) {
	auto cover = file.videoCover
		? FileTask(
			session,
			std::move(*file.videoCover),
			SendMediaType::Photo,
			false,
			false,
			snapshots,
			frozen)
		: nullptr;
	auto stickers = std::vector<uint64>();
	const auto i = frozen.find(file.information.get());
	if (i != frozen.end()) {
		stickers = i->second;
	}
	auto task = std::make_unique<FileLoadTask>(FileLoadTask::Args{
		.session = session,
		.filepath = file.path,
		.content = std::move(file.content),
		.information = std::move(file.information),
		.videoCover = std::move(cover),
		.type = type,
		.to = { PeerId(), Api::SendOptions(), FullReplyTo(), MsgId() },
		.caption = std::move(file.caption),
		.spoiler = file.spoiler,
		.album = grouped ? std::make_shared<SendingAlbum>() : nullptr,
		.forceFile = force,
		.sendLargePhotos = file.sendLargePhotos,
		.animationJob = std::move(file.animationJob),
		.archive = std::move(file.archive),
		.displayName = file.displayName,
	});
	if (!stickers.empty()) {
		snapshots.emplace(task->fileid(), std::move(stickers));
	}
	return task;
}

struct FileAccess {
	explicit FileAccess(Core::FileLocation value)
	: location(std::move(value))
	, enabled(location.accessEnable()) {
	}
	~FileAccess() {
		if (enabled) {
			location.accessDisable();
		}
	}
	Core::FileLocation location;
	bool enabled = false;
};

struct AssetSource {
	QString role = u"asset"_q;
	QString filename;
	QString mime;
	QString path;
	QByteArray bytes;
	DownloadLocation location;
	FileOrigin origin;
	LocationType locationType = UnknownFileLocation;
	uint8 cacheTag = 0;
	int64 size = 0;
	DocumentData *document = nullptr;
	std::shared_ptr<FileAccess> access;
	bool owned = false;
};

struct Input {
	std::unique_ptr<FileLoadTask> task;
	StickerSnapshots stickers;
	Ui::PreparedFileInformation::Image *coverImage = nullptr;
	QSize coverTarget;
	std::shared_ptr<FilePrepareResult> prepared;
	std::vector<AssetSource> sources;
	QJsonObject media;
	TextWithEntities caption;
	QString groupRef;
	QString parentRole;
	PhotoData *photo = nullptr;
	DocumentData *document = nullptr;
	bool coverRequired = false;
	int ttlSeconds = 0;
};

struct PreparedPart {
	QJsonObject media;
	QStringList paths;
};

void ShiftAssets(QJsonObject &media, int offset) {
	for (const auto &key : { u"asset"_q, u"thumbnail_asset"_q,
		u"cover_asset"_q, u"video_asset"_q, u"good_thumbnail_asset"_q,
		u"video_thumbnail_asset"_q }) {
		if (media.contains(key)) {
			media.insert(key, media.value(key).toInt() + offset);
		}
	}
	if (media.contains(u"cover_asset_media"_q)) {
		auto cover = media.value(u"cover_asset_media"_q).toObject();
		ShiftAssets(cover, offset);
		media.insert(u"cover_asset_media"_q, cover);
	}
}

} // namespace

struct LocalQuickReplyMediaAssets::Data {
	QTemporaryDir directory;
	std::vector<LocalQuickReplyMediaAsset> files;
};

LocalQuickReplyMediaAssets::LocalQuickReplyMediaAssets()
: _data(std::make_unique<Data>()) {
}

LocalQuickReplyMediaAssets::~LocalQuickReplyMediaAssets() = default;

const std::vector<LocalQuickReplyMediaAsset> &LocalQuickReplyMediaAssets::files() const {
	return _data->files;
}

namespace details {

struct LocalQuickReplyMediaJob final
	: std::enable_shared_from_this<LocalQuickReplyMediaJob> {
	LocalQuickReplyMediaJob(
		std::weak_ptr<LocalQuickReplyMediaState> owner,
		LocalQuickReplyMediaRequest request,
		Fn<void(LocalQuickReplyMediaResult)> done);
	void start(not_null<Main::Session*> session);
	void cancel(ErrorCode code = ErrorCode::Cancelled);
	void cancelOnMain(ErrorCode code);
	void complete(LocalQuickReplyMediaResult result);
	void release();
	void fail(ErrorCode code, QString detail);
	void next();
	void download();
	void downloadFinished(bool success);
	void process();
	void assemble();
	void report(float64 value);
	bool normalize(not_null<Main::Session*> session);
	bool inlineResult(not_null<Main::Session*> session,
		const LocalQuickReplyInline &source);
	bool richPage(not_null<Main::Session*> session, LocalQuickReplyRich source);
	void addDocument(not_null<DocumentData*> document, FileOrigin origin,
		QString role = {});
	void addPhoto(not_null<PhotoData*> photo, FileOrigin origin,
		QString role = {});
	QString reservePath(const QString &filename);
	std::optional<Error> stage(PreparedPart &part, const AssetSource &source);
	std::variant<PreparedPart, Error> prepareFile(
		const std::shared_ptr<FilePrepareResult> &file,
		const StickerSnapshots &snapshots);
	std::variant<PreparedPart, Error> prepareInput(Input &input);

	std::weak_ptr<LocalQuickReplyMediaState> owner;
	LocalQuickReplyMediaRequest request;
	LocalQuickReplyMediaRequest retainedRequest;
	FrozenStickerSnapshots frozenStickers;
	QString snapshotError;
	Fn<void(LocalQuickReplyMediaResult)> done;
	std::shared_ptr<LocalQuickReplyMediaAssets> assets;
	std::vector<Input> inputs;
	std::vector<PreparedPart> parts;
	std::vector<std::shared_ptr<DocumentMedia>> documentViews;
	std::vector<std::shared_ptr<PhotoMedia>> photoViews;
	std::shared_ptr<Iv::RichPage> rich;
	QJsonObject aggregate;
	std::unique_ptr<FileLoader> loader;
	rpl::lifetime loaderLifetime;
	rpl::event_stream<float64> progress;
	std::atomic<bool> cancelled = false;
	std::atomic<ErrorCode> cancelCode = ErrorCode::Cancelled;
	bool finished = false;
	bool working = false;
	bool normalizing = false;
	bool patch = false;
	bool compound = false;
	int64 limit = kFileSizeLimit;
	int index = 0;
	int sourceIndex = 0;
	QString downloadPath;
};

struct LocalQuickReplyMediaState final
	: std::enable_shared_from_this<LocalQuickReplyMediaState> {
	explicit LocalQuickReplyMediaState(not_null<Main::Session*> session)
	: session(session) {
	}
	void pump() {
		if (busy || stopped || queue.empty()) {
			return;
		}
		busy = true;
		const auto job = queue.front();
		if (const auto strong = session.get()) {
			job->start(strong);
		} else {
			job->cancelOnMain(ErrorCode::SessionStopped);
		}
	}
	void completed(const std::shared_ptr<LocalQuickReplyMediaJob> &job) {
		const auto i = ranges::find(queue, job);
		if (i != queue.end()) {
			if (i == queue.begin()) {
				busy = false;
			}
			queue.erase(i);
		}
		const auto weak = weak_from_this();
		crl::on_main([weak] {
			if (const auto strong = weak.lock()) {
				strong->pump();
			}
		});
	}
	base::weak_ptr<Main::Session> session;
	std::deque<std::shared_ptr<LocalQuickReplyMediaJob>> queue;
	bool busy = false;
	bool stopped = false;
};

LocalQuickReplyMediaJob::LocalQuickReplyMediaJob(
	std::weak_ptr<LocalQuickReplyMediaState> owner,
	LocalQuickReplyMediaRequest request,
	Fn<void(LocalQuickReplyMediaResult)> done)
: owner(std::move(owner))
, request(request)
, retainedRequest(std::move(request))
, done(std::move(done))
, assets(std::shared_ptr<LocalQuickReplyMediaAssets>(
	new LocalQuickReplyMediaAssets())) {
}

void LocalQuickReplyMediaJob::cancel(ErrorCode code) {
	cancelCode.store(code);
	cancelled.store(true);
	const auto weak = weak_from_this();
	crl::on_main([weak, code] {
		if (const auto strong = weak.lock()) {
			strong->cancelOnMain(code);
		}
	});
}

void LocalQuickReplyMediaJob::cancelOnMain(ErrorCode code) {
	if (finished) {
		return;
	}
	cancelCode.store(code);
	cancelled.store(true);
	loaderLifetime.destroy();
	if (loader) {
		loader->cancel();
		loader = nullptr;
	}
	if (!working) {
		fail(code, QString());
	}
}

void LocalQuickReplyMediaJob::complete(LocalQuickReplyMediaResult result) {
	if (finished) {
		return;
	}
	finished = true;
	loaderLifetime.destroy();
	loader = nullptr;
	if (!normalizing) {
		release();
	}
	const auto self = shared_from_this();
	if (const auto state = owner.lock()) {
		state->completed(self);
	}
	if (auto callback = base::take(done)) {
		callback(std::move(result));
	}
}

void LocalQuickReplyMediaJob::release() {
	inputs.clear();
	parts.clear();
	documentViews.clear();
	photoViews.clear();
	rich = nullptr;
	request = {};
	retainedRequest = {};
	frozenStickers.clear();
	aggregate = {};
	assets = nullptr;
}

void LocalQuickReplyMediaJob::fail(ErrorCode code, QString detail) {
	complete(Error{ code, index, std::move(detail) });
}

void LocalQuickReplyMediaJob::report(float64 value) {
	const auto weak = weak_from_this();
	crl::on_main([weak, value] {
		if (const auto strong = weak.lock(); strong && !strong->finished) {
			strong->progress.fire_copy(std::clamp(value, 0., 1.));
		}
	});
}

QString LocalQuickReplyMediaJob::reservePath(const QString &filename) {
	const auto directory = assets->_data->directory.path() + '/' + NewId();
	if (!QDir().mkpath(directory)
		|| !QFile::setPermissions(directory, kDirectoryPermissions)) {
		return QString();
	}
	const auto path = directory + '/' + SafeFilename(filename, u"file"_q);
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| !file.setPermissions(kFilePermissions)) {
		return QString();
	}
	return path;
}

std::optional<Error> LocalQuickReplyMediaJob::stage(
		PreparedPart &part,
		const AssetSource &source) {
	if (cancelled.load()) {
		return Error{ cancelCode.load(), index, QString() };
	}
	auto input = QFile(source.path);
	const auto inMemory = !source.bytes.isEmpty();
	if (!inMemory && !input.open(QIODevice::ReadOnly)) {
		return Error{ ErrorCode::Io, index, u"Could not read the media file."_q };
	}
	const auto size = inMemory ? int64(source.bytes.size()) : input.size();
	if (size <= 0 || size > limit) {
		return Error{ size > limit ? ErrorCode::TooLarge : ErrorCode::Invalid,
			index, u"The media file is empty or too large."_q };
	}
	auto mime = source.mime;
	if (mime.startsWith(u"image/"_q)) {
		const auto sniffed = Core::MimeTypeForData(inMemory
			? source.bytes.left(kCopyChunk) : input.peek(kCopyChunk)).name();
		if (sniffed.startsWith(u"image/"_q)) {
			mime = sniffed;
		}
	}
	if (source.owned && !inMemory) {
		if (!QFile::setPermissions(source.path, kFilePermissions)) {
			return Error{ ErrorCode::Io, index, u"Could not protect the media file."_q };
		}
		part.media.insert(source.role, int(part.paths.size()));
		part.paths.push_back(source.path);
		assets->_data->files.push_back({ source.path,
			SafeFilename(source.filename, u"file"_q), mime, size });
		return std::nullopt;
	}
	const auto path = reservePath(source.filename);
	auto output = QFile(path);
	if (path.isEmpty() || !output.open(QIODevice::WriteOnly)) {
		return Error{ ErrorCode::Io, index, u"Could not prepare a private media file."_q };
	}
	auto copied = int64(0);
	while (copied < size) {
		if (cancelled.load()) {
			return Error{ cancelCode.load(), index, QString() };
		}
		const auto count = int(std::min<int64>(kCopyChunk, size - copied));
		const auto chunk = inMemory ? QByteArray()
			: input.read(count);
		const auto data = inMemory ? source.bytes.constData() + copied
			: chunk.constData();
		if ((!inMemory && chunk.size() != count)
			|| output.write(data, count) != count) {
			return Error{ ErrorCode::Io, index, u"Could not copy the media file."_q };
		}
		copied += count;
	}
	if ((!inMemory && !input.atEnd()) || !output.flush()) {
		return Error{ ErrorCode::Io, index, u"The media file changed while preparing."_q };
	}
	output.close();
	if (!QFile::setPermissions(path, kFilePermissions)) {
		return Error{ ErrorCode::Io, index, u"Could not protect the media file."_q };
	}
	part.media.insert(source.role, int(part.paths.size()));
	part.paths.push_back(path);
	assets->_data->files.push_back({
		path,
		SafeFilename(source.filename, u"file"_q),
		mime,
		size,
	});
	return std::nullopt;
}

std::variant<PreparedPart, Error> LocalQuickReplyMediaJob::prepareFile(
		const std::shared_ptr<FilePrepareResult> &file,
		const StickerSnapshots &snapshots) {
	if (!file || file->filesize <= 0 || file->filesize > limit) {
		return Error{ file && file->filesize > limit
			? ErrorCode::TooLarge : ErrorCode::Invalid,
			index, u"Could not prepare this media file."_q };
	}
	auto part = PreparedPart{ PreparedMetadata(*file) };
	auto payload = AssetSource{
		.filename = PreparedFilename(*file),
		.mime = file->filemime,
		.path = file->filepath,
		.bytes = file->content,
	};
	auto temporary = QString();
	const auto cleanup = gsl::finally([&] {
		if (!temporary.isEmpty()) {
			QFile::remove(temporary);
		}
	});
	if (file->animationJob || file->videoSource || file->archive) {
		auto lock = std::unique_lock(EncodeMutex, std::defer_lock);
		while (!lock.try_lock_for(std::chrono::milliseconds(100))) {
			if (cancelled.load()) {
				return Error{ cancelCode.load(), index, QString() };
			}
		}
		auto last = -1.;
		const auto update = [&](float64 value) {
			if (value - last >= 0.01 || value >= 1.) {
				last = value;
				report((index + value * 0.8) / std::max(int(inputs.size()), 1));
			}
			return !cancelled.load();
		};
		if (cancelled.load()) {
			return Error{ cancelCode.load(), index, QString() };
		} else if (file->animationJob) {
			auto result = ::Media::Encode::Run(
				::Media::Encode::Job(*file->animationJob), update);
			payload.bytes = std::move(result.bytes);
			payload.path = QString();
			part.media.insert(u"width"_q, result.dimensions.width());
			part.media.insert(u"height"_q, result.dimensions.height());
			part.media.insert(u"duration"_q, result.duration / 1000.);
		} else if (file->videoSource) {
			auto result = ::Media::Encode::TranscodeVideo(*file->videoSource, update);
			temporary = result.path;
			payload.path = temporary;
			payload.bytes = QByteArray();
			part.media.insert(u"width"_q, result.dimensions.width());
			part.media.insert(u"height"_q, result.dimensions.height());
			part.media.insert(u"duration"_q, result.duration / 1000.);
			part.media.insert(u"video_start_ms"_q, double(result.coverOffset));
		} else if (file->archiveEntries) {
			auto result = Storage::WriteArchive(
				Storage::ArchiveEntries(*file->archiveEntries), limit, update);
			temporary = result.path;
			if (result.status != Storage::ArchiveWriteResult::Status::Done) {
				return Error{
					result.status == Storage::ArchiveWriteResult::Status::TooLarge
						? ErrorCode::TooLarge
						: result.status == Storage::ArchiveWriteResult::Status::Cancelled
						? ErrorCode::Cancelled : ErrorCode::TransformFailed,
					index, u"Could not prepare the archive."_q };
			}
			payload.path = temporary;
			payload.bytes = QByteArray();
		} else {
			return Error{ ErrorCode::TransformFailed, index,
				u"Could not prepare the archive."_q };
		}
		if (cancelled.load()) {
			return Error{ cancelCode.load(), index, QString() };
		} else if (payload.path.isEmpty() && payload.bytes.isEmpty()) {
			return Error{ ErrorCode::TransformFailed, index,
				u"Could not apply the media modifications."_q };
		}
	}
	if (file->type == SendMediaType::Photo) {
		payload.path = QString();
		payload.bytes = QByteArray();
		const auto i = file->photoThumbs.find('y');
		if (i != file->photoThumbs.end()) {
			payload.bytes = i->second.bytes;
		}
		if (payload.bytes.isEmpty()) {
			for (const auto &bytes : file->fileparts) {
				payload.bytes.append(bytes);
			}
		}
		payload.mime = u"image/jpeg"_q;
		payload.filename = QFileInfo(payload.filename).completeBaseName()
			+ u".jpg"_q;
	}
	if (auto error = stage(part, payload)) {
		return *error;
	}
	for (const auto &thumbnail : {
		std::pair(u"thumbnail_asset"_q, file->thumbbytes),
		std::pair(u"good_thumbnail_asset"_q, file->goodThumbnailBytes) }) {
		if (thumbnail.second.isEmpty()) {
			continue;
		}
		const auto webp = thumbnail.second.startsWith("RIFF");
		if (auto error = stage(part, AssetSource{
			.role = thumbnail.first,
			.filename = webp ? u"thumbnail.webp"_q : u"thumbnail.jpg"_q,
			.mime = webp ? u"image/webp"_q : u"image/jpeg"_q,
			.bytes = thumbnail.second,
		})) {
			return *error;
		}
	}
	if (file->videoCover) {
		auto cover = prepareFile(file->videoCover, snapshots);
		if (const auto error = std::get_if<Error>(&cover)) {
			return *error;
		}
		auto prepared = std::get<PreparedPart>(std::move(cover));
		const auto offset = int(part.paths.size());
		ShiftAssets(prepared.media, offset);
		part.media.insert(u"cover_asset"_q, offset);
		part.media.insert(u"cover_asset_media"_q, prepared.media);
		part.paths.append(prepared.paths);
	}
	auto stickers = QJsonArray();
	const auto snapshot = snapshots.find(file->id);
	if (snapshot != snapshots.end()) {
		for (const auto id : snapshot->second) {
			stickers.push_back(QString::number(id));
		}
	}
	for (const auto &sticker : file->attachedStickers) {
		if (sticker.type() == mtpc_inputDocument) {
			stickers.push_back(QString::number(sticker.c_inputDocument().vid().v));
		}
	}
	if (file->animationJob) {
		for (const auto id : file->animationJob->attachedStickerIds) {
			stickers.push_back(QString::number(id));
		}
	}
	if (!stickers.isEmpty()) {
		part.media.insert(u"attached_stickers"_q, stickers);
		part.media.insert(u"sticker_association_requires_server"_q, true);
	}
	return part;
}

std::variant<PreparedPart, Error> LocalQuickReplyMediaJob::prepareInput(
		Input &input) {
	if (input.task) {
		auto lock = std::unique_lock(EncodeMutex, std::defer_lock);
		while (!lock.try_lock_for(std::chrono::milliseconds(100))) {
			if (cancelled.load()) {
				return Error{ cancelCode.load(), index, QString() };
			}
		}
		if (cancelled.load()) {
			return Error{ cancelCode.load(), index, QString() };
		}
		if (const auto image = base::take(input.coverImage)) {
			image->data = Editor::ImageModified(
				std::move(image->data), image->modifications).scaled(
					input.coverTarget,
					Qt::IgnoreAspectRatio,
					Qt::SmoothTransformation);
			image->bytes.clear();
			image->format.clear();
			image->modifications = Editor::PhotoModifications();
		}
		input.task->process();
		input.prepared = input.task->peekResult();
	}
	if (input.prepared) {
		if (input.coverRequired && !input.prepared->videoCover) {
			return Error{ ErrorCode::Invalid, index,
				u"Could not prepare the selected video cover."_q };
		}
		auto prepared = prepareFile(input.prepared, input.stickers);
		if (auto part = std::get_if<PreparedPart>(&prepared)) {
			part->media.insert(u"invert_caption"_q, request.options.invertCaption);
			part->media.insert(u"ttl_seconds"_q, input.ttlSeconds);
			if (request.options.videoTimestamp) {
				part->media.insert(u"video_timestamp"_q,
					double(*request.options.videoTimestamp));
			}
		}
		return prepared;
	}
	auto part = PreparedPart{ input.media };
	for (const auto &source : input.sources) {
		if (auto error = stage(part, source)) {
			return *error;
		}
	}
	return part;
}

void LocalQuickReplyMediaJob::process() {
	working = true;
	const auto self = shared_from_this();
	crl::async([self] {
		auto result = self->prepareInput(self->inputs[self->index]);
		crl::on_main([self, result = std::move(result)]() mutable {
			self->working = false;
			if (self->cancelled.load()) {
				self->fail(self->cancelCode.load(), QString());
			} else if (const auto error = std::get_if<Error>(&result)) {
				self->complete(*error);
			} else {
				self->parts.push_back(std::get<PreparedPart>(std::move(result)));
				++self->index;
				self->sourceIndex = 0;
				self->next();
			}
		});
	});
}

void LocalQuickReplyMediaJob::next() {
	if (finished) {
		return;
	} else if (cancelled.load()) {
		fail(cancelCode.load(), QString());
		return;
	} else if (index == int(inputs.size())) {
		assemble();
		return;
	}
	auto &input = inputs[index];
	while (sourceIndex < int(input.sources.size())) {
		const auto &source = input.sources[sourceIndex];
		if (source.bytes.isEmpty() && source.path.isEmpty()) {
			download();
			return;
		}
		++sourceIndex;
	}
	process();
}

void LocalQuickReplyMediaJob::download() {
	const auto state = owner.lock();
	const auto session = state ? state->session.get() : nullptr;
	if (!session) {
		fail(ErrorCode::SessionStopped, QString());
		return;
	}
	auto &source = inputs[index].sources[sourceIndex];
	if (!source.location.valid() || source.size > limit) {
		fail(source.size > limit ? ErrorCode::TooLarge : ErrorCode::DownloadFailed,
			u"The selected media is not available for download."_q);
		return;
	}
	downloadPath = reservePath(source.filename);
	if (downloadPath.isEmpty()) {
		fail(ErrorCode::Io, u"Could not create a private download file."_q);
		return;
	}
	if (source.document) {
		const auto storage = std::get_if<StorageFileLocation>(&source.location.data);
		const auto reader = storage
			? session->data().streaming().sharedReader(
				source.document, source.origin, true)
			: nullptr;
		if (reader) {
			loader = std::make_unique<Storage::StreamedFileDownloader>(
				session, source.document->id, storage->dcId(), source.origin,
				source.document->cacheKey(), source.document->mediaKey(),
				reader, downloadPath, source.size, source.locationType,
				LoadToFileOnly, LoadFromCloudOrLocal, false, source.cacheTag);
		}
	}
	if (!loader) {
		loader = CreateFileLoader(session, source.location, source.origin,
			downloadPath, source.size, source.size, source.locationType,
			LoadToFileOnly, LoadFromCloudOrLocal, false, source.cacheTag);
	}
	if (!loader->setTemporaryOutput(downloadPath)) {
		loader->cancel();
		loader = nullptr;
		fail(ErrorCode::DownloadFailed, u"Could not prepare the media download."_q);
		return;
	}
	const auto weak = weak_from_this();
	loader->updates() | rpl::on_next_error_done([weak] {
		if (const auto self = weak.lock()) {
			if (!self->loader) {
				return;
			} else if (self->loader->fullSize() > self->limit
				|| self->loader->currentOffset() > self->limit) {
				crl::on_main([weak] {
					if (const auto strong = weak.lock()) {
						strong->cancelOnMain(ErrorCode::TooLarge);
					}
				});
			} else {
				self->report((self->index + self->loader->currentProgress() * 0.4)
					/ std::max(int(self->inputs.size()), 1));
			}
		}
	}, [weak](FileLoader::Error) {
		crl::on_main([weak] {
			if (const auto self = weak.lock()) {
				self->downloadFinished(false);
			}
		});
	}, [weak] {
		crl::on_main([weak] {
			if (const auto self = weak.lock()) {
				self->downloadFinished(true);
			}
		});
	}, loaderLifetime);
	loader->start();
}

void LocalQuickReplyMediaJob::downloadFinished(bool success) {
	if (finished || !loader) {
		return;
	}
	const auto complete = success && loader->finished() && !loader->cancelled();
	loaderLifetime.destroy();
	loader = nullptr;
	if (!complete) {
		fail(ErrorCode::DownloadFailed, u"Could not download the selected media."_q);
		return;
	} else if (!QFile::setPermissions(downloadPath, kFilePermissions)) {
		fail(ErrorCode::Io, u"Could not protect the downloaded media."_q);
		return;
	}
	inputs[index].sources[sourceIndex].path = base::take(downloadPath);
	inputs[index].sources[sourceIndex].owned = true;
	++sourceIndex;
	next();
}

void LocalQuickReplyMediaJob::addDocument(
		not_null<DocumentData*> document,
		FileOrigin origin,
		QString role) {
	auto input = Input();
	input.media = DocumentMetadata(document);
	input.caption = request.caption;
	input.parentRole = std::move(role);
	input.document = document;
	const auto view = document->createMediaView();
	documentViews.push_back(view);
	auto source = AssetSource{
		.filename = document->filename(),
		.mime = document->mimeString(),
		.bytes = view->bytes(),
		.location = document->downloadLocation(),
		.origin = origin,
		.locationType = document->isVoiceMessage() ? AudioFileLocation
			: document->isVideoFile() ? VideoFileLocation : DocumentFileLocation,
		.cacheTag = document->cacheTag(),
		.size = document->size,
		.document = document,
	};
	if (source.bytes.isEmpty()) {
		const auto location = document->location(true);
		if (!location.isEmpty()) {
			auto access = std::make_shared<FileAccess>(location);
			if (access->enabled && QFileInfo(location.name()).isFile()) {
				source.path = location.name();
				source.access = std::move(access);
			}
		}
	}
	input.sources.push_back(std::move(source));
	if (document->hasThumbnail() && document->thumbnailLocation().valid()) {
		input.sources.push_back({
			.role = u"thumbnail_asset"_q,
			.filename = u"thumbnail.jpg"_q,
			.mime = u"image/jpeg"_q,
			.location = document->thumbnailLocation().file(),
			.origin = origin,
			.cacheTag = Data::kImageCacheTag,
			.size = document->thumbnailByteSize(),
		});
	}
	if (document->hasVideoThumbnail()
		&& document->videoThumbnailLocation().valid()) {
		input.sources.push_back({
			.role = u"video_thumbnail_asset"_q,
			.filename = u"thumbnail.mp4"_q,
			.mime = u"video/mp4"_q,
			.bytes = view->videoThumbnailContent(),
			.location = document->videoThumbnailLocation().file(),
			.origin = origin,
			.cacheTag = Data::kImageCacheTag,
			.size = document->videoThumbnailByteSize(),
		});
	}
	inputs.push_back(std::move(input));
}

void LocalQuickReplyMediaJob::addPhoto(
		not_null<PhotoData*> photo,
		FileOrigin origin,
		QString role) {
	auto input = Input();
	input.media = {
		{ u"kind"_q, u"photo"_q },
		{ u"width"_q, photo->width() },
		{ u"height"_q, photo->height() },
	};
	input.caption = request.caption;
	input.parentRole = std::move(role);
	input.photo = photo;
	const auto view = photo->createMediaView();
	photoViews.push_back(view);
	constexpr auto size = PhotoSize::Large;
	auto source = AssetSource{
		.filename = u"photo.jpg"_q,
		.mime = u"image/jpeg"_q,
		.bytes = view->imageBytes(size),
		.location = photo->location(size).file(),
		.origin = origin,
		.cacheTag = Data::kImageCacheTag,
		.size = photo->imageByteSize(size),
	};
	if (source.bytes.isEmpty()) {
		const auto location = photo->location(true);
		if (!location.isEmpty()) {
			auto access = std::make_shared<FileAccess>(location);
			if (access->enabled && QFileInfo(location.name()).isFile()) {
				source.path = location.name();
				source.access = std::move(access);
			}
		}
	}
	input.sources.push_back(std::move(source));
	if (photo->hasVideo()) {
		input.media.insert(u"video_start_ms"_q, double(photo->videoStartPosition()));
		input.sources.push_back({
			.role = u"video_asset"_q,
			.filename = u"photo.mp4"_q,
			.mime = u"video/mp4"_q,
			.bytes = view->videoContent(size),
			.location = photo->videoLocation(size).file(),
			.origin = origin,
			.cacheTag = Data::kImageCacheTag,
			.size = photo->videoByteSize(size),
		});
	}
	inputs.push_back(std::move(input));
}

bool LocalQuickReplyMediaJob::richPage(
		not_null<Main::Session*> session,
		LocalQuickReplyRich source) {
	if (!source.page) {
		fail(ErrorCode::Invalid, u"The rich message is missing."_q);
		return false;
	}
	rich = std::make_shared<Iv::RichPage>(*source.page);
	const auto resolve = [&](auto &item) {
		if (!item.photo && item.photoId) {
			item.photo = session->data().photo(item.photoId);
		}
		if constexpr (requires { item.document; item.documentId; }) {
			if (!item.document && item.documentId) {
				item.document = session->data().document(item.documentId);
			}
		}
	};
	const auto resolveBlocks = [&](const auto &self, auto &blocks) -> void {
		for (auto &block : blocks) {
			resolve(block);
			for (auto &media : block.mediaItems) {
				resolve(media);
			}
			for (auto &article : block.relatedArticles) {
				resolve(article);
			}
			for (auto &item : block.listItems) {
				self(self, item.blocks);
			}
			self(self, block.blocks);
		}
	};
	resolveBlocks(resolveBlocks, rich->blocks);
	auto collected = CollectLocalQuickReplyRichMedia(*rich);
	if (const auto error = std::get_if<QString>(&collected)) {
		fail(ErrorCode::UnsupportedNativePayload, *error);
		return false;
	}
	const auto &media = std::get<LocalQuickReplyRichMedia>(collected);
	for (const auto photo : media.photos) {
		const auto pending = source.pendingMedia.find(photo->id);
		if (pending != source.pendingMedia.end()) {
			if (!pending->second.file) {
				fail(ErrorCode::Invalid, u"A rich media attachment is missing."_q);
				return false;
			}
			auto input = Input();
			input.photo = photo;
			input.prepared = pending->second.file;
			inputs.push_back(std::move(input));
		} else {
			addPhoto(photo, FileOrigin());
		}
	}
	for (const auto document : media.documents) {
		const auto pending = source.pendingMedia.find(document->id);
		if (pending != source.pendingMedia.end()) {
			if (!pending->second.file) {
				fail(ErrorCode::Invalid, u"A rich media attachment is missing."_q);
				return false;
			}
			auto input = Input();
			input.document = document;
			input.prepared = pending->second.file;
			inputs.push_back(std::move(input));
		} else {
			addDocument(document, document->stickerOrGifOrigin());
		}
	}
	return true;
}

namespace {

[[nodiscard]] QJsonObject GeoMetadata(const MTPGeoPoint &point) {
	return point.match([](const MTPDgeoPoint &data) {
		auto result = QJsonObject{
			{ u"latitude"_q, data.vlat().v },
			{ u"longitude"_q, data.vlong().v },
		};
		if (const auto accuracy = data.vaccuracy_radius()) {
			result.insert(u"accuracy_radius"_q, accuracy->v);
		}
		return result;
	}, [](const MTPDgeoPointEmpty &) {
		return QJsonObject();
	});
}

[[nodiscard]] QString ButtonKind(HistoryMessageMarkupButton::Type type) {
	using Type = HistoryMessageMarkupButton::Type;
	switch (type) {
	case Type::Default: return u"default"_q;
	case Type::Url: return u"url"_q;
	case Type::Callback: return u"callback"_q;
	case Type::CallbackWithPassword: return u"callback_password"_q;
	case Type::RequestPhone: return u"request_phone"_q;
	case Type::RequestLocation: return u"request_location"_q;
	case Type::RequestPoll: return u"request_poll"_q;
	case Type::RequestPeer: return u"request_peer"_q;
	case Type::SwitchInline: return u"switch_inline"_q;
	case Type::SwitchInlineSame: return u"switch_inline_same"_q;
	case Type::Game: return u"game"_q;
	case Type::Buy: return u"buy"_q;
	case Type::Auth: return u"auth"_q;
	case Type::UserProfile: return u"user_profile"_q;
	case Type::WebView: return u"web_view"_q;
	case Type::SimpleWebView: return u"simple_web_view"_q;
	case Type::CopyText: return u"copy_text"_q;
	case Type::Disabled: return u"disabled"_q;
	case Type::SuggestDecline: return u"suggest_decline"_q;
	case Type::SuggestAccept: return u"suggest_accept"_q;
	case Type::SuggestChange: return u"suggest_change"_q;
	case Type::CreateBot: return u"create_bot"_q;
	case Type::kCount: break;
	}
	return QString();
}

[[nodiscard]] QJsonObject MarkupMetadata(const MTPReplyMarkup *markup) {
	const auto data = HistoryMessageMarkupData(markup);
	auto rows = QJsonArray();
	for (const auto &row : data.rows) {
		auto buttons = QJsonArray();
		for (const auto &button : row) {
			auto object = QJsonObject{
				{ u"type"_q, ButtonKind(button.type) },
				{ u"text"_q, button.text },
				{ u"data"_q, QString::fromLatin1(button.data.toBase64()) },
				{ u"forward_text"_q, button.forwardText },
				{ u"icon_id"_q, QString::number(button.visual.iconId) },
				{ u"color"_q, int(button.visual.color) },
				{ u"button_id"_q, QString::number(button.buttonId) },
				{ u"peer_types"_q, int(button.peerTypes.value()) },
			};
			using Type = HistoryMessageMarkupButton::Type;
			if (button.type == Type::RequestPeer
				&& button.data.size() == sizeof(RequestPeerQuery)) {
				auto query = RequestPeerQuery();
				std::memcpy(&query, button.data.constData(), sizeof(query));
				object.remove(u"data"_q);
				object.insert(u"request_peer"_q, QJsonObject{
					{ u"type"_q, int(query.type) },
					{ u"max_quantity"_q, query.maxQuantity },
					{ u"user_is_bot"_q, int(query.userIsBot) },
					{ u"user_is_premium"_q, int(query.userIsPremium) },
					{ u"group_is_forum"_q, int(query.groupIsForum) },
					{ u"has_username"_q, int(query.hasUsername) },
					{ u"am_creator"_q, query.amCreator },
					{ u"bot_participant"_q, query.isBotParticipant },
					{ u"my_rights"_q, double(query.myRights.value()) },
					{ u"bot_rights"_q, double(query.botRights.value()) },
				});
			}
			if (button.type == Type::CreateBot) {
				auto stream = QDataStream(button.data);
				auto name = QString();
				auto username = QString();
				stream >> name >> username;
				object.remove(u"data"_q);
				object.insert(u"suggested_name"_q, name);
				object.insert(u"suggested_username"_q, username);
			}
			if (button.type != Type::Url && button.type != Type::CopyText
				&& button.type != Type::Default && button.type != Type::Disabled) {
				object.insert(u"requires_server_action"_q, true);
			}
			buttons.push_back(std::move(object));
		}
		rows.push_back(buttons);
	}
	return {
		{ u"inline"_q, bool(data.flags & ReplyMarkupFlag::Inline) },
		{ u"resize"_q, bool(data.flags & ReplyMarkupFlag::Resize) },
		{ u"single_use"_q, bool(data.flags & ReplyMarkupFlag::SingleUse) },
		{ u"selective"_q, bool(data.flags & ReplyMarkupFlag::Selective) },
		{ u"persistent"_q, bool(data.flags & ReplyMarkupFlag::Persistent) },
		{ u"force_reply"_q, bool(data.flags & ReplyMarkupFlag::ForceReply) },
		{ u"hide"_q, bool(data.flags & ReplyMarkupFlag::None) },
		{ u"placeholder"_q, data.placeholder },
		{ u"rows"_q, rows },
	};
}

} // namespace

bool LocalQuickReplyMediaJob::inlineResult(
		not_null<Main::Session*> session,
		const LocalQuickReplyInline &source) {
	if (!source.result || !source.result->nativeMessage()) {
		fail(ErrorCode::Invalid, u"The selected inline result is unavailable."_q);
		return false;
	}
	const auto history = session->data().history(session->user()->id);
	auto item = std::unique_ptr<HistoryItem, HistoryItem::Destroyer>(
		source.result->makeMessage(history, {
			.id = session->data().nextLocalMessageId(),
			.from = session->user()->id,
			.date = base::unixtime::now(),
			.localQuickReply = true,
		}));
	request.caption = item->originalText();
	aggregate = {
		{ u"inline_bot_id"_q, QString::number(peerToUser(source.bot->id).bare) },
		{ u"inline_bot_username"_q, source.bot->username() },
	};
	const auto native = source.result->nativeMessage();
	auto valid = true;
	native->match([&](const MTPDbotInlineMessageMediaAuto &data) {
		request.options.invertCaption = data.is_invert_media();
		const auto media = item->media();
		if (const auto photo = media ? media->photo() : nullptr) {
			addPhoto(photo, FileOrigin());
		} else if (const auto document = media ? media->document() : nullptr) {
			addDocument(document, document->stickerOrGifOrigin());
		} else if (const auto game = media ? media->game() : nullptr) {
			compound = true;
			aggregate.insert(u"kind"_q, u"game"_q);
			aggregate.insert(u"title"_q, game->title);
			aggregate.insert(u"description"_q, game->description);
			aggregate.insert(u"short_name"_q, game->shortName);
			aggregate.insert(u"requires_server_replay"_q, true);
			aggregate.insert(u"replay_limitation"_q,
				u"The bot must provide a valid game before this can be sent."_q);
			if (game->photo) {
				addPhoto(game->photo, FileOrigin(), u"photo_asset"_q);
			}
			if (game->document) {
				addDocument(game->document, FileOrigin(), u"document_asset"_q);
			}
		} else {
			valid = false;
		}
	}, [&](const MTPDbotInlineMessageText &data) {
		compound = true;
		aggregate.insert(u"kind"_q, u"text"_q);
		aggregate.insert(u"no_webpage"_q, data.is_no_webpage());
		request.options.invertCaption = data.is_invert_media();
	}, [&](const MTPDbotInlineMessageMediaWebPage &data) {
		compound = true;
		aggregate.insert(u"kind"_q, u"text"_q);
		aggregate.insert(u"webpage"_q, QJsonObject{
			{ u"url"_q, qs(data.vurl()) },
			{ u"force_large_media"_q, data.is_force_large_media() },
			{ u"force_small_media"_q, data.is_force_small_media() },
			{ u"manual"_q, data.is_manual() },
			{ u"safe"_q, data.is_safe() },
		});
		request.options.invertCaption = data.is_invert_media();
	}, [&](const MTPDbotInlineMessageMediaContact &data) {
		compound = true;
		aggregate.insert(u"kind"_q, u"contact"_q);
		aggregate.insert(u"first_name"_q, qs(data.vfirst_name()));
		aggregate.insert(u"last_name"_q, qs(data.vlast_name()));
		aggregate.insert(u"phone_number"_q, qs(data.vphone_number()));
		aggregate.insert(u"vcard"_q, qs(data.vvcard()));
	}, [&](const MTPDbotInlineMessageMediaGeo &data) {
		compound = true;
		const auto point = GeoMetadata(data.vgeo());
		valid = !point.isEmpty();
		aggregate.insert(u"kind"_q, data.vperiod() ? u"live_geo"_q : u"geo"_q);
		aggregate.insert(u"point"_q, point);
		if (const auto period = data.vperiod()) {
			aggregate.insert(u"period"_q, period->v);
		}
		if (const auto heading = data.vheading()) {
			aggregate.insert(u"heading"_q, heading->v);
		}
		if (const auto radius = data.vproximity_notification_radius()) {
			aggregate.insert(u"proximity_notification_radius"_q, radius->v);
		}
	}, [&](const MTPDbotInlineMessageMediaVenue &data) {
		compound = true;
		const auto point = GeoMetadata(data.vgeo());
		valid = !point.isEmpty();
		aggregate.insert(u"kind"_q, u"venue"_q);
		aggregate.insert(u"point"_q, point);
		aggregate.insert(u"title"_q, qs(data.vtitle()));
		aggregate.insert(u"address"_q, qs(data.vaddress()));
		aggregate.insert(u"provider"_q, qs(data.vprovider()));
		aggregate.insert(u"venue_id"_q, qs(data.vvenue_id()));
		aggregate.insert(u"venue_type"_q, qs(data.vvenue_type()));
	}, [&](const MTPDbotInlineMessageMediaInvoice &data) {
		compound = true;
		aggregate.insert(u"kind"_q, u"invoice"_q);
		aggregate.insert(u"title"_q, qs(data.vtitle()));
		aggregate.insert(u"description"_q, qs(data.vdescription()));
		aggregate.insert(u"currency"_q, qs(data.vcurrency()));
		aggregate.insert(u"total_amount"_q, QString::number(data.vtotal_amount().v));
		aggregate.insert(u"test"_q, data.is_test());
		aggregate.insert(u"shipping_address_requested"_q,
			data.is_shipping_address_requested());
		aggregate.insert(u"requires_server_replay"_q, true);
		aggregate.insert(u"replay_limitation"_q,
			u"The bot must create a new invoice before payment is available."_q);
		const auto invoice = item->media() ? item->media()->invoice() : nullptr;
		if (invoice && invoice->photo) {
			addPhoto(invoice->photo, FileOrigin(), u"photo_asset"_q);
		} else if (data.vphoto()) {
			valid = false;
		}
	}, [&](const MTPDbotInlineMessageRichMessage &) {
		valid = richPage(session, { .page = item->richPage() });
	});
	native->match([&](const auto &data) {
		if (const auto markup = data.vreply_markup()) {
			aggregate.insert(u"reply_markup"_q, MarkupMetadata(markup));
		}
	});
	if (!valid && !finished) {
		fail(ErrorCode::UnsupportedNativePayload,
			u"This inline result has missing native media."_q);
	}
	return valid;
}

bool LocalQuickReplyMediaJob::normalize(not_null<Main::Session*> session) {
	return std::visit([&](auto &source) {
		using Type = std::decay_t<decltype(source)>;
		if constexpr (std::is_same_v<Type, LocalQuickReplyFileBundle>) {
			if (!source.bundle) {
				return false;
			}
			const auto compressed = source.bundle->way.sendImagesAsPhotos();
			for (auto &group : source.bundle->groups) {
				if (group.list.error != Ui::PreparedList::Error::None
					|| !group.list.filesToProcess.empty()) {
					return false;
				}
				const auto grouped = group.type != Ui::AlbumType::None
					&& !ranges::any_of(group.list.files, &Ui::PreparedFile::ttlSeconds);
				const auto groupRef = grouped ? NewId() : QString();
				for (auto &file : group.list.files) {
					auto input = Input();
					input.caption = Caption(file.caption);
					input.groupRef = groupRef;
					input.coverRequired = (file.videoCover != nullptr);
					input.ttlSeconds = int(file.ttlSeconds);
					const auto type = !compressed ? SendMediaType::File
						: !grouped || file.type == Ui::PreparedFile::Type::Photo
						? SendMediaType::Photo : SendMediaType::File;
					const auto force = !compressed
						&& file.type == Ui::PreparedFile::Type::Video;
					input.task = FileTask(
						session,
						std::move(file),
						type,
						grouped,
						force,
						input.stickers,
						frozenStickers);
					inputs.push_back(std::move(input));
				}
			}
			return !inputs.empty();
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyBytes>) {
			if (source.bytes.isEmpty()
				|| source.bytes.size() > limit
				|| (source.type != SendMediaType::Photo
					&& source.type != SendMediaType::File
					&& source.type != SendMediaType::ThemeFile)) {
				return false;
			}
			auto file = Ui::PreparedFile(QString());
			file.content = std::move(source.bytes);
			file.spoiler = request.options.spoiler;
			auto input = Input();
			input.caption = request.caption;
			input.ttlSeconds = request.options.ttlSeconds;
			input.task = FileTask(
				session,
				std::move(file),
				source.type,
				false,
				false,
				input.stickers,
				frozenStickers);
			inputs.push_back(std::move(input));
			return true;
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyRecording>) {
			if (source.bytes.isEmpty() || source.bytes.size() > limit
				|| source.duration <= 0) {
				return false;
			}
			auto input = Input();
			input.caption = request.caption;
			input.ttlSeconds = request.options.ttlSeconds;
			input.task = std::make_unique<FileLoadTask>(FileLoadTask::VoiceArgs{
				.session = session,
				.voice = std::move(source.bytes),
				.duration = source.duration,
				.waveform = std::move(source.waveform),
				.video = source.roundVideo,
				.to = { PeerId(), Api::SendOptions(), FullReplyTo(), MsgId() },
			});
			inputs.push_back(std::move(input));
			return true;
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyDocument>) {
			addDocument(source.document, source.origin);
			return true;
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyPhoto>) {
			addPhoto(source.photo, source.origin);
			return true;
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyInline>) {
			return inlineResult(session, source);
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyPrepared>) {
			if (!source.file) {
				return false;
			}
			auto input = Input();
			input.caption = request.caption.text.isEmpty()
				? Caption(source.file->caption) : request.caption;
			input.prepared = std::move(source.file);
			input.ttlSeconds = request.options.ttlSeconds;
			inputs.push_back(std::move(input));
			return true;
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyRich>) {
			return richPage(session, std::move(source));
		} else if constexpr (std::is_same_v<Type, LocalQuickReplyMediaPatch>) {
			if (!request.target.previous) {
				return false;
			}
			patch = true;
			aggregate = request.target.previous->media;
			return std::visit([&](auto &cover) {
				using Cover = std::decay_t<decltype(cover)>;
				if constexpr (std::is_same_v<Cover, LocalQuickReplyKeepCover>) {
					return true;
				} else if constexpr (std::is_same_v<Cover, LocalQuickReplyRemoveCover>) {
					aggregate.remove(u"cover_asset"_q);
					aggregate.remove(u"cover_asset_media"_q);
					return true;
				} else if constexpr (std::is_same_v<Cover, LocalQuickReplyPhoto>) {
					addPhoto(cover.photo, cover.origin, u"cover_asset"_q);
					return true;
				} else {
					if (!cover || !cover->information
						|| source.videoDimensions.isEmpty()) {
						return false;
					}
					const auto image = std::get_if<Ui::PreparedFileInformation::Image>(
						&cover->information->media);
					if (!image || image->data.isNull()) {
						return false;
					}
					const auto limit = PhotoSideLimit(true);
					const auto dimensions = source.videoDimensions;
					const auto target = dimensions.width() > limit
						|| dimensions.height() > limit
						? dimensions.scaled(limit, limit, Qt::KeepAspectRatio)
						: dimensions;
					cover->path.clear();
					cover->content.clear();
					auto input = Input();
					input.parentRole = u"cover_asset"_q;
					input.coverImage = image;
					input.coverTarget = target;
					input.task = FileTask(
						session,
						std::move(*cover),
						SendMediaType::Photo,
						false,
						false,
						input.stickers,
						frozenStickers);
					inputs.push_back(std::move(input));
					return true;
				}
			}, source.cover);
		}
		return false;
	}, request.source);
}

void LocalQuickReplyMediaJob::start(not_null<Main::Session*> session) {
	if (finished) {
		return;
	} else if (cancelled.load()) {
		fail(cancelCode.load(), QString());
		return;
	} else if (!snapshotError.isEmpty()) {
		fail(ErrorCode::UnsupportedNativePayload, snapshotError);
		return;
	}
	limit = session->premium() ? kFileSizePremiumLimit : kFileSizeLimit;
	if (QUuid(request.target.replyRef).isNull()
		|| request.target.firstPosition < 0
		|| (request.target.previous
			&& (!request.target.previous->sourcePaths.empty()
				|| request.target.previous->replyRef != request.target.replyRef))) {
		fail(ErrorCode::Invalid, u"The local reply target is invalid."_q);
		return;
	} else if (!assets->_data->directory.isValid()
		|| !QFile::setPermissions(assets->_data->directory.path(), kDirectoryPermissions)) {
		fail(ErrorCode::Io, u"Could not create private media storage."_q);
		return;
	}
	if (request.target.previous) {
		const auto kind = request.target.previous->media.value(u"kind"_q).toString();
		const auto edit = std::get_if<LocalQuickReplyMediaPatch>(&request.source);
		const auto keep = edit
			&& std::holds_alternative<LocalQuickReplyKeepCover>(edit->cover);
		if (!KnownMediaKind(kind) && !keep) {
			fail(ErrorCode::UnsupportedNativePayload,
				u"This imported media must be preserved by a newer editor."_q);
			return;
		} else if (edit && !keep && kind != u"video"_q) {
			fail(ErrorCode::Invalid, u"Only videos can have a video cover."_q);
			return;
		}
	}
	normalizing = true;
	const auto normalized = normalize(session);
	normalizing = false;
	if (finished) {
		release();
		return;
	} else if (!normalized) {
		if (!finished) {
			fail(ErrorCode::Invalid, u"The media request is invalid."_q);
		}
		return;
	}
	request.source = LocalQuickReplyFileBundle();
	if (request.target.previous && !patch && !compound && !rich && inputs.size() != 1) {
		fail(ErrorCode::Invalid, u"A replacement must contain one message."_q);
		return;
	}
	for (auto &input : inputs) {
		if (!input.task && !input.prepared) {
			input.media.insert(u"spoiler"_q, request.options.spoiler);
			input.media.insert(u"invert_caption"_q, request.options.invertCaption);
			input.media.insert(u"ttl_seconds"_q, request.options.ttlSeconds);
			if (request.options.videoTimestamp) {
				input.media.insert(u"video_timestamp"_q,
					double(*request.options.videoTimestamp));
			}
		}
	}
	next();
}

namespace {

[[nodiscard]] bool ValidStickerSetShortName(const QString &name) {
	return !name.isEmpty()
		&& name.size() <= 64
		&& ranges::all_of(name, [](QChar ch) {
			const auto value = ch.unicode();
			return (value >= 'a' && value <= 'z')
				|| (value >= 'A' && value <= 'Z')
				|| (value >= '0' && value <= '9')
				|| value == '_';
		});
}

void FillStickerSetShortName(
		not_null<Main::Session*> session,
		QJsonObject &metadata) {
	if (!metadata.value(u"short_name"_q).toString().isEmpty()) {
		return;
	}
	const auto id = metadata.value(u"id"_q).toString().toULongLong();
	const auto &sets = session->data().stickers().sets();
	const auto i = sets.find(id);
	if (id && i != sets.end()
		&& !(i->second->flags & StickersSetFlag::Special)
		&& ValidStickerSetShortName(i->second->shortName)) {
		metadata.insert(u"short_name"_q, i->second->shortName);
	}
}

[[nodiscard]] QJsonObject AttachedStickerSetHint(
		not_null<Main::Session*> session,
		uint64 id) {
	auto result = QJsonObject();
	const auto sticker = session->data().document(id)->sticker();
	if (sticker) {
		if (sticker->set.id) {
			result.insert(u"id"_q, QString::number(sticker->set.id));
		}
		if (ValidStickerSetShortName(sticker->set.shortName)) {
			result.insert(u"short_name"_q, sticker->set.shortName);
		}
		FillStickerSetShortName(session, result);
	}
	return result;
}

[[nodiscard]] bool CaptureStickerHints(
		not_null<Main::Session*> session,
		QJsonObject &media,
		std::map<uint64, QJsonObject> &known) {
	if (media.contains(u"sticker_set"_q)) {
		auto set = media.value(u"sticker_set"_q).toObject();
		FillStickerSetShortName(session, set);
		media.insert(u"sticker_set"_q, set);
	}
	auto hints = QJsonObject();
	for (const auto &value : media.value(u"attached_stickers"_q).toArray()) {
		const auto key = value.toString();
		const auto id = key.toULongLong();
		if (!id || QString::number(id) != key) {
			return false;
		}
		auto i = known.find(id);
		if (i == known.end()) {
			if (known.size() >= kMaxStickerReferences) {
				return false;
			}
			i = known.emplace(id, AttachedStickerSetHint(session, id)).first;
		}
		if (!i->second.isEmpty()) {
			hints.insert(key, i->second);
		}
	}
	if (!hints.isEmpty()) {
		media.insert(u"attached_sticker_sets"_q, hints);
	}
	if (media.contains(u"cover_asset_media"_q)) {
		auto cover = media.value(u"cover_asset_media"_q).toObject();
		if (!CaptureStickerHints(session, cover, known)) {
			return false;
		}
		media.insert(u"cover_asset_media"_q, cover);
	}
	return true;
}

[[nodiscard]] QJsonObject ReplaceKnownMedia(
		QJsonObject retained,
		const QJsonObject &replacement) {
	for (const auto &key : {
		u"kind"_q, u"width"_q, u"height"_q, u"duration"_q, u"title"_q,
		u"performer"_q, u"waveform"_q, u"alt"_q, u"spoiler"_q,
		u"invert_caption"_q, u"force_file"_q, u"supports_streaming"_q,
		u"nosound"_q, u"video_start_ms"_q, u"video_timestamp"_q,
		u"ttl_seconds"_q, u"sticker_type"_q, u"mask"_q,
		u"asset"_q, u"thumbnail_asset"_q, u"good_thumbnail_asset"_q,
		u"cover_asset"_q, u"video_asset"_q, u"video_thumbnail_asset"_q,
		u"rich"_q, u"rich_assets"_q, u"attached_stickers"_q,
		u"inline_bot_id"_q, u"inline_bot_username"_q, u"reply_markup"_q,
		u"no_webpage"_q, u"webpage"_q, u"requires_server_replay"_q,
		u"replay_limitation"_q, u"first_name"_q, u"last_name"_q,
		u"phone_number"_q, u"vcard"_q, u"point"_q, u"period"_q,
		u"heading"_q, u"proximity_notification_radius"_q, u"address"_q,
		u"provider"_q, u"venue_id"_q, u"venue_type"_q, u"description"_q,
		u"currency"_q, u"total_amount"_q, u"test"_q, u"short_name"_q,
		u"shipping_address_requested"_q, u"photo_asset"_q, u"photo_asset_media"_q,
		u"document_asset"_q, u"document_asset_media"_q, u"cover_asset_media"_q,
		u"video_codec"_q, u"preload_prefix_size"_q, u"sticker_set"_q,
		u"mask_coords"_q, u"custom_emoji"_q, u"free"_q, u"text_color"_q,
		u"sticker_association_requires_server"_q,
		u"attached_sticker_sets"_q,
	}) {
		retained.remove(key);
	}
	for (auto i = replacement.begin(); i != replacement.end(); ++i) {
		retained.insert(i.key(), i.value());
	}
	return retained;
}

} // namespace

void LocalQuickReplyMediaJob::assemble() {
	if (cancelled.load()) {
		fail(cancelCode.load(), QString());
		return;
	}
	const auto state = owner.lock();
	const auto session = state ? state->session.get() : nullptr;
	if (!session) {
		fail(ErrorCode::SessionStopped, QString());
		return;
	}
	auto knownStickers = std::map<uint64, QJsonObject>();
	for (auto &part : parts) {
		if (!CaptureStickerHints(session, part.media, knownStickers)) {
			fail(ErrorCode::UnsupportedNativePayload,
				u"The selected media has invalid or too many attached sticker references."_q);
			return;
		}
	}
	const auto makeMessage = [&](int position) {
		auto result = request.target.previous.value_or(LocalQuickReplyMessage());
		if (!request.target.previous) {
			result.id = NewId();
			result.replyRef = request.target.replyRef;
			result.replyToRef = request.target.replyToRef;
			result.position = request.target.firstPosition + position;
		}
		return result;
	};
	auto messages = std::vector<LocalQuickReplyMessage>();
	if (rich || compound || patch) {
		auto message = makeMessage(0);
		message.message = request.caption;
		auto media = aggregate;
		auto photos = std::map<PhotoData*, int>();
		auto documents = std::map<DocumentData*, int>();
		auto richAssets = QJsonArray();
		for (auto i = 0; i != int(parts.size()); ++i) {
			auto &part = parts[i];
			const auto offset = int(message.attachments.size()
				+ message.sourcePaths.size());
			ShiftAssets(part.media, offset);
			message.sourcePaths.append(part.paths);
			if (inputs[i].photo) {
				photos.emplace(inputs[i].photo, part.media.value(u"asset"_q).toInt());
			}
			if (inputs[i].document) {
				documents.emplace(inputs[i].document,
					part.media.value(u"asset"_q).toInt());
			}
			if (rich) {
				richAssets.push_back(QJsonObject{
					{ u"asset"_q, part.media.value(u"asset"_q) },
					{ u"media"_q, part.media },
				});
			} else if (!inputs[i].parentRole.isEmpty()) {
				media.insert(inputs[i].parentRole, part.media.value(u"asset"_q));
				media.insert(inputs[i].parentRole + u"_media"_q, part.media);
			}
		}
		if (rich) {
			auto encoded = EncodeLocalQuickReplyRich(*rich,
				[&](PhotoData *photo) -> std::optional<int> {
					const auto i = photos.find(photo);
					return i != photos.end()
						? std::make_optional(i->second) : std::nullopt;
				}, [&](DocumentData *document) -> std::optional<int> {
					const auto i = documents.find(document);
					return i != documents.end()
						? std::make_optional(i->second) : std::nullopt;
				});
			if (const auto error = std::get_if<QString>(&encoded)) {
				fail(ErrorCode::UnsupportedNativePayload, *error);
				return;
			}
			media.insert(u"kind"_q, u"rich_message"_q);
			media.insert(u"rich"_q, std::get<QJsonObject>(std::move(encoded)));
			media.insert(u"rich_assets"_q, richAssets);
		}
		media.insert(u"spoiler"_q, request.options.spoiler);
		media.insert(u"invert_caption"_q, request.options.invertCaption);
		if (request.options.videoTimestamp) {
			media.insert(u"video_timestamp"_q, double(*request.options.videoTimestamp));
		}
		message.media = patch ? std::move(media)
			: ReplaceKnownMedia(std::move(message.media), media);
		messages.push_back(std::move(message));
	} else {
		for (auto i = 0; i != int(parts.size()); ++i) {
			auto message = makeMessage(i);
			message.message = inputs[i].caption;
			if (!request.target.previous) {
				message.groupRef = inputs[i].groupRef;
			}
			auto media = parts[i].media;
			ShiftAssets(media, int(message.attachments.size()));
			for (auto j = aggregate.begin(); j != aggregate.end(); ++j) {
				media.insert(j.key(), j.value());
			}
			message.media = ReplaceKnownMedia(std::move(message.media), media);
			message.sourcePaths = parts[i].paths;
			messages.push_back(std::move(message));
		}
	}
	progress.fire_copy(1.);
	if (cancelled.load()) {
		fail(cancelCode.load(), QString());
	} else {
		complete(PreparedLocalQuickReplyMedia{ std::move(messages), assets });
	}
}

} // namespace details

LocalQuickReplyMediaOperation::LocalQuickReplyMediaOperation(
	std::shared_ptr<details::LocalQuickReplyMediaJob> job)
: _job(std::move(job)) {
}

LocalQuickReplyMediaOperation::~LocalQuickReplyMediaOperation() {
	cancel();
}

void LocalQuickReplyMediaOperation::cancel() {
	_job->cancel();
}

rpl::producer<float64> LocalQuickReplyMediaOperation::progress() const {
	return _job->progress.events();
}

LocalQuickReplyMedia::LocalQuickReplyMedia(not_null<Main::Session*> session)
: _state(std::make_shared<details::LocalQuickReplyMediaState>(session)) {
}

LocalQuickReplyMedia::~LocalQuickReplyMedia() {
	stop();
}

std::unique_ptr<LocalQuickReplyMediaOperation> LocalQuickReplyMedia::prepare(
		const LocalQuickReplyMediaRequest &request,
		Fn<void(LocalQuickReplyMediaResult)> done) {
	const auto job = std::make_shared<details::LocalQuickReplyMediaJob>(
		_state, request, std::move(done));
	auto operation = std::unique_ptr<LocalQuickReplyMediaOperation>(
		new LocalQuickReplyMediaOperation(job));
	if (_state->stopped) {
		job->cancel(ErrorCode::SessionStopped);
	} else {
		job->snapshotError = SnapshotRequest(job->request, job->frozenStickers);
		_state->queue.push_back(job);
		const auto weak = std::weak_ptr(_state);
		crl::on_main([weak] {
			if (const auto strong = weak.lock()) {
				strong->pump();
			}
		});
	}
	return operation;
}

void LocalQuickReplyMedia::stop() {
	if (_state->stopped) {
		return;
	}
	_state->stopped = true;
	const auto jobs = _state->queue;
	for (const auto &job : jobs) {
		job->cancelOnMain(ErrorCode::SessionStopped);
	}
}

} // namespace Data
