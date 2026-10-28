/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_quick_replies.h"

#include "data/local_quick_reply_media.h"
#include "data/local_quick_reply_rich_codec.h"
#include "main/main_session.h"
#include "storage/local_quick_replies_format.h"
#include "storage/storage_domain.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QMimeDatabase>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>

namespace Data {
namespace {

using namespace Storage::LocalData;
namespace Format = Storage::LocalData::QuickReplies;

constexpr auto kMaterializedCacheLimit = 128;

const auto kModule = u"quick_replies"_q;
const auto kEntityTypes = std::vector<std::pair<QString, EntityType>>{
	{ u"bold"_q, EntityType::Bold },
	{ u"italic"_q, EntityType::Italic },
	{ u"underline"_q, EntityType::Underline },
	{ u"strike"_q, EntityType::StrikeOut },
	{ u"code"_q, EntityType::Code },
	{ u"pre"_q, EntityType::Pre },
	{ u"blockquote"_q, EntityType::Blockquote },
	{ u"spoiler"_q, EntityType::Spoiler },
	{ u"link"_q, EntityType::CustomUrl },
	{ u"mention"_q, EntityType::MentionName },
	{ u"custom_emoji"_q, EntityType::CustomEmoji },
};

[[nodiscard]] QJsonObject Encode(const LocalQuickReply &reply) {
	auto result = reply.retained;
	auto entities = QJsonArray();
	const auto original = result.value(u"entities"_q).toArray();
	for (const auto &entity : reply.message.entities) {
		for (const auto &[name, type] : kEntityTypes) {
			if (entity.type() != type) continue;
			auto value = QJsonObject();
			for (const auto &retained : original) {
				const auto object = retained.toObject();
				if (object.value(u"type"_q).toString() == name
					&& object.value(u"offset"_q).toInt() == entity.offset()
					&& object.value(u"length"_q).toInt() == entity.length()
					&& object.value(u"data"_q).toString() == entity.data()) {
					value = object;
					break;
				}
			}
			value.insert(u"type"_q, name);
			value.insert(u"offset"_q, entity.offset());
			value.insert(u"length"_q, entity.length());
			value.insert(u"data"_q, entity.data());
			entities.push_back(value);
			break;
		}
	}
	if (reply.message.text == result.value(u"text"_q).toString()) {
		for (const auto &entry : original) {
			const auto name = entry.toObject().value(u"type"_q).toString();
			if (ranges::none_of(kEntityTypes, [&](const auto &type) {
				return type.first == name;
			})) entities.push_back(entry);
		}
	}
	result.insert(u"id"_q, reply.id);
	result.insert(u"kind"_q, u"reply"_q);
	result.insert(u"deleted"_q, false);
	result.insert(u"title"_q, reply.title);
	result.insert(u"shortcut"_q, reply.shortcut);
	result.insert(u"category"_q, reply.category);
	result.insert(u"text"_q, reply.message.text);
	result.insert(u"entities"_q, entities);
	result.insert(u"variables"_q, reply.variables);
	result.insert(u"attachments"_q, reply.attachments);
	result.insert(u"position"_q, reply.position);
	return result;
}

[[nodiscard]] QJsonObject MergeDraftFields(
		QJsonObject previous,
		const QJsonObject &replacement) {
	for (auto i = replacement.begin(); i != replacement.end(); ++i) {
		if (i.value().isObject() && previous.value(i.key()).isObject()) {
			previous.insert(i.key(), MergeDraftFields(
				previous.value(i.key()).toObject(), i.value().toObject()));
		} else {
			previous.insert(i.key(), i.value());
		}
	}
	return previous;
}

[[nodiscard]] QJsonObject MergeEditorDraftFields(
		const QJsonObject &previous,
		const QJsonObject &replacement) {
	auto result = MergeDraftFields(previous, replacement);
	for (const auto &key : {
		u"source_version_vector"_q,
		u"source_legacy_parent_version_vector"_q,
	}) {
		const auto value = replacement.value(key);
		if (value.isUndefined() || value.isNull()) result.remove(key);
		else result.insert(key, value);
	}
	return result;
}

[[nodiscard]] TextWithEntities DecodeContent(const QJsonObject &record) {
	auto message = TextWithEntities{ record.value(u"text"_q).toString() };
	for (const auto &entry : record.value(u"entities"_q).toArray()) {
		const auto entity = entry.toObject();
		for (const auto &[name, type] : kEntityTypes) {
			if (entity.value(u"type"_q).toString() == name) {
				message.entities.push_back(EntityInText(type,
					entity.value(u"offset"_q).toInt(),
					entity.value(u"length"_q).toInt(),
					entity.value(u"data"_q).toString()));
				break;
			}
		}
	}
	return message;
}

[[nodiscard]] QString LegacyMessageId(const QString &replyRef) {
	return QUuid::createUuidV5(QUuid(replyRef), QByteArray("first-message"))
		.toString(QUuid::WithoutBraces);
}

[[nodiscard]] std::vector<LocalQuickReplyMessage> LegacyMessages(
		const LocalQuickReply &reply) {
	auto result = std::vector<LocalQuickReplyMessage>();
	if (reply.message.text.isEmpty() && reply.attachments.isEmpty()) return result;
	for (auto index = 0; index != std::max(1, int(reply.attachments.size())); ++index) {
		const auto first = (index == 0);
		const auto id = first ? LegacyMessageId(reply.id) : QUuid::createUuidV5(
			QUuid(reply.id), QByteArray("attachment-message-") + QByteArray::number(index))
			.toString(QUuid::WithoutBraces);
		const auto attachments = reply.attachments.isEmpty()
			? QJsonArray() : QJsonArray{ reply.attachments.at(index) };
		auto media = QJsonObject{ { u"kind"_q,
			attachments.isEmpty() ? u"text"_q : u"file"_q } };
		if (!attachments.isEmpty()) media.insert(u"asset"_q, 0);
		result.push_back(LocalQuickReplyMessage{
			.id = id,
			.replyRef = reply.id,
			.message = first ? reply.message : TextWithEntities(),
			.attachments = attachments,
			.media = media,
			.position = index,
			.retained = QJsonObject{
				{ u"text"_q, first ? reply.message.text : QString() },
				{ u"entities"_q, first ? reply.retained.value(u"entities"_q).toArray() : QJsonArray() },
				{ u"attachments"_q, attachments },
			},
			.legacyParentRevision = reply.retained.value(u"version_vector"_q).toObject(),
		});
	}
	return result;
}

[[nodiscard]] QJsonObject EncodeMessage(const LocalQuickReplyMessage &message) {
	auto temporary = LocalQuickReply();
	temporary.retained = message.retained;
	temporary.message = message.message;
	temporary.attachments = message.attachments;
	const auto content = Encode(temporary);
	auto result = message.retained;
	for (const auto &key : { u"text"_q, u"entities"_q, u"attachments"_q }) {
		result.insert(key, content.value(key));
	}
	result.insert(u"id"_q, message.id);
	result.insert(u"kind"_q, u"message"_q);
	result.insert(u"deleted"_q, false);
	result.insert(u"reply_ref"_q, message.replyRef);
	result.insert(u"position"_q, message.position);
	result.insert(u"media"_q, message.media);
	result.insert(u"group_ref"_q, message.groupRef.isEmpty()
		? QJsonValue(QJsonValue::Null) : QJsonValue(message.groupRef));
	result.insert(u"reply_to_ref"_q, message.replyToRef.isEmpty()
		? QJsonValue(QJsonValue::Null) : QJsonValue(message.replyToRef));
	return result;
}

[[nodiscard]] bool KnownFields(const QJsonObject &object, const QStringList &keys) {
	for (auto i = object.begin(); i != object.end(); ++i) {
		if (!keys.contains(i.key())) return false;
	}
	return true;
}

[[nodiscard]] QString MediaEditError(const QJsonObject &media, int depth = 0) {
	if (depth > 16) return u"The media metadata is too deeply nested."_q;
	const auto knownKinds = QStringList{
		u"text"_q, u"photo"_q, u"file"_q, u"video"_q, u"round_video"_q,
		u"voice"_q, u"audio"_q, u"sticker"_q, u"animation"_q, u"contact"_q,
		u"geo"_q, u"live_geo"_q, u"venue"_q, u"game"_q, u"invoice"_q,
		u"rich_message"_q,
	};
	if (!knownKinds.contains(media.value(u"kind"_q).toString())) {
		return u"This message uses media from a newer client."_q;
	}
	const auto knownFields = QStringList{
		u"kind"_q, u"width"_q, u"height"_q, u"duration"_q, u"title"_q,
		u"performer"_q, u"waveform"_q, u"alt"_q, u"spoiler"_q,
		u"invert_caption"_q, u"force_file"_q, u"supports_streaming"_q,
		u"nosound"_q, u"video_start_ms"_q, u"video_timestamp"_q,
		u"ttl_seconds"_q, u"sticker_type"_q, u"mask"_q,
		u"asset"_q, u"thumbnail_asset"_q, u"good_thumbnail_asset"_q,
		u"cover_asset"_q, u"video_asset"_q, u"video_thumbnail_asset"_q,
		u"rich"_q, u"rich_assets"_q, u"attached_stickers"_q,
		u"attached_sticker_sets"_q,
		u"inline_bot_id"_q, u"inline_bot_username"_q, u"reply_markup"_q,
		u"no_webpage"_q, u"webpage"_q, u"requires_server_replay"_q,
		u"replay_limitation"_q, u"first_name"_q, u"last_name"_q,
		u"phone_number"_q, u"vcard"_q, u"user_id"_q, u"point"_q, u"period"_q,
		u"heading"_q, u"proximity_notification_radius"_q, u"address"_q,
		u"provider"_q, u"venue_id"_q, u"venue_type"_q, u"description"_q,
		u"currency"_q, u"total_amount"_q, u"test"_q, u"short_name"_q,
		u"shipping_address_requested"_q, u"photo_asset"_q, u"photo_asset_media"_q,
		u"document_asset"_q, u"document_asset_media"_q, u"cover_asset_media"_q,
		u"video_codec"_q, u"preload_prefix_size"_q, u"sticker_set"_q,
		u"mask_coords"_q, u"custom_emoji"_q, u"free"_q, u"text_color"_q,
		u"sticker_association_requires_server"_q,
	};
	for (auto i = media.begin(); i != media.end(); ++i) {
		if (!knownFields.contains(i.key())) {
			return u"This message has media fields from a newer client."_q;
		}
	}
	if (!Format::ValidAttachedStickerSets(media, true)) {
		return u"This message has invalid or unsupported attached sticker hints."_q;
	}
	const auto nestedFields = std::map<QString, QStringList>{
		{ u"point"_q, { u"latitude"_q, u"longitude"_q, u"accuracy_radius"_q } },
		{ u"sticker_set"_q, { u"constructor"_q, u"id"_q, u"short_name"_q, u"emoticon"_q } },
		{ u"mask_coords"_q, { u"n"_q, u"x"_q, u"y"_q, u"zoom"_q } },
		{ u"webpage"_q, { u"url"_q, u"force_large_media"_q, u"force_small_media"_q,
			u"manual"_q, u"safe"_q } },
		{ u"reply_markup"_q, { u"inline"_q, u"resize"_q, u"single_use"_q,
			u"selective"_q, u"persistent"_q, u"force_reply"_q, u"hide"_q,
			u"placeholder"_q, u"rows"_q } },
	};
	for (const auto &[key, fields] : nestedFields) {
		if (media.contains(key) && (!media.value(key).isObject()
			|| !KnownFields(media.value(key).toObject(), fields))) {
			return u"This message has unsupported nested media metadata."_q;
		}
	}
	for (const auto &row : media.value(u"reply_markup"_q).toObject().value(u"rows"_q).toArray()) {
		for (const auto &value : row.toArray()) {
			const auto button = value.toObject();
			if (!KnownFields(button, { u"type"_q, u"text"_q, u"data"_q,
				u"forward_text"_q, u"icon_id"_q, u"color"_q, u"button_id"_q,
				u"peer_types"_q, u"request_peer"_q, u"suggested_name"_q,
				u"suggested_username"_q, u"requires_server_action"_q })
				|| !KnownFields(button.value(u"request_peer"_q).toObject(), {
					u"type"_q, u"max_quantity"_q, u"user_is_bot"_q, u"user_is_premium"_q,
					u"group_is_forum"_q, u"has_username"_q, u"am_creator"_q,
					u"bot_participant"_q, u"my_rights"_q, u"bot_rights"_q })) {
				return u"This message has keyboard metadata from a newer client."_q;
			}
		}
	}
	if (media.value(u"kind"_q).toString() == u"rich_message"_q) {
		if (const auto error = ValidateLocalQuickReplyRich(media.value(u"rich"_q).toObject()); !error.isEmpty()) return error;
	}
	for (const auto &key : { u"cover_asset_media"_q, u"photo_asset_media"_q, u"document_asset_media"_q }) {
		if (!media.contains(key)) continue;
		if (const auto error = MediaEditError(media.value(key).toObject(), depth + 1); !error.isEmpty()) return error;
	}
	for (const auto &entry : media.value(u"rich_assets"_q).toArray()) {
		const auto descriptor = entry.toObject();
		if (descriptor.size() != 2 || !descriptor.contains(u"asset"_q)
			|| !descriptor.contains(u"media"_q)) {
			return u"This message has unsupported rich media descriptors."_q;
		}
		if (const auto error = MediaEditError(descriptor.value(u"media"_q).toObject(), depth + 1); !error.isEmpty()) return error;
	}
	return {};
}

[[nodiscard]] Result CheckMediaEdit(const LocalQuickReplyMessage &message) {
	const auto previous = message.retained.value(u"media"_q).toObject();
	if (previous.isEmpty()) return {};
	const auto changed = message.media != previous || !message.sourcePaths.isEmpty()
		|| message.message.text != message.retained.value(u"text"_q).toString()
		|| EncodeMessage(message).value(u"entities"_q) != message.retained.value(u"entities"_q);
	if (changed) {
		if (const auto error = MediaEditError(previous); !error.isEmpty()) {
			return { Error::Unsupported, error };
		}
	}
	const auto original = message.retained.value(u"attachments"_q).toArray();
	if (message.attachments.size() < original.size()) {
		return { Error::Unsupported, u"Existing media assets must be retained."_q };
	}
	for (auto i = 0; i != original.size(); ++i) {
		if (message.attachments.at(i) != original.at(i)) {
			return { Error::Unsupported, u"Existing media asset indices cannot be changed."_q };
		}
	}

	return {};
}

[[nodiscard]] Result AddAssets(
		LocalQuickReply &reply,
		const QStringList &paths,
		QMap<QString, Asset> &assets,
		std::shared_ptr<const LocalQuickReplyMediaAssets> lease = nullptr,
		int maximum = Format::kMaximumAttachments) {
	if (reply.attachments.size() + paths.size() > maximum) {
		return { Error::Invalid, u"Too many attachments."_q };
	}
	for (const auto &path : paths) {
		auto file = QFile(path);
		const auto info = QFileInfo(file);
		if (!info.isFile() || !file.open(QIODevice::ReadOnly)) {
			return { Error::Io, u"Could not read the attachment."_q };
		}
		auto digest = QCryptographicHash(QCryptographicHash::Sha256);
		if (!digest.addData(&file)) {
			return { Error::Io, u"Could not read the attachment."_q };
		}
		const auto hash = QString::fromLatin1(digest.result().toHex());
		const auto size = file.size();
		auto asset = Asset();
		asset.sha256 = hash;
		asset.size = size;
		asset.read = [path, lease](AssetSink sink, Cancel cancel) {
			if (lease && ranges::find(lease->files(), path,
					&LocalQuickReplyMediaAsset::path) == lease->files().end()) {
				return Result{ Error::Invalid, u"The attachment lease is invalid."_q };
			}
			auto input = QFile(path);
			if (!input.open(QIODevice::ReadOnly)) {
				return Result{ Error::Io, u"Could not read the attachment."_q };
			}
			while (!input.atEnd()) {
				if (cancel && cancel->load()) return Result{ Error::Cancelled };
				const auto chunk = input.read(64 * 1024);
				if (chunk.isEmpty() || !sink(chunk)) {
					return Result{ Error::Io, u"Could not store the attachment."_q };
				}
			}
			return Result();
		};
		assets.insert(hash, std::move(asset));
		const auto descriptor = lease ? ranges::find(
			lease->files(), path, &LocalQuickReplyMediaAsset::path)
			: std::vector<LocalQuickReplyMediaAsset>::const_iterator();
		if (lease && (descriptor == lease->files().end()
			|| descriptor->size != size)) {
			return { Error::Invalid, u"The prepared attachment changed."_q };
		}
		reply.attachments.push_back(QJsonObject{
			{ u"sha256"_q, hash },
			{ u"size"_q, QString::number(size) },
			{ u"filename"_q, lease ? descriptor->filename : info.fileName() },
			{ u"mime"_q, lease ? descriptor->mime
				: QMimeDatabase().mimeTypeForFile(info).name() },
		});
	}
	return {};
}

} // namespace

QString QuickReplyMessageEditError(const LocalQuickReplyMessage &message) {
	auto temporary = LocalQuickReply();
	temporary.retained = message.retained;
	if (!QuickReplyTextSupported(temporary)) {
		return u"This message uses formatting from a newer client."_q;
	}
	return MediaEditError(message.media);
}

bool QuickReplyTextSupported(const LocalQuickReply &reply) {
	return ranges::all_of(reply.retained.value(u"entities"_q).toArray(),
		[](const auto &entry) {
			const auto type = entry.toObject().value(u"type"_q).toString();
			return ranges::any_of(kEntityTypes, [&](const auto &known) {
				return known.first == type;
			});
		});
}

QStringList QuickReplyVariables(const QString &text) {
	static const auto pattern = QRegularExpression(
		u"\\{\\{([\\p{L}\\p{N}_]{1,32})\\}\\}"_q);
	auto result = QStringList();
	auto matches = pattern.globalMatch(text);
	while (matches.hasNext()) result.push_back(matches.next().captured(1));
	result.removeDuplicates();
	return result;
}

namespace {

struct QuickReplyReplacement final {
	int start = 0;
	int length = 0;
	QString text;
};

struct QuickReplyExpansionPlan final {
	int length = 0;
	std::vector<QuickReplyReplacement> replacements;
};

[[nodiscard]] std::optional<QuickReplyExpansionPlan> PlanQuickReplyExpansion(
		const TextWithEntities &message,
		const QMap<QString, QString> &variables,
		int maximum,
		QString *error) {
	auto result = QuickReplyExpansionPlan();
	auto length = int64(message.text.size());
	const auto tooLong = [&] {
		if (error) *error = u"The expanded quick reply is too long."_q;
		return std::nullopt;
	};
	if (length > maximum) return tooLong();
	for (const auto &entity : message.entities) {
		if (!entity.validForText(message.text.size())) {
			if (error) *error = u"The quick reply has invalid text entities."_q;
			return std::nullopt;
		}
	}
	static const auto pattern = QRegularExpression(
		u"\\{\\{([\\p{L}\\p{N}_]{1,32})\\}\\}"_q);
	auto matches = pattern.globalMatch(message.text);
	while (matches.hasNext()) {
		const auto match = matches.next();
		const auto i = variables.constFind(match.captured(1));
		if (i == variables.cend()) continue;
		length += int64(i->size()) - match.capturedLength();
		result.replacements.push_back({
			int(match.capturedStart()), int(match.capturedLength()), *i,
		});
	}
	if (length > maximum) return tooLong();
	result.length = int(length);
	return result;
}

[[nodiscard]] TextWithEntities ApplyQuickReplyExpansion(
		const TextWithEntities &message,
		const QuickReplyExpansionPlan &plan) {
	const auto mapPosition = [&](int position, bool end) -> int {
		auto shift = 0;
		for (const auto &replacement : plan.replacements) {
			if (position <= replacement.start) break;
			if (position < replacement.start + replacement.length) {
				return replacement.start + shift
					+ (end ? int(replacement.text.size()) : 0);
			}
			shift += int(replacement.text.size()) - replacement.length;
		}
		return position + shift;
	};
	auto result = TextWithEntities();
	result.text.reserve(plan.length);
	auto position = 0;
	for (const auto &replacement : plan.replacements) {
		result.text.append(QStringView(message.text).mid(
			position, replacement.start - position));
		result.text.append(replacement.text);
		position = replacement.start + replacement.length;
	}
	result.text.append(QStringView(message.text).mid(position));
	for (const auto &entity : message.entities) {
		const auto start = mapPosition(entity.offset(), false);
		const auto end = mapPosition(entity.offset() + entity.length(), true);
		if (end > start) {
			result.entities.push_back(EntityInText(
				entity.type(), start, end - start, entity.data()));
		}
	}
	return result;
}

[[nodiscard]] std::vector<LocalQuickReplyMessage> ExpansionSource(
		const LocalQuickReply &reply) {
	return reply.messages.empty() ? LegacyMessages(reply) : reply.messages;
}

} // namespace

std::optional<QStringList> QuickReplyVariables(
		const LocalQuickReply &reply,
		QString *error) {
	auto result = QStringList();
	for (const auto &message : ExpansionSource(reply)) {
		const auto unsupported = QuickReplyMessageEditError(message);
		if (!unsupported.isEmpty()) {
			if (error) *error = unsupported;
			return std::nullopt;
		}
		result.append(QuickReplyVariables(message.message.text));
		if (message.media.value(u"kind"_q).toString() != u"rich_message"_q) {
			continue;
		}
		const auto failure = VisitLocalQuickReplyRichText(
			message.media.value(u"rich"_q).toObject(),
			[&](const TextWithEntities &text) {
				result.append(QuickReplyVariables(text.text));
				return true;
			});
		if (!failure.isEmpty()) {
			if (error) *error = failure;
			return std::nullopt;
		}
	}
	result.removeDuplicates();
	return result;
}

std::optional<TextWithEntities> ExpandQuickReplyChecked(
		const TextWithEntities &message,
		const QMap<QString, QString> &variables,
		int maximum,
		QString *error) {
	const auto plan = PlanQuickReplyExpansion(message, variables, maximum, error);
	return plan ? std::make_optional(ApplyQuickReplyExpansion(message, *plan))
		: std::nullopt;
}

TextWithEntities ExpandQuickReply(
		const TextWithEntities &message,
		const QMap<QString, QString> &variables) {
	const auto expanded = ExpandQuickReplyChecked(
		message,
		variables,
		Storage::LocalData::QuickReplies::kMaximumText);
	return expanded.value_or(TextWithEntities());
}

std::optional<std::vector<LocalQuickReplyMessage>> ExpandQuickReplyMessages(
		const LocalQuickReply &reply,
		const QMap<QString, QString> &variables,
		QString *error) {
	const auto source = ExpansionSource(reply);
	auto aggregate = int64(0);
	constexpr auto kAggregateLimit = int64(Format::kMaximumText) * 16;
	const auto measure = [&](const TextWithEntities &text) {
		const auto plan = PlanQuickReplyExpansion(
			text, variables, Format::kMaximumText, error);
		if (!plan) return false;
		aggregate += plan->length;
		if (aggregate > kAggregateLimit) {
			if (error) *error = u"The expanded quick reply is too long."_q;
			return false;
		}
		return true;
	};
	for (const auto &message : source) {
		const auto unsupported = QuickReplyMessageEditError(message);
		if (!unsupported.isEmpty()) {
			if (error) *error = unsupported;
			return std::nullopt;
		}
		if (!measure(message.message)) return std::nullopt;
		if (message.media.value(u"kind"_q).toString() != u"rich_message"_q) {
			continue;
		}
		auto measured = true;
		auto fixedTextLength = 0;
		const auto before = aggregate;
		const auto failure = VisitLocalQuickReplyRichText(
			message.media.value(u"rich"_q).toObject(),
			[&](const TextWithEntities &text) {
				measured = measure(text);
				return measured;
			}, &fixedTextLength);
		if (!measured) return std::nullopt;
		if (!failure.isEmpty()) {
			if (error) *error = failure;
			return std::nullopt;
		}
		aggregate += fixedTextLength;
		if (aggregate > kAggregateLimit
			|| aggregate - before > Iv::RichMessageLimits().lengthLimit) {
			if (error) *error = u"The expanded rich message is too long."_q;
			return std::nullopt;
		}
	}
	auto result = source;
	for (auto &message : result) {
		auto expanded = ExpandQuickReplyChecked(
			message.message, variables, Format::kMaximumText, error);
		if (!expanded) return std::nullopt;
		message.message = std::move(*expanded);
		if (message.media.value(u"kind"_q).toString() != u"rich_message"_q) {
			continue;
		}
		auto rich = TransformLocalQuickReplyRichText(
			message.media.value(u"rich"_q).toObject(),
			[&](const TextWithEntities &text) {
				return ExpandQuickReplyChecked(
					text, variables, Format::kMaximumText, error);
			});
		if (const auto failure = std::get_if<QString>(&rich)) {
			if (error) *error = *failure;
			return std::nullopt;
		}
		message.media.insert(u"rich"_q, std::get<QJsonObject>(std::move(rich)));
	}
	return result;
}

LocalQuickReplies::LocalQuickReplies(
		QObject *parent,
		not_null<Main::Session*> session)
: QObject(parent)
, _store(session->domainLocal().localData())
, _account{ session->isTestMode() ? u"test"_q : u"production"_q,
	QString::number(session->userId().bare) } {
	_store->changes() | rpl::on_next([=] { reload(); }, _lifetime);
	crl::on_main(this, [=] { reload(); });
}

LocalQuickReplies::~LocalQuickReplies() = default;

bool LocalQuickReplies::loaded() const { return _loaded; }
bool LocalQuickReplies::busy() const { return _busy || !_queue.empty(); }
const QString &LocalQuickReplies::error() const { return _error; }
const std::vector<LocalQuickReply> &LocalQuickReplies::list() const { return _list; }
const std::vector<LocalQuickReply> &LocalQuickReplies::recoveryList() const {
	return _recovery;
}
rpl::producer<> LocalQuickReplies::changes() const { return _changes.events(); }

std::optional<LocalQuickReply> LocalQuickReplies::find(const QString &id) const {
	for (const auto &reply : _list) if (reply.id == id) return reply;
	return std::nullopt;
}

std::vector<LocalQuickReply> LocalQuickReplies::Parse(const QJsonObject &module) {
	auto result = std::vector<LocalQuickReply>();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		if (record.value(u"kind"_q).toString() != u"reply"_q
			|| record.value(u"deleted"_q).toBool()) continue;
		const auto message = DecodeContent(record);
		result.push_back({ record.value(u"id"_q).toString(),
			record.value(u"title"_q).toString(),
			record.value(u"shortcut"_q).toString(),
			record.value(u"category"_q).toString(),
			std::move(message), record.value(u"variables"_q).toArray(),
			record.value(u"attachments"_q).toArray(),
			record.value(u"position"_q).toInt(), record });
	}
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		if (record.value(u"kind"_q).toString() != u"message"_q
			|| record.value(u"deleted"_q).toBool()) continue;
		const auto parent = ranges::find(result,
			record.value(u"reply_ref"_q).toString(), &LocalQuickReply::id);
		if (parent == result.end()) continue;
		parent->messages.push_back({
			.id = record.value(u"id"_q).toString(),
			.replyRef = parent->id,
			.groupRef = record.value(u"group_ref"_q).toString(),
			.replyToRef = record.value(u"reply_to_ref"_q).toString(),
			.message = DecodeContent(record),
			.attachments = record.value(u"attachments"_q).toArray(),
			.media = record.value(u"media"_q).toObject(),
			.position = record.value(u"position"_q).toInt(),
			.retained = record,
		});
	}
	for (auto &reply : result) {
		if (!reply.message.text.isEmpty() || !reply.attachments.isEmpty()) {
			const auto legacy = LegacyMessages(reply);
			reply.messages.insert(reply.messages.begin(), legacy.begin(), legacy.end());
		}
		ranges::sort(reply.messages, [](const auto &a, const auto &b) {
			return std::tie(a.position, a.id) < std::tie(b.position, b.id);
		});
	}
	ranges::sort(result, [](const auto &a, const auto &b) {
		return std::tie(a.position, a.id) < std::tie(b.position, b.id);
	});
	return result;
}

