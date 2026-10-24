/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_saved_tags_search.h"

#include "data/data_message_reactions.h"
#include "data/data_session.h"
#include "data/local_saved_tags.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"

#include <QtCore/QPointer>
#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

namespace Data {
namespace {

constexpr auto kPageSize = 50;

} // namespace

LocalSavedTagsSearch::LocalSavedTagsSearch(not_null<Main::Session*> session)
: _session(session)
, _model(session->data().reactions().localSavedTags())
, _api(&session->mtp())
, _requestTimer([=] {
	_api.request(base::take(_requestId)).cancel();
	_loading = false;
	publish(tr::lng_local_saved_tags_unavailable(tr::now));
}) {
	_model.changes() | rpl::on_next([=] {
		if (_active && _model.loaded()
			&& (_waitingForModel || _model.revision() != _revision)) {
			search(_query);
		} else if (_active && !_model.loaded() && !_model.busy()
			&& !_model.error().isEmpty()) {
			_loading = false;
			publish(_model.error());
		}
	}, _lifetime);
}

LocalSavedTagsSearch::~LocalSavedTagsSearch() {
	cancel();
}

void LocalSavedTagsSearch::search(LocalSavedTagsSearchQuery query) {
	cancel();
	_query = std::move(query);
	_active = _loading = true;
	_first = true;
	_complete = false;
	_offset = _matched = 0;
	_candidates.clear();
	_page.clear();
	_cancel = std::make_shared<std::atomic<bool>>(false);
	_revision = _model.revision();
	_waitingForModel = !_model.loaded();
	if (_model.loaded()) {
		prepare();
	} else if (!_model.busy()) {
		_model.reload();
	}
}

void LocalSavedTagsSearch::cancel() {
	++_generation;
	_active = _loading = false;
	_requestTimer.cancel();
	if (_cancel) {
		_cancel->store(true, std::memory_order_relaxed);
	}
	_api.request(base::take(_requestId)).cancel();
}

bool LocalSavedTagsSearch::busy() const {
	return _loading;
}

rpl::producer<LocalSavedTagsSearchPage> LocalSavedTagsSearch::pages() const {
	return _pages.events();
}

void LocalSavedTagsSearch::prepare() {
	const auto messages = _model.messagesSnapshot();
	auto wanted = QStringList();
	for (const auto &reaction : _query.tags) {
		const auto id = _model.tagId(reaction);
		if (id.isEmpty()) {
			_complete = true;
			_loading = false;
			const auto generation = _generation;
			crl::on_main(this, [=] {
				if (generation == _generation) {
					publish();
				}
			});
			return;
		}
		wanted.push_back(id);
	}
	const auto weak = QPointer(this);
	const auto generation = _generation;
	const auto cancel = _cancel;
	crl::async([=] {
		auto candidates = MessageIdsList();
		if (messages) {
			for (const auto &message : *messages) {
				if (cancel->load(std::memory_order_relaxed)) {
					return;
				}
				if (ranges::all_of(wanted, [&](const auto &tag) {
					return message.tags.contains(tag);
				})) {
					candidates.push_back(message.id);
				}
			}
		}
		crl::on_main([=, candidates = std::move(candidates)]() mutable {
			if (!weak || generation != weak->_generation) {
				return;
			}
			weak->_candidates = std::move(candidates);
			weak->loadBatch();
		});
	});
}

void LocalSavedTagsSearch::more() {
	if (!_active || _loading || _complete) {
		return;
	}
	_loading = true;
	if (!_model.loaded()) {
		_model.reload();
	} else {
		loadBatch();
	}
}

void LocalSavedTagsSearch::loadBatch() {
	if (_offset == int(_candidates.size())) {
		_complete = true;
		_loading = false;
		publish();
		return;
	}
	const auto end = std::min(_offset + kPageSize, int(_candidates.size()));
	auto missing = QVector<MTPInputMessage>();
	for (auto i = _offset; i != end; ++i) {
		const auto id = _candidates[i];
		if (!_session->data().message(id)) {
			missing.push_back(MTP_inputMessageID(MTP_int(id.msg)));
		}
	}
	if (missing.empty()) {
		finishBatch(end);
		return;
	}
	const auto generation = _generation;
	_requestId = _api.request(MTPmessages_GetMessages(
		MTP_vector<MTPInputMessage>(missing)
	)).done([=](const MTPmessages_Messages &result) {
		if (generation != _generation) {
			return;
		}
		_requestId = 0;
		_requestTimer.cancel();
		const auto weak = QPointer(this);
		_session->data().processExistingMessages(nullptr, result);
		if (weak && generation == _generation) {
			finishBatch(end);
		}
	}).fail([=](const MTP::Error &error) {
		if (generation != _generation) {
			return;
		}
		_requestId = 0;
		_requestTimer.cancel();
		_loading = false;
		publish(MTP::IgnoreError(error)
			? tr::lng_local_saved_tags_unavailable(tr::now)
			: error.type());
	}).handleFloodErrors().send();
	_requestTimer.callOnce(crl::time(30000));
}

void LocalSavedTagsSearch::finishBatch(int end) {
	const auto query = _query.text.trimmed();
	for (auto i = _offset; i != end; ++i) {
		const auto item = _session->data().message(_candidates[i]);
		if (item && item->history()->peer->isSelf()
			&& (!_query.savedPeer || item->sublistPeerId() == _query.savedPeer)
			&& (!_query.topMsgId || item->inThread(_query.topMsgId))
			&& (!_query.pinned || item->isPinned())
			&& item->originalText().text.contains(query, Qt::CaseInsensitive)) {
			_page.push_back(item->fullId());
			++_matched;
		}
	}
	_offset = end;
	_complete = (_offset == int(_candidates.size()));
	if (_page.size() >= kPageSize || _complete) {
		_loading = false;
		publish();
	} else {
		const auto generation = _generation;
		crl::on_main(this, [=] {
			if (generation == _generation) {
				loadBatch();
			}
		});
	}
}

void LocalSavedTagsSearch::publish(QString error) {
	auto page = LocalSavedTagsSearchPage{
		.messages = base::take(_page),
		.total = _complete ? _matched : -1,
		.start = std::exchange(_first, false),
		.complete = _complete,
		.error = std::move(error),
	};
	_pages.fire(std::move(page));
}

} // namespace Data
