/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/local_pinned_items_format.h"

#include <QtCore/QJsonArray>
#include <QtCore/QSet>
#include <QtCore/QUuid>

namespace Storage::LocalData::PinnedItems {
namespace {

[[nodiscard]] bool ValidUuid(const QString &id) {
	const auto uuid = QUuid(id);
	return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
}

[[nodiscard]] bool Decimal(const QString &id, quint64 maximum) {
	auto ok = false;
	const auto value = id.toULongLong(&ok);
	return ok && value && value <= maximum && QString::number(value) == id;
}

[[nodiscard]] Result Invalid() {
	return { Error::Invalid, u"Invalid local pinned items data."_q };
}

} // namespace

bool ValidFilterId(const QString &id) {
	return Decimal(id, 2147483647ULL);
}

bool ValidScope(const QString &scope) {
	if (scope == u"main"_q
		|| scope == u"archive"_q
		|| scope == u"saved"_q) {
		return true;
	}
	const auto parts = scope.split(u':');
	return (parts.size() == 2
			&& parts[0] == u"local"_q
			&& ValidUuid(parts[1]))
		|| (parts.size() == 3
			&& parts[0] == u"filter"_q
			&& ValidFilterId(parts[1])
			&& ValidUuid(parts[2]));
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
		+ u":pinned_items:"_q + key;
	return QUuid::createUuidV5(
		QUuid(u"{19fa5d14-f51b-4fe8-b4cd-43bbfce55628}"_q),
		input.toUtf8()).toString(QUuid::WithoutBraces);
}

QString PinId(
		const Account &account,
		const QString &scope,
		const QJsonObject &peer) {
	return StableId(account, u"pin:"_q + scope + u":"_q + PeerKey(peer));
}

QString OrderId(const Account &account, const QString &scope) {
	return StableId(account, u"order:"_q + scope);
}

QString FilterId(const Account &account, const QString &id) {
	return StableId(account, u"filter:"_q + id);
}

Result Validate(const QJsonObject &module, const Account &account) {
	if (module.value(u"module_id"_q).toString() != u"pinned_items"_q
		|| module.value(u"schema_version"_q).toInt() != 1
		|| !module.value(u"records"_q).isArray()) {
		return Invalid();
	}
	auto ids = QSet<QString>();
	auto pinnedCounts = QMap<QString, int>();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		const auto id = record.value(u"id"_q).toString();
		const auto kind = record.value(u"kind"_q).toString();
		const auto scope = record.value(u"scope"_q).toString();
		if (!ValidUuid(id) || ids.contains(id)
			|| !record.value(u"deleted"_q).isBool()) {
			return Invalid();
		}
		ids.insert(id);
		if (kind == u"pin"_q) {
			const auto peer = record.value(u"peer"_q).toObject();
			if (!ValidScope(scope) || PeerKey(peer).isEmpty()
				|| id != PinId(account, scope, peer)) {
				return Invalid();
			}
			if (!record.value(u"deleted"_q).toBool()
				&& ++pinnedCounts[scope] > kMaximumOrder) {
				return Invalid();
			}
		} else if (kind == u"order"_q) {
			const auto values = record.value(u"peers"_q);
			if (!ValidScope(scope) || id != OrderId(account, scope)
				|| !values.isArray() || values.toArray().size() > kMaximumOrder) {
				return Invalid();
			}
			auto seen = QSet<QString>();
			for (const auto &peer : values.toArray()) {
				const auto key = PeerKey(peer.toObject());
				if (key.isEmpty() || seen.contains(key)) {
					return Invalid();
				}
				seen.insert(key);
			}
		} else if (kind == u"filter"_q) {
			const auto filter = record.value(u"filter_id"_q).toString();
			if (!ValidFilterId(filter) || id != FilterId(account, filter)
				|| !ValidUuid(record.value(u"scope_id"_q).toString())) {
				return Invalid();
			}
		}
	}
	return {};
}

} // namespace Storage::LocalData::PinnedItems
