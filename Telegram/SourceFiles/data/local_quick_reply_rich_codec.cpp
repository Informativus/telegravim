/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_quick_reply_rich_codec.h"

#include "data/data_document.h"
#include "data/data_photo.h"
#include "history/history_item_reply_markup.h"
#include "ui/text/text_entity.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <functional>
#include <utility>

namespace Data {
namespace {

using Block = Iv::RichPage::Block;
using BlockKind = Iv::RichPage::BlockKind;
using Button = Iv::RichPage::Button;
using ButtonAlignment = Iv::RichPage::ButtonAlignment;
using GroupedMediaIntent = Iv::RichPage::GroupedMediaIntent;
using GroupedMediaItem = Iv::RichPage::GroupedMediaItem;
using ListItem = Iv::RichPage::ListItem;
using ListKind = Iv::RichPage::ListKind;
using RichText = Iv::RichPage::RichText;
using TableAlignment = Iv::RichPage::TableAlignment;
using TableCell = Iv::RichPage::TableCell;
using TableRow = Iv::RichPage::TableRow;
using TableVerticalAlignment = Iv::RichPage::TableVerticalAlignment;
using TaskState = Iv::RichPage::TaskState;

constexpr auto kVersion = 1;
constexpr auto kMaxDepth = 16;
constexpr auto kMaxArray = 500;
constexpr auto kMaxString = 1 << 20;
constexpr auto kMaxJsonBytes = 16 << 20;

[[nodiscard]] QString Error(QString text) {
	return u"rich codec: "_q + text;
}

[[nodiscard]] bool IsString(const QJsonValue &value) {
	return value.type() == QJsonValue::String;
}

[[nodiscard]] bool IsNumber(const QJsonValue &value) {
	return value.type() == QJsonValue::Double
		&& std::isfinite(value.toDouble());
}

[[nodiscard]] bool IsInteger(const QJsonValue &value) {
	if (!IsNumber(value)) {
		return false;
	}
	const auto number = value.toDouble();
	return std::floor(number) == number
		&& number >= std::numeric_limits<int>::min()
		&& number <= std::numeric_limits<int>::max();
}

[[nodiscard]] bool IsBool(const QJsonValue &value) {
	return value.type() == QJsonValue::Bool;
}

[[nodiscard]] bool IsArray(const QJsonValue &value) {
	return value.type() == QJsonValue::Array;
}

[[nodiscard]] bool IsObject(const QJsonValue &value) {
	return value.type() == QJsonValue::Object;
}

[[nodiscard]] bool IsReasonableString(const QString &value) {
	return value.size() <= kMaxString;
}

[[nodiscard]] QString KindName(BlockKind kind) {
	switch (kind) {
	case BlockKind::Unsupported: return u"unsupported"_q;
	case BlockKind::Heading: return u"heading"_q;
	case BlockKind::Paragraph: return u"paragraph"_q;
	case BlockKind::Footer: return u"footer"_q;
	case BlockKind::Thinking: return u"thinking"_q;
	case BlockKind::AuthorDate: return u"author_date"_q;
	case BlockKind::Code: return u"code"_q;
	case BlockKind::Divider: return u"divider"_q;
	case BlockKind::Anchor: return u"anchor"_q;
	case BlockKind::ButtonRow: return u"button_row"_q;
	case BlockKind::List: return u"list"_q;
	case BlockKind::Quote: return u"quote"_q;
	case BlockKind::Photo: return u"photo"_q;
	case BlockKind::Video: return u"video"_q;
	case BlockKind::Embed: return u"embed"_q;
	case BlockKind::EmbedPost: return u"embed_post"_q;
	case BlockKind::GroupedMedia: return u"grouped_media"_q;
	case BlockKind::Channel: return u"channel"_q;
	case BlockKind::Audio: return u"audio"_q;
	case BlockKind::File: return u"file"_q;
	case BlockKind::Math: return u"math"_q;
	case BlockKind::Table: return u"table"_q;
	case BlockKind::Details: return u"details"_q;
	case BlockKind::RelatedArticles: return u"related_articles"_q;
	case BlockKind::Map: return u"map"_q;
	}
	return QString();
}

[[nodiscard]] std::optional<BlockKind> ParseKind(const QString &name) {
	static const auto values = std::array<std::pair<QString, BlockKind>, 27>{
		std::pair{ u"unsupported"_q, BlockKind::Unsupported },
		std::pair{ u"heading"_q, BlockKind::Heading },
		std::pair{ u"paragraph"_q, BlockKind::Paragraph },
		std::pair{ u"footer"_q, BlockKind::Footer },
		std::pair{ u"thinking"_q, BlockKind::Thinking },
		std::pair{ u"author_date"_q, BlockKind::AuthorDate },
		std::pair{ u"code"_q, BlockKind::Code },
		std::pair{ u"divider"_q, BlockKind::Divider },
		std::pair{ u"anchor"_q, BlockKind::Anchor },
		std::pair{ u"button_row"_q, BlockKind::ButtonRow },
		std::pair{ u"list"_q, BlockKind::List },
		std::pair{ u"quote"_q, BlockKind::Quote },
		std::pair{ u"photo"_q, BlockKind::Photo },
		std::pair{ u"video"_q, BlockKind::Video },
		std::pair{ u"embed"_q, BlockKind::Embed },
		std::pair{ u"embed_post"_q, BlockKind::EmbedPost },
		std::pair{ u"grouped_media"_q, BlockKind::GroupedMedia },
		std::pair{ u"channel"_q, BlockKind::Channel },
		std::pair{ u"audio"_q, BlockKind::Audio },
		std::pair{ u"file"_q, BlockKind::File },
		std::pair{ u"math"_q, BlockKind::Math },
		std::pair{ u"table"_q, BlockKind::Table },
		std::pair{ u"details"_q, BlockKind::Details },
		std::pair{ u"related_articles"_q, BlockKind::RelatedArticles },
		std::pair{ u"map"_q, BlockKind::Map },
		std::pair{ u""_q, BlockKind::Unsupported },
		std::pair{ u"invalid"_q, BlockKind::Unsupported },
	};
	for (const auto &[value, kind] : values) {
		if (value == name && !value.isEmpty()) {
			return kind;
		}
	}
	return std::nullopt;
}

[[nodiscard]] QString ListKindName(ListKind kind) {
	return (kind == ListKind::Ordered) ? u"ordered"_q : u"bullet"_q;
}

[[nodiscard]] std::optional<ListKind> ParseListKind(const QString &value) {
	if (value == u"bullet"_q) return ListKind::Bullet;
	if (value == u"ordered"_q) return ListKind::Ordered;
	return std::nullopt;
}

[[nodiscard]] QString TaskStateName(TaskState state) {
	switch (state) {
	case TaskState::None: return u"none"_q;
	case TaskState::Unchecked: return u"unchecked"_q;
	case TaskState::Checked: return u"checked"_q;
	}
	return QString();
}

[[nodiscard]] std::optional<TaskState> ParseTaskState(const QString &value) {
	if (value == u"none"_q) return TaskState::None;
	if (value == u"unchecked"_q) return TaskState::Unchecked;
	if (value == u"checked"_q) return TaskState::Checked;
	return std::nullopt;
}

[[nodiscard]] QString AlignmentName(TableAlignment value) {
	switch (value) {
	case TableAlignment::Left: return u"left"_q;
	case TableAlignment::Center: return u"center"_q;
	case TableAlignment::Right: return u"right"_q;
	}
	return QString();
}

[[nodiscard]] std::optional<TableAlignment> ParseAlignment(const QString &value) {
	if (value == u"left"_q) return TableAlignment::Left;
	if (value == u"center"_q) return TableAlignment::Center;
	if (value == u"right"_q) return TableAlignment::Right;
	return std::nullopt;
}

[[nodiscard]] QString VerticalAlignmentName(TableVerticalAlignment value) {
	switch (value) {
	case TableVerticalAlignment::Top: return u"top"_q;
	case TableVerticalAlignment::Middle: return u"middle"_q;
	case TableVerticalAlignment::Bottom: return u"bottom"_q;
	}
	return QString();
}

[[nodiscard]] std::optional<TableVerticalAlignment> ParseVerticalAlignment(
		const QString &value) {
	if (value == u"top"_q) return TableVerticalAlignment::Top;
	if (value == u"middle"_q) return TableVerticalAlignment::Middle;
	if (value == u"bottom"_q) return TableVerticalAlignment::Bottom;
	return std::nullopt;
}

[[nodiscard]] QString MediaIntentName(GroupedMediaIntent value) {
	return (value == GroupedMediaIntent::Slideshow)
		? u"slideshow"_q
		: u"collage"_q;
}

[[nodiscard]] std::optional<GroupedMediaIntent> ParseMediaIntent(
		const QString &value) {
	if (value == u"collage"_q) return GroupedMediaIntent::Collage;
	if (value == u"slideshow"_q) return GroupedMediaIntent::Slideshow;
	return std::nullopt;
}

[[nodiscard]] QString ButtonAlignmentName(ButtonAlignment value) {
	switch (value) {
	case ButtonAlignment::Stretch: return u"stretch"_q;
	case ButtonAlignment::Left: return u"left"_q;
	case ButtonAlignment::Center: return u"center"_q;
	case ButtonAlignment::Right: return u"right"_q;
	}
	return QString();
}

[[nodiscard]] std::optional<ButtonAlignment> ParseButtonAlignment(
		const QString &value) {
	if (value == u"stretch"_q) return ButtonAlignment::Stretch;
	if (value == u"left"_q) return ButtonAlignment::Left;
	if (value == u"center"_q) return ButtonAlignment::Center;
	if (value == u"right"_q) return ButtonAlignment::Right;
	return std::nullopt;
}

[[nodiscard]] QString EntityName(EntityType type) {
	switch (type) {
	case EntityType::Invalid: return u"invalid"_q;
	case EntityType::Url: return u"url"_q;
	case EntityType::CustomUrl: return u"custom_url"_q;
	case EntityType::Email: return u"email"_q;
	case EntityType::Hashtag: return u"hashtag"_q;
	case EntityType::Cashtag: return u"cashtag"_q;
	case EntityType::Mention: return u"mention"_q;
	case EntityType::MentionName: return u"mention_name"_q;
	case EntityType::CustomEmoji: return u"custom_emoji"_q;
	case EntityType::BotCommand: return u"bot_command"_q;
	case EntityType::MediaTimestamp: return u"media_timestamp"_q;
	case EntityType::Colorized: return u"colorized"_q;
	case EntityType::Phone: return u"phone"_q;
	case EntityType::BankCard: return u"bank_card"_q;
	case EntityType::Bold: return u"bold"_q;
	case EntityType::Semibold: return u"semibold"_q;
	case EntityType::Italic: return u"italic"_q;
	case EntityType::Underline: return u"underline"_q;
	case EntityType::StrikeOut: return u"strike_out"_q;
	case EntityType::Code: return u"code"_q;
	case EntityType::Pre: return u"pre"_q;
	case EntityType::Blockquote: return u"blockquote"_q;
	case EntityType::Spoiler: return u"spoiler"_q;
	case EntityType::Subscript: return u"subscript"_q;
	case EntityType::Superscript: return u"superscript"_q;
	case EntityType::Marked: return u"marked"_q;
	case EntityType::FormattedDate: return u"formatted_date"_q;
	}
	return QString();
}

[[nodiscard]] std::optional<EntityType> ParseEntity(const QString &value) {
	for (auto type = EntityType::Invalid;
			int(type) <= int(EntityType::FormattedDate);
			type = EntityType(int(type) + 1)) {
		if (EntityName(type) == value) {
			return type;
		}
	}
	return std::nullopt;
}

[[nodiscard]] QString ButtonTypeName(HistoryMessageMarkupButton::Type type) {
	return QString::number(int(type));
}

[[nodiscard]] std::optional<HistoryMessageMarkupButton::Type>
ParseButtonType(const QJsonValue &value) {
	if (!IsInteger(value)) return std::nullopt;
	const auto number = value.toInt();
	if (number < 0
		|| number >= int(HistoryMessageMarkupButton::Type::kCount)) {
		return std::nullopt;
	}
	return static_cast<HistoryMessageMarkupButton::Type>(number);
}

[[nodiscard]] bool HasOnlyKnownKeys(
		const QJsonObject &object,
		std::initializer_list<QString> keys,
		QString *error) {
	for (const auto &key : object.keys()) {
		if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
			*error = Error(u"unknown field '"_q + key + u"'"_q);
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool ReadString(
		const QJsonObject &object,
		const QString &key,
		QString *result,
		QString *error,
		bool required = false) {
	const auto value = object.value(key);
	if (value.isUndefined()) {
		if (required) *error = Error(u"missing string field '"_q + key + u"'"_q);
		return !required;
	}
	if (!IsString(value) || !IsReasonableString(value.toString())) {
		*error = Error(u"invalid string field '"_q + key + u"'"_q);
		return false;
	}
	*result = value.toString();
	return true;
}

[[nodiscard]] bool ReadBool(
		const QJsonObject &object,
		const QString &key,
		bool *result,
		QString *error,
		bool required = false) {
	const auto value = object.value(key);
	if (value.isUndefined()) {
		if (required) *error = Error(u"missing bool field '"_q + key + u"'"_q);
		return !required;
	}
	if (!IsBool(value)) {
		*error = Error(u"invalid bool field '"_q + key + u"'"_q);
		return false;
	}
	*result = value.toBool();
	return true;
}

[[nodiscard]] bool ReadInt(
		const QJsonObject &object,
		const QString &key,
		int *result,
		QString *error,
		bool required = false) {
	const auto value = object.value(key);
	if (value.isUndefined()) {
		if (required) *error = Error(u"missing integer field '"_q + key + u"'"_q);
		return !required;
	}
	if (!IsInteger(value)) {
		*error = Error(u"invalid integer field '"_q + key + u"'"_q);
		return false;
	}
	*result = value.toInt();
	return true;
}

[[nodiscard]] bool ReadDouble(
		const QJsonObject &object,
		const QString &key,
		double *result,
		QString *error,
		bool required = false) {
	const auto value = object.value(key);
	if (value.isUndefined()) {
		if (required) *error = Error(u"missing number field '"_q + key + u"'"_q);
		return !required;
	}
	if (!IsNumber(value)) {
		*error = Error(u"invalid number field '"_q + key + u"'"_q);
		return false;
	}
	*result = value.toDouble();
	return true;
}

[[nodiscard]] bool ReadObject(
		const QJsonObject &object,
		const QString &key,
		QJsonObject *result,
		QString *error,
		bool required = false) {
	const auto value = object.value(key);
	if (value.isUndefined()) {
		if (required) *error = Error(u"missing object field '"_q + key + u"'"_q);
		return !required;
	}
	if (!IsObject(value)) {
		*error = Error(u"invalid object field '"_q + key + u"'"_q);
		return false;
	}
	*result = value.toObject();
	return true;
}

[[nodiscard]] bool ReadArray(
		const QJsonObject &object,
		const QString &key,
		QJsonArray *result,
		QString *error,
		bool required = false) {
	const auto value = object.value(key);
	if (value.isUndefined()) {
		if (required) *error = Error(u"missing array field '"_q + key + u"'"_q);
		return !required;
	}
	if (!IsArray(value) || value.toArray().size() > kMaxArray) {
		*error = Error(u"invalid array field '"_q + key + u"'"_q);
		return false;
	}
	*result = value.toArray();
	return true;
}


[[nodiscard]] std::optional<QByteArray> DecodeBase64(const QString &value) {
	const auto encoded = value.toLatin1();
	const auto decoded = QByteArray::fromBase64(
		encoded,
		QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
	if (decoded.toBase64() != encoded) return std::nullopt;
	return decoded;
}

void PutText(QJsonObject &object, const QString &key, const RichText &text);
[[nodiscard]] bool DecodeText(
	const QJsonObject &object,
	const QString &key,
	RichText *result,
	QString *error);

void PutTextWithEntities(QJsonObject &object, const TextWithEntities &text) {
	auto entities = QJsonArray();
	for (const auto &entity : text.entities) {
		entities.push_back(QJsonObject{
			{ u"type"_q, EntityName(entity.type()) },
			{ u"offset"_q, entity.offset() },
			{ u"length"_q, entity.length() },
			{ u"data"_q, entity.data() },
		});
	}
	object = QJsonObject{
		{ u"text"_q, text.text },
		{ u"entities"_q, entities },
	};
}

[[nodiscard]] bool DecodeTextWithEntities(
	const QJsonObject &object,
	TextWithEntities *result,
	QString *error) {
	if (!HasOnlyKnownKeys(object, { u"text"_q, u"entities"_q }, error)) {
		return false;
	}
	if (!ReadString(object, u"text"_q, &result->text, error, true)) {
		return false;
	}
	auto entities = QJsonArray();
	if (!ReadArray(object, u"entities"_q, &entities, error, true)) {
		return false;
	}
	for (const auto &value : entities) {
		if (!IsObject(value)) {
			*error = Error(u"text entity is not an object"_q);
			return false;
		}
		const auto entity = value.toObject();
		if (!HasOnlyKnownKeys(entity,
			{ u"type"_q, u"offset"_q, u"length"_q, u"data"_q }, error)) {
			return false;
		}
		QString typeName;
		QString data;
		int offset = 0;
		int length = 0;
		if (!ReadString(entity, u"type"_q, &typeName, error, true)
			|| !ReadInt(entity, u"offset"_q, &offset, error, true)
			|| !ReadInt(entity, u"length"_q, &length, error, true)
			|| !ReadString(entity, u"data"_q, &data, error, true)) {
			return false;
		}
		const auto type = ParseEntity(typeName);
		if (!type || !EntityInText(*type, offset, length, data).validForText(
				result->text.size())) {
			*error = Error(u"invalid text entity range or type"_q);
			return false;
		}
		result->entities.push_back(EntityInText(*type, offset, length, data));
	}
	return true;
}

void PutText(QJsonObject &object, const QString &key, const RichText &text) {
	auto value = QJsonObject();
	PutTextWithEntities(value, text.text);
	value.insert(u"anchor_id"_q, text.anchorId);
	auto anchors = QJsonArray();
	for (const auto &anchor : text.anchorIds) anchors.push_back(anchor);
	value.insert(u"anchor_ids"_q, anchors);
	object.insert(key, value);
}

[[nodiscard]] bool DecodeText(
	const QJsonObject &object,
	const QString &key,
	RichText *result,
	QString *error) {
	QJsonObject value;
	if (!ReadObject(object, key, &value, error, true)
		|| !HasOnlyKnownKeys(value,
			{ u"text"_q, u"entities"_q, u"anchor_id"_q, u"anchor_ids"_q },
			error)) {
		return false;
	}
	const auto textValue = QJsonObject{
		{ u"text"_q, value.value(u"text"_q) },
		{ u"entities"_q, value.value(u"entities"_q) },
	};
	if (!DecodeTextWithEntities(textValue, &result->text, error)
		|| !ReadString(value, u"anchor_id"_q, &result->anchorId, error, true)) {
		return false;
	}
	QJsonArray anchors;
	if (!ReadArray(value, u"anchor_ids"_q, &anchors, error, true)) {
		return false;
	}
	for (const auto &anchor : anchors) {
		if (!IsString(anchor) || !IsReasonableString(anchor.toString())) {
			*error = Error(u"invalid text anchor"_q);
			return false;
		}
		result->anchorIds.push_back(anchor.toString());
	}
	return true;
}

[[nodiscard]] bool AssetIndex(
	const QJsonObject &object,
	const QString &key,
	int *result,
	QString *error) {
	if (!ReadInt(object, key, result, error, true) || *result < 0) {
		*error = Error(u"invalid non-negative asset index"_q);
		return false;
	}
	return true;
}

void PutAsset(QJsonObject &object, const QString &key, int index) {
	object.insert(key, index);
}

[[nodiscard]] bool EncodeAsset(
	QJsonObject &object,
	const QString &key,
	PhotoData *photo,
	uint64 photoId,
	Fn<std::optional<int>(PhotoData*)> &asset,
	QString *error) {
	if (!photo) {
		if (photoId) *error = Error(u"unresolved photo reference"_q);
		return !photoId;
	}
	const auto index = asset(photo);
	if (!index || *index < 0) {
		*error = Error(u"photo asset is not staged"_q);
		return false;
	}
	PutAsset(object, key, *index);
	return true;
}

[[nodiscard]] bool EncodeAsset(
	QJsonObject &object,
	const QString &key,
	DocumentData *document,
	uint64 documentId,
	Fn<std::optional<int>(DocumentData*)> &asset,
	QString *error) {
	if (!document) {
		if (documentId) *error = Error(u"unresolved document reference"_q);
		return !documentId;
	}
	const auto index = asset(document);
	if (!index || *index < 0) {
		*error = Error(u"document asset is not staged"_q);
		return false;
	}
	PutAsset(object, key, *index);
	return true;
}

[[nodiscard]] bool DecodePhotoAsset(
	const QJsonObject &object,
	const QString &key,
	Fn<PhotoData*(int)> &asset,
	PhotoData **result,
	QString *error,
	bool metadataOnly) {
	int index = 0;
	if (!AssetIndex(object, key, &index, error)) return false;
	if (metadataOnly) return true;
	*result = asset(index);
	if (!*result) {
		*error = Error(u"photo asset index cannot be resolved"_q);
		return false;
	}
	return true;
}

[[nodiscard]] bool DecodeDocumentAsset(
	const QJsonObject &object,
	const QString &key,
	Fn<DocumentData*(int)> &asset,
	DocumentData **result,
	QString *error,
	bool metadataOnly) {
	int index = 0;
	if (!AssetIndex(object, key, &index, error)) return false;
	if (metadataOnly) return true;
	*result = asset(index);
	if (!*result) {
		*error = Error(u"document asset index cannot be resolved"_q);
		return false;
	}
	return true;
}

void PutButton(QJsonObject &object, const Button &button) {
	const auto &value = button.button;
	object = QJsonObject{
		{ u"text"_q, QJsonObject() },
		{ u"type"_q, int(value.type) },
		{ u"visual_color"_q, int(value.visual.color) },
		{ u"visual_icon"_q, QString::number(value.visual.iconId) },
		{ u"forward_text"_q, value.forwardText },
		{ u"data"_q, QString::fromLatin1(value.data.toBase64()) },
		{ u"peer_types"_q, int(value.peerTypes.value()) },
	};
	PutText(object, u"text"_q, button.text);
}

[[nodiscard]] bool DecodeButton(
	const QJsonObject &object,
	Button *result,
	QString *error) {
	if (!HasOnlyKnownKeys(object,
		{ u"text"_q, u"type"_q, u"visual_color"_q, u"visual_icon"_q,
			u"forward_text"_q, u"data"_q, u"peer_types"_q }, error)) {
		return false;
	}
	if (!DecodeText(object, u"text"_q, &result->text, error)) return false;
	int color = 0;
	QString visualIcon;
	int peerTypes = 0;
	QString forwardText;
	QString data;
	if (!ReadInt(object, u"visual_color"_q, &color, error, true)
		|| !ReadString(object, u"visual_icon"_q, &visualIcon, error, true)
		|| !ReadString(object, u"forward_text"_q, &forwardText, error, true)
		|| !ReadString(object, u"data"_q, &data, error, true)
		|| !ReadInt(object, u"peer_types"_q, &peerTypes, error, true)) {
		return false;
	}
	const auto decodedData = DecodeBase64(data);
	const auto type = ParseButtonType(object.value(u"type"_q));
	if (!decodedData || !type || color < 0 || color > int(HistoryMessageMarkupButton::Color::Success)) {
		*error = Error(u"invalid rich button type or color"_q);
		return false;
	}
	bool visualIconOk = false;
	const auto visualIconValue = visualIcon.toULongLong(&visualIconOk);
	if (!visualIconOk) {
		*error = Error(u"invalid rich button icon"_q);
		return false;
	}
	using PeerTypes = decltype(result->button.peerTypes);
	using PeerType = PeerTypes::Type;
	if (peerTypes < 0
		|| uint64(peerTypes) > uint64(std::numeric_limits<PeerType>::max())) {
		*error = Error(u"invalid rich button peer types"_q);
		return false;
	}
	result->button = HistoryMessageMarkupButton(
		*type,
		QString(),
		HistoryMessageMarkupButton::Visual{
			.iconId = visualIconValue,
			.color = static_cast<HistoryMessageMarkupButton::Color>(color),
		},
		*decodedData,
		forwardText);
	result->button.peerTypes = PeerTypes::from_raw(
		static_cast<PeerType>(peerTypes));
	return true;
}

void PutListItem(
	QJsonObject &object,
	const ListItem &item,
	Fn<std::optional<int>(PhotoData*)> &photoAsset,
	Fn<std::optional<int>(DocumentData*)> &documentAsset,
	QString *error,
	int depth);

void PutBlock(
	QJsonObject &object,
	const Block &block,
	Fn<std::optional<int>(PhotoData*)> &photoAsset,
	Fn<std::optional<int>(DocumentData*)> &documentAsset,
	QString *error,
	int depth);

void PutBlockScalars(QJsonObject &object, const Block &block) {
	object = QJsonObject{
		{ u"kind"_q, KindName(block.kind) },
		{ u"anchor_id"_q, block.anchorId },
		{ u"language"_q, block.language },
		{ u"formula"_q, block.formula },
		{ u"url"_q, block.url },
		{ u"html"_q, QString::fromLatin1(block.html.toBase64()) },
		{ u"author"_q, block.author },
		{ u"username"_q, block.username },
		{ u"channel_title"_q, block.channelTitle },
		{ u"channel_id"_q, QString::number(block.channelId) },
		{ u"audio_title"_q, block.audioTitle },
		{ u"audio_performer"_q, block.audioPerformer },
		{ u"file_name"_q, block.fileName },
		{ u"date"_q, int(block.date) },
		{ u"audio_duration"_q, block.audioDuration },
		{ u"heading_level"_q, block.headingLevel },
		{ u"width"_q, block.width },
		{ u"height"_q, block.height },
		{ u"zoom"_q, block.zoom },
		{ u"full_width"_q, block.fullWidth },
		{ u"fixed_height"_q, block.fixedHeight },
		{ u"allow_scrolling"_q, block.allowScrolling },
		{ u"autoplay"_q, block.autoplay },
		{ u"loop"_q, block.loop },
		{ u"spoiler"_q, block.spoiler },
		{ u"open"_q, block.open },
		{ u"bordered"_q, block.bordered },
		{ u"striped"_q, block.striped },
		{ u"compact"_q, block.compact },
		{ u"pullquote"_q, block.pullquote },
		{ u"collapsed"_q, block.collapsed },
		{ u"list_kind"_q, ListKindName(block.listKind) },
		{ u"media_intent"_q, MediaIntentName(block.mediaIntent) },
		{ u"button_alignment"_q, ButtonAlignmentName(block.buttonAlignment) },
		{ u"latitude"_q, block.latitude },
		{ u"longitude"_q, block.longitude },
	};
	PutText(object, u"text"_q, block.text);
	PutText(object, u"caption"_q, block.caption);
	auto ordered = QJsonObject{
		{ u"reversed"_q, block.orderedList.reversed },
		{ u"start"_q, block.orderedList.start
			? QJsonValue(*block.orderedList.start) : QJsonValue(QJsonValue::Null) },
		{ u"type"_q, block.orderedList.type
			? QJsonValue(*block.orderedList.type) : QJsonValue(QJsonValue::Null) },
	};
	object.insert(u"ordered_list"_q, ordered);
}

void PutListItem(
	QJsonObject &object,
	const ListItem &item,
	Fn<std::optional<int>(PhotoData*)> &photoAsset,
	Fn<std::optional<int>(DocumentData*)> &documentAsset,
	QString *error,
	int depth) {
	object = QJsonObject{
		{ u"task_state"_q, TaskStateName(item.taskState) },
		{ u"anchor_id"_q, item.anchorId },
		{ u"number"_q, QJsonObject{
			{ u"num"_q, item.number.num
				? QJsonValue(*item.number.num) : QJsonValue(QJsonValue::Null) },
			{ u"value"_q, item.number.value
				? QJsonValue(*item.number.value) : QJsonValue(QJsonValue::Null) },
			{ u"type"_q, item.number.type
				? QJsonValue(*item.number.type) : QJsonValue(QJsonValue::Null) },
		} },
	};
	PutText(object, u"text"_q, item.text);
	auto blocks = QJsonArray();
	for (const auto &block : item.blocks) {
		auto value = QJsonObject();
		PutBlock(value, block, photoAsset, documentAsset, error, depth + 1);
		if (!error->isEmpty()) return;
		blocks.push_back(value);
	}
	object.insert(u"blocks"_q, blocks);
}

void PutBlock(
	QJsonObject &object,
	const Block &block,
	Fn<std::optional<int>(PhotoData*)> &photoAsset,
	Fn<std::optional<int>(DocumentData*)> &documentAsset,
	QString *error,
	int depth) {
	if (depth > kMaxDepth) {
		*error = Error(u"rich block nesting exceeds the portable limit"_q);
		return;
	}
	if (block.kind == BlockKind::Unsupported) {
		*error = Error(u"unsupported rich block cannot be edited portably"_q);
		return;
	}
	PutBlockScalars(object, block);
	if ((block.photo || block.photoId) && block.kind != BlockKind::Photo) {
		if (!EncodeAsset(object, u"photo_asset"_q, block.photo, block.photoId,
				photoAsset, error)) return;
	}
	if (block.kind == BlockKind::Photo) {
		if (!block.photo && !block.photoId) {
			*error = Error(u"photo block has no asset reference"_q);
			return;
		}
		if (!EncodeAsset(object, u"asset"_q, block.photo, block.photoId,
				photoAsset, error)) return;
	} else if (block.kind == BlockKind::Video
		|| block.kind == BlockKind::Audio
		|| block.kind == BlockKind::File) {
		if (!block.document && !block.documentId) {
			*error = Error(u"document block has no asset reference"_q);
			return;
		}
		if (!EncodeAsset(object, u"asset"_q, block.document, block.documentId,
				documentAsset, error)) return;
	}
	auto blocks = QJsonArray();
	for (const auto &child : block.blocks) {
		auto value = QJsonObject();
		PutBlock(value, child, photoAsset, documentAsset, error, depth + 1);
		if (!error->isEmpty()) return;
		blocks.push_back(value);
	}
	object.insert(u"blocks"_q, blocks);
	auto listItems = QJsonArray();
	for (const auto &item : block.listItems) {
		auto value = QJsonObject();
		PutListItem(value, item, photoAsset, documentAsset, error, depth + 1);
		if (!error->isEmpty()) return;
		listItems.push_back(value);
	}
	object.insert(u"list_items"_q, listItems);
	auto mediaItems = QJsonArray();
	for (const auto &item : block.mediaItems) {
		auto value = QJsonObject{
			{ u"kind"_q, KindName(item.kind) },
			{ u"width"_q, item.width },
			{ u"height"_q, item.height },
			{ u"autoplay"_q, item.autoplay },
			{ u"loop"_q, item.loop },
			{ u"spoiler"_q, item.spoiler },
		};
		if (item.kind == BlockKind::Photo) {
			if (!item.photo && !item.photoId) {
				*error = Error(u"grouped photo has no asset reference"_q);
				return;
			}
			if (!EncodeAsset(value, u"asset"_q, item.photo, item.photoId,
					photoAsset, error)) return;
		} else if (item.kind == BlockKind::Video
			|| item.kind == BlockKind::Audio
			|| item.kind == BlockKind::File) {
			if (!item.document && !item.documentId) {
				*error = Error(u"grouped document has no asset reference"_q);
				return;
			}
			if (!EncodeAsset(value, u"asset"_q, item.document, item.documentId,
					documentAsset, error)) return;
		}
		mediaItems.push_back(value);
	}
	object.insert(u"media_items"_q, mediaItems);
	auto rows = QJsonArray();
	for (const auto &row : block.tableRows) {
		auto cells = QJsonArray();
		for (const auto &cell : row.cells) {
			auto value = QJsonObject{
				{ u"colspan"_q, cell.colspan },
				{ u"rowspan"_q, cell.rowspan },
				{ u"header"_q, cell.header },
				{ u"alignment"_q, AlignmentName(cell.alignment) },
				{ u"vertical_alignment"_q,
					VerticalAlignmentName(cell.verticalAlignment) },
			};
			PutText(value, u"text"_q, cell.text);
			cells.push_back(value);
		}
		rows.push_back(cells);
	}
	object.insert(u"table_rows"_q, rows);
	auto articles = QJsonArray();
	for (const auto &article : block.relatedArticles) {
		auto value = QJsonObject{
			{ u"url"_q, article.url },
			{ u"webpage_id"_q, QString::number(article.webpageId) },
			{ u"title"_q, article.title },
			{ u"description"_q, article.description },
			{ u"author"_q, article.author },
			{ u"published_date"_q, int(article.publishedDate) },
		};
		if (!EncodeAsset(value, u"asset"_q, article.photo, article.photoId,
				photoAsset, error)) return;
		articles.push_back(value);
	}
	object.insert(u"related_articles"_q, articles);
	auto buttons = QJsonArray();
	for (const auto &button : block.buttons) {
		auto value = QJsonObject();
		PutButton(value, button);
		buttons.push_back(value);
	}
	object.insert(u"buttons"_q, buttons);
}

[[nodiscard]] bool DecodeOrderedList(
	const QJsonObject &object,
	Iv::RichPage::OrderedListData *result,
	QString *error) {
	if (!HasOnlyKnownKeys(object, { u"reversed"_q, u"start"_q, u"type"_q }, error)) {
		return false;
	}
	if (!ReadBool(object, u"reversed"_q, &result->reversed, error, true)) return false;
	const auto start = object.value(u"start"_q);
	if (!start.isUndefined() && start.type() != QJsonValue::Null) {
		if (!IsInteger(start)) {
			*error = Error(u"invalid ordered list start"_q);
			return false;
		}
		result->start = start.toInt();
	}
	const auto type = object.value(u"type"_q);
	if (!type.isUndefined() && type.type() != QJsonValue::Null) {
		if (!IsString(type) || !IsReasonableString(type.toString())) {
			*error = Error(u"invalid ordered list type"_q);
			return false;
		}
		result->type = type.toString();
	}
	return true;
}

[[nodiscard]] bool DecodeNumberData(
	const QJsonObject &object,
	Iv::RichPage::OrderedListItemData *result,
	QString *error) {
	if (!HasOnlyKnownKeys(object, { u"num"_q, u"value"_q, u"type"_q }, error)) {
		return false;
	}
	const auto num = object.value(u"num"_q);
	if (!num.isUndefined() && num.type() != QJsonValue::Null) {
		if (!IsString(num) || !IsReasonableString(num.toString())) {
			*error = Error(u"invalid ordered list marker"_q);
			return false;
		}
		result->num = num.toString();
	}
	const auto value = object.value(u"value"_q);
	if (!value.isUndefined() && value.type() != QJsonValue::Null) {
		if (!IsInteger(value)) {
			*error = Error(u"invalid ordered list value"_q);
			return false;
		}
		result->value = value.toInt();
	}
	const auto type = object.value(u"type"_q);
	if (!type.isUndefined() && type.type() != QJsonValue::Null) {
		if (!IsString(type) || !IsReasonableString(type.toString())) {
			*error = Error(u"invalid ordered list item type"_q);
			return false;
		}
		result->type = type.toString();
	}
	return true;
}

[[nodiscard]] bool DecodeListItem(
	const QJsonObject &object,
	ListItem *result,
	Fn<PhotoData*(int)> &photoAsset,
	Fn<DocumentData*(int)> &documentAsset,
	QString *error,
	int depth,
	bool metadataOnly);

[[nodiscard]] bool DecodeBlock(
	const QJsonObject &object,
	Block *result,
	Fn<PhotoData*(int)> &photoAsset,
	Fn<DocumentData*(int)> &documentAsset,
	QString *error,
	int depth,
	bool metadataOnly);

[[nodiscard]] bool DecodeBlockScalars(
	const QJsonObject &object,
	Block *result,
	QString *error) {
	QString kind;
	QString channelId;
	if (!ReadString(object, u"kind"_q, &kind, error, true)) return false;
	const auto parsedKind = ParseKind(kind);
	if (!parsedKind) {
		*error = Error(u"unknown rich block kind"_q);
		return false;
	}
	result->kind = *parsedKind;
	if (result->kind == BlockKind::Unsupported) {
		*error = Error(u"unsupported rich block cannot be edited portably"_q);
		return false;
	}
	if (!ReadString(object, u"anchor_id"_q, &result->anchorId, error, true)
		|| !DecodeText(object, u"text"_q, &result->text, error)
		|| !DecodeText(object, u"caption"_q, &result->caption, error)
		|| !ReadString(object, u"language"_q, &result->language, error, true)
		|| !ReadString(object, u"formula"_q, &result->formula, error, true)
		|| !ReadString(object, u"url"_q, &result->url, error, true)
		|| !ReadString(object, u"author"_q, &result->author, error, true)
		|| !ReadString(object, u"username"_q, &result->username, error, true)
		|| !ReadString(object, u"channel_title"_q, &result->channelTitle, error, true)
		|| !ReadString(object, u"audio_title"_q, &result->audioTitle, error, true)
		|| !ReadString(object, u"audio_performer"_q, &result->audioPerformer, error, true)
		|| !ReadString(object, u"file_name"_q, &result->fileName, error, true)
		|| !ReadString(object, u"channel_id"_q, &channelId, error, true)
		|| !ReadInt(object, u"audio_duration"_q, &result->audioDuration, error, true)
		|| !ReadInt(object, u"heading_level"_q, &result->headingLevel, error, true)
		|| !ReadInt(object, u"width"_q, &result->width, error, true)
		|| !ReadInt(object, u"height"_q, &result->height, error, true)
		|| !ReadInt(object, u"zoom"_q, &result->zoom, error, true)
		|| !ReadBool(object, u"full_width"_q, &result->fullWidth, error, true)
		|| !ReadBool(object, u"fixed_height"_q, &result->fixedHeight, error, true)
		|| !ReadBool(object, u"allow_scrolling"_q, &result->allowScrolling, error, true)
		|| !ReadBool(object, u"autoplay"_q, &result->autoplay, error, true)
		|| !ReadBool(object, u"loop"_q, &result->loop, error, true)
		|| !ReadBool(object, u"spoiler"_q, &result->spoiler, error, true)
		|| !ReadBool(object, u"open"_q, &result->open, error, true)
		|| !ReadBool(object, u"bordered"_q, &result->bordered, error, true)
		|| !ReadBool(object, u"striped"_q, &result->striped, error, true)
		|| !ReadBool(object, u"compact"_q, &result->compact, error, true)
		|| !ReadBool(object, u"pullquote"_q, &result->pullquote, error, true)
		|| !ReadBool(object, u"collapsed"_q, &result->collapsed, error, true)) {
		return false;
	}
	bool channelOk = false;
	const auto parsedChannelId = channelId.toULongLong(&channelOk);
	if (!channelOk) {
		*error = Error(u"invalid channel id"_q);
		return false;
	}
	result->channelId = parsedChannelId;
	int date = 0;
	if (!ReadInt(object, u"date"_q, &date, error, true)) {
		return false;
	}
	result->date = TimeId(date);
	QString listKind;
	QString mediaIntent;
	QString buttonAlignment;
	if (!ReadString(object, u"list_kind"_q, &listKind, error, true)
		|| !ReadString(object, u"media_intent"_q, &mediaIntent, error, true)
		|| !ReadString(object, u"button_alignment"_q, &buttonAlignment, error, true)) {
		return false;
	}
	const auto parsedList = ParseListKind(listKind);
	const auto parsedIntent = ParseMediaIntent(mediaIntent);
	const auto parsedAlignment = ParseButtonAlignment(buttonAlignment);
	if (!parsedList || !parsedIntent || !parsedAlignment) {
		*error = Error(u"invalid rich block option"_q);
		return false;
	}
	result->listKind = *parsedList;
	result->mediaIntent = *parsedIntent;
	result->buttonAlignment = *parsedAlignment;
	QJsonObject ordered;
	if (!ReadObject(object, u"ordered_list"_q, &ordered, error, true)
		|| !DecodeOrderedList(ordered, &result->orderedList, error)) return false;
	if (!ReadDouble(object, u"latitude"_q, &result->latitude, error, true)
		|| !ReadDouble(object, u"longitude"_q, &result->longitude, error, true)) {
		return false;
	}
	const auto html = object.value(u"html"_q);
	if (!IsString(html) || !IsReasonableString(html.toString())) {
		*error = Error(u"invalid rich block html"_q);
		return false;
	}
	const auto decodedHtml = DecodeBase64(html.toString());
	if (!decodedHtml) {
		*error = Error(u"invalid rich block html encoding"_q);
		return false;
	}
	result->html = *decodedHtml;
	return true;
}

[[nodiscard]] bool DecodeListItem(
	const QJsonObject &object,
	ListItem *result,
	Fn<PhotoData*(int)> &photoAsset,
	Fn<DocumentData*(int)> &documentAsset,
	QString *error,
	int depth,
	bool metadataOnly) {
	if (depth > kMaxDepth
		|| !HasOnlyKnownKeys(object,
			{ u"task_state"_q, u"number"_q, u"anchor_id"_q, u"text"_q,
				u"blocks"_q }, error)) return false;
	QString state;
	if (!ReadString(object, u"task_state"_q, &state, error, true)
		|| !ReadString(object, u"anchor_id"_q, &result->anchorId, error, true)
		|| !DecodeText(object, u"text"_q, &result->text, error)) return false;
	const auto parsedState = ParseTaskState(state);
	if (!parsedState) {
		*error = Error(u"invalid task state"_q);
		return false;
	}
	result->taskState = *parsedState;
	QJsonObject number;
	if (!ReadObject(object, u"number"_q, &number, error, true)
		|| !DecodeNumberData(number, &result->number, error)) return false;
	QJsonArray blocks;
	if (!ReadArray(object, u"blocks"_q, &blocks, error, true)) return false;
	for (const auto &value : blocks) {
		if (!IsObject(value)) {
			*error = Error(u"list child block is not an object"_q);
			return false;
		}
		Block block;
		if (!DecodeBlock(value.toObject(), &block, photoAsset, documentAsset,
				error, depth + 1, metadataOnly)) return false;
		result->blocks.push_back(std::move(block));
	}
	return true;
}

[[nodiscard]] bool DecodeBlock(
	const QJsonObject &object,
	Block *result,
	Fn<PhotoData*(int)> &photoAsset,
	Fn<DocumentData*(int)> &documentAsset,
	QString *error,
	int depth,
	bool metadataOnly) {
	if (depth > kMaxDepth) {
		*error = Error(u"rich block nesting exceeds the portable limit"_q);
		return false;
	}
	if (!HasOnlyKnownKeys(object,
		{ u"kind"_q, u"anchor_id"_q, u"text"_q, u"caption"_q,
			u"language"_q, u"formula"_q, u"url"_q, u"html"_q,
			u"author"_q, u"username"_q, u"channel_title"_q, u"channel_id"_q, u"audio_title"_q,
			u"audio_performer"_q, u"file_name"_q, u"date"_q,
			u"audio_duration"_q, u"heading_level"_q, u"width"_q, u"height"_q,
			u"zoom"_q, u"full_width"_q, u"fixed_height"_q,
			u"allow_scrolling"_q, u"autoplay"_q, u"loop"_q, u"spoiler"_q,
			u"open"_q, u"bordered"_q, u"striped"_q, u"compact"_q,
			u"pullquote"_q, u"collapsed"_q, u"list_kind"_q,
			u"media_intent"_q, u"button_alignment"_q, u"latitude"_q,
			u"longitude"_q, u"ordered_list"_q, u"asset"_q,
			u"photo_asset"_q, u"blocks"_q, u"list_items"_q, u"media_items"_q,
			u"table_rows"_q, u"related_articles"_q, u"buttons"_q }, error)) {
		return false;
	}
	if (!DecodeBlockScalars(object, result, error)) return false;
	const auto photoAssetValue = object.value(u"photo_asset"_q);
	if (!photoAssetValue.isUndefined()) {
		if (!DecodePhotoAsset(object, u"photo_asset"_q, photoAsset, &result->photo, error, metadataOnly)) return false;
		result->photoId = result->photo ? result->photo->id : 0;
	}
	if (result->kind == BlockKind::Photo) {
		if (!DecodePhotoAsset(object, u"asset"_q, photoAsset, &result->photo, error, metadataOnly)) return false;
		result->photoId = result->photo ? result->photo->id : 0;
	} else if (result->kind == BlockKind::Video
		|| result->kind == BlockKind::Audio
		|| result->kind == BlockKind::File) {
		if (!DecodeDocumentAsset(object, u"asset"_q, documentAsset, &result->document, error, metadataOnly)) return false;
		result->documentId = result->document ? result->document->id : 0;
	}
	QJsonArray blocks;
	QJsonArray listItems;
	QJsonArray mediaItems;
	QJsonArray rows;
	QJsonArray articles;
	QJsonArray buttons;
	if (!ReadArray(object, u"blocks"_q, &blocks, error, true)
		|| !ReadArray(object, u"list_items"_q, &listItems, error, true)
		|| !ReadArray(object, u"media_items"_q, &mediaItems, error, true)
		|| !ReadArray(object, u"table_rows"_q, &rows, error, true)
		|| !ReadArray(object, u"related_articles"_q, &articles, error, true)
		|| !ReadArray(object, u"buttons"_q, &buttons, error, true)) return false;
	for (const auto &value : blocks) {
		if (!IsObject(value)) {
			*error = Error(u"child block is not an object"_q);
			return false;
		}
		Block child;
		if (!DecodeBlock(value.toObject(), &child, photoAsset, documentAsset,
				error, depth + 1, metadataOnly)) return false;
		result->blocks.push_back(std::move(child));
	}
	for (const auto &value : listItems) {
		if (!IsObject(value)) {
			*error = Error(u"list item is not an object"_q);
			return false;
		}
		ListItem item;
		if (!DecodeListItem(value.toObject(), &item, photoAsset, documentAsset,
			error, depth + 1, metadataOnly)) return false;
		result->listItems.push_back(std::move(item));
	}
	for (const auto &value : mediaItems) {
		if (!IsObject(value)) {
			*error = Error(u"grouped media item is not an object"_q);
			return false;
		}
		const auto itemObject = value.toObject();
		if (!HasOnlyKnownKeys(itemObject,
			{ u"kind"_q, u"width"_q, u"height"_q, u"autoplay"_q,
				u"loop"_q, u"spoiler"_q, u"asset"_q }, error)) return false;
		QString kind;
		GroupedMediaItem item;
		if (!ReadString(itemObject, u"kind"_q, &kind, error, true)
			|| !ReadInt(itemObject, u"width"_q, &item.width, error, true)
			|| !ReadInt(itemObject, u"height"_q, &item.height, error, true)
			|| !ReadBool(itemObject, u"autoplay"_q, &item.autoplay, error, true)
			|| !ReadBool(itemObject, u"loop"_q, &item.loop, error, true)
			|| !ReadBool(itemObject, u"spoiler"_q, &item.spoiler, error, true)) return false;
		const auto parsedKind = ParseKind(kind);
		if (!parsedKind) {
			*error = Error(u"invalid grouped media kind"_q);
			return false;
		}
		item.kind = *parsedKind;
		if (item.kind == BlockKind::Photo) {
			if (!DecodePhotoAsset(itemObject, u"asset"_q, photoAsset, &item.photo, error, metadataOnly)) return false;
			item.photoId = item.photo ? item.photo->id : 0;
		} else if (item.kind == BlockKind::Video
			|| item.kind == BlockKind::Audio
			|| item.kind == BlockKind::File) {
			if (!DecodeDocumentAsset(itemObject, u"asset"_q, documentAsset, &item.document, error, metadataOnly)) return false;
			item.documentId = item.document ? item.document->id : 0;
		} else {
			*error = Error(u"unsupported grouped media kind"_q);
			return false;
		}
		result->mediaItems.push_back(item);
	}
	for (const auto &rowValue : rows) {
		if (!IsArray(rowValue) || rowValue.toArray().size() > kMaxArray) {
			*error = Error(u"invalid table row"_q);
			return false;
		}
		TableRow row;
		for (const auto &cellValue : rowValue.toArray()) {
			if (!IsObject(cellValue)) {
				*error = Error(u"invalid table cell"_q);
				return false;
			}
			const auto cellObject = cellValue.toObject();
			if (!HasOnlyKnownKeys(cellObject,
				{ u"colspan"_q, u"rowspan"_q, u"header"_q, u"alignment"_q,
					u"vertical_alignment"_q, u"text"_q }, error)) return false;
			TableCell cell;
			QString alignment;
			QString vertical;
			if (!ReadInt(cellObject, u"colspan"_q, &cell.colspan, error, true)
				|| !ReadInt(cellObject, u"rowspan"_q, &cell.rowspan, error, true)
				|| !ReadBool(cellObject, u"header"_q, &cell.header, error, true)
				|| !ReadString(cellObject, u"alignment"_q, &alignment, error, true)
				|| !ReadString(cellObject, u"vertical_alignment"_q, &vertical, error, true)
				|| !DecodeText(cellObject, u"text"_q, &cell.text, error)) return false;
			const auto parsedAlignment = ParseAlignment(alignment);
			const auto parsedVertical = ParseVerticalAlignment(vertical);
		if (!parsedAlignment || !parsedVertical || cell.colspan < 1 || cell.rowspan < 1) {
			*error = Error(u"invalid table cell dimensions or alignment"_q);
			return false;
		}
		cell.alignment = *parsedAlignment;
		cell.verticalAlignment = *parsedVertical;
		row.cells.push_back(std::move(cell));
		}
		result->tableRows.push_back(std::move(row));
	}
	for (const auto &articleValue : articles) {
		if (!IsObject(articleValue)) {
			*error = Error(u"related article is not an object"_q);
			return false;
		}
		const auto articleObject = articleValue.toObject();
		if (!HasOnlyKnownKeys(articleObject,
			{ u"url"_q, u"webpage_id"_q, u"title"_q, u"description"_q, u"author"_q,
				u"published_date"_q, u"asset"_q }, error)) return false;
		Iv::RichPage::RelatedArticle article;
		QString webpageId;
		int published = 0;
		if (!ReadString(articleObject, u"url"_q, &article.url, error, true)
			|| !ReadString(articleObject, u"webpage_id"_q, &webpageId, error, true)
			|| !ReadString(articleObject, u"title"_q, &article.title, error, true)
			|| !ReadString(articleObject, u"description"_q, &article.description, error, true)
			|| !ReadString(articleObject, u"author"_q, &article.author, error, true)
			|| !ReadInt(articleObject, u"published_date"_q, &published, error, true)) return false;
		const auto articleAsset = articleObject.value(u"asset"_q);
		if (!articleAsset.isUndefined()) {
			if (!DecodePhotoAsset(articleObject, u"asset"_q, photoAsset, &article.photo, error, metadataOnly)) return false;
			article.photoId = article.photo ? article.photo->id : 0;
		}
		bool webpageOk = false;
		article.webpageId = webpageId.toULongLong(&webpageOk);
		if (!webpageOk) {
			*error = Error(u"invalid related article id"_q);
			return false;
		}
		article.publishedDate = TimeId(published);
		result->relatedArticles.push_back(std::move(article));
	}
	for (const auto &buttonValue : buttons) {
		if (!IsObject(buttonValue)) {
			*error = Error(u"rich button is not an object"_q);
			return false;
		}
		Button button;
		if (!DecodeButton(buttonValue.toObject(), &button, error)) return false;
		result->buttons.push_back(std::move(button));
	}
	return true;
}

void CollectPhoto(
	LocalQuickReplyRichMedia *result,
	PhotoData *photo) {
	if (photo && std::find(result->photos.begin(), result->photos.end(), photo)
			== result->photos.end()) {
		result->photos.push_back(photo);
	}
}

void CollectDocument(
	LocalQuickReplyRichMedia *result,
	DocumentData *document) {
	if (document && std::find(result->documents.begin(), result->documents.end(), document)
			== result->documents.end()) {
		result->documents.push_back(document);
	}
}

[[nodiscard]] bool CollectBlock(
	const Block &block,
	LocalQuickReplyRichMedia *result,
	QString *error,
	int depth) {
	if (depth > kMaxDepth) {
		*error = Error(u"rich block nesting exceeds the portable limit"_q);
		return false;
	}
	if (block.photoId && !block.photo) {
		*error = Error(u"rich block has an unresolved photo id"_q);
		return false;
	}
	if (block.documentId && !block.document) {
		*error = Error(u"rich block has an unresolved document id"_q);
		return false;
	}
	if ((block.kind == BlockKind::Photo) && !block.photo) {
		*error = Error(u"photo block has no resolved media"_q);
		return false;
	}
	if ((block.kind == BlockKind::Video || block.kind == BlockKind::Audio
		|| block.kind == BlockKind::File) && !block.document) {
		*error = Error(u"document block has no resolved media"_q);
		return false;
	}
	CollectPhoto(result, block.photo);
	CollectDocument(result, block.document);
	for (const auto &child : block.blocks) {
		if (!CollectBlock(child, result, error, depth + 1)) return false;
	}
	for (const auto &item : block.listItems) {
		for (const auto &child : item.blocks) {
			if (!CollectBlock(child, result, error, depth + 1)) return false;
		}
	}
	for (const auto &item : block.mediaItems) {
		if (item.photoId && !item.photo) {
			*error = Error(u"grouped media has an unresolved photo id"_q);
			return false;
		}
		if (item.documentId && !item.document) {
			*error = Error(u"grouped media has an unresolved document id"_q);
			return false;
		}
		if ((item.kind == BlockKind::Photo) && !item.photo) {
			*error = Error(u"grouped photo has no resolved media"_q);
			return false;
		}
		if ((item.kind == BlockKind::Video || item.kind == BlockKind::Audio
			|| item.kind == BlockKind::File) && !item.document) {
			*error = Error(u"grouped document has no resolved media"_q);
			return false;
		}
		CollectPhoto(result, item.photo);
		CollectDocument(result, item.document);
	}
	for (const auto &article : block.relatedArticles) {
		if (article.photoId && !article.photo) {
			*error = Error(u"related article has an unresolved photo id"_q);
			return false;
		}
		CollectPhoto(result, article.photo);
	}
	return true;
}

} // namespace

QString ValidateLocalQuickReplyRich(const QJsonObject &object) {
	if (QJsonDocument(object).toJson(QJsonDocument::Compact).size()
		> kMaxJsonBytes) {
		return Error(u"rich JSON exceeds the portable size limit"_q);
	}
	const auto allowed = QStringList{
		u"version"_q, u"url"_q, u"rtl"_q, u"part"_q, u"views"_q,
		u"blocks"_q, u"kind"_q, u"anchor_id"_q, u"anchor_ids"_q,
		u"text"_q, u"entities"_q, u"type"_q, u"offset"_q, u"length"_q,
		u"data"_q, u"caption"_q, u"language"_q, u"formula"_q, u"url"_q,
		u"html"_q, u"author"_q, u"username"_q, u"channel_title"_q,
		u"channel_id"_q, u"audio_title"_q, u"audio_performer"_q,
		u"file_name"_q, u"date"_q, u"audio_duration"_q, u"heading_level"_q,
		u"width"_q, u"height"_q, u"zoom"_q, u"full_width"_q,
		u"fixed_height"_q, u"allow_scrolling"_q, u"autoplay"_q,
		u"loop"_q, u"spoiler"_q, u"open"_q, u"bordered"_q,
		u"striped"_q, u"compact"_q, u"pullquote"_q, u"collapsed"_q,
		u"list_kind"_q, u"media_intent"_q, u"button_alignment"_q,
		u"latitude"_q, u"longitude"_q, u"ordered_list"_q, u"reversed"_q,
		u"start"_q, u"asset"_q, u"photo_asset"_q, u"blocks"_q,
		u"list_items"_q, u"task_state"_q, u"number"_q, u"num"_q,
		u"value"_q, u"media_items"_q, u"table_rows"_q, u"colspan"_q,
		u"rowspan"_q, u"header"_q, u"alignment"_q, u"vertical_alignment"_q,
		u"related_articles"_q, u"webpage_id"_q, u"title"_q, u"description"_q,
		u"published_date"_q, u"buttons"_q, u"visual_color"_q,
		u"visual_icon"_q, u"forward_text"_q, u"peer_types"_q,
	};
	const auto version = object.value(u"version"_q);
	if (!IsInteger(version) || version.toInt() != kVersion) {
		return Error(u"unsupported rich codec version"_q);
	}
	std::function<QString(const QJsonValue &, int)> visit;
	visit = [&](const QJsonValue &value, int depth) -> QString {
		if (depth > (kMaxDepth * 4 + 8)) return Error(u"rich JSON nesting exceeds the portable limit"_q);
		if (value.type() == QJsonValue::Array) {
			const auto array = value.toArray();
			if (array.size() > kMaxArray) return Error(u"rich JSON array is too large"_q);
			for (const auto &child : array) {
				if (const auto error = visit(child, depth + 1); !error.isEmpty()) return error;
			}
		} else if (value.type() == QJsonValue::Object) {
			const auto object = value.toObject();
			for (const auto &key : object.keys()) {
				if (!allowed.contains(key)) return Error(u"unknown rich field '"_q + key + u"'"_q);
			}
			if (object.contains(u"kind"_q)) {
				const auto kind = object.value(u"kind"_q);
				const auto parsed = IsString(kind) ? ParseKind(kind.toString()) : std::nullopt;
				if (!parsed || *parsed == BlockKind::Unsupported) {
					return Error(u"unsupported rich block kind"_q);
				}
			}
			if (object.contains(u"offset"_q) && object.contains(u"length"_q)
				&& object.contains(u"type"_q)) {
				const auto type = object.value(u"type"_q);
				if (!IsString(type) || !ParseEntity(type.toString())) {
					return Error(u"invalid rich text entity type"_q);
				}
			}
			if (object.contains(u"visual_color"_q)) {
				if (!ParseButtonType(object.value(u"type"_q))) {
					return Error(u"invalid rich button type"_q);
				}
			}
			for (const auto &child : object) {
				if (const auto error = visit(child, depth + 1); !error.isEmpty()) return error;
			}
		}
		return QString();
	};
	return visit(object, 0);
}

std::variant<QJsonObject, QString> EncodeLocalQuickReplyRich(
	const Iv::RichPage &page,
	Fn<std::optional<int>(PhotoData*)> photoAsset,
	Fn<std::optional<int>(DocumentData*)> documentAsset) {
	if (const auto limit = Iv::ValidateRichMessage(
			page, Iv::RichMessageLimits()); limit.has_value()) {
		return Error(u"rich page exceeds native limits"_q);
	}
	if (page.blocks.size() > kMaxArray) {
		return Error(u"rich page contains too many blocks"_q);
	}
	QString error;
	auto blocks = QJsonArray();
	for (const auto &block : page.blocks) {
		auto value = QJsonObject();
		PutBlock(value, block, photoAsset, documentAsset, &error, 1);
		if (!error.isEmpty()) return error;
		blocks.push_back(value);
	}
	auto result = QJsonObject{
		{ u"version"_q, kVersion },
		{ u"url"_q, page.url },
		{ u"rtl"_q, page.rtl },
		{ u"part"_q, page.part },
		{ u"views"_q, page.views },
		{ u"blocks"_q, blocks },
	};
	if (QJsonDocument(result).toJson(QJsonDocument::Compact).size()
		> kMaxJsonBytes) {
		return Error(u"rich JSON exceeds the portable size limit"_q);
	}
	return result;
}

namespace {

std::variant<std::shared_ptr<Iv::RichPage>, QString> DecodeRich(
	const QJsonObject &object,
	Fn<PhotoData*(int)> photoAsset,
	Fn<DocumentData*(int)> documentAsset,
	bool metadataOnly) {
	QString error;
	if (!(error = ValidateLocalQuickReplyRich(object)).isEmpty()) return error;
	if (!HasOnlyKnownKeys(object,
		{ u"version"_q, u"url"_q, u"rtl"_q, u"part"_q, u"views"_q,
			u"blocks"_q }, &error)) {
		return error;
	}
	int version = 0;
	if (!ReadInt(object, u"version"_q, &version, &error, true)
		|| version != kVersion) {
		return Error(u"unsupported rich codec version"_q);
	}
	auto result = std::make_shared<Iv::RichPage>();
	if (!ReadString(object, u"url"_q, &result->url, &error, true)
		|| !ReadBool(object, u"rtl"_q, &result->rtl, &error, true)
		|| !ReadBool(object, u"part"_q, &result->part, &error, true)
		|| !ReadInt(object, u"views"_q, &result->views, &error, true)) return error;
	QJsonArray blocks;
	if (!ReadArray(object, u"blocks"_q, &blocks, &error, true)) return error;
	for (const auto &value : blocks) {
		if (!IsObject(value)) return Error(u"rich block is not an object"_q);
		Block block;
		if (!DecodeBlock(value.toObject(), &block, photoAsset, documentAsset,
				&error, 1, metadataOnly)) return error;
		result->blocks.push_back(std::move(block));
	}
	if (Iv::ValidateRichMessage(*result, Iv::RichMessageLimits()).has_value()) {
		return Error(u"decoded rich page exceeds native limits"_q);
	}
	return result;
}

[[nodiscard]] bool WalkTextSlot(
		QJsonObject &object,
		const QString &key,
		Fn<bool(TextWithEntities &)> &process,
		bool transform,
		QString *error) {
	auto text = RichText();
	if (!DecodeText(object, key, &text, error)) return false;
	if (!process(text.text)) {
		*error = Error(u"rich text operation was rejected"_q);
		return false;
	}
	if (transform) {
		auto encoded = QJsonObject();
		PutTextWithEntities(encoded, text.text);
		auto value = object.value(key).toObject();
		value.insert(u"text"_q, encoded.value(u"text"_q));
		value.insert(u"entities"_q, encoded.value(u"entities"_q));
		object.insert(key, value);
	}
	return true;
}

[[nodiscard]] bool WalkBlockText(
	QJsonObject &object,
	Fn<bool(TextWithEntities &)> &process,
	bool transform,
	QString *error,
	int *fixedTextLength);

[[nodiscard]] bool WalkChildText(
		QJsonObject &object,
		Fn<bool(TextWithEntities &)> &process,
		bool transform,
		QString *error,
		int *fixedTextLength) {
	auto blocks = object.value(u"blocks"_q).toArray();
	for (auto i = 0; i != blocks.size(); ++i) {
		auto block = blocks[i].toObject();
		if (!WalkBlockText(block, process, transform, error, fixedTextLength)) return false;
		if (transform) blocks[i] = block;
	}
	if (transform) object.insert(u"blocks"_q, blocks);
	return true;
}

[[nodiscard]] bool WalkBlockText(
		QJsonObject &object,
		Fn<bool(TextWithEntities &)> &process,
		bool transform,
		QString *error,
		int *fixedTextLength) {
	if (fixedTextLength) {
		*fixedTextLength += int(object.value(u"formula"_q).toString().size());
	}
	if (!WalkTextSlot(object, u"text"_q, process, transform, error)
		|| !WalkTextSlot(object, u"caption"_q, process, transform, error)
		|| !WalkChildText(object, process, transform, error, fixedTextLength)) return false;
	auto items = object.value(u"list_items"_q).toArray();
	for (auto i = 0; i != items.size(); ++i) {
		auto item = items[i].toObject();
		if (!WalkTextSlot(item, u"text"_q, process, transform, error)
			|| !WalkChildText(item, process, transform, error, fixedTextLength)) return false;
		if (transform) items[i] = item;
	}
	auto rows = object.value(u"table_rows"_q).toArray();
	for (auto i = 0; i != rows.size(); ++i) {
		auto row = rows[i].toArray();
		for (auto j = 0; j != row.size(); ++j) {
			auto cell = row[j].toObject();
			if (!WalkTextSlot(cell, u"text"_q, process, transform, error)) {
				return false;
			}
			if (transform) row[j] = cell;
		}
		if (transform) rows[i] = row;
	}
	auto buttons = object.value(u"buttons"_q).toArray();
	for (auto i = 0; i != buttons.size(); ++i) {
		auto button = buttons[i].toObject();
		if (!WalkTextSlot(button, u"text"_q, process, transform, error)) {
			return false;
		}
		if (transform) buttons[i] = button;
	}
	if (transform) {
		object.insert(u"list_items"_q, items);
		object.insert(u"table_rows"_q, rows);
		object.insert(u"buttons"_q, buttons);
	}
	return true;
}

} // namespace

std::variant<std::shared_ptr<Iv::RichPage>, QString> DecodeLocalQuickReplyRich(
		const QJsonObject &object,
		Fn<PhotoData*(int)> photoAsset,
		Fn<DocumentData*(int)> documentAsset) {
	return DecodeRich(object, std::move(photoAsset), std::move(documentAsset), false);
}

QString VisitLocalQuickReplyRichText(
		const QJsonObject &object,
		Fn<bool(const TextWithEntities &)> visit,
		int *fixedTextLength) {
	const auto decoded = DecodeRich(object, nullptr, nullptr, true);
	if (const auto error = std::get_if<QString>(&decoded)) return *error;
	auto result = object;
	auto error = QString();
	auto process = Fn<bool(TextWithEntities &)>([&](TextWithEntities &text) {
		return visit(text);
	});
	auto fixed = 0;
	if (!WalkChildText(result, process, false, &error, &fixed)) return error;
	if (fixedTextLength) *fixedTextLength = fixed;
	return QString();
}

std::variant<QJsonObject, QString> TransformLocalQuickReplyRichText(
		const QJsonObject &object,
		Fn<std::optional<TextWithEntities>(const TextWithEntities &)> transform) {
	const auto decoded = DecodeRich(object, nullptr, nullptr, true);
	if (const auto error = std::get_if<QString>(&decoded)) return *error;
	auto result = object;
	auto error = QString();
	auto process = Fn<bool(TextWithEntities &)>([&](TextWithEntities &text) {
		auto updated = transform(text);
		if (!updated) return false;
		text = std::move(*updated);
		return true;
	});
	if (!WalkChildText(result, process, true, &error, nullptr)) return error;
	const auto validated = DecodeRich(result, nullptr, nullptr, true);
	if (const auto error = std::get_if<QString>(&validated)) return *error;
	return result;
}

std::variant<LocalQuickReplyRichMedia, QString>
CollectLocalQuickReplyRichMedia(const Iv::RichPage &page) {
	if (page.blocks.size() > kMaxArray) {
		return Error(u"rich page contains too many blocks"_q);
	}
	if (Iv::ValidateRichMessage(page, Iv::RichMessageLimits()).has_value()) {
		return Error(u"rich page exceeds native limits"_q);
	}
	auto result = LocalQuickReplyRichMedia();
	QString error;
	for (const auto &block : page.blocks) {
		if (!CollectBlock(block, &result, &error, 1)) return error;
	}
	return result;
}

} // namespace Data
