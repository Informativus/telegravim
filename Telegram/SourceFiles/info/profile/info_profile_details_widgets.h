/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/wrap/slide_wrap.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Info::Profile {

[[nodiscard]] object_ptr<Ui::SlideWrap<>> CreateWorkingHours(
	not_null<QWidget*> parent,
	not_null<UserData*> user);

[[nodiscard]] object_ptr<Ui::SlideWrap<>> CreateNotes(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller,
	not_null<UserData*> user);

[[nodiscard]] object_ptr<Ui::SlideWrap<>> CreateBirthday(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller,
	not_null<UserData*> user);

} // namespace Info::Profile
