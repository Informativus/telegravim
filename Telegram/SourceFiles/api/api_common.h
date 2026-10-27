/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_drafts.h"

#include <QtCore/QString>

class History;
class PhotoData;

namespace Data {
class Thread;
} // namespace Data

namespace Ui {
struct PreparedFile;
} // namespace Ui

namespace Api {

inline constexpr auto kScheduledUntilOnlineTimestamp = TimeId(0x7FFFFFFE);

[[nodiscard]] MTPSuggestedPost SuggestToMTP(SuggestOptions suggest);

struct LocalQuickReplyOutgoingMedia {
	QVector<MTPDocumentAttribute> attributes;
	TimeId videoTimestamp = 0;
};

struct SendOptions {
	uint64 price = 0;
	PeerData *sendAs = nullptr;
	TimeId scheduled = 0;
	TimeId scheduleRepeatPeriod = 0;
	BusinessShortcutId shortcutId = 0;
	EffectId effectId = 0;
	QByteArray stakeSeedHash;
	int64 stakeNanoTon = 0;
	int starsApproved = 0;
	bool silent = false;
	bool handleSupportSwitch = false;
	bool invertCaption = false;
	bool hideViaBot = false;
	bool mediaSpoiler = false;
	bool welcomeTemplate = false;
	crl::time ttlSeconds = 0;
	SuggestOptions suggest;
	QString localQuickReplyRef;
	std::shared_ptr<const LocalQuickReplyOutgoingMedia> localQuickReplyOutgoingMedia;

	friend inline bool operator==(
		const SendOptions &,
		const SendOptions &) = default;
};

[[nodiscard]] inline bool IsLocalQuickReplyContext(
		const SendOptions &options) {
	return !options.localQuickReplyRef.isEmpty();
}

[[nodiscard]] SendOptions DefaultSendWhenOnlineOptions();

enum class SendType {
	Normal,
	Scheduled,
	ScheduledToUser, // For "Send when online".
};

struct SendAction {
	explicit SendAction(
		not_null<Data::Thread*> thread,
		SendOptions options = SendOptions());

	not_null<History*> history;
	SendOptions options;
	FullReplyTo replyTo;
	bool clearDraft = true;
	bool generateLocal = true;
	MsgId replaceMediaOf = 0;

	[[nodiscard]] MTPInputReplyTo mtpReplyTo() const;

	friend inline bool operator==(
		const SendAction &,
		const SendAction &) = default;
};

struct MessageToSend {
	explicit MessageToSend(SendAction action) : action(action) {
	}

	SendAction action;
	TextWithTags textWithTags;
	Data::WebPageDraft webPage;
	bool e2eControl = false;
};

struct RemoteFileInfo {
	MTPInputFile file;
	std::optional<MTPInputFile> thumb;
	std::optional<MTPInputPhoto> videoCover;
	std::vector<MTPInputDocument> attachedStickers;
	bool forceFile = false;

};

struct VideoCoverEdit {
	PhotoData *photo = nullptr;
	std::shared_ptr<Ui::PreparedFile> localPrepared;
	bool cleared = false;
	Fn<void(Fn<void(PhotoData*)> done)> refresh;

	[[nodiscard]] explicit operator bool() const {
		return (photo != nullptr) || localPrepared || cleared;
	}
};

} // namespace Api
