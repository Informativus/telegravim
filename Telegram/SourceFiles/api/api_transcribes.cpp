/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_transcribes.h"

#include "apiwrap.h"
#include "api/api_local_transcription.h"
#include "api/api_text_entities.h"
#include "data/data_channel.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "spellcheck/spellcheck_types.h"

namespace Api {

Transcribes::Transcribes(not_null<ApiWrap*> api)
: _session(&api->session())
, _api(&api->instance())
, _local(std::make_unique<LocalTranscription>(
		_session,
		[=](FullMsgId id, QString text, bool done, bool failed) {
			auto i = _map.find(id);
			if (i == _map.end() || !i->second.local) {
				return;
			}
			auto &entry = i->second;
			entry.result = std::move(text);
			entry.pending = !done;
			entry.failed = failed;
			if (const auto item = _session->data().message(id)) {
				_session->data().requestItemResize(item);
			}
		})) {
	_session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Destroyed
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		const auto id = update.item->fullId();
		_local->cancel(id);
		_map.remove(id);
	}, _lifetime);
}

Transcribes::~Transcribes() = default;

bool Transcribes::localNeedsModel() const {
	return _local->needsModel();
}

bool Transcribes::isRated(not_null<HistoryItem*> item) const {
	const auto fullId = item->fullId();
	for (const auto &[transcribeId, id] : _ids) {
		if (id == fullId) {
			return _session->settings().isTranscriptionRated(transcribeId);
		}
	}
	return false;
}

void Transcribes::rate(not_null<HistoryItem*> item, bool isGood) {
	const auto fullId = item->fullId();
	for (const auto &[transcribeId, id] : _ids) {
		if (id == fullId) {
			_api.request(MTPmessages_RateTranscribedAudio(
				item->history()->peer->input(),
				MTP_int(item->id),
				MTP_long(transcribeId),
				MTP_bool(isGood))).send();
			_session->settings().markTranscriptionAsRated(transcribeId);
			_session->saveSettings();
			return;
		}
	}
}

bool Transcribes::freeFor(not_null<HistoryItem*> item) const {
	if (const auto channel = item->history()->peer->asMegagroup()) {
		const auto owner = &channel->owner();
		return channel->levelHint() >= owner->groupFreeTranscribeLevel();
	}
	return false;
}

bool Transcribes::trialsSupport() {
	if (!_trialsSupport) {
		const auto count = _session->appConfig().get<int>(
			u"transcribe_audio_trial_weekly_number"_q,
			0);
		const auto until = _session->appConfig().get<int>(
			u"transcribe_audio_trial_cooldown_until"_q,
			0);
		_trialsSupport = (count > 0) || (until > 0);
	}
	return *_trialsSupport;
}

TimeId Transcribes::trialsRefreshAt() {
	if (_trialsRefreshAt < 0) {
		_trialsRefreshAt = _session->appConfig().get<int>(
			u"transcribe_audio_trial_cooldown_until"_q,
			0);
	}
	return _trialsRefreshAt;
}

int Transcribes::trialsCount() {
	if (_trialsCount < 0) {
		_trialsCount = _session->appConfig().get<int>(
			u"transcribe_audio_trial_weekly_number"_q,
			-1);
		return std::max(_trialsCount, 0);
	}
	return _trialsCount;
}

crl::time Transcribes::trialsMaxLengthMs() const {
	return 1000 * _session->appConfig().get<int>(
		u"transcribe_audio_trial_duration_max"_q,
		300);
}

void Transcribes::toggle(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	auto i = _map.find(id);
	if (i == _map.end() || (i->second.local && i->second.failed)) {
		load(item);
		_session->data().requestItemResize(item);
	} else if (i->second.local && i->second.pending) {
		_local->cancel(id);
		i->second.pending = false;
		i->second.failed = true;
		i->second.shown = false;
		if (i->second.roundview) {
			_session->data().requestItemViewRefresh(item);
		}
		_session->data().requestItemResize(item);
	} else if (!i->second.requestId) {
		i->second.shown = !i->second.shown;
		if (i->second.roundview) {
			_session->data().requestItemViewRefresh(item);
		}
		_session->data().requestItemResize(item);
	}
}

