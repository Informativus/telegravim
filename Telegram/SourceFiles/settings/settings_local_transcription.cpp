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
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/multi_select.h"
#include "ui/wrap/slide_wrap.h"
#include "window/window_session_controller.h"

#include <QtCore/QLocale>

#include "styles/style_layers.h"

namespace Settings {
namespace {

QString LanguageName(const QString &code) {
	if (code == u"auto"_q) {
		return tr::lng_local_transcribe_language_auto(tr::now);
	} else if (code == u"system"_q) {
		return tr::lng_local_transcribe_language_system(
			tr::now,
			lt_language,
			LanguageName(Media::Transcription::NormalizeLanguage(
				Lang::GetInstance().systemLangCode())));
	}
	const auto locale = QLocale(code);
	const auto name = locale.nativeLanguageName();
	return (locale.language() == QLocale::C || name.isEmpty())
		? code
		: name;
}

void LanguageBox(
		not_null<Ui::GenericBox*> box,
		QString selected,
		Fn<void(QString)> callback) {
	box->setTitle(tr::lng_local_transcribe_language());
	box->setMinHeight(st::boxWidth);
	box->setMaxHeight(st::boxWidth);
	const auto search = box->setPinnedToTopContent(
		object_ptr<Ui::MultiSelect>(
			box,
			st::defaultMultiSelect,
			tr::lng_participant_filter()));
	box->setFocusCallback([=] { search->setInnerFocus(); });
	auto languages = Media::Transcription::Languages();
	ranges::sort(languages, [](const QString &a, const QString &b) {
		return QString::localeAwareCompare(LanguageName(a), LanguageName(b)) < 0;
	});
	const auto chosen = ranges::find(languages, selected);
	if (chosen != languages.end()) {
		std::rotate(languages.begin(), chosen, chosen + 1);
	}
	languages.insert(languages.begin(), { u"system"_q, u"auto"_q });
	struct Row {
		not_null<Ui::SlideWrap<Ui::SettingsButton>*> widget;
		QString query;
	};
	auto rows = std::vector<Row>();
	for (const auto &code : languages) {
		const auto name = LanguageName(code);
		const auto button = box->addRow(
			object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
				box,
				object_ptr<Ui::SettingsButton>(box, rpl::single(
					(code == selected) ? name + u" ✓"_q : name))));
		button->entity()->setClickedCallback([=] {
			callback(code);
			box->closeBox();
		});
		rows.push_back({
			button,
			name + u' ' + code + u' '
				+ QLocale::languageToString(QLocale(code).language()),
		});
	}
	search->setQueryChangedCallback([=](const QString &query) {
		for (const auto &row : rows) {
			row.widget->toggle(
				row.query.contains(query.trimmed(), Qt::CaseInsensitive),
				anim::type::instant);
		}
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

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
		const auto dictation = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			tr::lng_local_dictation_enable(tr::now),
			local->dictationEnabled()));
		dictation->checkedChanges(
		) | rpl::on_next(crl::guard(session, [=](bool value) {
			local->setDictationEnabled(value);
		}), box->lifetime());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_local_dictation_about(),
			st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_local_transcribe_language(),
			st::boxLabel));
		const auto language = box->lifetime().make_state<rpl::variable<QString>>(
			local->language());
		const auto chooseLanguage = box->addRow(
			object_ptr<Ui::SettingsButton>(box, language->value() | rpl::map(
				[](const QString &code) { return LanguageName(code); })));
		chooseLanguage->setClickedCallback(crl::guard(session, [=] {
			controller->show(Box(
				LanguageBox,
				language->current(),
				crl::guard(box, crl::guard(session, [=](QString code) {
					local->setLanguage(code);
					*language = local->language();
				}))), Ui::LayerOption::KeepOther);
		}));
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
