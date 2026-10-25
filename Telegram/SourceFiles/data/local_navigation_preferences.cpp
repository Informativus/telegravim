/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_navigation_preferences.h"

#include "main/main_session.h"
#include "storage/local_data_store.h"
#include "storage/local_navigation_preferences_format.h"
#include "storage/storage_domain.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>
#include <QtCore/QPointer>
#include <QtCore/QSet>

namespace Data {
namespace {

using Account = Storage::LocalData::Account;
using Result = Storage::LocalData::Result;
namespace Format = Storage::LocalData::NavigationPreferences;

[[nodiscard]] Account SessionAccount(not_null<Main::Session*> session) {
	return {
		session->isTestMode() ? u"test"_q : u"production"_q,
		QString::number(session->userId().bare),
	};
}

[[nodiscard]] int FindSettingsRecord(const QJsonArray &records) {
	for (auto i = 0; i != records.size(); ++i) {
		if (records.at(i).toObject().value(u"kind"_q).toString()
				== u"settings"_q) {
			return i;
		}
	}
	return -1;
}

[[nodiscard]] QJsonObject SettingsRecord(
		const Account &account,
		const LocalNavigationPreferencesState &state) {
	auto order = QJsonArray();
	for (const auto &reference : state.visibleOrder) {
		order.push_back(reference);
	}
	auto result = QJsonObject{
		{ u"id"_q, Format::SettingsId(account) },
		{ u"kind"_q, u"settings"_q },
		{ u"scope"_q, u"account"_q },
		{ u"startup_ref"_q, state.startupRef.isEmpty()
			? QJsonValue(QJsonValue::Null)
			: QJsonValue(state.startupRef) },
		{ u"last_section_ref"_q, state.lastSectionRef.isEmpty()
			? QJsonValue(QJsonValue::Null)
			: QJsonValue(state.lastSectionRef) },
		{ u"restore_last_section"_q, state.restoreLastSection },
		{ u"visible_order"_q, std::move(order) },
		{ u"deleted"_q, false },
	};
	return result;
}

void PutRecordPreserving(
		QJsonArray &records,
		int index,
		QJsonObject value) {
	if (index >= 0) {
		auto merged = records.at(index).toObject();
		for (auto i = value.begin(); i != value.end(); ++i) {
			merged.insert(i.key(), i.value());
		}
		value = std::move(merged);
	}
	if (index < 0) {
		records.push_back(std::move(value));
	} else {
		records[index] = std::move(value);
	}
}

} // namespace

LocalNavigationPreferences::LocalNavigationPreferences(
		QObject *parent,
		not_null<Main::Session*> session)
: QObject(parent)
, _session(session)
, _store(session->domainLocal().localData()) {
	_store->changes() | rpl::on_next([=] { reload(); }, _lifetime);
	crl::on_main(this, [=] { reload(); });
}

LocalNavigationPreferences::~LocalNavigationPreferences() = default;

bool LocalNavigationPreferences::loaded() const {
	return _loaded;
}

bool LocalNavigationPreferences::busy() const {
	return _busy || !_mutations.empty();
}

const QString &LocalNavigationPreferences::error() const {
	return _error;
}

const LocalNavigationPreferencesState &LocalNavigationPreferences::state() const {
	return _state;
}

rpl::producer<> LocalNavigationPreferences::changes() const {
	return _changes.events();
}

void LocalNavigationPreferences::reload() {
	if (_stopped) {
		return;
	}
	if (_busy || !_mutations.empty()) {
		_reloadPending = true;
		return;
	}
	loadSnapshot();
}

void LocalNavigationPreferences::stop() {
	_stopped = true;
	_busy = false;
	_reloadPending = false;
	_mutations.clear();
	_lifetime.destroy();
}

LocalNavigationPreferencesState LocalNavigationPreferences::Parse(
		const QJsonObject &module) {
	auto result = LocalNavigationPreferencesState();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		if (record.value(u"kind"_q).toString() != u"settings"_q
			|| record.value(u"deleted"_q).toBool()) {
			continue;
		}
		result.startupRef = record.value(u"startup_ref"_q).toString();
		result.lastSectionRef = record.value(u"last_section_ref"_q).toString();
		result.restoreLastSection = record.value(
			u"restore_last_section"_q).toBool();
		for (const auto &entry : record.value(u"visible_order"_q).toArray()) {
			result.visibleOrder.push_back(entry.toString());
		}
		break;
	}
	return result;
}