void Transcribes::toggleSummary(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	auto i = _summaries.find(id);
	if (i == _summaries.end()) {
		summarize(item);
	} else if (!i->second.loading) {
		auto &entry = i->second;
		if (entry.result.empty()) {
			summarize(item);
		} else {
			entry.shown = entry.premiumRequired ? false : !entry.shown;
			_session->data().requestItemResize(item);
			if (entry.shown) {
				_session->data().requestItemShowHighlight(item);
			}
		}
	}
}

const Transcribes::Entry &Transcribes::entry(
		not_null<HistoryItem*> item) const {
	static auto empty = Entry();
	const auto i = _map.find(item->fullId());
	return (i != _map.end()) ? i->second : empty;
}

const SummaryEntry &Transcribes::summary(
		not_null<const HistoryItem*> item) const {
	static const auto empty = SummaryEntry();
	const auto i = _summaries.find(item->fullId());
	return (i != _summaries.end()) ? i->second : empty;
}

void Transcribes::apply(const MTPDupdateTranscribedAudio &update) {
	const auto id = update.vtranscription_id().v;
	const auto i = _ids.find(id);
	if (i == _ids.end()) {
		return;
	}
	const auto j = _map.find(i->second);
	if (j == _map.end()) {
		return;
	}
	const auto text = qs(update.vtext());
	j->second.result = text;
	j->second.pending = update.is_pending();
	if (const auto item = _session->data().message(i->second)) {
		if (j->second.roundview) {
			_session->data().requestItemViewRefresh(item);
		}
		_session->data().requestItemResize(item);
	}
}

void Transcribes::load(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry() || item->isLocal()) {
		return;
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || media->ttlSeconds()
		|| (!document->isVoiceMessage() && !document->isVideoMessage())) {
		return;
	}
	auto &entry = _map[item->fullId()];
	entry = Entry();
	entry.local = true;
	entry.shown = true;
	entry.pending = true;
	entry.roundview = document->isVideoMessage();
	if (entry.roundview) {
		_session->data().requestItemViewRefresh(item);
	}
	_local->enqueue(item->fullId());
}

void Transcribes::summarize(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry() || item->isLocal()) {
		return;
	}

	const auto id = item->fullId();
	const auto translatedTo = item->history()->translatedTo();
	const auto langCode = translatedTo
		? translatedTo.twoLetterCode()
		: QString();
	const auto requestId = _api.request(MTPmessages_SummarizeText(
		langCode.isEmpty()
			? MTP_flags(0)
			: MTP_flags(MTPmessages_summarizeText::Flag::f_to_lang),
		item->history()->peer->input(),
		MTP_int(item->id),
		langCode.isEmpty() ? MTPstring() : MTP_string(langCode),
		MTPstring() // tone
	)).done([=](const MTPTextWithEntities &result) {
		const auto &data = result.data();
		auto &entry = _summaries[id];
		entry.requestId = 0;
		entry.loading = false;
		entry.premiumRequired = false;
		entry.languageId = translatedTo;
		entry.result = TextWithEntities(
			qs(data.vtext()),
			Api::EntitiesFromMTP(_session, data.ventities().v));
		if (const auto item = _session->data().message(id)) {
			_session->data().requestItemTextRefresh(item);
			_session->data().requestItemShowHighlight(item);
		}
	}).fail([=](const MTP::Error &error) {
		auto &entry = _summaries[id];
		if (error.type() == u"SUMMARY_FLOOD_PREMIUM"_q) {
			entry.premiumRequired = true;
		}
		entry.requestId = 0;
		entry.shown = false;
		entry.loading = false;
		if (const auto item = _session->data().message(id)) {
			_session->data().requestItemTextRefresh(item);
		}
	}).send();

	auto &entry = _summaries.emplace(id).first->second;
	entry.requestId = requestId;
	entry.shown = true;
	entry.loading = true;

	item->setHasSummaryEntry();
	_session->data().requestItemResize(item);
}

void Transcribes::checkSummaryToTranslate(FullMsgId id) {
	const auto i = _summaries.find(id);
	if (i == _summaries.end() || i->second.result.empty()) {
		return;
	}
	const auto item = _session->data().message(id);
	if (!item) {
		return;
	}
	const auto translatedTo = item->history()->translatedTo();
	if (i->second.languageId != translatedTo) {
		i->second.result = tr::lng_contacts_loading(tr::now, tr::italic);
		summarize(item);
	}
}

} // namespace Api
