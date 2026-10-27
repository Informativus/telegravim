/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QByteArrayView>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <rpl/event_stream.h>

namespace MTP {
class AuthKey;
} // namespace MTP

namespace Storage::LocalData {

enum class Error {
	None,
	Cancelled,
	Invalid,
	Io,
	Locked,
	Conflict,
	Stale,
	Corrupt,
	Unsupported,
};

struct Result {
	Error code = Error::None;
	QString message;
	[[nodiscard]] bool ok() const { return code == Error::None; }
	[[nodiscard]] explicit operator bool() const { return ok(); }
};

using Cancel = const std::atomic<bool>*;
using AssetSink = std::function<bool(QByteArrayView)>;
using AssetReader = std::function<Result(AssetSink, Cancel)>;

struct Account {
	QString environment;
	QString userId;
	friend bool operator==(const Account &, const Account &) = default;
};

struct Asset {
	QString sha256;
	qint64 size = 0;
	QJsonObject extra;
	AssetReader read;
	QJsonObject transfer;
};

struct Profile {
	QString id;
	Account account;
	QJsonObject extra;
	QMap<QString, QJsonObject> modules;
	QJsonObject transfer;
};

struct Snapshot {
	QString generation;
	QString deviceId;
	QJsonObject extra;
	QMap<QString, Profile> profiles;
	QMap<QString, Asset> assets;
	QJsonObject transfer;
};

enum class ConflictChoice {
	KeepLocal,
	UseIncoming,
	KeepBoth,
};

struct Conflict {
	QString id;
	QString profileId;
	QString moduleId;
	QString recordId;
	QJsonObject local;
	QJsonObject incoming;
};

struct MergePlan {
	QString baseGeneration;
	Snapshot merged;
	std::vector<Conflict> conflicts;
	QStringList unsupportedModules;
	QStringList accounts;
};

enum class ArchiveScope {
	Record,
	Module,
	RootExtra,
	RootTransfer,
	ProfileExtra,
	ProfileTransfer,
	ModuleExtra,
	AssetExtra,
	AssetTransfer,
};

struct ArchivedConflict {
	QString id;
	QString profileId;
	QString moduleId;
	QString recordId;
	QJsonObject current;
	QJsonObject alternate;
	ArchiveScope scope = ArchiveScope::Record;
	QString field;
};

struct Limits {
	qint64 maxJsonBytes = 256 * 1024 * 1024;
	qint64 maxModuleBytes = 16 * 1024 * 1024;
	qint64 maxRecordBytes = 256 * 1024;
	qint64 maxAssetBytes = 32LL * 1024 * 1024 * 1024;
	qint64 maxAssetTotalBytes = 32LL * 1024 * 1024 * 1024;
	int maxProfiles = 128;
	int maxModules = 128;
	int maxRecords = 1000000;
	int maxAssets = 100000;
	int maxDepth = 32;
};

struct ModuleDefinition {
	QString id;
	int schemaVersion = 1;
	std::function<Result(const QJsonObject &, const Account &)> validate;
	std::function<QStringList(const QJsonObject &)> assetReferences;
};

[[nodiscard]] QString NewId();
[[nodiscard]] Result RegisterModule(ModuleDefinition definition);
[[nodiscard]] bool ValidAccount(const Account &account);
[[nodiscard]] bool ValidModuleId(const QString &id);
[[nodiscard]] bool SupportedModule(const QString &id, int version);
[[nodiscard]] Result ValidateSnapshot(
	const Snapshot &snapshot,
	const Limits &limits = Limits(),
	Cancel cancel = nullptr);
[[nodiscard]] QJsonObject EmptyModule(const QString &id);
[[nodiscard]] QJsonObject ReadModule(
	const Snapshot &snapshot,
	const Account &account,
	const QString &moduleId);
[[nodiscard]] std::vector<ArchivedConflict> ArchivedConflicts(
	const Snapshot &snapshot,
	int offset,
	int limit = 50);
[[nodiscard]] int ArchivedConflictCount(const Snapshot &snapshot);

class Store final : public std::enable_shared_from_this<Store> {
public:
	Store(QString path, std::shared_ptr<MTP::AuthKey> key);
	~Store();

	[[nodiscard]] Result snapshot(Snapshot &result, Cancel cancel = nullptr);
	[[nodiscard]] Result mutate(
		const Account &account,
		const QString &moduleId,
		std::function<Result(QJsonObject &)> change,
		Cancel cancel = nullptr);
	[[nodiscard]] Result mutateWithAssets(
		const Account &account,
		const QString &moduleId,
		std::function<Result(QJsonObject &)> change,
		const QMap<QString, Asset> &assets,
		Cancel cancel = nullptr);
	[[nodiscard]] Result prepareImport(
		const Snapshot &incoming,
		MergePlan &plan,
		Cancel cancel = nullptr);
	[[nodiscard]] Result applyImport(
		const MergePlan &plan,
		const QMap<QString, ConflictChoice> &choices,
		Cancel cancel = nullptr);
	[[nodiscard]] rpl::producer<> changes() const;
	[[nodiscard]] Result restoreConflict(
		const QString &archiveId,
		const QString &expectedGeneration,
		Cancel cancel = nullptr);

private:
	class Private;
	std::unique_ptr<Private> _private;
	rpl::event_stream<> _changes;

};

} // namespace Storage::LocalData
