/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "iv/iv_rich_page.h"
#include "scheme.h"

#include <map>

struct FilePrepareResult;

namespace Iv {

using LocalRichSendFiles
	= std::map<uint64, std::shared_ptr<FilePrepareResult>>;
using LocalRichSendDone = Fn<void(
	std::shared_ptr<const RichPage>,
	std::optional<MTPInputRichMessage>,
	QString)>;

class LocalRichSendPreparation {
public:
	virtual ~LocalRichSendPreparation() = default;
	virtual void cancel() = 0;

};

[[nodiscard]] QString ValidateLocalRichForSend(
	not_null<Main::Session*> session,
	const RichPage &page,
	const LocalRichSendFiles &files);

/*
Invoke only after explicit Send into an ordinary chat. File owners must retain
their materialized asset leases. Preparation copies the page and upload inputs,
starts asynchronously, and never sends a message. Keep the returned operation
alive until completion; cancellation or destruction suppresses the callback.
The callback runs on the main thread and must guard any shorter-lived UI state.
Failure returns a null page and value; an empty error means a silent API error.
*/
[[nodiscard]] std::shared_ptr<LocalRichSendPreparation> PrepareLocalRichForSend(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	std::shared_ptr<const RichPage> page,
	LocalRichSendFiles files,
	LocalRichSendDone done);

} // namespace Iv
