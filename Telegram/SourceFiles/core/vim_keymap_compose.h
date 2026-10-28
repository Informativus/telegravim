/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"

class QKeyEvent;

namespace Ui {
class InputField;
} // namespace Ui

namespace Core::VimKeymap {

struct ComposeKeyContext {
	Ui::InputField *field = nullptr;
	int &operation;
	int &visualMode;
	int &visualAnchor;
	int &pending;
	base::Timer &deleteTimer;
	bool searchActive = false;
	bool panelOpen = false;
	Fn<void()> refreshCursor;
	Fn<void()> saveDraft;
	Fn<bool()> canShowDeleteHints;
	Fn<bool()> showDeleteHints;
};

[[nodiscard]] bool HandleComposeTextKey(
	ComposeKeyContext context,
	not_null<QKeyEvent*> e);
void RefreshComposeCursor(not_null<Ui::InputField*> field, bool visualMode);

} // namespace Core::VimKeymap
