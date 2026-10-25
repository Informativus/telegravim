/*
This file is part of Telegram Desktop,
the official desktop application of the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <rpl/event_stream.h>

#include <deque>
#include <memory>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Storage::LocalData {
class Store;
struct Result;
struct Account;
} // namespace Storage::LocalData

namespace Data {

struct LocalNavigationPreferencesState final {
	QString startupRef;
	QString lastSectionRef;
	std::vector<QString> visibleOrder;
	bool restoreLastSection = false;
};

class LocalNavigationPreferences final : public QObject {
public:
	LocalNavigationPreferences(QObject *parent, not_null<Main::Session*> session);
	~LocalNavigationPreferences();

	void reload();
	void stop();
	[[nodiscard]] bool loaded() const;
	[[nodiscard]] bool busy() const;
	[[nodiscard]] const QString &error() const;
	[[nodiscard]] const LocalNavigationPreferencesState &state() const;
	[[nodiscard]] rpl::producer<> changes() const;

	void setStartupRef(QString reference, Fn<void(bool)> done = nullptr);
	void setLastSectionRef(QString reference, Fn<void(bool)> done = nullptr);
	void setRestoreLastSection(bool value, Fn<void(bool)> done = nullptr);
	void setVisibleOrder(std::vector<QString> order, Fn<void(bool)> done = nullptr);

private:
	struct Mutation {
		Fn<Storage::LocalData::Result(QJsonObject &)> change;
		Fn<void(LocalNavigationPreferencesState &)> optimistic;
		Fn<void(bool)> done;
	};

	void loadSnapshot();
	void mutate(Mutation mutation);
	void runNextMutation();
	void fail(QString message, Fn<void(bool)> done);
	[[nodiscard]] static LocalNavigationPreferencesState Parse(
		const QJsonObject &module);

	const not_null<Main::Session*> _session;
	const std::shared_ptr<Storage::LocalData::Store> _store;
	LocalNavigationPreferencesState _committed;
	LocalNavigationPreferencesState _state;
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