std::vector<LocalQuickReply> LocalQuickReplies::ParseRecovery(
		const QJsonObject &module) {
	auto records = module.value(u"records"_q).toArray();
	auto references = QSet<QString>();
	for (const auto &value : records) {
		const auto record = value.toObject();
		if (record.value(u"kind"_q).toString() == u"message"_q
			&& !record.value(u"deleted"_q).toBool()) {
			references.insert(record.value(u"reply_ref"_q).toString());
		}
	}
	auto recoverable = QSet<QString>();
	for (auto i = 0; i != records.size(); ++i) {
		auto record = records.at(i).toObject();
		const auto id = record.value(u"id"_q).toString();
		if (record.value(u"kind"_q).toString() != u"reply"_q
			|| !record.value(u"deleted"_q).toBool() || !references.contains(id)) continue;
		record.insert(u"deleted"_q, false);
		record.insert(u"text"_q, QString());
		record.insert(u"entities"_q, QJsonArray());
		record.insert(u"attachments"_q, QJsonArray());
		records[i] = record;
		recoverable.insert(id);
	}
	auto copy = module;
	copy.insert(u"records"_q, records);
	auto parsed = Parse(copy);
	parsed.erase(std::remove_if(parsed.begin(), parsed.end(), [&](const auto &reply) {
		return !recoverable.contains(reply.id);
	}), parsed.end());
	return parsed;
}

