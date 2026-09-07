/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/spellchecker_bundled.h"

#include <crl/crl_object_on_queue.h>

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>

#include <array>
#include <string_view>

namespace Spellchecker {
namespace {

constexpr auto kMaxWordLength = 24;
constexpr auto kMaxSuggestions = 8;
constexpr auto kEditCost = 100;
constexpr auto kTransposeCost = 70;
constexpr auto kMinFrequency = 300;
constexpr auto kNativeFirstBonus = 40;

[[nodiscard]] bool IsRussian(const QString &word) {
	return !word.isEmpty()
		&& word.size() <= kMaxWordLength
		&& ranges::all_of(word, [](QChar ch) {
			return (ch >= u'а' && ch <= u'я') || ch == u'ё';
		});
}

[[nodiscard]] QString Normalize(QString word) {
	return word.toLower().replace(u'ё', u'е');
}

[[nodiscard]] int SubstitutionCost(QChar a, QChar b) {
	if (a == b) {
		return 0;
	}
	if (a < u'а' || a > u'я' || b < u'а' || b > u'я') {
		return kEditCost;
	}
	static const auto costs = [] {
		auto result = std::array<std::array<int, 32>, 32>();
		constexpr auto rows = std::array<std::u16string_view, 3>{
			u"йцукенгшщзхъ", u"фывапролджэ", u"ячсмитьбю",
		};
		auto positions = std::array<std::pair<int, int>, 32>();
		for (auto y = 0; y != int(rows.size()); ++y) {
			for (auto x = 0; x != int(rows[y].size()); ++x) {
				positions[rows[y][x] - u'а'] = { 4 * x + y, y };
			}
		}
		for (auto i = 0; i != 32; ++i) {
			for (auto j = 0; j != 32; ++j) {
				const auto [ax, ay] = positions[i];
				const auto [bx, by] = positions[j];
				result[i][j] = (std::abs(ax - bx) <= 5
					&& std::abs(ay - by) <= 1) ? 80 : kEditCost;
			}
		}
		for (const auto pair : { u"ао", u"еи", u"ий", u"зс", u"жш",
			u"бп", u"вф", u"дт", u"гк" }) {
			result[pair[0] - u'а'][pair[1] - u'а'] = 65;
			result[pair[1] - u'а'][pair[0] - u'а'] = 65;
		}
		return result;
	}();
	return costs[a.unicode() - u'а'][b.unicode() - u'а'];
}

[[nodiscard]] int Distance(const QString &a, const QString &b) {
	if (a.size() > kMaxWordLength || b.size() > kMaxWordLength) {
		return kEditCost * kMaxWordLength;
	}
	auto previous = std::array<int, kMaxWordLength + 1>();
	auto beforePrevious = previous;
	auto current = previous;
	for (auto j = 0; j <= b.size(); ++j) {
		previous[j] = j * kEditCost;
	}
	for (auto i = 1; i <= a.size(); ++i) {
		current[0] = i * kEditCost;
		for (auto j = 1; j <= b.size(); ++j) {
			current[j] = std::min({
				previous[j] + kEditCost,
				current[j - 1] + kEditCost,
				previous[j - 1] + SubstitutionCost(a[i - 1], b[j - 1]),
			});
			if (i > 1 && j > 1
				&& a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
				current[j] = std::min(
					current[j],
					beforePrevious[j - 2] + kTransposeCost);
			}
		}
		beforePrevious = previous;
		previous = current;
	}
	return previous[b.size()];
}

[[nodiscard]] QString MatchCase(QString suggestion, const QString &word) {
	if (word == word.toUpper()) {
		return suggestion.toUpper();
	} else if (word.front().isUpper()) {
		suggestion[0] = suggestion.front().toUpper();
	}
	return suggestion;
}

class RussianSuggestions final {
public:
	RussianSuggestions();
	[[nodiscard]] std::vector<QString> suggest(
		const QString &word,
		std::vector<QString> original) const;

private:
	struct Entry {
		QString word;
		QString normalized;
		int frequency = 0;
	};

