/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/local_transfer_format.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QUuid>

namespace Storage::LocalTransferFormat {
namespace {

using namespace LocalData;

[[nodiscard]] Result Fail(Error code, const QString &message) {
	return { code, message };
}

[[nodiscard]] Result Invalid() {
	return Fail(Error::Invalid, u"Invalid or unsupported transfer package."_q);
}

[[nodiscard]] Result LimitError() {
	return Fail(Error::Invalid,
		u"Transfer limit exceeded: 256 MiB of JSON, 16 MiB per module, "
		"32 GiB of resources, 33 GiB per package and 116512 files (32 MiB manifest)."_q);
}

[[nodiscard]] QJsonObject Canonical(QJsonObject extra, const QJsonObject &known) {
	for (auto i = known.begin(); i != known.end(); ++i) {
		extra.insert(i.key(), i.value());
	}
	return extra;
}

[[nodiscard]] QJsonObject RetainedMetadata(
		QJsonObject transfer,
		std::initializer_list<QString> structural) {
	for (const auto &key : structural) {
		transfer.remove(key);
	}
	return transfer;
}

[[nodiscard]] bool Cancelled(Cancel cancel) {
	return cancel && cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] Result CancelError() {
	return Fail(Error::Cancelled, u"Transfer cancelled."_q);
}

[[nodiscard]] QByteArray Hash(QByteArrayView bytes) {
	return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] bool IsHash(const QString &value) {
	static const auto pattern = QRegularExpression(u"^[0-9a-f]{64}$"_q);
	return pattern.match(value).hasMatch();
}

[[nodiscard]] bool IsUuid(const QString &value) {
	const auto parsed = QUuid(value);
	return !parsed.isNull()
		&& parsed.toString(QUuid::WithoutBraces) == value;
}

[[nodiscard]] bool BoundedJson(QByteArrayView bytes) {
	auto depth = 0;
	auto quoted = false;
	auto escaped = false;
	for (const auto ch : bytes) {
		if (quoted) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				quoted = false;
			}
		} else if (ch == '"') {
			quoted = true;
		} else if (ch == '{' || ch == '[') {
			if (++depth > kDepthLimit) {
				return false;
			}
		} else if ((ch == '}' || ch == ']') && --depth < 0) {
			return false;
		}
	}
	return !quoted && !depth;
}

} // namespace