void LocalQuickReplies::reload() {
	if (_stopped) return;
	if (ranges::any_of(_queue, [](const Mutation &entry) { return !entry.job; })) {
		return;
	}
	enqueue(nullptr, nullptr);
}

void LocalQuickReplies::stop() {
	_stopped = true;
	if (_materializeCancelled) _materializeCancelled->store(true);
	_queue.clear();
	_lifetime.destroy();
	_materialized.clear();
}

void LocalQuickReplies::enqueue(Job job, Fn<void(bool)> done) {
	if (_stopped) { if (done) done(false); return; }
	_queue.push_back({ std::move(job), std::move(done) });
	runNext();
}

void LocalQuickReplies::runNext() {
	if (_busy || _queue.empty() || _stopped) return;
	_busy = true;
	const auto weak = QPointer(this);
	const auto store = _store;
	const auto account = _account;
	const auto job = _queue.front().job;
	crl::async([=] {
		auto result = job ? job(*store) : Result();
		auto snapshot = Snapshot();
		auto read = store->snapshot(snapshot);
		const auto module = read ? ReadModule(snapshot, account, kModule) : QJsonObject();
		if (read) read = Format::Validate(module, account);
		auto parsed = read ? Parse(module) : std::vector<LocalQuickReply>();
		auto recovery = read ? ParseRecovery(module) : std::vector<LocalQuickReply>();
		auto drafts = QMap<QString, QJsonObject>();
		if (read) {
			for (const auto &value : module.value(u"records"_q).toArray()) {
				const auto record = value.toObject();
				if (record.value(u"kind"_q).toString() == u"editor_draft"_q) {
					drafts.insert(record.value(u"reply_ref"_q).toString(), record);
				}
			}
		}
		if (result && !read) result = read;
		crl::on_main([=, parsed = std::move(parsed), recovery = std::move(recovery), drafts = std::move(drafts)]() mutable {
			if (!weak || weak->_stopped) return;
			auto done = std::move(weak->_queue.front().done);
			weak->_queue.pop_front();
			weak->_busy = false;
			weak->_loaded = true;
			weak->_error = result.message;
			if (read) {
				weak->_list = std::move(parsed);
				weak->_recovery = std::move(recovery);
				weak->_editorDrafts = std::move(drafts);
			}
			weak->_changes.fire({});
			if (done) done(result.ok());
			if (weak && !weak->_stopped) weak->runNext();
		});
	});
}

