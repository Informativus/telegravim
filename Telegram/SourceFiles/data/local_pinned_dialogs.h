/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "data/data_peer_id.h"
#include "dialogs/dialogs_key.h"

#include <rpl/event_stream.h>
#include <QtCore/QObject>
#include <map>
#include <set>

class History;

namespace MTP {
class Sender;
} // namespace MTP

namespace Dialogs {
class PinnedList;
} // namespace Dialogs

namespace Data {

class Session;
class Folder;
class SavedMessages;
class LocalPinnedItems;

class LocalPinnedDialogs final : public QObject {
public:
	explicit LocalPinnedDialogs(not_null<Session*> owner);
	~LocalPinnedDialogs();

	[[nodiscard]] bool supports(Dialogs::Key key, FilterId filterId = 0) const;
	[[nodiscard]] bool includes(FilterId filterId, not_null<History*> history) const;
	[[nodiscard]] bool busy() const;
	[[nodiscard]] rpl::producer<QString> errors() const;

	void setPinned(
		Dialogs::Key key,
		FilterId filterId,
		bool pinned,
		Fn<void(bool)> done = nullptr);
	void remove(PeerId peer);
	void saveOrder(Folder *folder);
	void saveOrder(FilterId filterId);
	void saveOrder(not_null<SavedMessages*> saved);

	void attach(not_null<Dialogs::PinnedList*> list, Folder *folder);
	void attach(not_null<Dialogs::PinnedList*> list, FilterId filterId);
	void attach(not_null<Dialogs::PinnedList*> list, not_null<SavedMessages*> saved);
	void scheduleRefresh();
	void stop();

private:
	struct Hydration {
		crl::time next = 0;
		int attempts = 0;
		int count = -1;
		int requestId = 0;
		bool pending = false;
	};

	[[nodiscard]] QString scope(Dialogs::Key key, FilterId filterId) const;
	[[nodiscard]] std::vector<Dialogs::Key> project(
		const QString &scope,
		const std::vector<Dialogs::Key> &cloud) const;
	[[nodiscard]] Dialogs::Key resolve(const QString &scope, PeerId peer) const;
	void refresh();
	void syncFilters();
	void hydrate();
	void storeOrder(const QString &scope, not_null<Dialogs::PinnedList*> list);

	const not_null<Session*> _owner;
	const std::unique_ptr<LocalPinnedItems> _items;
	std::unique_ptr<MTP::Sender> _api;
	base::Timer _hydrateTimer;
	std::map<std::pair<bool, PeerId>, Hydration> _hydration;
	std::map<QString, Hydration> _pages;
	QString _filterSyncAttempt;
	std::set<PeerId> _watched;
	std::set<PeerId> _confirmed;
	std::set<PeerId> _removed;
	QString _error;
	int _hydrating = 0;
	bool _ready = false;
	bool _scheduled = false;
	bool _refreshing = false;
	bool _filtersSyncing = false;
	bool _stopped = false;
	rpl::event_stream<QString> _errors;
	rpl::lifetime _lifetime;

};

} // namespace Data
