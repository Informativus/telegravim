/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_pinned_items.h"

#include "main/main_session.h"
#include "storage/local_data_store.h"
#include "storage/local_pinned_items_format.h"
#include "storage/storage_domain.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include <QtCore/QJsonArray>
#include <QtCore/QPointer>

#include <algorithm>
#include <map>
#include <set>

namespace Data {
namespace {

using Account = Storage::LocalData::Account;
using Result = Storage::LocalData::Result;
namespace Format = Storage::LocalData::PinnedItems;

[[nodiscard]] Account SessionAccount(not_null<Main::Session*> session) {
	return {
		session->isTestMode() ? u"test"_q : u"production"_q,
		QString::number(session->userId().bare),
	};
}

[[nodiscard]] QJsonObject WritePeer(PeerId peer, QJsonObject result = {}) {
	const auto type = peerIsUser(peer)
		? u"user"_q
		: peerIsChat(peer)
		? u"chat"_q
		: peerIsChannel(peer)
		? u"channel"_q
		: QString();
	result.insert(u"peer_type"_q, type);
	result.insert(u"peer_id"_q,
		QString::number(peer.value & PeerId::kChatTypeMask));
	return result;
}

[[nodiscard]] PeerId ReadPeer(const QJsonObject &peer) {
	const auto type = peer.value(u"peer_type"_q).toString();
	const auto id = peer.value(u"peer_id"_q).toString().toULongLong();
	return (type == u"user"_q)
		? peerFromUser(UserId(id))
		: (type == u"chat"_q)
		? peerFromChat(ChatId(id))
		: (type == u"channel"_q)
		? peerFromChannel(ChannelId(id))
		: PeerId();
}

[[nodiscard]] bool ValidPeer(PeerId peer) {
	return !Format::PeerKey(WritePeer(peer)).isEmpty()
		&& (peer.value >> 48) <= ChannelId::kShift;
}

[[nodiscard]] std::vector<PeerId> ReadOrder(const QJsonArray &values) {
	auto result = std::vector<PeerId>();
	result.reserve(values.size());
	for (const auto &value : values) {
		if (const auto peer = ReadPeer(value.toObject())) {
			result.push_back(peer);
		}
	}
	return result;
}

[[nodiscard]] std::vector<PeerId> MergeOrder(
		const std::vector<PeerId> &desired,
		const std::vector<PeerId> &retained) {
	auto result = std::vector<PeerId>();
	auto seen = std::set<PeerId>();
	result.reserve(desired.size() + retained.size());
	for (const auto &list : { &desired, &retained }) {
		for (const auto peer : *list) {
			if (seen.insert(peer).second) {
				result.push_back(peer);
			}
		}
	}
	return result;
}

[[nodiscard]] QJsonArray WriteOrder(
		const std::vector<PeerId> &order,
		const QJsonArray &previous) {
	auto preserved = std::map<PeerId, QJsonObject>();
	for (const auto &value : previous) {
		const auto object = value.toObject();
		preserved.emplace(ReadPeer(object), object);
	}
	auto result = QJsonArray();
	for (const auto peer : order) {
		result.push_back(WritePeer(peer, preserved[peer]));
	}
	return result;
}

[[nodiscard]] int FindRecord(const QJsonArray &records, const QString &id) {
	for (auto i = 0; i != records.size(); ++i) {
		if (records[i].toObject().value(u"id"_q).toString() == id) {
			return i;
		}
	}
	return -1;
}

void PutRecord(QJsonArray &records, int index, QJsonObject value) {
	if (index < 0) {
		records.push_back(std::move(value));
	} else {
		records[index] = std::move(value);
	}
}

[[nodiscard]] Result Invalid(const QString &message) {
	return { Storage::LocalData::Error::Invalid, message };
}

[[nodiscard]] bool ActiveScope(
		const QJsonObject &module,
		const Account &account,
		const QString &scope) {
	if (!scope.startsWith(u"filter:"_q)) {
		return Format::ValidScope(scope);
	}
	const auto parts = scope.split(u':');
	if (parts.size() != 3) {
		return false;
	}
	const auto records = module.value(u"records"_q).toArray();
	const auto index = FindRecord(records, Format::FilterId(account, parts[1]));
	if (index < 0) {
		return false;
	}
	const auto record = records[index].toObject();
	return !record.value(u"deleted"_q).toBool()
		&& record.value(u"scope_id"_q).toString() == parts[2];
}

[[nodiscard]] Result UpdateOrder(
		QJsonArray &records,
		const Account &account,
		const QString &scope,
		const std::vector<PeerId> &visible) {
	const auto id = Format::OrderId(account, scope);
	const auto index = FindRecord(records, id);
	auto record = index < 0 ? QJsonObject() : records[index].toObject();
	const auto previous = record.value(u"peers"_q).toArray();
	const auto order = MergeOrder(visible, ReadOrder(previous));
	if (order.size() > Format::kMaximumOrder) {
		return Invalid(u"Too many local pinned items in this list."_q);
	}
	record.insert(u"id"_q, id);
	record.insert(u"kind"_q, u"order"_q);
	record.insert(u"scope"_q, scope);
	record.insert(u"deleted"_q, false);
	record.insert(u"peers"_q, WriteOrder(order, previous));
	PutRecord(records, index, std::move(record));
	return {};
}

void PruneOrderPeer(
		QJsonArray &records,
		const Account &account,
		const QString &scope,
		PeerId peer) {
	const auto index = FindRecord(records, Format::OrderId(account, scope));
	if (index < 0) {
		return;
	}
	auto record = records[index].toObject();
	auto order = record.value(u"peers"_q).toArray();
	for (auto i = order.size(); i > 0; --i) {
		if (ReadPeer(order[i - 1].toObject()) == peer) {
			order.removeAt(i - 1);
		}
	}
	record.insert(u"peers"_q, order);
	records[index] = std::move(record);
}

void UpdatePin(
		QJsonArray &records,
		const Account &account,
		const QString &scope,
		PeerId peer,
		bool pinned) {
	const auto id = Format::PinId(account, scope, WritePeer(peer));
	const auto index = FindRecord(records, id);
	auto record = index < 0 ? QJsonObject() : records[index].toObject();
	record.insert(u"id"_q, id);
	record.insert(u"kind"_q, u"pin"_q);
	record.insert(u"scope"_q, scope);
	auto peerData = record.value(u"peer"_q).toObject();
	const auto orderIndex = FindRecord(records, Format::OrderId(account, scope));
	if (orderIndex >= 0) {
		const auto orderRecord = records[orderIndex].toObject();
		for (const auto &value : orderRecord.value(u"peers"_q).toArray()) {
			const auto object = value.toObject();
			if (ReadPeer(object) == peer) {
				if (!pinned && object.size() > 2) {
					auto retained = record.value(u"retained_order_peers"_q).toArray();
					if (!retained.contains(object)) {
						retained.push_back(object);
					}
					record.insert(u"retained_order_peers"_q, retained);
				}
				for (auto i = object.begin(); i != object.end(); ++i) {
					if (!peerData.contains(i.key())) {
						peerData.insert(i.key(), i.value());
					}
				}
				break;
			}
		}
	}
	record.insert(u"peer"_q, WritePeer(peer, std::move(peerData)));
	record.insert(u"deleted"_q, !pinned);
	PutRecord(records, index, std::move(record));
}

[[nodiscard]] Result ValidateLoaded(
		const QJsonObject &module,
		const Account &account) {
	if (module.value(u"schema_version"_q).toInt() != 1) {
		return { Storage::LocalData::Error::Unsupported,
			u"Local pinned items require a newer application."_q };
	}
	return Format::Validate(module, account);
}

} // namespace

struct LocalPinnedItems::State {
	struct Scope {
		std::map<PeerId, bool> pins;
		std::vector<PeerId> order;
	};
	struct Filter {
		QString incarnation;
		bool deleted = false;
	};
	std::map<QString, Scope> scopes;
	std::map<::FilterId, Filter> filters;
};

LocalPinnedItems::LocalPinnedItems(
	QObject *parent,
	not_null<Main::Session*> session)
: QObject(parent)
, _session(session)
, _store(session->domainLocal().localData())
, _committed(std::make_unique<State>())
, _state(std::make_unique<State>()) {
	_store->changes() | rpl::on_next([=] { reload(); }, _lifetime);
}

LocalPinnedItems::~LocalPinnedItems() = default;

auto LocalPinnedItems::Parse(const QJsonObject &module)
-> std::unique_ptr<State> {
	auto result = std::make_unique<State>();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		const auto kind = record.value(u"kind"_q).toString();
		const auto scope = record.value(u"scope"_q).toString();
		const auto deleted = record.value(u"deleted"_q).toBool();
		if (kind == u"pin"_q) {
			result->scopes[scope].pins[ReadPeer(record.value(u"peer"_q).toObject())]
				= !deleted;
		} else if (kind == u"order"_q && !deleted) {
			result->scopes[scope].order = ReadOrder(record.value(u"peers"_q).toArray());
		} else if (kind == u"filter"_q) {
			result->filters[record.value(u"filter_id"_q).toString().toInt()] = {
				record.value(u"scope_id"_q).toString(),
				deleted,
			};
		}
	}
	return result;
}

