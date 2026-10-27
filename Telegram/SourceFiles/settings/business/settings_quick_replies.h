/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/local_quick_replies.h"
#include "settings/settings_type.h"

namespace Ui {
class GenericBox;
} // namespace Ui

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Data {
struct LocalQuickReply;
} // namespace Data

namespace Window {
class SessionController;
} // namespace Window

struct TextWithEntities;

namespace Settings {

void PreviewLocalQuickReplyMessages(
	std::shared_ptr<ChatHelpers::Show> show,
	Data::LocalQuickReply reply,
	Fn<void(std::vector<Data::LocalQuickReplyMessage>)> insert);

void ShowLocalQuickReplyMetadata(
	not_null<Window::SessionController*> controller,
	Data::LocalQuickReply reply);

[[nodiscard]] Type QuickRepliesId();

void EditShortcutNameBox(
	not_null<Ui::GenericBox*> box,
	QString name,
	Fn<void(QString, Fn<void()>)> submit);

} // namespace Settings