void LocalQuickReplies::save(
		LocalQuickReply reply,
		QStringList paths,
		Fn<void(bool)> done) {
	if (reply.id.isEmpty()) reply.id = NewId();
	const auto account = _account;
	enqueue([=](Store &store) mutable {
		if (!reply.retained.isEmpty()
			&& reply.message.text != reply.retained.value(u"text"_q).toString()) {
			for (const auto &entry : reply.retained.value(u"entities"_q).toArray()) {
				const auto type = entry.toObject().value(u"type"_q).toString();
				if (ranges::none_of(kEntityTypes, [&](const auto &known) {
					return known.first == type;
				})) {
					return Result{ Error::Unsupported,
						u"This reply uses formatting from a newer client. Its text cannot be edited here."_q };
				}
			}
		}
		auto assets = QMap<QString, Asset>();
		auto result = AddAssets(reply, paths, assets);
		if (!result) return result;
		const auto encoded = Encode(reply);
		return store.mutateWithAssets(account, kModule, [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			auto found = false;
			for (auto i = 0; i != records.size(); ++i) {
				auto record = records.at(i).toObject();
				if (record.value(u"id"_q).toString() == reply.id) {
					if (record.value(u"deleted"_q).toBool()
						|| record.value(u"version_vector"_q)
							!= reply.retained.value(u"version_vector"_q)) {
						return Result{ Error::Stale,
							u"The quick reply changed. Reopen it before saving."_q };
					}
					for (auto j = encoded.begin(); j != encoded.end(); ++j) {
						record.insert(j.key(), j.value());
					}
					records[i] = record;
					found = true;
					break;
				}
			}
			if (!found) records.push_back(encoded);
			module.insert(u"records"_q, records);
			return Format::Validate(module, account);
		}, assets);
	}, std::move(done));
}