bool LocalPinnedItems::loaded() const {
	return _loaded;
}

bool LocalPinnedItems::busy() const {
	return _busy || !_mutations.empty();
}

const QString &LocalPinnedItems::error() const {
	return _error;
}

rpl::producer<> LocalPinnedItems::changes() const {
	return _changes.events();
}

std::vector<QString> LocalPinnedItems::scopes() const {
	auto result = std::vector<QString>();
	result.reserve(_state->scopes.size());
	for (const auto &entry : _state->scopes) {
		result.push_back(entry.first);
	}
	return result;
}

std::vector<PeerId> LocalPinnedItems::localPins(const QString &scope) const {
	auto result = std::vector<PeerId>();
	const auto i = _state->scopes.find(scope);
	if (i == end(_state->scopes)) {
		return result;
	}
	for (const auto &[peer, pinned] : i->second.pins) {
		if (pinned) {
			result.push_back(peer);
		}
	}
	return result;
}

std::optional<bool> LocalPinnedItems::overridden(
		const QString &scope,
		PeerId peer) const {
	const auto i = _state->scopes.find(scope);
	if (i == end(_state->scopes)) {
		return std::nullopt;
	}
	const auto pin = i->second.pins.find(peer);
	return (pin == end(i->second.pins))
		? std::nullopt : std::optional<bool>(pin->second);
}

