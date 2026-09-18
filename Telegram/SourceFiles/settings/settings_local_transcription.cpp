/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_local_transcription.h"

#include "api/api_local_transcription.h"
#include "api/api_transcribes.h"
#include "apiwrap.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

namespace Settings {

void ShowLocalTranscriptionSettings(
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto local = &session->api().transcribes().local();
		box->setTitle(tr::lng_local_transcribe_title());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_local_transcribe_settings_about(),
			st::boxLabel));
		const auto enabled = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			tr::lng_local_transcribe_enable(tr::now),
			local->enabled()));
		enabled->checkedChanges(
		) | rpl::on_next(crl::guard(session, [=](bool value) {
			local->setEnabled(value);
		}), box->lifetime());
		const auto dismissed = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			tr::lng_local_transcribe_dont_show(tr::now),
			local->offerDismissed()));
		dismissed->checkedChanges(
		) | rpl::on_next(crl::guard(session, [=](bool value) {
			local->setOfferDismissed(value);
		}), box->lifetime());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			local->modelStateValue() | rpl::map([=] {
				return local->modelStatus();
			}),
			st::boxLabel));
		const auto download = box->addButton(
			local->modelStateValue() | rpl::map([=] {
				return local->downloadingModel()
					? tr::lng_local_transcribe_cancel_download(tr::now)
					: tr::lng_local_transcribe_download_button(tr::now);
			}),
			crl::guard(session, [=] {
				if (local->downloadingModel()) {
					local->cancelModelDownload();
				} else {
					local->downloadModel();
				}
			}));
		local->modelStateValue(
		) | rpl::on_next([=] {
			download->setDisabled(
				!local->downloadingModel() && !local->needsModel());
		}, box->lifetime());
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ShowLocalTranscriptionOffer(
		not_null<Window::SessionController*> controller,
		FullMsgId id) {
	const auto session = &controller->session();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto local = &session->api().transcribes().local();
		box->setTitle(tr::lng_local_transcribe_title());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			local->needsModel()
				? tr::lng_local_transcribe_download()
				: tr::lng_local_transcribe_offer_ready(),
			st::boxLabel));
		const auto dismissed = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			tr::lng_local_transcribe_dont_show(tr::now),
			local->offerDismissed()));
		dismissed->checkedChanges(
		) | rpl::on_next(crl::guard(session, [=](bool value) {
			local->setOfferDismissed(value);
		}), box->lifetime());
		box->addButton(
			local->needsModel()
				? tr::lng_local_transcribe_download_enable()
				: tr::lng_local_transcribe_enable_button(),
			crl::guard(session, [=] {
				if (!session->premium()) {
					local->setEnabled(true);
				}
				box->closeBox();
				if (const auto item = session->data().message(id)) {
					session->api().transcribes().toggle(item);
				}
			}));
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace Settings