void LocalQuickReplies::remove(QString id, Fn<void(bool)> done) {
	const auto account = _account;
	enqueue([=](Store &store) {
		return store.mutate(account, kModule, [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			for (auto i = 0; i != records.size(); ++i) {
				auto record = records.at(i).toObject();
				if (record.value(u"id"_q).toString() == id
					|| ((record.value(u"kind"_q).toString() == u"message"_q
							|| record.value(u"kind"_q).toString() == u"editor_draft"_q)
						&& record.value(u"reply_ref"_q).toString() == id)) {
					record.insert(u"deleted"_q, true);
					records[i] = record;
				}
			}
			module.insert(u"records"_q, records);
			return Result();
		});
	}, std::move(done));
}

void LocalQuickReplies::recover(LocalQuickReply reply, Fn<void(bool)> done) {
	const auto account = _account;
	enqueue([=](Store &store) {
		return store.mutate(account, kModule, [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			for (auto i = 0; i != records.size(); ++i) {
				auto record = records.at(i).toObject();
				if (record.value(u"id"_q).toString() != reply.id) continue;
				if (!record.value(u"deleted"_q).toBool()) {
					return Result{ Error::Stale, u"The quick reply changed. Reopen it."_q };
				}
				auto restored = Encode(reply);
				restored.insert(u"text"_q, QString());
				restored.insert(u"entities"_q, QJsonArray());
				restored.insert(u"attachments"_q, QJsonArray());
				for (auto j = restored.begin(); j != restored.end(); ++j) {
					record.insert(j.key(), j.value());
				}
				records[i] = record;
				module.insert(u"records"_q, records);
				return Format::Validate(module, account);
			}
			return Result{ Error::Stale, u"The quick reply was removed."_q };
		});
	}, std::move(done));
}

