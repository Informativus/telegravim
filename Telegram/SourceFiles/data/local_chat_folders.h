/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flags.h"
#include "data/data_chat_filters.h"
#include "data/data_types.h"

#include <rpl/event_stream.h>

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QObject>

#include <deque>
#include <memory>
#include <optional>
#include <vector>

class History;

namespace Main {
class Session;
} // namespace Main

namespace Storage::LocalData {
class Store;
struct Result;
struct Account;
} // namespace Storage::LocalData

namespace Data {

struct LocalChatFolder final {
	QString id;
	QString title;
	QJsonArray titleEntities;
	bool staticTitle = false;
	QString iconEmoji;
	std::optional<uint8> colorIndex;
	int position = 0;
	base::flags<ChatFilter::Flag> flags;
	QJsonArray rules;
	std::vector<PeerId> include;
	std::vector<PeerId> exclude;
};

class LocalChatFolders final : public QObject {
public:
	using CloudColors = base::flat_map<QString, std::optional<uint8>>;

	LocalChatFolders(QObject *parent, not_null<Main::Session*> session);
	~LocalChatFolders();

	void reload();
	void stop();
	[[nodiscard]] bool loaded() const;
	[[nodiscard]] bool busy() const;
	[[nodiscard]] const QString &error() const;
	[[nodiscard]] const std::vector<LocalChatFolder> &folders() const;
	[[nodiscard]] std::optional<bool> tagsEnabled() const;
	[[nodiscard]] std::optional<std::optional<uint8>> cloudColor(
		const QString &scope) const;
	[[nodiscard]] rpl::producer<> changes() const;

	void save(LocalChatFolder folder, Fn<void(bool)> done = nullptr);
	void setTagsEnabled(bool value, Fn<void(bool)> done = nullptr);
	void setCloudColor(QString scope, std::optional<uint8> color);
	void remove(QString id, Fn<void(bool)> done = nullptr);
	void reorder(std::vector<QString> ids, Fn<void(bool)> done = nullptr);

private:
	struct Mutation {
		Fn<Storage::LocalData::Result(QJsonObject &)> change;
		Fn<void(std::vector<LocalChatFolder> &)> optimistic;
		Fn<void(std::optional<bool> &)> optimisticTagsEnabled;
		Fn<void(CloudColors &)> optimisticCloudColors;
		Fn<void(bool)> done;
	};

	void loadSnapshot();
	void mutate(Mutation mutation);
	void runNextMutation();
	void rebuildVisible();
	void fail(QString message, Fn<void(bool)> done);
	[[nodiscard]] static std::vector<LocalChatFolder> Parse(
		const QJsonObject &module);
	[[nodiscard]] static std::optional<bool> ParseTagsEnabled(
		const QJsonObject &module,
		const Storage::LocalData::Account &account);
	[[nodiscard]] static CloudColors ParseCloudColors(const QJsonObject &module);

	const not_null<Main::Session*> _session;
	const std::shared_ptr<Storage::LocalData::Store> _store;
	std::vector<LocalChatFolder> _committed;
	std::vector<LocalChatFolder> _folders;
	std::optional<bool> _committedTagsEnabled;
	std::optional<bool> _tagsEnabled;
	CloudColors _committedCloudColors;
	CloudColors _cloudColors;
	std::deque<Mutation> _mutations;
	QString _error;
	bool _loaded = false;
	bool _busy = false;
	bool _reloadPending = false;
	bool _stopped = false;
	rpl::event_stream<> _changes;
	rpl::lifetime _lifetime;

};

} // namespace Data
