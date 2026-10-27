/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_common.h"

#include <QtCore/QStringList>

namespace Data {
class LocalQuickReplyStage;
enum class ForwardOptions;
} // namespace Data

namespace Api {

/*
Call only for an explicit ordinary Send of an insertion snapshot. Preparation
retains the stage and materialized file leases and validates the whole batch.
The prepared callback must verify the same draft snapshot before clearing it;
false cancels without sending. Guard both callbacks for their UI lifetime.
After acceptance, native send failures stop the remaining batch without replay.
*/
void SendLocalQuickReplyMessages(
	std::shared_ptr<const Data::LocalQuickReplyStage> stage,
	QStringList selectedMessageRefs,
	Data::ForwardOptions forwardOptions,
	SendAction action,
	Fn<bool()> prepared,
	Fn<void(QString)> failed);

} // namespace Api