void LocalQuickReplies::saveMessages(
		QString replyRef,
		std::vector<LocalQuickReplyMessage> messages,
		Fn<void(bool)> done,
		std::shared_ptr<const LocalQuickReplyMediaAssets> lease) {
	for (auto &message : messages) {
		if (message.id.isEmpty()) message.id = NewId();
		message.replyRef = replyRef;
	}
	const auto account = _account;
	enqueue([=](Store &store) mutable {
		auto assets = QMap<QString, Asset>();
		for (auto &message : messages) {
			if (!message.sourcePaths.isEmpty() && !lease) {
				return Result{ Error::Invalid, u"The prepared attachment lifetime is missing."_q };
			}
			if (const auto checked = CheckMediaEdit(message); !checked) return checked;
			auto temporary = LocalQuickReply();
			temporary.retained = message.retained;
			if (!QuickReplyTextSupported(temporary)
				&& message.message.text != message.retained.value(u"text"_q).toString()) {
				return Result{ Error::Unsupported,
					u"This message uses formatting from a newer client."_q };
			}
			temporary.attachments = message.attachments;
			const auto added = AddAssets(temporary, message.sourcePaths, assets, lease,
				Format::kMaximumMessageAssets);
			if (!added) return added;
			message.attachments = temporary.attachments;
		}
		return store.mutateWithAssets(account, kModule, [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			const auto parsed = Parse(module);
			const auto parent = ranges::find(parsed, replyRef, &LocalQuickReply::id);
			if (parent == parsed.end()) {
				return Result{ Error::Stale, u"The quick reply was removed."_q };
			}
			auto changes = messages;
			if (!parent->message.text.isEmpty() || !parent->attachments.isEmpty()) {
				for (const auto &legacy : LegacyMessages(*parent)) {
					const auto edited = ranges::find(changes, legacy.id, &LocalQuickReplyMessage::id);
					if (edited == changes.end()) {
						changes.push_back(legacy);
					} else if (edited->legacyParentRevision
						!= parent->retained.value(u"version_vector"_q).toObject()) {
						return Result{ Error::Stale, u"The message changed. Reopen it before saving."_q };
					}
				}
				for (auto i = 0; i != records.size(); ++i) {
					auto record = records.at(i).toObject();
					if (record.value(u"id"_q).toString() != replyRef) continue;
					record.insert(u"text"_q, QString());
					record.insert(u"entities"_q, QJsonArray());
					record.insert(u"attachments"_q, QJsonArray());
					records[i] = record;
				}
			}
			for (const auto &message : changes) {
				auto encoded = EncodeMessage(message);
				auto found = false;
				for (auto i = 0; i != records.size(); ++i) {
					const auto previous = records.at(i).toObject();
					if (previous.value(u"id"_q).toString() != message.id) continue;
					if (previous.value(u"kind"_q).toString() != u"message"_q
						|| previous.value(u"reply_ref"_q).toString() != replyRef
						|| previous.value(u"deleted"_q).toBool()
						|| previous.value(u"version_vector"_q)
							!= message.retained.value(u"version_vector"_q)) {
						return Result{ Error::Stale,
							u"The message changed. Reopen it before saving."_q };
					}
					auto merged = previous;
					for (auto j = encoded.begin(); j != encoded.end(); ++j) {
						merged.insert(j.key(), j.value());
					}
					records[i] = merged;
					found = true;
					break;
				}
				if (!found) records.push_back(encoded);
			}
			module.insert(u"records"_q, records);
			return Format::Validate(module, account);
		}, assets);
	}, std::move(done));
}

