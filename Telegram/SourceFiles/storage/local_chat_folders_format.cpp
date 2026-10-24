/*
This file is part of Telegram Desktop,
the official Telegram Desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/local_chat_folders_format.h"

#include "storage/local_pinned_items_format.h"

#include <QtCore/QJsonArray>
#include <QtCore/QSet>
#include <QtCore/QUuid>

#include <cmath>
#include <limits>

namespace Storage::LocalData::ChatFolders {
namespace {

constexpr auto kMaximumRuleDepth = 16;
constexpr auto kMaximumRules = 128;
constexpr auto kMaximumRuleProperties = 64;
constexpr auto kMaximumRuleOpLength = 128;
constexpr auto kMaximumTitleEntities = 128;

[[nodiscard]] bool Decimal(const QString &id, quint64 maximum) {
	auto ok = false;
	const auto value = id.toULongLong(&ok);
	return ok
		&& value
		&& value <= maximum
		&& QString::number(value) == id;
}

[[nodiscard]] bool IntegralInRange(
		const QJsonValue &value,
		qint64 minimum,
		qint64 maximum) {
	if (!value.isDouble()) {
		return false;
	}
	const auto number = value.toDouble();
	return std::isfinite(number)
		&& std::floor(number) == number
		&& number >= minimum
		&& number <= maximum;
}

[[nodiscard]] bool ValidRuleValue(
		const QJsonValue &value,
		int depth) {
	if (depth > kMaximumRuleDepth) {
		return false;
	} else if (value.isArray()) {
		const auto array = value.toArray();
		if (array.size() > kMaximumRuleProperties) {
			return false;
		}
		for (const auto &child : array) {
			if ((child.isArray() || child.isObject())
				&& !ValidRuleValue(child, depth + 1)) {
				return false;
			}
		}
	} else if (value.isObject()) {
		const auto object = value.toObject();
		if (object.size() > kMaximumRuleProperties) {
			return false;
		}
		for (auto i = object.begin(); i != object.end(); ++i) {
			if ((i.value().isArray() || i.value().isObject())
				&& !ValidRuleValue(i.value(), depth + 1)) {
				return false;
			}
		}
	} else if (!value.isBool() && !value.isString() && !value.isDouble()
		&& !value.isNull()) {
		return false;
	}
	return true;
}

[[nodiscard]] Result Invalid() {
	return { Error::Invalid, u"Invalid local chat folders data."_q };
}

} // namespace

bool ValidUuid(const QString &id) {
	const auto uuid = QUuid(id);
	return !uuid.isNull()
		&& uuid.toString(QUuid::WithoutBraces) == id;
}

QString PeerKey(const QJsonObject &peer) {
	const auto type = peer.value(u"peer_type"_q).toString();
	const auto id = peer.value(u"peer_id"_q).toString();
	if ((type != u"user"_q && type != u"chat"_q && type != u"channel"_q)
		|| !Decimal(id, 0xFFFFFFFFFFFFULL)) {
		return {};
	}
	return type + u":"_q + id;
}

QString StableId(const Account &account, const QString &key) {
	const auto input = account.environment + u":"_q + account.userId
		+ u":chat_folders:"_q + key;
	return QUuid::createUuidV5(
		QUuid(u"{1f9b4b61-3c7c-4f26-9e49-ecb3a31c11de}"_q),
		input.toUtf8()).toString(QUuid::WithoutBraces);
}

QString FolderId(const Account &account, const QString &folderId) {
	return StableId(account, u"folder:"_q + folderId);
}

QString SettingsId(const Account &account) {
	return StableId(account, u"settings:account"_q);
}

bool ValidCloudColorScope(const QString &scope) {
	return scope.startsWith(u"filter:"_q) && PinnedItems::ValidScope(scope);
}

QString CloudColorId(const Account &account, const QString &scope) {
	return StableId(account, u"cloud_color:"_q + scope);
}

QString MemberId(
		const Account &account,
		const QString &folderId,
		const QJsonObject &peer,
		const QString &mode) {
	return StableId(
		account,
		u"member:"_q + folderId + u":"_q + PeerKey(peer)
			+ u":"_q + mode);
}

Result Validate(const QJsonObject &module, const Account &account) {
	if (module.value(u"module_id"_q).toString() != u"chat_folders"_q
		|| module.value(u"schema_version"_q).toInt() != 1
		|| !module.value(u"records"_q).isArray()) {
		return Invalid();
	}
	auto ids = QSet<QString>();
	auto folderIds = QSet<QString>();
	auto memberCounts = QMap<QString, int>();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		if (!value.isObject()) {
			return Invalid();
		}
		const auto record = value.toObject();
		const auto id = record.value(u"id"_q).toString();
		const auto kind = record.value(u"kind"_q).toString();
		if (!ValidUuid(id) || ids.contains(id)
			|| !record.value(u"deleted"_q).isBool()) {
			return Invalid();
		}
		ids.insert(id);
		if (kind == u"folder"_q) {
			const auto folderId = record.value(u"folder_id"_q).toString();
			const auto title = record.value(u"title"_q);
			const auto rules = record.value(u"rules"_q);
			if (!ValidUuid(folderId)
				|| id != ChatFolders::FolderId(account, folderId)
				|| !title.isString()
				|| title.toString().trimmed().isEmpty()
				|| title.toString().size() > 100
				|| !rules.isArray()
				|| rules.toArray().size() > kMaximumRules
				|| !IntegralInRange(
					record.value(u"position"_q),
					0,
					std::numeric_limits<qint32>::max())) {
				return Invalid();
			}
			if (record.contains(u"icon"_q)
				&& (!record.value(u"icon"_q).isString()
					|| record.value(u"icon"_q).toString().size() > 32)) {
				return Invalid();
			}
			if (record.contains(u"static_title"_q)
				&& !record.value(u"static_title"_q).isBool()) {
				return Invalid();
			}
			if (record.contains(u"title_entities"_q)) {
				const auto entities = record.value(u"title_entities"_q);
				if (!entities.isArray()
					|| entities.toArray().size() > kMaximumTitleEntities) {
					return Invalid();
				}
				for (const auto &entity : entities.toArray()) {
					const auto object = entity.toObject();
					const auto offset = object.value(u"offset"_q).toDouble();
					const auto length = object.value(u"length"_q).toDouble();
					const auto type = object.value(u"type"_q).toString();
					if (!entity.isObject()
						|| !object.value(u"type"_q).isString()
						|| object.value(u"type"_q).toString().isEmpty()
						|| !IntegralInRange(
							object.value(u"offset"_q),
							0,
							std::numeric_limits<qint32>::max())
						|| !IntegralInRange(
							object.value(u"length"_q),
							1,
							std::numeric_limits<qint32>::max())
						|| (type == u"custom_emoji"_q
							&& offset + length > title.toString().size())
						|| (object.contains(u"data"_q)
							&& (!object.value(u"data"_q).isString()
								|| object.value(u"data"_q).toString().size() > 1024))) {
						return Invalid();
					}
				}
			}
			if (record.contains(u"color"_q)
				&& !IntegralInRange(record.value(u"color"_q), 0, 255)) {
				return Invalid();
			}
			for (const auto &rule : rules.toArray()) {
				const auto object = rule.toObject();
				const auto op = object.value(u"op"_q);
				if (!rule.isObject()
					|| !op.isString()
					|| op.toString().isEmpty()
					|| op.toString().size() > kMaximumRuleOpLength
					|| (object.contains(u"enabled"_q)
						&& !object.value(u"enabled"_q).isBool())
					|| !ValidRuleValue(rule, 0)) {
					return Invalid();
				}
			}
			if (!record.value(u"deleted"_q).toBool()) {
				if (folderIds.contains(folderId)
					|| folderIds.size() >= kMaximumFolders) {
					return Invalid();
				}
				folderIds.insert(folderId);
			}
		} else if (kind == u"member"_q) {
			const auto folderId = record.value(u"folder_id"_q).toString();
			const auto peer = record.value(u"peer"_q).toObject();
			const auto mode = record.value(u"mode"_q).toString();
			if (!ValidUuid(folderId) || PeerKey(peer).isEmpty()
				|| (mode != u"include"_q && mode != u"exclude"_q)
				|| id != ChatFolders::MemberId(account, folderId, peer, mode)) {
				return Invalid();
			}
			const auto countKey = folderId + u":"_q + mode;
			if (!record.value(u"deleted"_q).toBool()
				&& ++memberCounts[countKey] > kMaximumMembers) {
				return Invalid();
			}
		} else if (kind == u"cloud_color"_q) {
			const auto scope = record.value(u"scope"_q).toString();
			const auto color = record.value(u"color_index"_q);
			if (!ValidCloudColorScope(scope)
				|| id != CloudColorId(account, scope)
				|| (!color.isNull() && !IntegralInRange(color, 0, 255))) {
				return Invalid();
			}
		} else if (kind == u"settings"_q) {
			if (record.value(u"scope"_q).toString() != u"account"_q
				|| id != ChatFolders::SettingsId(account)
				|| !record.value(u"tags_enabled"_q).isBool()) {
				return Invalid();
			}
		}
	}
	return {};
}

} // namespace Storage::LocalData::ChatFolders
