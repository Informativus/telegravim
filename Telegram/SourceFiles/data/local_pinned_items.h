/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_types.h"

#include <rpl/event_stream.h>

#include <QtCore/QObject>
#include <QtCore/QJsonObject>

#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Storage::LocalData {
class Store;
struct Result;
} // namespace Storage::LocalData

namespace Data {

class LocalPinnedItems final : public QObject {
public:
	LocalPinnedItems(QObject *parent, not_null<Main::Session*> session);
	~LocalPinnedItems();

	void reload();
	[[nodiscard]] bool loaded() const;
	[[nodiscard]] bool busy() const;
	[[nodiscard]] const QString &error() const;
	[[nodiscard]] rpl::producer<> changes() const;
	[[nodiscard]] std::vector<QString> scopes() const;
	[[nodiscard]] std::vector<PeerId> localPins(const QString &scope) const;
	[[nodiscard]] std::optional<bool> overridden(
		const QString &scope,
		PeerId peer) const;
	[[nodiscard]] std::vector<PeerId> overlay(
		const QString &scope,
		const std::vector<PeerId> &cloud) const;
	[[nodiscard]] QString filterScope(FilterId id) const;
	[[nodiscard]] std::vector<std::pair<FilterId, QString>> filterScopes() const;

	void setPinned(
		QString scope,
		PeerId peer,
		bool pinned,
		std::vector<PeerId> visibleOrder,
		Fn<void(bool)> done = nullptr);
	void saveOrder(
		QString scope,
		std::vector<PeerId> visibleOrder,
		Fn<void(bool)> done = nullptr);
	void removePeer(PeerId peer, Fn<void(bool)> done = nullptr);
	void syncFilters(
		std::vector<FilterId> ids,
		Fn<void(bool)> done = nullptr);

private:
	struct State;
	struct Mutation {
		Fn<Storage::LocalData::Result(QJsonObject &)> change;
		Fn<void(State &)> optimistic;
		Fn<void(bool)> done;
	};

	void mutate(Mutation mutation);
	void loadSnapshot();
	void runNextMutation();
	void rebuildVisible();
	void fail(QString message, Fn<void(bool)> done);
	[[nodiscard]] static std::unique_ptr<State> Parse(const QJsonObject &module);

	const not_null<Main::Session*> _session;
	const std::shared_ptr<Storage::LocalData::Store> _store;
	std::unique_ptr<State> _committed;
	std::unique_ptr<State> _state;
	std::deque<Mutation> _mutations;
	QString _error;
	bool _loaded = false;
	bool _busy = false;
	bool _reloadPending = false;
	rpl::event_stream<> _changes;
	rpl::lifetime _lifetime;

};

} // namespace Data