std::vector<PeerId> LocalPinnedItems::overlay(
		const QString &scope,
		const std::vector<PeerId> &cloud) const {
	const auto i = _state->scopes.find(scope);
	if (i == end(_state->scopes)) {
		return cloud;
	}
	const auto &data = i->second;
	auto active = std::set<PeerId>(begin(cloud), end(cloud));
	for (const auto &[peer, pinned] : data.pins) {
		if (pinned) {
			active.insert(peer);
		} else {
			active.erase(peer);
		}
	}
	auto result = std::vector<PeerId>();
	result.reserve(active.size());
	for (const auto &order : { &data.order, &cloud }) {
		for (const auto peer : *order) {
			if (active.erase(peer)) {
				result.push_back(peer);
			}
		}
	}
	result.insert(end(result), begin(active), end(active));
	return result;
}

QString LocalPinnedItems::filterScope(FilterId id) const {
	const auto i = _state->filters.find(id);
	return (i == end(_state->filters) || i->second.deleted)
		? QString()
		: u"filter:"_q + QString::number(id) + u":"_q + i->second.incarnation;
}

auto LocalPinnedItems::filterScopes() const
-> std::vector<std::pair<FilterId, QString>> {
	auto result = std::vector<std::pair<FilterId, QString>>();
	for (const auto &[id, filter] : _state->filters) {
		if (!filter.deleted) {
			result.emplace_back(id, filterScope(id));
		}
	}
	return result;
}

void LocalPinnedItems::reload() {
	if (_busy || !_mutations.empty()) {
		_reloadPending = true;
		return;
	}
	loadSnapshot();
}

void LocalPinnedItems::loadSnapshot() {
	_busy = true;
	const auto weak = QPointer(this);
	const auto store = _store;
	const auto account = SessionAccount(_session);
	crl::async([=] {
		auto snapshot = Storage::LocalData::Snapshot();
		auto result = store->snapshot(snapshot);
		const auto module = result
			? Storage::LocalData::ReadModule(snapshot, account, u"pinned_items"_q)
			: QJsonObject();
		if (result) {
			result = ValidateLoaded(module, account);
		}
		auto parsed = result ? Parse(module) : std::make_unique<State>();
		crl::on_main([=, parsed = std::move(parsed)]() mutable {
			if (!weak) {
				return;
			}
			weak->_busy = false;
			weak->_error = result.message;
			if (result) {
				weak->_committed = std::move(parsed);
				weak->_loaded = true;
				weak->rebuildVisible();
			} else {
				auto pending = std::move(weak->_mutations);
				weak->_mutations.clear();
				weak->_reloadPending = false;
				weak->rebuildVisible();
				for (auto &mutation : pending) {
					if (mutation.done) {
						mutation.done(false);
					}
				}
			}
			if (!weak) {
				return;
			}
			weak->_changes.fire({});
			if (weak) {
				weak->runNextMutation();
			}
		});
	});
}