void LocalNavigationPreferences::loadSnapshot() {
	_busy = true;
	const auto weak = QPointer(this);
	const auto store = _store;
	const auto account = SessionAccount(_session);
	crl::async([=] {
		auto snapshot = Storage::LocalData::Snapshot();
		auto result = store->snapshot(snapshot);
		const auto module = result
			? Storage::LocalData::ReadModule(
				snapshot,
				account,
				u"navigation_preferences"_q)
			: QJsonObject();
		if (result) {
			result = Format::Validate(module, account);
		}
		auto parsed = result
			? Parse(module)
			: LocalNavigationPreferencesState();
		crl::on_main([=, parsed = std::move(parsed)]() mutable {
			if (!weak || weak->_stopped) {
				return;
			}
			weak->_busy = false;
			weak->_error = result.message;
			if (result) {
				weak->_committed = std::move(parsed);
				weak->_loaded = true;
				weak->_state = weak->_committed;
				for (const auto &pending : weak->_mutations) {
					if (pending.optimistic) pending.optimistic(weak->_state);
				}
			} else {
				weak->_loaded = true;
				weak->_committed = LocalNavigationPreferencesState();
				weak->_state = weak->_committed;
				auto pending = std::move(weak->_mutations);
				weak->_mutations.clear();
				weak->_reloadPending = false;
				for (auto &mutation : pending) {
					if (mutation.done) mutation.done(false);
					if (!weak) return;
				}
			}
			weak->_changes.fire({});
			if (!weak || weak->_stopped) return;
			weak->runNextMutation();
		});
	});
}

void LocalNavigationPreferences::mutate(Mutation mutation) {
	const auto weak = QPointer(this);
	_error.clear();
	_mutations.push_back(std::move(mutation));
	_state = _committed;
	for (const auto &pending : _mutations) {
		if (pending.optimistic) pending.optimistic(_state);
	}
	_changes.fire({});
	if (weak && !weak->_stopped) {
		runNextMutation();
	}
}

void LocalNavigationPreferences::fail(QString message, Fn<void(bool)> done) {
	_error = std::move(message);
	_changes.fire({});
	if (done) done(false);
}

void LocalNavigationPreferences::runNextMutation() {
	if (_busy) return;
	if (!_loaded && !_mutations.empty()) {
		loadSnapshot();
		return;
	}
	if (_mutations.empty()) {
		if (std::exchange(_reloadPending, false)) loadSnapshot();
		return;
	}
	_busy = true;
	const auto weak = QPointer(this);
	const auto store = _store;
	const auto account = SessionAccount(_session);
	const auto change = _mutations.front().change;
	crl::async([=] {
		auto result = store->mutate(
			account,
			u"navigation_preferences"_q,
			[=](QJsonObject &module) { return change(module); });
		auto snapshot = Storage::LocalData::Snapshot();
		auto read = store->snapshot(snapshot);
		const auto module = read
			? Storage::LocalData::ReadModule(
				snapshot,
				account,
				u"navigation_preferences"_q)
			: QJsonObject();
		if (read) {
			read = Format::Validate(module, account);
		}
		auto parsed = read
			? Parse(module)
			: LocalNavigationPreferencesState();
		if (result && !read) result = read;
		crl::on_main([=, parsed = std::move(parsed)]() mutable {
			if (!weak || weak->_stopped) return;
			auto done = std::move(weak->_mutations.front().done);
			weak->_mutations.pop_front();
			weak->_busy = false;
			weak->_error = result.message;
			if (read) {
				weak->_committed = std::move(parsed);
			}
			weak->_state = weak->_committed;
			for (const auto &pending : weak->_mutations) {
				if (pending.optimistic) pending.optimistic(weak->_state);
			}
			weak->_changes.fire({});
			if (!weak || weak->_stopped) return;
			if (done) done(result.ok());
			if (!weak || weak->_stopped) return;
			weak->runNextMutation();
		});
	});
}

