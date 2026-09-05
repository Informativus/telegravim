/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <optional>
#include <utility>
#include <vector>

namespace Info {

template <typename Entry, typename Current, typename Valid>
[[nodiscard]] std::optional<Entry> TakeHistoryStep(
		std::vector<Entry> &from,
		std::vector<Entry> &to,
		Current &&current,
		Valid &&valid) {
	while (!from.empty() && !valid(from.back())) {
		from.pop_back();
	}
	if (from.empty()) {
		return std::nullopt;
	}
	to.push_back(current());
	auto result = std::move(from.back());
	from.pop_back();
	return result;
}

} // namespace Info