Result Prepare(
		const Snapshot &snapshot,
		Manifest &result,
		bool includeContent,
		Cancel cancel) {
	result = {};
	if (!IsUuid(snapshot.deviceId) || !IsUuid(snapshot.generation)) {
		return Invalid();
	}
	auto &entries = result.entries;
	auto &manifest = result.json;
	auto &total = result.payloadBytes;
	auto profiles = QJsonArray();
	auto assets = QJsonArray();
	auto files = QJsonArray();
	auto jsonBytes = qint64(0);
	const auto addJson = [&](QString path, QJsonObject object, QJsonObject extra) {
		auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
		jsonBytes += bytes.size();
		if (bytes.size() > kJsonLimit || jsonBytes > kAllJsonLimit
			|| !BoundedJson(bytes)) {
			return false;
		}
		entries.push_back({
			.path = std::move(path),
			.size = bytes.size(),
			.hash = includeContent ? Hash(bytes) : QByteArray(64, '0'),
			.json = includeContent ? std::move(bytes) : QByteArray(),
			.extra = std::move(extra),
		});
		return true;
	};
	for (const auto &profile : snapshot.profiles) {
		if (Cancelled(cancel)) {
			return CancelError();
		}
		const auto prefix = u"profiles/"_q + profile.id + '/';
		const auto accountPath = prefix + u"account.json"_q;
		const auto metadata = profile.transfer;
		const auto fileMetadata = metadata.value(u"files"_q).toObject();
		if (!IsUuid(profile.id) || !addJson(accountPath, Canonical(
			metadata.value(u"account"_q).toObject(), {
			{ u"profile_id"_q, profile.id },
			{ u"environment"_q, profile.account.environment },
			{ u"user_id"_q, profile.account.userId },
			{ u"extra"_q, profile.extra },
			{ u"retained_transfer"_q, RetainedMetadata(metadata, {
				u"account"_q, u"profile"_q, u"modules"_q, u"files"_q,
			}) },
		}), fileMetadata.value(u"account"_q).toObject())) {
			return LimitError();
		}
		auto modules = QJsonArray();
		for (auto i = profile.modules.begin(); i != profile.modules.end(); ++i) {
			if (Cancelled(cancel)) {
				return CancelError();
			}
			const auto path = prefix + u"modules/"_q + i.key() + u".json"_q;
			if (!ValidModuleId(i.key()) || !addJson(
				path,
				i.value(),
				fileMetadata.value(u"modules"_q).toObject()
					.value(i.key()).toObject())) {
				return LimitError();
			}
			modules.push_back(Canonical(metadata.value(u"modules"_q)
				.toObject().value(i.key()).toObject(), {
				{ u"module_id"_q, i.key() },
				{ u"path"_q, path },
			}));
		}
		profiles.push_back(Canonical(metadata.value(u"profile"_q).toObject(), {
			{ u"profile_id"_q, profile.id },
			{ u"account"_q, accountPath },
			{ u"modules"_q, modules },
		}));
	}
	for (const auto &asset : snapshot.assets) {
		if (Cancelled(cancel)) {
			return CancelError();
		}
		if (!IsHash(asset.sha256) || (includeContent && !asset.read)
			|| asset.size < 0 || asset.size > kAssetLimit) {
			return Invalid();
		}
		entries.push_back({
			.path = u"assets/"_q + asset.sha256,
			.size = asset.size,
			.hash = asset.sha256.toLatin1(),
			.read = asset.read,
			.extra = asset.transfer.value(u"file"_q).toObject(),
		});
		assets.push_back(Canonical(asset.transfer.value(u"registry"_q).toObject(), {
			{ u"sha256"_q, asset.sha256 },
			{ u"size"_q, QString::number(asset.size) },
			{ u"extra"_q, asset.extra },
			{ u"retained_transfer"_q, RetainedMetadata(asset.transfer, {
				u"registry"_q, u"file"_q,
			}) },
		}));
	}
	total = 0;
	for (const auto &entry : entries) {
		if (Cancelled(cancel)) {
			return CancelError();
		}
		total += entry.size;
		files.push_back(Canonical(entry.extra, {
			{ u"path"_q, entry.path },
			{ u"size"_q, QString::number(entry.size) },
			{ u"sha256"_q, QString::fromLatin1(entry.hash) },
		}));
	}
	const auto object = Canonical(snapshot.transfer.value(u"manifest"_q).toObject(), {
		{ u"format"_q, u"telegravim-transfer"_q },
		{ u"version"_q, 1 },
		{ u"package_id"_q, NewId() },
		{ u"created_at"_q, QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
		{ u"device_id"_q, snapshot.deviceId },
		{ u"generation"_q, snapshot.generation },
		{ u"extra"_q, snapshot.extra },
		{ u"retained_transfer"_q, RetainedMetadata(snapshot.transfer, {
			u"manifest"_q,
		}) },
		{ u"profiles"_q, profiles },
		{ u"assets"_q, assets },
		{ u"files"_q, files },
	});
	manifest = QJsonDocument(object).toJson(QJsonDocument::Compact);
	total += 12 + manifest.size();
	if (entries.size() > kEntryLimit || manifest.size() > kManifestLimit
		|| total > kPackageLimit || !BoundedJson(manifest)) {
		return LimitError();
	}
	return {};
}

Result ValidateShape(const Snapshot &snapshot, Cancel cancel) {
	auto manifest = Manifest();
	return Prepare(snapshot, manifest, false, cancel);
}

} // namespace Storage::LocalTransferFormat
