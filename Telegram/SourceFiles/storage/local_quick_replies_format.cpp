/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/local_quick_replies_format.h"

#include <QtCore/QJsonArray>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QUuid>

#include <cmath>
#include <vector>

namespace Storage::LocalData::QuickReplies {
namespace {

[[nodiscard]] Result Invalid() {
	return { Error::Invalid, u"Invalid local quick replies data."_q };
}

[[nodiscard]] bool Integer(const QJsonValue &value, int maximum) {
	return value.isDouble() && value.toDouble() >= 0
		&& value.toDouble() <= maximum
		&& value.toDouble() == value.toInt(-1);
}

[[nodiscard]] Result ValidateContent(const QJsonObject &record) {
	const auto maximum = record.value(u"kind"_q).toString() == u"message"_q
		? kMaximumMessageAssets : kMaximumAttachments;
	const auto text = record.value(u"text"_q).toString();
	const auto hashPattern = QRegularExpression(u"^[0-9a-f]{64}$"_q);
	if (!record.value(u"text"_q).isString() || text.size() > kMaximumText
		|| !Integer(record.value(u"position"_q), 2147483647)
		|| !record.value(u"entities"_q).isArray()
		|| !record.value(u"attachments"_q).isArray()
		|| record.value(u"attachments"_q).toArray().size() > maximum) {
		return Invalid();
	}
	for (const auto &entry : record.value(u"entities"_q).toArray()) {
		const auto entity = entry.toObject();
		if (!entry.isObject() || !entity.value(u"type"_q).isString()
			|| !Integer(entity.value(u"offset"_q), text.size())
			|| !Integer(entity.value(u"length"_q), text.size())
			|| entity.value(u"offset"_q).toInt()
				+ entity.value(u"length"_q).toInt() > text.size()
			|| !entity.value(u"data"_q).isString()) {
			return Invalid();
		}
	}
	for (const auto &entry : record.value(u"attachments"_q).toArray()) {
		const auto attachment = entry.toObject();
		const auto size = attachment.value(u"size"_q).toString();
		auto ok = false;
		const auto number = size.toLongLong(&ok);
		const auto name = attachment.value(u"filename"_q).toString();
		if (!entry.isObject() || !hashPattern.match(
				attachment.value(u"sha256"_q).toString()).hasMatch()
			|| !ok || number < 0 || QString::number(number) != size
			|| name.isEmpty() || name.size() > 255
			|| name.contains(u'/') || name.contains(u'\\') || name.contains(QChar(0))
			|| name == u"."_q || name == u".."_q
			|| !attachment.value(u"mime"_q).isString()) {
			return Invalid();
		}
	}
	return {};
}

[[nodiscard]] bool ValidReference(const QJsonValue &value) {
	if (value.isNull() || value.isUndefined()) return true;
	const auto id = value.toString();
	const auto uuid = QUuid(id);
	return value.isString() && !uuid.isNull()
		&& uuid.toString(QUuid::WithoutBraces) == id;
}

[[nodiscard]] bool ValidDraftText(const QJsonObject &value) {
	const auto text = value.value(u"text"_q).toString();
	if (!value.value(u"text"_q).isString() || text.size() > kMaximumText
		|| !value.value(u"tags"_q).isArray()
		|| value.value(u"tags"_q).toArray().size() > kMaximumText) {
		return false;
	}
	for (const auto &entry : value.value(u"tags"_q).toArray()) {
		const auto tag = entry.toObject();
		if (!entry.isObject() || !Integer(tag.value(u"offset"_q), text.size())
			|| !Integer(tag.value(u"length"_q), text.size())
			|| tag.value(u"offset"_q).toInt()
				+ tag.value(u"length"_q).toInt() > text.size()
			|| !tag.value(u"id"_q).isString()) return false;
	}
	return true;
}

[[nodiscard]] bool ValidMediaStickerHints(const QJsonObject &media) {
	auto pending = std::vector<QJsonObject>{ media };
	while (!pending.empty()) {
		const auto current = std::move(pending.back());
		pending.pop_back();
		if (!ValidAttachedStickerSets(current)) return false;
		for (const auto &key : { u"cover_asset_media"_q,
			u"photo_asset_media"_q, u"document_asset_media"_q }) {
			const auto child = current.value(key);
			if (child.isObject()) pending.push_back(child.toObject());
		}
		for (const auto &value : current.value(u"rich_assets"_q).toArray()) {
			const auto child = value.toObject().value(u"media"_q);
			if (child.isObject()) pending.push_back(child.toObject());
		}
	}
	return true;
}

[[nodiscard]] bool ValidSourceVersion(const QJsonValue &value) {
	if (value.isUndefined() || value.isNull()) return true;
	if (!value.isObject()) return false;
	const auto vector = value.toObject();
	if (vector.isEmpty() || vector.size() > 256) return false;
	for (auto i = vector.begin(); i != vector.end(); ++i) {
		const auto counter = i.value().toDouble(-1.);
		if (!ValidReference(i.key()) || counter < 1.
			|| counter > 9007199254740991. || std::floor(counter) != counter) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool ValidEditorDraft(const QJsonObject &record) {
	for (const auto &key : { u"normal"_q, u"edit"_q }) {
		const auto value = record.value(key);
		if (value.isNull() || value.isUndefined()) continue;
		const auto draft = value.toObject();
		const auto cursor = draft.value(u"cursor"_q).toObject();
		const auto webpage = draft.value(u"webpage"_q).toObject();
		if (!value.isObject() || !ValidDraftText(draft)
			|| !ValidReference(draft.value(u"message_ref"_q))
			|| !ValidSourceVersion(draft.value(u"source_version_vector"_q))
			|| !ValidSourceVersion(draft.value(u"source_legacy_parent_version_vector"_q))
			|| (draft.value(u"source_version_vector"_q).isObject()
				&& draft.value(u"source_legacy_parent_version_vector"_q).isObject())
			|| !ValidDraftText(draft.value(u"quote"_q).toObject())
			|| !Integer(draft.value(u"quote_offset"_q), kMaximumText)
			|| !draft.value(u"cursor"_q).isObject()
			|| !Integer(cursor.value(u"position"_q), draft.value(u"text"_q).toString().size())
			|| !Integer(cursor.value(u"anchor"_q), draft.value(u"text"_q).toString().size())
			|| !Integer(cursor.value(u"scroll"_q), 2147483647)
			|| !draft.value(u"webpage"_q).isObject()
			|| !webpage.value(u"url"_q).isString()) return false;
		for (const auto &flag : { u"large"_q, u"small"_q, u"invert"_q,
				u"manual"_q, u"removed"_q }) {
			if (!webpage.value(flag).isBool()) return false;
		}
	}
	return true;
}

} // namespace

bool ValidShortcut(const QString &value) {
	if (value.isEmpty() || value.size() > 32) return false;
	return ranges::all_of(value, [](QChar ch) {
		return ch.isLetterOrNumber() || ch == u'_'
			|| ch.unicode() == 0x200c || ch.unicode() == 0x00b7
			|| (ch.unicode() >= 0x0d80 && ch.unicode() <= 0x0dff);
	});
}

bool ValidAttachedStickerSets(
		const QJsonObject &media,
		bool rejectUnknownFields) {
	const auto value = media.value(u"attached_sticker_sets"_q);
	if (value.isUndefined()) return true;
	if (!value.isObject() || value.toObject().size() > kMaximumAttachedStickerSets) return false;
	const auto hints = value.toObject();
	if (hints.isEmpty()) return true;
	const auto attached = media.value(u"attached_stickers"_q);
	if (!attached.isArray()) return false;
	auto ids = QSet<QString>();
	for (const auto &entry : attached.toArray()) {
		if (entry.isString()) ids.insert(entry.toString());
	}
	const auto validId = [](const QJsonValue &value) {
		auto ok = false;
		const auto text = value.toString();
		const auto id = text.toULongLong(&ok);
		return value.isString() && ok && id && QString::number(id) == text;
	};
	static const auto shortName = QRegularExpression(u"^[A-Za-z0-9_]{1,64}$"_q);
	for (auto i = hints.begin(); i != hints.end(); ++i) {
		if (!validId(i.key()) || !ids.contains(i.key()) || !i.value().isObject()) {
			return false;
		}
		const auto hint = i.value().toObject();
		const auto id = hint.value(u"id"_q);
		const auto name = hint.value(u"short_name"_q);
		if (id.isUndefined() && name.isUndefined()) return false;
		if (!id.isUndefined() && !validId(id)) return false;
		if (!name.isUndefined() && (!name.isString()
			|| !shortName.match(name.toString()).hasMatch())) return false;
		if (rejectUnknownFields) {
			for (auto field = hint.begin(); field != hint.end(); ++field) {
				if (field.key() != u"id"_q && field.key() != u"short_name"_q) {
					return false;
				}
			}
		}
	}
	return true;
}

Result Validate(const QJsonObject &module, const Account &) {
	if (module.value(u"module_id"_q).toString() != u"quick_replies"_q
		|| module.value(u"schema_version"_q).toInt() != 1
		|| !module.value(u"records"_q).isArray()) {
		return Invalid();
	}
	auto ids = QSet<QString>();
	auto parents = QSet<QString>();
	auto shortcuts = QSet<QString>();
	const auto records = module.value(u"records"_q).toArray();
	for (const auto &value : records) {
		if (!value.isObject()) return Invalid();
		const auto record = value.toObject();
		const auto id = record.value(u"id"_q).toString();
		const auto uuid = QUuid(id);
		if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id
			|| ids.contains(id) || !record.value(u"deleted"_q).isBool()) {
			return Invalid();
		}
		ids.insert(id);
		if (record.value(u"kind"_q).toString() == u"reply"_q) parents.insert(id);
		if (record.value(u"kind"_q).toString() != u"reply"_q
			|| record.value(u"deleted"_q).toBool()) continue;
		const auto shortcut = record.value(u"shortcut"_q).toString();
		if (!ValidShortcut(shortcut) || shortcuts.contains(shortcut.toCaseFolded())
			|| !record.value(u"title"_q).isString()
			|| record.value(u"title"_q).toString().trimmed().isEmpty()
			|| record.value(u"title"_q).toString().size() > 128
			|| !record.value(u"category"_q).isString()
			|| record.value(u"category"_q).toString().size() > 128
			|| !record.value(u"variables"_q).isArray()) return Invalid();
		if (const auto content = ValidateContent(record); !content) return content;
		shortcuts.insert(shortcut.toCaseFolded());
		auto names = QSet<QString>();
		for (const auto &entry : record.value(u"variables"_q).toArray()) {
			const auto variable = entry.toObject();
			const auto name = variable.value(u"name"_q).toString();
			if (!entry.isObject() || !ValidShortcut(name) || names.contains(name)
				|| !variable.value(u"default"_q).isString()
				|| variable.value(u"default"_q).toString().size() > kMaximumText) {
				return Invalid();
			}
			names.insert(name);
		}
	}
	for (const auto &value : records) {
		const auto record = value.toObject();
		if (record.value(u"kind"_q).toString() == u"editor_draft"_q
			&& !record.value(u"deleted"_q).toBool()) {
			if (!parents.contains(record.value(u"reply_ref"_q).toString())
				|| !ValidEditorDraft(record)) return Invalid();
		}
		if (record.value(u"kind"_q).toString() != u"message"_q
			|| record.value(u"deleted"_q).toBool()) continue;
		if (!parents.contains(record.value(u"reply_ref"_q).toString())
			|| !record.value(u"media"_q).isObject()
			|| record.value(u"media"_q).toObject().value(u"kind"_q).toString().isEmpty()
			|| !ValidMediaStickerHints(record.value(u"media"_q).toObject())) {
			return Invalid();
		}
		for (const auto &key : { u"group_ref"_q, u"reply_to_ref"_q }) {
			const auto reference = record.value(key);
			if (!reference.isNull() && !reference.isUndefined()) {
				const auto id = reference.toString();
				const auto uuid = QUuid(id);
				if (!reference.isString() || uuid.isNull()
					|| uuid.toString(QUuid::WithoutBraces) != id) return Invalid();
			}
		}
		if (const auto content = ValidateContent(record); !content) return content;
	}
	return {};
}

QStringList AssetReferences(const QJsonObject &module) {
	auto result = QStringList();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		const auto kind = record.value(u"kind"_q).toString();
		if ((kind != u"reply"_q && kind != u"message"_q)
			|| record.value(u"deleted"_q).toBool()) continue;
		for (const auto &entry : record.value(u"attachments"_q).toArray()) {
			result.push_back(entry.toObject().value(u"sha256"_q).toString());
		}
	}
	result.removeDuplicates();
	return result;
}

} // namespace Storage::LocalData::QuickReplies
