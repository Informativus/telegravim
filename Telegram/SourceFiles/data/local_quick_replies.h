/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/local_data_store.h"
#include "ui/text/text_entity.h"

#include <QtCore/QJsonArray>
#include <QtCore/QObject>

#include <atomic>
#include <deque>

class QTemporaryDir;

namespace Main {
class Session;
} // namespace Main

namespace Data {

class LocalQuickReplyMediaAssets;

class LocalQuickReplyAssetLease final {
private:
	friend class LocalQuickReplies;
	std::shared_ptr<QTemporaryDir> _directory;
	std::shared_ptr<const LocalQuickReplyMediaAssets> _prepared;

};

struct LocalQuickReplyMaterialized final {
	QStringList paths;
	std::shared_ptr<const LocalQuickReplyAssetLease> assets;
};

struct LocalQuickReplyMessage final {
	QString id;
	QString replyRef;
	QString groupRef;
	QString replyToRef;
	TextWithEntities message;
	QJsonArray attachments;
	QJsonObject media;
	int position = 0;
	QJsonObject retained;
	QStringList sourcePaths;
	QJsonObject legacyParentRevision;
};

struct LocalQuickReply final {
	QString id;
	QString title;
	QString shortcut;
	QString category;
	TextWithEntities message;
	QJsonArray variables;
	QJsonArray attachments;
	int position = 0;
	QJsonObject retained;
	std::vector<LocalQuickReplyMessage> messages;
};

[[nodiscard]] bool QuickReplyTextSupported(const LocalQuickReply &reply);
[[nodiscard]] QString QuickReplyMessageEditError(
	const LocalQuickReplyMessage &message);
[[nodiscard]] QStringList QuickReplyVariables(const QString &text);
[[nodiscard]] std::optional<QStringList> QuickReplyVariables(
	const LocalQuickReply &reply,
	QString *error = nullptr);
[[nodiscard]] TextWithEntities ExpandQuickReply(
	const TextWithEntities &message,
	const QMap<QString, QString> &variables);
[[nodiscard]] std::optional<TextWithEntities> ExpandQuickReplyChecked(
	const TextWithEntities &message,
	const QMap<QString, QString> &variables,
	int maximum,
	QString *error = nullptr);
[[nodiscard]] std::optional<std::vector<LocalQuickReplyMessage>>
ExpandQuickReplyMessages(
	const LocalQuickReply &reply,
	const QMap<QString, QString> &variables,
	QString *error = nullptr);

class LocalQuickReplies final : public QObject {
public:
	LocalQuickReplies(QObject *parent, not_null<Main::Session*> session);
	~LocalQuickReplies();

	void reload();
	void stop();
	[[nodiscard]] bool loaded() const;
	[[nodiscard]] bool busy() const;
	[[nodiscard]] const QString &error() const;
	[[nodiscard]] const std::vector<LocalQuickReply> &list() const;
	[[nodiscard]] const std::vector<LocalQuickReply> &recoveryList() const;
	[[nodiscard]] std::optional<LocalQuickReply> find(const QString &id) const;
	[[nodiscard]] rpl::producer<> changes() const;

	void save(LocalQuickReply reply, QStringList paths, Fn<void(bool)> done);
	void remove(QString id, Fn<void(bool)> done = nullptr);
	void recover(LocalQuickReply reply, Fn<void(bool)> done);
	void saveMessages(
		QString replyRef,
		std::vector<LocalQuickReplyMessage> messages,
		Fn<void(bool)> done,
		std::shared_ptr<const LocalQuickReplyMediaAssets> assets = nullptr);
	void removeMessages(QString replyRef, QStringList ids, Fn<void(bool)> done);
	[[nodiscard]] QJsonObject editorDraft(const QString &replyRef) const;
	void saveEditorDraft(QString replyRef, QJsonObject draft,
		QJsonObject expectedRevision, Fn<void(bool)> done);
	void move(QString id, int direction);
	void materializeMessage(
		LocalQuickReplyMessage message,
		Fn<void(LocalQuickReplyMaterialized, QString)> done,
		std::shared_ptr<const LocalQuickReplyMediaAssets> assets = nullptr);

private:
	using Result = Storage::LocalData::Result;
	using Job = Fn<Result(Storage::LocalData::Store &)>;
	struct Mutation {
		Job job;
		Fn<void(bool)> done;
	};
	struct MaterializedCacheEntry {
		QStringList paths;
		std::weak_ptr<const LocalQuickReplyAssetLease> assets;
	};
	void enqueue(Job job, Fn<void(bool)> done);
	void runNext();
	[[nodiscard]] static std::vector<LocalQuickReply> Parse(
		const QJsonObject &module);
	[[nodiscard]] static std::vector<LocalQuickReply> ParseRecovery(
		const QJsonObject &module);

	const std::shared_ptr<Storage::LocalData::Store> _store;
	const Storage::LocalData::Account _account;
	std::vector<LocalQuickReply> _list;
	std::vector<LocalQuickReply> _recovery;
	QMap<QString, QJsonObject> _editorDrafts;
	QMap<QString, MaterializedCacheEntry> _materialized;
	std::shared_ptr<std::atomic<bool>> _materializeCancelled;
	std::deque<Mutation> _queue;
	QString _error;
	bool _loaded = false;
	bool _busy = false;
	bool _stopped = false;
	rpl::event_stream<> _changes;
	rpl::lifetime _lifetime;

};

} // namespace Data
