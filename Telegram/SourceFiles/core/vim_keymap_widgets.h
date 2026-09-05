/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

class QObject;
class QWidget;

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Core::VimKeymap {

[[nodiscard]] bool KeyHandlerInScope(QObject *owner, QWidget *scope);
void FocusModalNextPrevChild(not_null<Ui::RpWidget*> box, bool next);

} // namespace Core::VimKeymap
