/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_file_origin.h"
#include "data/local_quick_replies.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"

#include <map>
#include <variant>

class DocumentData;
class PhotoData;
class UserData;

namespace InlineBots {
class Result;
} // namespace InlineBots

namespace Iv {
struct RichPage;
} // namespace Iv

namespace Data {
namespace details {
struct LocalQuickReplyMediaJob;
struct LocalQuickReplyMediaState;
} // namespace details

struct LocalQuickReplyMediaAsset {
	QString path;
	QString filename;
	QString mime;
	int64 size = 0;
};

class LocalQuickReplyMediaAssets final {
public:
	~LocalQuickReplyMediaAssets();
	[[nodiscard]] const std::vector<LocalQuickReplyMediaAsset> &files() const;

private:
	friend struct details::LocalQuickReplyMediaJob;
	LocalQuickReplyMediaAssets();
	struct Data;
	std::unique_ptr<Data> _data;

};

struct LocalQuickReplyMediaTarget {
	QString replyRef;
	QString replyToRef;
	int firstPosition = 0;
	std::optional<LocalQuickReplyMessage> previous;
};

struct LocalQuickReplyMediaOptions {
	bool spoiler = false;
	bool invertCaption = false;
	int ttlSeconds = 0;
	std::optional<crl::time> videoTimestamp;
};

struct LocalQuickReplyFileBundle {
	std::shared_ptr<Ui::PreparedBundle> bundle;
};

struct LocalQuickReplyBytes {
	QByteArray bytes;
	SendMediaType type = SendMediaType::File;
};

struct LocalQuickReplyRecording {
	QByteArray bytes;
	VoiceWaveform waveform;
	crl::time duration = 0;
	bool roundVideo = false;
};

struct LocalQuickReplyDocument {
	not_null<DocumentData*> document;
	FileOrigin origin;
};

struct LocalQuickReplyPhoto {
	not_null<PhotoData*> photo;
	FileOrigin origin;
};

struct LocalQuickReplyInline {
	std::shared_ptr<InlineBots::Result> result;
	not_null<UserData*> bot;
};

struct LocalQuickReplyPrepared {
	std::shared_ptr<FilePrepareResult> file;
};

struct LocalQuickReplyRich {
	std::shared_ptr<const Iv::RichPage> page;
	std::map<uint64, LocalQuickReplyPrepared> pendingMedia;
};

struct LocalQuickReplyKeepCover {};
struct LocalQuickReplyRemoveCover {};

using LocalQuickReplyCover = std::variant<
	LocalQuickReplyKeepCover,
	LocalQuickReplyRemoveCover,
	LocalQuickReplyPhoto,
	std::shared_ptr<Ui::PreparedFile>>;

struct LocalQuickReplyMediaPatch {
	LocalQuickReplyCover cover;
	QSize videoDimensions;
};

struct LocalQuickReplyMediaRequest {
	LocalQuickReplyMediaTarget target;
	TextWithEntities caption;
	LocalQuickReplyMediaOptions options;
	std::variant<
		LocalQuickReplyFileBundle,
		LocalQuickReplyBytes,
		LocalQuickReplyRecording,
		LocalQuickReplyDocument,
		LocalQuickReplyPhoto,
		LocalQuickReplyInline,
		LocalQuickReplyPrepared,
		LocalQuickReplyRich,
		LocalQuickReplyMediaPatch> source;
};

struct PreparedLocalQuickReplyMedia {
	std::vector<LocalQuickReplyMessage> messages;
	std::shared_ptr<const LocalQuickReplyMediaAssets> assets;
};

struct LocalQuickReplyMediaError {
	enum class Code {
		Cancelled,
		Invalid,
		TooLarge,
		DownloadFailed,
		TransformFailed,
		Io,
		UnsupportedNativePayload,
		SessionStopped,
	};
	Code code = Code::Invalid;
	int sourceIndex = 0;
	QString detail;
};

using LocalQuickReplyMediaResult = std::variant<
	PreparedLocalQuickReplyMedia,
	LocalQuickReplyMediaError>;

class LocalQuickReplyMediaOperation final {
public:
	~LocalQuickReplyMediaOperation();
	void cancel();
	[[nodiscard]] rpl::producer<float64> progress() const;

private:
	friend class LocalQuickReplyMedia;
	explicit LocalQuickReplyMediaOperation(
		std::shared_ptr<details::LocalQuickReplyMediaJob> job);
	const std::shared_ptr<details::LocalQuickReplyMediaJob> _job;

};

/*
Requests and completion run on the session main thread; preparation is queued.
Bundles come from the native composer after Storage::ApplyModifications.
prepare() snapshots mutable bundle, prepared-file and cover inputs synchronously
without consuming the request. Retain the original request to retry or reopen
the selected native files after failure; bytes and images use Qt value copies.
The operation retains original typed owners until its background work finishes,
including on cancellation, so temporary source assets remain valid.
Destroying the operation cancels outstanding work; completion may report that
cancellation and must be guarded by the caller's UI lifetime. Successful results
own their temporary assets through the shared lease, which persistence jobs and
asset readers must retain independently of the operation and its completion.
*/
class LocalQuickReplyMedia final {
public:
	explicit LocalQuickReplyMedia(not_null<Main::Session*> session);
	~LocalQuickReplyMedia();

	[[nodiscard]] std::unique_ptr<LocalQuickReplyMediaOperation> prepare(
		const LocalQuickReplyMediaRequest &request,
		Fn<void(LocalQuickReplyMediaResult)> done);
	void stop();

private:
	const std::shared_ptr<details::LocalQuickReplyMediaState> _state;

};

} // namespace Data