void LocalPinnedItems::rebuildVisible() {
	_state = std::make_unique<State>(*_committed);
	for (const auto &mutation : _mutations) {
		mutation.optimistic(*_state);
	}
}

void LocalPinnedItems::fail(QString message, Fn<void(bool)> done) {
	_error = std::move(message);
	const auto weak = QPointer(this);
	_changes.fire({});
	if (weak && done) {
		done(false);
	}
}

void LocalPinnedItems::mutate(Mutation mutation) {
	_error.clear();
	_mutations.push_back(std::move(mutation));
	_mutations.back().optimistic(*_state);
	const auto weak = QPointer(this);
	_changes.fire({});
	if (weak) {
		runNextMutation();
	}
}

void LocalPinnedItems::runNextMutation() {
	if (_busy) {
		return;
	}
	if (!_loaded && !_mutations.empty()) {
		loadSnapshot();
		return;
	}
	if (_mutations.empty()) {
		if (std::exchange(_reloadPending, false)) {
			loadSnapshot();
		}
		return;
	}
	_busy = true;
	const auto weak = QPointer(this);
	const auto store = _store;
	const auto account = SessionAccount(_session);
	const auto change = _mutations.front().change;
	crl::async([=] {
		auto result = store->mutate(account, u"pinned_items"_q,
			[=](QJsonObject &module) { return change(module); });
		auto snapshot = Storage::LocalData::Snapshot();
		auto read = store->snapshot(snapshot);
		const auto module = read
			? Storage::LocalData::ReadModule(snapshot, account, u"pinned_items"_q)
			: QJsonObject();
		if (read) {
			read = ValidateLoaded(module, account);
		}
		auto parsed = read ? Parse(module) : std::make_unique<State>();
		if (result && !read) {
			result = read;
		}
		crl::on_main([=, parsed = std::move(parsed)]() mutable {
			if (!weak) {
				return;
			}
			auto done = std::move(weak->_mutations.front().done);
			weak->_mutations.pop_front();
			weak->_busy = false;
			weak->_error = result.message;
			if (read) {
				weak->_committed = std::move(parsed);
			}
			weak->rebuildVisible();
			weak->_changes.fire({});
			if (done) {
				done(result.ok());
			}
			if (weak) {
				weak->runNextMutation();
			}
		});
	});
}

void LocalPinnedItems::setPinned(
		QString scope,
		PeerId peer,
		bool pinned,
		std::vector<PeerId> visibleOrder,
		Fn<void(bool)> done) {
	if (!Format::ValidScope(scope) || !ValidPeer(peer)) {
		return fail(u"Invalid local pinned item."_q, std::move(done));
	}
	if (pinned) {
		if (visibleOrder.size() > Format::kMaximumOrder
			|| !ranges::all_of(visibleOrder, ValidPeer)) {
			return fail(u"Invalid local pinned item."_q, std::move(done));
		}
		std::erase(visibleOrder, peer);
		visibleOrder.insert(begin(visibleOrder), peer);
		if (visibleOrder.size() > Format::kMaximumOrder) {
			return fail(u"Too many local pinned items in this list."_q, std::move(done));
		}
	} else {
		visibleOrder.clear();
	}
	const auto account = SessionAccount(_session);
	mutate({
		.change = [=](QJsonObject &module) {
			if (!ActiveScope(module, account, scope)) {
				return Invalid(u"This folder is no longer available."_q);
			}
			auto records = module.value(u"records"_q).toArray();
			UpdatePin(records, account, scope, peer, pinned);
			if (pinned) {
				const auto ordered = UpdateOrder(records, account, scope, visibleOrder);
				if (!ordered) {
					return ordered;
				}
			} else {
				PruneOrderPeer(records, account, scope, peer);
			}
			module.insert(u"records"_q, records);
			return Result();
		},
		.optimistic = [=](State &state) {
			auto &data = state.scopes[scope];
			data.pins[peer] = pinned;
			if (pinned) {
				data.order = MergeOrder(visibleOrder, data.order);
			} else {
				std::erase(data.order, peer);
			}
		},
		.done = std::move(done),
	});
}

