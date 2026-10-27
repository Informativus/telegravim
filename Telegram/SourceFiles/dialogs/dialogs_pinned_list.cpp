/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "dialogs/dialogs_pinned_list.h"

#include "data/data_saved_messages.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/dialogs_entry.h"
#include "history/history.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_forum.h"

#include <set>

namespace Dialogs {

PinnedList::PinnedList(FilterId filterId, int limit)
: _filterId(filterId)
, _limit(limit) {
	Expects(limit > 0);
}

void PinnedList::setProjection(
		Fn<std::vector<Key>(const std::vector<Key>&)> project,
		Fn<bool(Key)> owns) {
	Expects(bool(project));

	if (!_projection) {
		_cloudData = _data;
	}
	_projection = std::move(project);
	_owns = std::move(owns);
	refreshProjection();
}

bool PinnedList::hasProjection() const {
	return bool(_projection);
}

const std::vector<Key> &PinnedList::cloudOrder() const {
	return _projection ? _cloudData : _data;
}

void PinnedList::applyVisibleOrder(std::vector<Key> order) {
	const auto previous = base::take(_data);
	_data = std::move(order);
	const auto included = std::set<Key>(begin(_data), end(_data));
	for (const auto key : previous) {
		if (!included.contains(key) && (!_owns || _owns(key))) {
			key.entry()->cachePinnedIndex(_filterId, 0);
		}
	}
	for (auto i = 0; i < int(_data.size()); ++i) {
		_data[i].entry()->cachePinnedIndex(_filterId, i + 1);
	}
}

void PinnedList::refreshProjection() {
	if (!_projection) {
		return;
	} else if (_refreshing) {
		_refreshAgain = true;
		return;
	}
	_refreshing = true;
	do {
		_refreshAgain = false;
		auto order = _projection(_cloudData);
		if (order != _data) {
			applyVisibleOrder(std::move(order));
		} else {
			for (auto i = 0; i < int(_data.size()); ++i) {
				_data[i].entry()->cachePinnedIndex(_filterId, i + 1);
			}
		}
	} while (_refreshAgain);
	_refreshing = false;
}

void PinnedList::removeEntry(Key key) {
	if (!_projection) {
		setPinned(key, false);
		return;
	}
	if (const auto i = ranges::find(_cloudData, key); i != end(_cloudData)) {
		_cloudData.erase(i);
	}
	if (const auto i = ranges::find(_data, key); i != end(_data)) {
		_data.erase(i);
		key.entry()->cachePinnedIndex(_filterId, 0);
		for (auto index = 0; index < int(_data.size()); ++index) {
			_data[index].entry()->cachePinnedIndex(_filterId, index + 1);
		}
	}
}

void PinnedList::applyCloudOrder(std::vector<Key> order) {
	_cloudData = std::move(order);
	refreshProjection();
}

void PinnedList::setLimit(int limit) {
	Expects(limit > 0);

	if (_limit == limit) {
		return;
	}
	_limit = limit;
	if (_projection) {
		if (_cloudData.size() > _limit) {
			_cloudData.resize(_limit);
		}
		refreshProjection();
	} else {
		applyLimit(_limit);
	}
}

void PinnedList::addPinned(Key key) {
	Expects(key.entry()->folderKnown());

	if (_projection) {
		if (!ranges::contains(_cloudData, key)) {
			if (_cloudData.size() >= _limit) {
				_cloudData.resize(_limit - 1);
			}
			_cloudData.push_back(key);
		}
		refreshProjection();
	} else {
		addPinnedGetPosition(key);
	}
}

int PinnedList::addPinnedGetPosition(Key key) {
	const auto already = ranges::find(_data, key);
	if (already != end(_data)) {
		return already - begin(_data);
	}
	applyLimit(_limit - 1);
	const auto position = int(_data.size());
	_data.push_back(key);
	key.entry()->cachePinnedIndex(_filterId, position + 1);
	return position;
}

void PinnedList::setPinned(Key key, bool pinned) {
	Expects(key.entry()->folderKnown() || _filterId != 0);

	if (_projection) {
		const auto i = ranges::find(_cloudData, key);
		if (i != end(_cloudData)) {
			_cloudData.erase(i);
		}
		if (pinned) {
			if (_cloudData.size() >= _limit) {
				_cloudData.resize(_limit - 1);
			}
			_cloudData.insert(begin(_cloudData), key);
		}
		refreshProjection();
		return;
	}
	if (pinned) {
		const int position = addPinnedGetPosition(key);
		if (position) {
			const auto begin = _data.begin();
			std::rotate(begin, begin + position, begin + position + 1);
			for (auto i = 0; i != position + 1; ++i) {
				_data[i].entry()->cachePinnedIndex(_filterId, i + 1);
			}
		}
	} else if (const auto it = ranges::find(_data, key); it != end(_data)) {
		const auto index = int(it - begin(_data));
		_data.erase(it);
		key.entry()->cachePinnedIndex(_filterId, 0);
		// cachePinnedIndex() may re-enter and erase from _data (for example
		// while a filter is being removed), so re-check the size each
		// iteration instead of caching it to avoid an out-of-bounds access.
		for (auto i = index; i < int(size(_data)); ++i) {
			_data[i].entry()->cachePinnedIndex(_filterId, i + 1);
		}
	}
}

void PinnedList::applyLimit(int limit) {
	Expects(limit >= 0);

	while (_data.size() > limit) {
		setPinned(_data.back(), false);
	}
}

void PinnedList::clear() {
	if (_projection) {
		applyCloudOrder({});
	} else {
		applyLimit(0);
	}
}

void PinnedList::applyList(
		not_null<Data::Session*> owner,
		const QVector<MTPDialogPeer> &list) {
	Expects(this != owner->savedMessages().chatsList()->pinned());

	auto order = std::vector<Key>();
	auto included = std::set<Key>();
	if (!_projection) {
		clear();
	}
	const auto add = [&](Key key) {
		if (!_projection) {
			addPinned(key);
		} else if (included.emplace(key).second) {
			order.push_back(key);
		}
	};
	for (const auto &peer : list) {
		peer.match([&](const MTPDdialogPeer &data) {
			if (const auto peerId = peerFromMTP(data.vpeer())) {
				add(owner->history(peerId));
			}
		}, [&](const MTPDdialogPeerFolder &data) {
			add(owner->folder(data.vfolder_id().v));
		}, [&](const MTPDdialogPeerCommunity &data) {
			const auto channelId = ChannelId(data.vcommunity_id().v);
			if (const auto channel = owner->channelLoaded(channelId)) {
				const auto history = owner->history(channel);
				if (!history->folderKnown()) {
					history->clearFolder();
				}
				add(history);
			}
		});
	}
	if (_projection) {
		if (order.size() > _limit) {
			order.resize(_limit);
		}
		applyCloudOrder(std::move(order));
	}
}

void PinnedList::applyList(
		not_null<Data::SavedMessages*> sublistsOwner,
		const QVector<MTPDialogPeer> &list) {
	Expects(this == sublistsOwner->chatsList()->pinned());

	auto order = std::vector<Key>();
	auto included = std::set<Key>();
	if (!_projection) {
		clear();
	}
	const auto add = [&](Key key) {
		if (!_projection) {
			addPinned(key);
		} else if (included.emplace(key).second) {
			order.push_back(key);
		}
	};
	for (const auto &peer : list) {
		peer.match([&](const MTPDdialogPeer &data) {
			if (const auto peerId = peerFromMTP(data.vpeer())) {
				const auto peer = sublistsOwner->owner().peer(peerId);
				add(sublistsOwner->sublist(peer));
			}
		}, [](const MTPDdialogPeerFolder &data) {
		}, [](const MTPDdialogPeerCommunity &data) {
		});
	}
	if (_projection) {
		if (order.size() > _limit) {
			order.resize(_limit);
		}
		applyCloudOrder(std::move(order));
	}
}

void PinnedList::applyList(
		not_null<Data::Forum*> forum,
		const QVector<MTPint> &list) {
	Expects(this == forum->topicsList()->pinned());

	clear();
	for (const auto &topicId : list) {
		addPinned(forum->topicFor(topicId.v));
	}
}

void PinnedList::applyList(const std::vector<not_null<History*>> &list) {
	Expects(_filterId != 0);

	if (_projection) {
		auto order = std::vector<Key>();
		order.reserve(list.size());
		for (const auto history : list) {
			order.push_back(history);
		}
		applyCloudOrder(std::move(order));
		return;
	}
	const auto old = base::take(_data);

	const auto count = int(list.size());
	_data.reserve(count);
	for (auto i = 0; i != count; ++i) {
		const auto history = list[i];
		_data.emplace_back(history);
		history->cachePinnedIndex(_filterId, i + 1);
	}

	for (const auto &key : old) {
		const auto history = key.history();
		if (!history || !ranges::contains(_data, history, &Key::history)) {
			key.entry()->cachePinnedIndex(_filterId, 0);
		}
	}
}

void PinnedList::reorder(Key key1, Key key2) {
	const auto index1 = ranges::find(_data, key1) - begin(_data);
	const auto index2 = ranges::find(_data, key2) - begin(_data);
	Assert(index1 >= 0 && index1 < _data.size());
	Assert(index2 >= 0 && index2 < _data.size());
	Assert(index1 != index2);
	std::swap(_data[index1], _data[index2]);
	key1.entry()->cachePinnedIndex(_filterId, index2 + 1);
	key2.entry()->cachePinnedIndex(_filterId, index1 + 1);
}

} // namespace Dialogs