void LocalQuickReplies::removeMessages(
		QString replyRef,
		QStringList ids,
		Fn<void(bool)> done) {
	const auto account = _account;
	enqueue([=](Store &store) {
		return store.mutate(account, kModule, [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			const auto parsed = Parse(module);
			const auto parent = ranges::find(parsed, replyRef, &LocalQuickReply::id);
			const auto legacy = parent == parsed.end()
				? std::vector<LocalQuickReplyMessage>() : LegacyMessages(*parent);
			const auto migrate = ranges::any_of(legacy, [&](const auto &message) {
				return ids.contains(message.id);
			});
			if (migrate) {
				for (const auto &message : legacy) records.push_back(EncodeMessage(message));
			}
			for (auto i = 0; i != records.size(); ++i) {
				auto record = records.at(i).toObject();
				const auto id = record.value(u"id"_q).toString();
				if (record.value(u"kind"_q).toString() == u"message"_q
					&& record.value(u"reply_ref"_q).toString() == replyRef
					&& ids.contains(id)) {
					record.insert(u"deleted"_q, true);
					records[i] = record;
				} else if (id == replyRef && migrate) {
					record.insert(u"text"_q, QString());
					record.insert(u"entities"_q, QJsonArray());
					record.insert(u"attachments"_q, QJsonArray());
					records[i] = record;
				}
			}
			module.insert(u"records"_q, records);
			return Format::Validate(module, account);
		});
	}, std::move(done));
}

QJsonObject LocalQuickReplies::editorDraft(const QString &replyRef) const {
	return _editorDrafts.value(replyRef);
}

void LocalQuickReplies::saveEditorDraft(
		QString replyRef,
		QJsonObject draft,
		QJsonObject expectedRevision,
		Fn<void(bool)> done) {
	const auto account = _account;
	enqueue([=](Store &store) {
		return store.mutate(account, kModule, [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			const auto parsed = Parse(module);
			if (ranges::find(parsed, replyRef, &LocalQuickReply::id) == parsed.end()) {
				return Result{ Error::Stale, u"The quick reply was removed."_q };
			}
			const auto id = QUuid::createUuidV5(QUuid(replyRef),
				QByteArray("editor-draft")).toString(QUuid::WithoutBraces);
			auto index = -1;
			auto record = QJsonObject();
			for (auto i = 0; i != records.size(); ++i) {
				if (records.at(i).toObject().value(u"id"_q).toString() == id) {
					index = i;
					record = records.at(i).toObject();
					break;
				}
			}
			if (record.value(u"version_vector"_q).toObject() != expectedRevision) {
				return Result{ Error::Stale,
					u"The editor draft changed. Reopen it before saving."_q };
			}
			if (index >= 0
				&& (record.value(u"kind"_q).toString() != u"editor_draft"_q
					|| record.value(u"reply_ref"_q).toString() != replyRef)) {
				return Result{ Error::Invalid, u"The editor draft identity is invalid."_q };
			}
			record.insert(u"id"_q, id);
			record.insert(u"kind"_q, u"editor_draft"_q);
			record.insert(u"reply_ref"_q, replyRef);
			record.insert(u"deleted"_q, draft.isEmpty());
			for (const auto &key : { u"normal"_q, u"edit"_q }) {
				if (draft.contains(key)) {
					record.insert(key, MergeEditorDraftFields(record.value(key).toObject(),
						draft.value(key).toObject()));
				} else {
					record.remove(key);
				}
			}
			if (index >= 0) records[index] = record;
			else if (!draft.isEmpty()) records.push_back(record);
			module.insert(u"records"_q, records);
			return Format::Validate(module, account);
		});
	}, std::move(done));
}

