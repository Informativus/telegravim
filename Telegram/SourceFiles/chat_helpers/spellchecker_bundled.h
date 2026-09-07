/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <vector>

namespace Spellchecker {

void SuggestRussianWords(
	QString word,
	std::vector<QString> original,
	FnMut<void(std::vector<QString>)> done);

[[nodiscard]] std::vector<int> InstallBundledDictionaries(
	const QString &workingDir);

} // namespace Spellchecker
