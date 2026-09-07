/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

namespace Ui {
class InputField;
} // namespace Ui

namespace Spellchecker {

class SpellingHighlighter;

void InitSuggestionsMenu(
	not_null<Ui::InputField*> field,
	not_null<SpellingHighlighter*> highlighter);
bool ShowSuggestionsMenu(not_null<Ui::InputField*> field);

} // namespace Spellchecker
