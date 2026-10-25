/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/local_navigation_preferences_format.h"

#include <QtCore/QJsonArray>
#include <QtCore/QSet>
#include <QtCore/QUuid>

namespace Storage::LocalData::NavigationPreferences {
namespace {

[[nodiscard]] bool ValidUuid(const QString &value) {
	const auto uuid = QUuid(value);
	return !uuid.isNull()
		&& uuid.toString(QUuid::WithoutBraces) == value;
}

[[nodiscard]] bool ValidDecimal(const QString &value) {
	auto ok = false;
	const auto number = value.toULongLong(&ok);
	return ok && number > 0 && number <= 2147483647ULL
		&& QString::number(number) == value;
}

[[nodiscard]] Result Invalid() {
	return { Error::Invalid,
		u"Invalid local navigation preferences data."_q };
}

} // namespace

bool ValidReference(const QString &reference) {
	if (reference == u"all"_q) {
		return true;
	}
	const auto parts = reference.split(u':');
	if (parts.size() == 2 && parts[0] == u"local"_q) {
		return ValidUuid(parts[1]);
	}
	return parts.size() == 3
		&& parts[0] == u"filter"_q
		&& ValidDecimal(parts[1])
		&& ValidUuid(parts[2]);
}

QString SettingsId(const Account &account) {
	const auto input = account.environment + u":"_q + account.userId
		+ u":navigation_preferences:settings:account"_q;
	return QUuid::createUuidV5(
		QUuid(u"{8e5e7aaf-58e6-4c5a-ae47-90c23a2e24fb}"_q),
		input.toUtf8()).toString(QUuid::WithoutBraces);
}

Result Validate(const QJsonObject &module, const Account &account) {
	if (module.value(u"module_id"_q).toString()
			!= u"navigation_preferences"_q
		|| module.value(u"schema_version"_q).toInt() != 1
		|| !module.value(u"records"_q).isArray()) {
		return Invalid();
	}
	auto ids = QSet<QString>();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		if (!value.isObject()) {
			return Invalid();
		}
		const auto record = value.toObject();
		const auto id = record.value(u"id"_q).toString();
		if (!ValidUuid(id) || ids.contains(id)
			|| !record.value(u"deleted"_q).isBool()) {
			return Invalid();
		}
		ids.insert(id);
		if (record.value(u"kind"_q).toString() != u"settings"_q) {
			continue;
		}
		if (id != SettingsId(account)
			|| record.value(u"scope"_q).toString() != u"account"_q
			|| !record.value(u"restore_last_section"_q).isBool()
			|| !record.value(u"visible_order"_q).isArray()
			|| record.value(u"visible_order"_q).toArray().size() > kMaximumOrder) {
			return Invalid();
		}
		auto references = QSet<QString>();
		for (const auto &entry : record.value(u"visible_order"_q).toArray()) {
			if (!entry.isString() || !ValidReference(entry.toString())
				|| references.contains(entry.toString())) {
				return Invalid();
			}
			references.insert(entry.toString());
		}
		for (const auto key : { u"startup_ref"_q, u"last_section_ref"_q }) {
			const auto ref = record.value(key);
			if (!ref.isNull() && !ref.isUndefined()
				&& (!ref.isString() || !ValidReference(ref.toString()))) {
				return Invalid();
			}
		}
	}
	return {};
}

} // namespace Storage::LocalData::NavigationPreferences