void LocalNavigationPreferences::setStartupRef(
		QString reference,
		Fn<void(bool)> done) {
	if (!reference.isEmpty() && !Format::ValidReference(reference)) {
		fail(u"Invalid startup folder reference."_q, std::move(done));
		return;
	}
	mutate({
		.change = [account = SessionAccount(_session), reference](QJsonObject &module) {
			const auto index = FindSettingsRecord(module.value(u"records"_q).toArray());
			auto records = module.value(u"records"_q).toArray();
			auto state = Parse(module);
			state.startupRef = reference;
			PutRecordPreserving(records, index, SettingsRecord(account, state));
			module.insert(u"records"_q, std::move(records));
			return Result();
		},
		.optimistic = [reference](auto &state) { state.startupRef = reference; },
		.done = std::move(done),
	});
}

void LocalNavigationPreferences::setLastSectionRef(
		QString reference,
		Fn<void(bool)> done) {
	if (!reference.isEmpty() && !Format::ValidReference(reference)) {
		fail(u"Invalid last section reference."_q, std::move(done));
		return;
	}
	mutate({
		.change = [account = SessionAccount(_session), reference](QJsonObject &module) {
			const auto index = FindSettingsRecord(module.value(u"records"_q).toArray());
			auto records = module.value(u"records"_q).toArray();
			auto state = Parse(module);
			state.lastSectionRef = reference;
			PutRecordPreserving(records, index, SettingsRecord(account, state));
			module.insert(u"records"_q, std::move(records));
			return Result();
		},
		.optimistic = [reference](auto &state) { state.lastSectionRef = reference; },
		.done = std::move(done),
	});
}

void LocalNavigationPreferences::setRestoreLastSection(
		bool value,
		Fn<void(bool)> done) {
	mutate({
		.change = [account = SessionAccount(_session), value](QJsonObject &module) {
			const auto index = FindSettingsRecord(module.value(u"records"_q).toArray());
			auto records = module.value(u"records"_q).toArray();
			auto state = Parse(module);
			state.restoreLastSection = value;
			PutRecordPreserving(records, index, SettingsRecord(account, state));
			module.insert(u"records"_q, std::move(records));
			return Result();
		},
		.optimistic = [value](auto &state) { state.restoreLastSection = value; },
		.done = std::move(done),
	});
}

void LocalNavigationPreferences::setVisibleOrder(
		std::vector<QString> order,
		Fn<void(bool)> done) {
	if (order.size() > Format::kMaximumOrder) {
		fail(u"Navigation order is too large."_q, std::move(done));
		return;
	}
	auto references = QSet<QString>();
	for (const auto &reference : order) {
		if (!Format::ValidReference(reference)
			|| references.contains(reference)) {
			fail(u"Invalid navigation folder reference."_q, std::move(done));
			return;
		}
		references.insert(reference);
	}
	mutate({
		.change = [account = SessionAccount(_session), order](QJsonObject &module) {
			const auto index = FindSettingsRecord(module.value(u"records"_q).toArray());
			auto records = module.value(u"records"_q).toArray();
			auto state = Parse(module);
			state.visibleOrder = order;
			PutRecordPreserving(records, index, SettingsRecord(account, state));
			module.insert(u"records"_q, std::move(records));
			return Result();
		},
		.optimistic = [order = std::move(order)](auto &state) {
			state.visibleOrder = order;
		},
		.done = std::move(done),
	});
}

} // namespace Data