	QHash<QString, int> _frequencies;
	std::array<std::vector<Entry>, kMaxWordLength + 1> _words;

};

RussianSuggestions::RussianSuggestions() {
	auto file = QFile(u":/dictionaries/ru_frequency.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll()).object();
	if (document.value(u"schema"_q).toInt() != 1) {
		return;
	}
	for (const auto &value : document.value(u"words"_q).toArray()) {
		const auto row = value.toArray();
		if (row.size() != 2) {
			continue;
		}
		const auto word = row[0].toString();
		const auto frequency = row[1].toInt();
		if (!IsRussian(word) || frequency < kMinFrequency || frequency > 900) {
			continue;
		}
		_frequencies.insert(word, frequency);
		_words[word.size()].push_back({ word, Normalize(word), frequency });
	}
}

std::vector<QString> RussianSuggestions::suggest(
		const QString &word,
		std::vector<QString> original) const {
	const auto lower = word.toLower();
	if (!IsRussian(lower) || lower.size() < 2 || _frequencies.empty()) {
		return original;
	}
	const auto normalized = Normalize(lower);
	const auto maximum = (word.size() < 4) ? 100
		: (word.size() < 8) ? 225 : 325;
	struct Candidate {
		QString text;
		int score = 0;
		int order = 0;
	};
	auto candidates = std::vector<Candidate>();
	auto seen = QSet<QString>();
	const auto add = [&](QString text, int distance, int frequency, int order) {
		const auto key = text.toLower();
		if (seen.contains(key)) {
			return;
		}
		seen.insert(key);
		const auto bonus = (!original.empty() && !order) ? kNativeFirstBonus : 0;
		candidates.push_back({
			std::move(text),
			3 * distance - frequency - bonus,
			order,
		});
	};
	for (auto i = 0; i != int(original.size()); ++i) {
		const auto &text = original[i];
		add(
			text,
			Distance(normalized, Normalize(text)),
			_frequencies.value(text.toLower(), kMinFrequency),
			i);
	}
	// Frequencies are logarithmic (Zipf * 100), so each additional edit
	// needs substantially stronger corpus evidence to outrank a close word.
	// Keyboard neighbours, Russian sound confusions and transpositions cost
	// less than unrelated substitutions. Native Hunspell suggestions remain
	// candidates even when the frequency corpus lacks a rare inflected form.
	// Those forms use the corpus cutoff as a smoothing floor; a small prior
	// preserves Hunspell's first morphological guess when evidence is close.
	// Length buckets bound the scan; all parsing and scoring run off the UI.
	const auto delta = maximum / kEditCost;
	for (auto size = std::max(1, int(word.size()) - delta)
		; size <= std::min(kMaxWordLength, int(word.size()) + delta)
		; ++size) {
		for (const auto &entry : _words[size]) {
			const auto distance = Distance(normalized, entry.normalized);
			if (distance <= maximum) {
				add(
					MatchCase(entry.word, word),
					distance,
					entry.frequency,
					int(original.size()));
			}
		}
	}
	ranges::sort(candidates, [](const Candidate &a, const Candidate &b) {
		return std::tie(a.score, a.order, a.text)
			< std::tie(b.score, b.order, b.text);
	});
	auto result = std::vector<QString>();
	for (auto &candidate : candidates) {
		result.push_back(std::move(candidate.text));
		if (result.size() == kMaxSuggestions) {
			break;
		}
	}
	return result;
}

} // namespace

void SuggestRussianWords(
		QString word,
		std::vector<QString> original,
		FnMut<void(std::vector<QString>)> done) {
	static auto queue = crl::object_on_queue<RussianSuggestions>();
	queue.with([
		word = std::move(word),
		original = std::move(original),
		done = std::move(done)
	](const RussianSuggestions &instance) mutable {
		auto result = instance.suggest(word, std::move(original));
		crl::on_main([
			result = std::move(result),
			done = std::move(done)
		]() mutable {
			done(std::move(result));
		});
	});
}

} // namespace Spellchecker