void LocalPinnedItems::saveOrder(
		QString scope,
		std::vector<PeerId> visibleOrder,
		Fn<void(bool)> done) {
	if (!Format::ValidScope(scope)
		|| visibleOrder.size() > Format::kMaximumOrder
		|| !ranges::all_of(visibleOrder, ValidPeer)) {
		return fail(u"Invalid local pinned order."_q, std::move(done));
	}
	const auto account = SessionAccount(_session);
	mutate({
		.change = [=](QJsonObject &module) {
			if (!ActiveScope(module, account, scope)) {
				return Invalid(u"This folder is no longer available."_q);
			}
			auto records = module.value(u"records"_q).toArray();
			const auto ordered = UpdateOrder(records, account, scope, visibleOrder);
			if (!ordered) {
				return ordered;
			}
			module.insert(u"records"_q, records);
			return Result();
		},
		.optimistic = [=](State &state) {
			auto &order = state.scopes[scope].order;
			order = MergeOrder(visibleOrder, order);
		},
		.done = std::move(done),
	});
}

void LocalPinnedItems::removePeer(PeerId peer, Fn<void(bool)> done) {
	if (!ValidPeer(peer)) {
		return fail(u"Invalid local pinned item."_q, std::move(done));
	}
	const auto account = SessionAccount(_session);
	mutate({
		.change = [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			auto scopes = std::set<QString>{ u"main"_q, u"archive"_q };
			for (const auto &value : records) {
				const auto record = value.toObject();
				const auto kind = record.value(u"kind"_q).toString();
				const auto scope = record.value(u"scope"_q).toString();
				if (kind == u"filter") {
					scopes.insert(u"filter:"_q
						+ record.value(u"filter_id"_q).toString()
						+ u":"_q + record.value(u"scope_id"_q).toString());
				} else if ((kind == u"pin" || kind == u"order")
					&& scope.startsWith(u"filter:")) {
					scopes.insert(scope);
				}
			}
			for (const auto &scope : scopes) {
				UpdatePin(records, account, scope, peer, false);
				PruneOrderPeer(records, account, scope, peer);
			}
			module.insert(u"records"_q, records);
			return Result();
		},
		.optimistic = [=](State &state) {
			auto scopes = std::set<QString>{ u"main"_q, u"archive"_q };
			for (const auto &[scope, data] : state.scopes) {
				if (scope.startsWith(u"filter:")) {
					scopes.insert(scope);
				}
			}
			for (const auto &[id, filter] : state.filters) {
				scopes.insert(u"filter:"_q + QString::number(id)
					+ u":"_q + filter.incarnation);
			}
			for (const auto &scope : scopes) {
				auto &data = state.scopes[scope];
				data.pins[peer] = false;
				std::erase(data.order, peer);
			}
		},
		.done = std::move(done),
	});
}

void LocalPinnedItems::syncFilters(
		std::vector<FilterId> ids,
		Fn<void(bool)> done) {
	auto active = std::set<FilterId>(begin(ids), end(ids));
	active.erase(0);
	if (!ranges::all_of(active, [](FilterId id) { return id > 0; })) {
		return fail(u"Invalid folder identity."_q, std::move(done));
	}
	if (!_loaded) {
		return fail(u"Local pinned items are still loading."_q, std::move(done));
	}
	const auto account = SessionAccount(_session);
	auto changes = std::map<FilterId, State::Filter>();
	for (const auto id : active) {
		const auto previous = _state->filters.find(id);
		if (previous == end(_state->filters) || previous->second.deleted) {
			changes.emplace(id, State::Filter{
				(previous == end(_state->filters))
					? Format::StableId(account, u"incarnation:"_q + QString::number(id))
					: Storage::LocalData::NewId(),
				false,
			});
		}
	}
	for (const auto &[id, filter] : _state->filters) {
		if (!filter.deleted && !active.contains(id)) {
			changes.emplace(id, State::Filter{ filter.incarnation, true });
		}
	}
	if (changes.empty()) {
		if (done) {
			done(true);
		}
		return;
	}
	mutate({
		.change = [=](QJsonObject &module) {
			auto records = module.value(u"records"_q).toArray();
			for (const auto &[id, filter] : changes) {
				const auto key = QString::number(id);
				const auto recordId = Format::FilterId(account, key);
				const auto index = FindRecord(records, recordId);
				auto record = index < 0 ? QJsonObject() : records[index].toObject();
				record.insert(u"id"_q, recordId);
				record.insert(u"kind"_q, u"filter"_q);
				record.insert(u"filter_id"_q, key);
				record.insert(u"scope_id"_q, filter.incarnation);
				record.insert(u"deleted"_q, filter.deleted);
				PutRecord(records, index, std::move(record));
			}
			module.insert(u"records"_q, records);
			return Result();
		},
		.optimistic = [=](State &state) {
			for (const auto &[id, filter] : changes) {
				state.filters[id] = filter;
			}
		},
		.done = std::move(done),
	});
}

} // namespace Data