void LocalQuickReplies::move(QString id, int direction) {
	const auto account = _account;
	enqueue([=](Store &store) {
		return store.mutate(account, kModule, [=](QJsonObject &module) {
			auto replies = Parse(module);
			const auto i = ranges::find(replies, id, &LocalQuickReply::id);
			if (i == replies.end()) return Result();
			const auto index = int(i - replies.begin());
			const auto next = index + (direction < 0 ? -1 : 1);
			if (next < 0 || next >= replies.size()) return Result();
			std::swap(replies[index], replies[next]);
			auto positions = QMap<QString, int>();
			for (auto j = 0; j != replies.size(); ++j) positions[replies[j].id] = j;
			auto records = module.value(u"records"_q).toArray();
			for (auto j = 0; j != records.size(); ++j) {
				auto record = records.at(j).toObject();
				const auto p = positions.constFind(record.value(u"id"_q).toString());
				if (p != positions.cend()) {
					record.insert(u"position"_q, *p);
					records[j] = record;
				}
			}
			module.insert(u"records"_q, records);
			return Result();
		});
	}, nullptr);
}

void LocalQuickReplies::materializeMessage(
		LocalQuickReplyMessage message,
		Fn<void(LocalQuickReplyMaterialized, QString)> done,
		std::shared_ptr<const LocalQuickReplyMediaAssets> prepared) {
	if (_stopped) { done({}, u"The account was closed."_q); return; }
	if (message.attachments.size() + message.sourcePaths.size()
		> Format::kMaximumMessageAssets) {
		done({}, u"Too many quick reply assets."_q);
		return;
	}
	for (const auto &path : message.sourcePaths) {
		if (!prepared || ranges::find(prepared->files(), path,
				&LocalQuickReplyMediaAsset::path) == prepared->files().end()) {
			done({}, u"The prepared attachment lifetime is missing."_q);
			return;
		}
	}
	const auto key = QString::fromLatin1(QCryptographicHash::hash(
		QJsonDocument(message.attachments).toJson(QJsonDocument::Compact),
		QCryptographicHash::Sha256).toHex());
	const auto complete = [=](LocalQuickReplyMaterialized result, QString error) {
		if (!error.isEmpty()) { done({}, std::move(error)); return; }
		if (!message.sourcePaths.isEmpty()) {
			auto lease = std::make_shared<LocalQuickReplyAssetLease>();
			if (result.assets) lease->_directory = result.assets->_directory;
			lease->_prepared = prepared;
			result.assets = std::move(lease);
			result.paths.append(message.sourcePaths);
		}
		done(std::move(result), {});
	};
	if (message.attachments.isEmpty()) { complete({}, {}); return; }
	for (auto i = _materialized.begin(); i != _materialized.end();) {
		if (i->assets.expired()) i = _materialized.erase(i);
		else ++i;
	}
	const auto cached = _materialized.constFind(key);
	if (cached != _materialized.cend()) {
		const auto assets = cached->assets.lock();
		if (assets && ranges::all_of(cached->paths, [](const auto &path) {
			return QFile::exists(path);
		})) {
			complete({ cached->paths, assets }, {});
			return;
		}
	}
	if (!_materializeCancelled) _materializeCancelled = std::make_shared<std::atomic<bool>>(false);
	const auto cancelled = _materializeCancelled;
	const auto weak = QPointer(this);
	const auto store = _store;
	crl::async([=] {
		auto snapshot = Snapshot();
		auto result = store->snapshot(snapshot);
		auto materialized = LocalQuickReplyMaterialized();
		auto lease = std::make_shared<LocalQuickReplyAssetLease>();
		lease->_directory = std::make_shared<QTemporaryDir>();
		materialized.assets = lease;
		if (result && !lease->_directory->isValid()) {
			result = { Error::Io, u"Could not prepare attachments."_q };
		}
		for (const auto &entry : message.attachments) {
			if (cancelled->load()) { result = { Error::Cancelled }; break; }
			if (!result) break;
			const auto attachment = entry.toObject();
			const auto asset = snapshot.assets.constFind(
				attachment.value(u"sha256"_q).toString());
			if (asset == snapshot.assets.cend() || !asset->read
				|| asset->size != attachment.value(u"size"_q).toString().toLongLong()) {
				result = { Error::Corrupt, u"The attachment is missing."_q };
				break;
			}
			const auto filename = attachment.value(u"filename"_q).toString();
			if (filename.isEmpty() || filename == u"."_q || filename == u".."_q
				|| filename.contains(u'/') || filename.contains(u'\\')) {
				result = { Error::Invalid, u"Invalid attachment filename."_q };
				break;
			}
			const auto directory = lease->_directory->path() + u'/'
				+ QString::number(materialized.paths.size());
			if (!QDir().mkpath(directory)
				|| !QFile::setPermissions(directory,
					QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
				result = { Error::Io, u"Could not protect temporary attachments."_q };
				break;
			}
			const auto path = directory + u'/' + filename;
			auto output = QFile(path);
			if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)
				|| !output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
				result = { Error::Io, u"Could not prepare attachments."_q };
				break;
			}
			auto digest = QCryptographicHash(QCryptographicHash::Sha256);
			auto written = int64(0);
			result = asset->read([&](QByteArrayView chunk) {
				written += chunk.size();
				if (written > asset->size) return false;
				digest.addData(chunk);
				return output.write(chunk.data(), chunk.size()) == chunk.size();
			}, cancelled.get());
			if (result && (written != asset->size
				|| QString::fromLatin1(digest.result().toHex()) != asset->sha256)) {
				result = { Error::Corrupt, u"The attachment checksum changed."_q };
			}
			if (result && !output.flush()) {
				result = { Error::Io, u"Could not prepare attachments."_q };
			}
			materialized.paths.push_back(path);
		}
		crl::on_main([=] {
			if (!weak || weak->_stopped) return;
			if (result) {
				auto &cache = weak->_materialized;
				for (auto i = cache.begin(); i != cache.end();) {
					if (i->assets.expired()) i = cache.erase(i);
					else ++i;
				}
				cache.remove(key);
				while (cache.size() >= kMaterializedCacheLimit) {
					cache.erase(cache.begin());
				}
				cache.insert(key, { materialized.paths, materialized.assets });
			}
			complete(result ? materialized : LocalQuickReplyMaterialized(), result.message);
		});
	});
}

} // namespace Data
