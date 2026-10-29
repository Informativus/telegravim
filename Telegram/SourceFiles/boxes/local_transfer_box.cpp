/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/local_transfer_box.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_peer_id.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "qr/qr_generate.h"
#include "storage/local_data_store.h"
#include "storage/local_transfer.h"
#include "storage/storage_domain.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/painter.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <crl/crl_async.h>
#include <crl/crl_on_main.h>
#include <openssl/crypto.h>

#include "styles/style_layers.h"
#include "styles/style_local_transfer.h"
#include "styles/style_widgets.h"

namespace {

using namespace Storage::LocalData;

using IncomingDeletions = QMap<QString, QMap<QString, int>>;

constexpr auto kPortableLinkLimit = 8192;
constexpr auto kPortableQrLimit = 2900;
constexpr auto kPortableFileLimit = 8 * 1024 * 1024;
constexpr auto kQrQuietModules = 4;

struct State {
	QPointer<Ui::GenericBox> box;
	std::shared_ptr<Store> store;
	std::atomic<bool> cancel = false;
	QPointer<Ui::FlatLabel> progress;
	Storage::LocalTransfer::Inspection inspection;
	IncomingDeletions incomingDeletions;
	MergePlan plan;
	QMap<QString, ConflictChoice> choices;
	QSet<QString> signedIn;
	Account account;
	Snapshot archiveSnapshot;
	std::vector<ArchivedConflict> archivePage;
	int archiveOffset = 0;
	int conflictIndex = 0;
	bool busy = false;
	bool settingsOnly = false;
};

using SharedState = std::shared_ptr<State>;

void Initial(const SharedState &state, QString message = {});
void PortableSettings(const SharedState &state);
void Preview(const SharedState &state);
void Archived(const SharedState &state);
void LoadArchived(const SharedState &state);
void LoadArchivedPage(const SharedState &state);

[[nodiscard]] QString Identity(const Account &account) {
	return account.environment + ':' + account.userId;
}

void Label(const SharedState &state, const QString &text) {
	state->box->addRow(object_ptr<Ui::FlatLabel>(
		state->box.data(), text, st::boxLabel));
}

[[nodiscard]] Ui::PasswordInput *PasswordField(
		const SharedState &state,
		rpl::producer<QString> placeholder) {
	auto wrapper = object_ptr<Ui::RpWidget>(state->box.data());
	const auto field = Ui::CreateChild<Ui::PasswordInput>(
		wrapper.data(),
		st::defaultInputField,
		std::move(placeholder));
	field->heightValue(
	) | rpl::on_next([wrap = wrapper.data()](int height) {
		wrap->resize(wrap->width(), height);
	}, wrapper->lifetime());
	wrapper->widthValue(
	) | rpl::on_next([=](int width) {
		field->resize(width, field->height());
	}, wrapper->lifetime());
	state->box->addRow(std::move(wrapper));
	return field;
}

void Reset(const SharedState &state) {
	state->box->clearButtons();
	state->box->verticalLayout()->clear();
}

void CloseButton(const SharedState &state) {
	state->box->addButton(tr::lng_close(), [=] {
		state->box->closeBox();
	});
}

[[nodiscard]] Storage::LocalTransfer::ProgressCallback Progress(
		const SharedState &state) {
	return [state, last = -1](Storage::LocalTransfer::Progress progress) mutable {
		const auto percent = progress.total
			? int(std::min<quint64>(100, progress.completed * 100 / progress.total))
			: 0;
		if (percent == last) {
			return;
		}
		last = percent;
		crl::on_main([state, percent] {
			if (state->progress) {
				state->progress->setText(tr::lng_local_transfer_working(
					tr::now, lt_progress, QString::number(percent)));
			}
		});
	};
}

void Working(const SharedState &state) {
	state->busy = true;
	state->cancel.store(false);
	Reset(state);
	state->progress = state->box->addRow(object_ptr<Ui::FlatLabel>(
		state->box.data(),
		tr::lng_local_transfer_working(tr::now, lt_progress, u"0"_q),
		st::boxLabel));
	state->box->addButton(tr::lng_cancel(), [=] {
		state->cancel.store(true);
	});
}

void Failure(const SharedState &state, const Result &result) {
	Initial(state, result.code == Error::Cancelled
		? QString()
		: tr::lng_local_transfer_error(tr::now, lt_error, result.message));
}

void ExportFile(const SharedState &state, QString path, QByteArray password) {
	Working(state);
	crl::async([state, path = std::move(path), password = std::move(password)]() mutable {
		auto snapshot = Snapshot();
		auto result = state->store->snapshot(snapshot, &state->cancel);
		if (result) {
			result = Storage::LocalTransfer::Export(
				snapshot, path, std::move(password), &state->cancel, Progress(state));
		} else {
			OPENSSL_cleanse(password.data(), password.size());
		}
		crl::on_main([state, result = std::move(result)] {
			if (!state->box) {
				return;
			}
			if (!result) {
				Failure(state, result);
			} else {
				Initial(state, tr::lng_local_transfer_export_done(tr::now));
			}
		});
	});
}

void InspectFile(const SharedState &state, QString path, QByteArray password) {
	Working(state);
	crl::async([state, path = std::move(path), password = std::move(password)]() mutable {
		auto inspection = Storage::LocalTransfer::Inspection();
		auto result = Storage::LocalTransfer::Inspect(
			path, std::move(password), inspection, &state->cancel, Progress(state));
		auto incomingDeletions = IncomingDeletions();
		const auto countDeletions = [&] {
			for (const auto &profile : inspection.snapshot.profiles) {
				auto &counts = incomingDeletions[profile.id];
				for (auto i = profile.modules.begin(); i != profile.modules.end(); ++i) {
					if (state->cancel.load(std::memory_order_relaxed)) {
						return false;
					} else if (!SupportedModule(
						i.key(), i->value(u"schema_version"_q).toInt())) {
						continue;
					}
					auto deleted = 0;
					for (const auto &record : i->value(u"records"_q).toArray()) {
						if (state->cancel.load(std::memory_order_relaxed)) {
							return false;
						}
						deleted += record.toObject().value(u"deleted"_q).toBool();
					}
					counts.insert(i.key(), deleted);
				}
			}
			return true;
		};
		if (result && !countDeletions()) {
			result = { Error::Cancelled, QString() };
		}
		auto plan = MergePlan();
		if (result) {
			result = state->store->prepareImport(inspection.snapshot, plan, &state->cancel);
		}
		crl::on_main([
			state,
			result = std::move(result),
			inspection = std::move(inspection),
			incomingDeletions = std::move(incomingDeletions),
			plan = std::move(plan)
		]() mutable {
			if (!state->box) {
				return;
			} else if (!result) {
				Failure(state, result);
				return;
			}
			state->inspection = std::move(inspection);
			state->incomingDeletions = std::move(incomingDeletions);
			state->plan = std::move(plan);
			state->choices.clear();
			state->conflictIndex = 0;
			for (const auto &conflict : state->plan.conflicts) {
				state->choices.insert(conflict.id, ConflictChoice::KeepBoth);
			}
			Preview(state);
		});
	});
}

void Apply(const SharedState &state) {
	if (state->busy) {
		return;
	}
	Working(state);
	crl::async([state] {
		const auto result = state->store->applyImport(
			state->plan, state->choices, &state->cancel);
		crl::on_main([state, result] {
			if (!state->box) {
				return;
			} else if (!result) {
				Failure(state, result);
			} else {
				Initial(state, tr::lng_local_transfer_import_done(tr::now));
			}
		});
	});
}

[[nodiscard]] QJsonObject PortableModule(QJsonObject values) {
	return {
		{ u"module_id"_q, u"portable_preferences"_q },
		{ u"schema_version"_q, 1 },
		{ u"scope"_q, u"application"_q },
		{ u"values"_q, std::move(values) },
		{ u"records"_q, QJsonArray() },
	};
}

void SavePortableFile(
		const SharedState &state,
		QByteArray bytes,
		bool json) {
	if (state->busy) {
		return;
	}
	state->busy = true;
	FileDialog::GetWritePath(
		state->box.data(),
		json
			? tr::lng_local_transfer_settings_json(tr::now)
			: tr::lng_local_transfer_export(tr::now),
		json
			? tr::lng_local_transfer_settings_json_filter(tr::now)
			: tr::lng_local_transfer_file_filter(tr::now),
		json ? u"Televim-settings.json"_q : u"Televim-settings.tgv"_q,
		crl::guard(state->box, [=](QString &&path) {
			state->busy = false;
			if (path.isEmpty()) {
				return;
			}
			auto file = QSaveFile(path);
			file.setDirectWriteFallback(false);
			const auto saved = file.open(QIODevice::WriteOnly)
				&& file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
				&& file.write(bytes) == bytes.size()
				&& file.commit();
			if (state->progress) {
				state->progress->setText(saved
					? (json
						? tr::lng_local_transfer_settings_json_done(tr::now)
						: tr::lng_local_transfer_export_done(tr::now))
					: tr::lng_local_transfer_settings_save_error(tr::now));
			}
		}),
		crl::guard(state->box, [=] { state->busy = false; }));
}

[[nodiscard]] QImage PortableQr(const QString &link) {
	const auto data = Qr::Encode(link, Qr::Redundancy::Low);
	const auto modules = data.size + 2 * kQrQuietModules;
	const auto ratio = style::DevicePixelRatio();
	const auto pixel = std::max(1, st::localTransferQrSize * ratio / modules);
	const auto side = modules * pixel;
	auto image = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::white);
	{
		auto painter = QPainter(&image);
		for (auto row = 0; row != data.size; ++row) {
			for (auto column = 0; column != data.size; ++column) {
				if (data.values[column * data.size + row]) {
					painter.fillRect(
						(column + kQrQuietModules) * pixel,
						(row + kQrQuietModules) * pixel,
						pixel,
						pixel,
						Qt::black);
				}
			}
		}
	}
	image.setDevicePixelRatio(ratio);
	return image;
}

void PortableResult(const SharedState &state, QByteArray bytes) {
	state->busy = false;
	Reset(state);
	state->box->setWidth(st::localTransferQrWidth);
	state->box->setMaxHeight(st::localTransferQrMaxHeight);
	Label(state, tr::lng_local_transfer_settings_ready(tr::now));
	const auto link = u"televim://settings/v1#"_q + QString::fromLatin1(
		bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
	if (link.size() <= kPortableQrLimit) {
		const auto image = PortableQr(link);
		const auto size = image.width() / style::DevicePixelRatio();
		auto row = object_ptr<Ui::RpWidget>(state->box.data());
		const auto widget = row.data();
		widget->setFixedHeight(size);
		widget->paintRequest(
		) | rpl::on_next([=] {
			QPainter(widget).drawImage(
				QPoint((widget->width() - size) / 2, 0),
				image);
		}, widget->lifetime());
		state->box->addRow(std::move(row));
	} else {
		Label(state, tr::lng_local_transfer_settings_qr_large(tr::now));
	}
	if (link.size() <= kPortableLinkLimit) {
		const auto copy = state->box->addRow(object_ptr<Ui::LinkButton>(
			state->box.data(), tr::lng_local_transfer_settings_copy(tr::now)));
		copy->setClickedCallback([=] {
			QGuiApplication::clipboard()->setText(link);
			if (state->progress) {
				state->progress->setText(tr::lng_local_transfer_settings_copied(tr::now));
			}
		});
	} else {
		Label(state, tr::lng_local_transfer_settings_link_large(tr::now));
	}
	const auto save = state->box->addRow(object_ptr<Ui::LinkButton>(
		state->box.data(), tr::lng_local_transfer_export(tr::now)));
	save->setClickedCallback([=] { SavePortableFile(state, bytes, false); });
	state->progress = state->box->addRow(object_ptr<Ui::FlatLabel>(
		state->box.data(), QString(), st::boxLabel));
	state->box->addButton(tr::lng_local_transfer_settings_back(), [=] {
		if (!state->busy) {
			PortableSettings(state);
		}
	});
	CloseButton(state);
}

void ExportPortable(
		const SharedState &state,
		QJsonObject module,
		QByteArray password) {
	Working(state);
	crl::async([
		state,
		module = std::move(module),
		password = std::move(password)
	]() mutable {
		auto source = Snapshot();
		auto result = state->store->snapshot(source, &state->cancel);
		auto bytes = QByteArray();
		if (result) {
			auto snapshot = Snapshot();
			snapshot.deviceId = source.deviceId;
			snapshot.generation = NewId();
			auto profile = Profile();
			profile.id = NewId();
			profile.account = state->account;
			for (const auto &existing : source.profiles) {
				if (existing.account == state->account) {
					profile.id = existing.id;
					break;
				}
			}
			profile.modules.insert(u"portable_preferences"_q, std::move(module));
			const auto profileId = profile.id;
			snapshot.profiles.insert(profileId, std::move(profile));
			source = {};
			auto directory = QTemporaryDir();
			if (!directory.isValid()) {
				result = { Error::Io, u"Could not create a private export file."_q };
				OPENSSL_cleanse(password.data(), password.size());
			} else {
				const auto path = directory.filePath(u"settings.tgv"_q);
				result = Storage::LocalTransfer::Export(
					snapshot,
					path,
					std::move(password),
					&state->cancel,
					Progress(state));
				if (result) {
					auto file = QFile(path);
					if (!file.open(QIODevice::ReadOnly) || file.size() > kPortableFileLimit) {
						result = { Error::Io, u"Could not read the encrypted settings file."_q };
					} else {
						bytes = file.read(kPortableFileLimit);
						if (bytes.size() != file.size()) {
							result = { Error::Io, u"Could not read the encrypted settings file."_q };
						}
					}
				}
			}
		} else {
			OPENSSL_cleanse(password.data(), password.size());
		}
		if (state->cancel.load(std::memory_order_relaxed)) {
			result = { Error::Cancelled, QString() };
		}
		crl::on_main([state, result, bytes = std::move(bytes)]() mutable {
			if (!state->box) {
				return;
			} else if (!result) {
				Failure(state, result);
			} else {
				PortableResult(state, std::move(bytes));
			}
		});
	});
}

void PortableSettings(const SharedState &state) {
	state->busy = false;
	Reset(state);
	state->box->setWidth(st::localTransferWidth);
	state->box->setMaxHeight(st::localTransferMaxHeight);
	Label(state, tr::lng_local_transfer_settings_about(tr::now));
	const auto &settings = Core::App().settings();
	const auto submit = settings.sendSubmitWay();
	const auto byEnter = submit == Ui::InputSubmitSettings::Enter
		|| submit == Ui::InputSubmitSettings::Both;
	const auto large = settings.largeEmoji();
	const auto valueText = [](bool value) {
		return value
			? tr::lng_local_transfer_settings_enabled(tr::now)
			: tr::lng_local_transfer_settings_disabled(tr::now);
	};
	const auto enter = state->box->addRow(object_ptr<Ui::Checkbox>(
		state->box.data(),
		tr::lng_local_transfer_settings_enter(tr::now, lt_state, valueText(byEnter)),
		true));
	const auto emoji = state->box->addRow(object_ptr<Ui::Checkbox>(
		state->box.data(),
		tr::lng_local_transfer_settings_emoji(tr::now, lt_state, valueText(large)),
		true));
	const auto password = PasswordField(state, tr::lng_local_transfer_password());
	const auto repeated = PasswordField(state, tr::lng_local_transfer_password_again());
	password->setMaxLength(1024);
	repeated->setMaxLength(1024);
	state->progress = state->box->addRow(object_ptr<Ui::FlatLabel>(
		state->box.data(), QString(), st::boxLabel));
	const auto selected = [=] {
		auto values = QJsonObject();
		if (enter->checked()) {
			values.insert(u"send_by_enter"_q, byEnter);
		}
		if (emoji->checked()) {
			values.insert(u"large_emoji"_q, large);
		}
		return values;
	};
	const auto prepare = state->box->addRow(object_ptr<Ui::LinkButton>(
		state->box.data(), tr::lng_local_transfer_settings_create(tr::now)));
	prepare->setClickedCallback([=] {
		if (state->busy) {
			return;
		}
		const auto values = selected();
		if (values.isEmpty()) {
			state->progress->setText(tr::lng_local_transfer_settings_select(tr::now));
			return;
		} else if (password->text().size() < 12
			|| password->text() != repeated->text()
			|| password->text().toUtf8().size() > 1024) {
			state->progress->setText(tr::lng_local_transfer_password_invalid(tr::now));
			password->showError();
			return;
		}
		auto secret = password->text().toUtf8();
		password->setText(QString());
		repeated->setText(QString());
		ExportPortable(state, PortableModule(values), std::move(secret));
	});
	Label(state, tr::lng_local_transfer_settings_json_about(tr::now));
	const auto json = state->box->addRow(object_ptr<Ui::LinkButton>(
		state->box.data(), tr::lng_local_transfer_settings_json(tr::now)));
	json->setClickedCallback([=] {
		if (state->busy) {
			return;
		}
		const auto values = selected();
		if (values.isEmpty()) {
			state->progress->setText(tr::lng_local_transfer_settings_select(tr::now));
			return;
		}
		SavePortableFile(
			state,
			QJsonDocument(PortableModule(values)).toJson(QJsonDocument::Compact),
			true);
	});
	if (!state->settingsOnly) {
		state->box->addButton(tr::lng_local_transfer_settings_back(), [=] {
			if (!state->busy) {
				Initial(state);
			}
		});
	}
	CloseButton(state);
	password->setFocus();
}

void Initial(const SharedState &state, QString message) {
	if (state->settingsOnly) {
		PortableSettings(state);
		state->progress->setText(message);
		return;
	}
	state->busy = false;
	state->inspection = {};
	state->incomingDeletions.clear();
	state->plan = {};
	state->choices.clear();
	state->archiveSnapshot = {};
	state->archivePage.clear();
	Reset(state);
	state->box->setWidth(st::localTransferWidth);
	state->box->setMaxHeight(st::localTransferMaxHeight);
	const auto portable = state->box->addRow(object_ptr<Ui::LinkButton>(
		state->box.data(), tr::lng_local_transfer_settings_title(tr::now)));
	portable->setClickedCallback([=] {
		if (!state->busy) {
			PortableSettings(state);
		}
	});
	Label(state, tr::lng_local_transfer_about(tr::now));
	if (!message.isEmpty()) {
		Label(state, message);
	}
	const auto password = PasswordField(state, tr::lng_local_transfer_password());
	const auto repeated = PasswordField(state, tr::lng_local_transfer_password_again());
	password->setMaxLength(1024);
	repeated->setMaxLength(1024);
	const auto error = state->box->addRow(object_ptr<Ui::FlatLabel>(
		state->box.data(), QString(), st::boxLabel));
	const auto exportButton = state->box->addRow(object_ptr<Ui::LinkButton>(
		state->box.data(), tr::lng_local_transfer_export(tr::now)));
	exportButton->setClickedCallback([=] {
		if (state->busy) {
			return;
		}
		if (password->text().size() < 12 || password->text() != repeated->text()
			|| password->text().toUtf8().size() > 1024) {
			error->setText(tr::lng_local_transfer_password_invalid(tr::now));
			password->showError();
			return;
		}
		state->busy = true;
		FileDialog::GetWritePath(
			state->box.data(),
			tr::lng_local_transfer_export(tr::now),
			tr::lng_local_transfer_file_filter(tr::now),
			u"Telegravim.tgv"_q,
			crl::guard(state->box, [=](QString &&path) {
				state->busy = false;
				if (path.isEmpty()) {
					return;
				}
				auto secret = password->text().toUtf8();
				password->setText(QString());
				repeated->setText(QString());
				ExportFile(state, std::move(path), std::move(secret));
			}),
			crl::guard(state->box, [=] { state->busy = false; }));
	});
	const auto importButton = state->box->addRow(object_ptr<Ui::LinkButton>(
		state->box.data(), tr::lng_local_transfer_import(tr::now)));
	importButton->setClickedCallback([=] {
		if (state->busy) {
			return;
		} else if (password->text().isEmpty()) {
			password->showError();
			return;
		}
		state->busy = true;
		FileDialog::GetOpenPath(
			state->box.data(),
			tr::lng_local_transfer_import(tr::now),
			tr::lng_local_transfer_file_filter(tr::now),
			crl::guard(state->box, [=](FileDialog::OpenResult &&result) {
				state->busy = false;
				if (result.paths.size() != 1 || !result.remoteContent.isEmpty()) {
					return;
				}
				auto secret = password->text().toUtf8();
				password->setText(QString());
				repeated->setText(QString());
				InspectFile(state, result.paths.front(), std::move(secret));
			}),
			crl::guard(state->box, [=] { state->busy = false; }));
	});
	const auto archived = state->box->addRow(object_ptr<Ui::LinkButton>(
		state->box.data(), tr::lng_local_transfer_archived(tr::now)));
	archived->setClickedCallback([state] {
		if (!state->busy) {
			state->archiveOffset = 0;
			LoadArchived(state);
		}
	});
	CloseButton(state);
	password->setFocus();
}

void AppendPreview(QString &result, const QJsonValue &value, int depth = 0) {
	constexpr auto kPreviewCharacters = 4096;
	constexpr auto kPreviewDepth = 8;
	const auto append = [&](const QString &text) {
		result += text.left(std::max(0, kPreviewCharacters - int(result.size())));
	};
	if (result.size() >= kPreviewCharacters) {
		return;
	} else if (depth >= kPreviewDepth) {
		append(u"…"_q);
	} else if (value.isString()) {
		append(value.toString());
	} else if (value.isObject()) {
		const auto object = value.toObject();
		append(u"{"_q);
		for (auto i = object.begin(); i != object.end(); ++i) {
			if (result.size() >= kPreviewCharacters) {
				break;
			}
			append(u"\n"_q);
			append(i.key());
			append(u": "_q);
			AppendPreview(result, i.value(), depth + 1);
		}
		append(u"\n}"_q);
	} else if (value.isArray()) {
		append(u"["_q);
		for (const auto item : value.toArray()) {
			if (result.size() >= kPreviewCharacters) {
				break;
			}
			AppendPreview(result, item, depth + 1);
			append(u", "_q);
		}
		append(u"]"_q);
	} else if (value.isBool()) {
		append(value.toBool() ? u"true"_q : u"false"_q);
	} else if (value.isDouble()) {
		append(QString::number(value.toDouble()));
	} else {
		append(u"null"_q);
	}
}

[[nodiscard]] QString RecordSummary(const QJsonObject &record) {
	const auto kind = record.value(u"kind"_q).toString();
	if (kind == u"tag"_q || kind == u"binding"_q) {
		auto result = (kind == u"tag"_q)
			? (record.value(u"emoji"_q).toString().left(128) + ' '
				+ record.value(u"title"_q).toString().left(4096)).trimmed()
			: tr::lng_local_transfer_saved_message(
				tr::now,
				lt_id,
				record.value(u"message"_q).toObject()
					.value(u"message_id"_q).toString().left(64));
		if (record.value(u"deleted"_q).toBool()) {
			result += '\n' + tr::lng_local_transfer_deleted(tr::now);
		}
		return result.left(4096);
	}
	auto result = QString();
	AppendPreview(result, record);
	return result.size() >= 4096 ? result + u"…"_q : result;
}

void LoadArchived(const SharedState &state) {
	Working(state);
	crl::async([state] {
		auto snapshot = Snapshot();
		const auto result = state->store->snapshot(snapshot, &state->cancel);
		auto page = result ? ArchivedConflicts(snapshot, state->archiveOffset, 2)
			: std::vector<ArchivedConflict>();
		crl::on_main([
			state,
			result,
			snapshot = std::move(snapshot),
			page = std::move(page)
		]() mutable {
			if (!state->box) {
				return;
			} else if (!result) {
				Failure(state, result);
				return;
			}
			state->archiveSnapshot = std::move(snapshot);
			state->archivePage = std::move(page);
			Archived(state);
		});
	});
}

void LoadArchivedPage(const SharedState &state) {
	Working(state);
	crl::async([state] {
		auto page = ArchivedConflicts(state->archiveSnapshot, state->archiveOffset, 2);
		crl::on_main([state, page = std::move(page)]() mutable {
			if (state->box) {
				state->archivePage = std::move(page);
				Archived(state);
			}
		});
	});
}

void RestoreArchived(const SharedState &state, QString id) {
	if (state->busy) {
		return;
	}
	Working(state);
	crl::async([state, id = std::move(id)] {
		const auto result = state->store->restoreConflict(
			id, state->archiveSnapshot.generation, &state->cancel);
		crl::on_main([state, result] {
			if (!state->box) {
				return;
			} else if (!result) {
				Failure(state, result);
			} else {
				LoadArchived(state);
			}
		});
	});
}

void Archived(const SharedState &state) {
	state->busy = false;
	Reset(state);
	Label(state, tr::lng_local_transfer_archived(tr::now));
	const auto &page = state->archivePage;
	if (page.empty()) {
		Label(state, tr::lng_local_transfer_no_archived(tr::now));
	} else {
		const auto &conflict = page.front();
		const auto profile = state->archiveSnapshot.profiles.constFind(conflict.profileId);
		Label(state, profile != state->archiveSnapshot.profiles.cend()
			? Identity(profile->account)
			: (conflict.scope == ArchiveScope::AssetExtra
				|| conflict.scope == ArchiveScope::AssetTransfer)
			? tr::lng_local_transfer_scope_resource(tr::now)
			: tr::lng_local_transfer_scope_store(tr::now));
		Label(state, tr::lng_local_transfer_archive_position(
			tr::now, lt_index, QString::number(state->archiveOffset + 1)));
		Label(state, tr::lng_local_transfer_conflict(
			tr::now, lt_name, conflict.moduleId));
		Label(state, tr::lng_local_transfer_current(
			tr::now, lt_text, RecordSummary(conflict.current)));
		Label(state, tr::lng_local_transfer_saved_version(
			tr::now, lt_text, RecordSummary(conflict.alternate)));
		state->box->addButton(tr::lng_local_transfer_restore(), [state, id = conflict.id] {
			state->box->getDelegate()->show(Ui::MakeConfirmBox({
				.text = tr::lng_local_transfer_restore_about(),
				.confirmed = crl::guard(state->box, [state, id] {
					RestoreArchived(state, id);
				}),
				.confirmText = tr::lng_local_transfer_restore(),
			}));
		});
	}
	if (state->archiveOffset > 0) {
		const auto previous = state->box->addRow(object_ptr<Ui::LinkButton>(
			state->box.data(), tr::lng_local_transfer_previous(tr::now)));
		previous->setClickedCallback([state] {
			if (state->busy) {
				return;
			}
			state->busy = true;
			--state->archiveOffset;
			crl::on_main(state->box, [state] { LoadArchivedPage(state); });
		});
	}
	if (page.size() > 1) {
		const auto next = state->box->addRow(object_ptr<Ui::LinkButton>(
			state->box.data(), tr::lng_local_transfer_next(tr::now)));
		next->setClickedCallback([state] {
			if (state->busy) {
				return;
			}
			state->busy = true;
			++state->archiveOffset;
			crl::on_main(state->box, [state] { LoadArchivedPage(state); });
		});
	}
	state->box->addButton(tr::lng_close(), [state] {
		if (!state->busy) {
			crl::on_main(state->box, [state] { Initial(state); });
		}
	});
}

void Preview(const SharedState &state) {
	state->busy = false;
	Reset(state);
	const auto &snapshot = state->inspection.snapshot;
	auto modules = 0;
	for (const auto &profile : snapshot.profiles) {
		modules += profile.modules.size();
	}
	Label(state, tr::lng_local_transfer_preview(
		tr::now,
		lt_profiles, QString::number(snapshot.profiles.size()),
		lt_modules, QString::number(modules),
		lt_size, Ui::FormatSizeText(state->inspection.packageBytes)));
	Label(state, tr::lng_local_transfer_assets(
		tr::now,
		lt_total, QString::number(snapshot.assets.size()),
		lt_size, Ui::FormatSizeText(state->inspection.assetBytes)));
	Label(state, tr::lng_local_transfer_scope(tr::now));
	for (const auto &profile : snapshot.profiles) {
		const auto identity = Identity(profile.account);
		Label(state, tr::lng_local_transfer_profile(
			tr::now,
			lt_name, identity,
			lt_text, state->signedIn.contains(identity)
				? tr::lng_local_transfer_account_match(tr::now)
				: tr::lng_local_transfer_account_unmatched(tr::now)));
		auto names = QStringList();
		const auto deletions = state->incomingDeletions.value(profile.id);
		for (auto i = profile.modules.begin(); i != profile.modules.end(); ++i) {
			const auto deleted = deletions.constFind(i.key());
			names.push_back((deleted == deletions.cend())
				? i.key()
				: i.key() + u" ("_q
					+ QString::number(i->value(u"records"_q).toArray().size())
					+ u"; "_q + tr::lng_local_transfer_deleted(tr::now)
					+ u": "_q + QString::number(*deleted) + ')');
		}
		Label(state, tr::lng_local_transfer_incoming(
			tr::now, lt_text, tr::lng_local_transfer_modules(
				tr::now, lt_text, names.join(u", "_q))));
	}
	if (!state->plan.unsupportedModules.isEmpty()) {
		Label(state, tr::lng_local_transfer_unsupported(tr::now)
			+ '\n' + state->plan.unsupportedModules.join(u", "_q));
	}
	if (!state->plan.conflicts.empty()) {
		Label(state, tr::lng_local_transfer_conflicts(
			tr::now, lt_total, QString::number(state->plan.conflicts.size())));
		Label(state, tr::lng_local_transfer_defaults(tr::now));
		const auto &conflict = state->plan.conflicts[state->conflictIndex];
		Label(state, tr::lng_local_transfer_conflict_position(
			tr::now,
			lt_index, QString::number(state->conflictIndex + 1),
			lt_total, QString::number(state->plan.conflicts.size())));
		Label(state, tr::lng_local_transfer_conflict(
			tr::now, lt_name, conflict.moduleId + '/' + conflict.recordId));
		Label(state, tr::lng_local_transfer_current(
			tr::now, lt_text, RecordSummary(conflict.local)));
		Label(state, tr::lng_local_transfer_incoming(
			tr::now, lt_text, RecordSummary(conflict.incoming)));
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(
			int(state->choices.value(conflict.id, ConflictChoice::KeepBoth)));
		for (const auto &[choice, text] : {
			std::pair(ConflictChoice::KeepBoth,
				tr::lng_local_transfer_keep_both(tr::now)),
			std::pair(ConflictChoice::KeepLocal,
				tr::lng_local_transfer_keep_local(tr::now)),
			std::pair(ConflictChoice::UseIncoming,
				tr::lng_local_transfer_use_incoming(tr::now)),
		}) {
			state->box->addRow(object_ptr<Ui::Radiobutton>(
				state->box.data(), group, int(choice), text));
		}
		group->setChangedCallback([state, id = conflict.id](int value) {
			state->choices[id] = ConflictChoice(value);
		});
		if (state->conflictIndex > 0) {
			const auto previous = state->box->addRow(object_ptr<Ui::LinkButton>(
				state->box.data(), tr::lng_local_transfer_previous(tr::now)));
			previous->setClickedCallback([state] {
				if (state->busy) {
					return;
				}
				state->busy = true;
				--state->conflictIndex;
				crl::on_main(state->box, [state] { Preview(state); });
			});
		}
		if (state->conflictIndex + 1 < state->plan.conflicts.size()) {
			const auto next = state->box->addRow(object_ptr<Ui::LinkButton>(
				state->box.data(), tr::lng_local_transfer_next(tr::now)));
			next->setClickedCallback([state] {
				if (state->busy) {
					return;
				}
				state->busy = true;
				++state->conflictIndex;
				crl::on_main(state->box, [state] { Preview(state); });
			});
		}
	}
	state->box->addButton(tr::lng_local_transfer_apply(), [state] { Apply(state); });
	state->box->addButton(tr::lng_cancel(), [state] {
		if (!state->busy) {
			Initial(state);
		}
	});
}

void PrepareTransferBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		bool settingsOnly) {
	box->setTitle(settingsOnly
		? tr::lng_local_transfer_settings_title()
		: tr::lng_local_transfer_title());
	box->setWidth(st::localTransferWidth);
	box->setMaxHeight(st::localTransferMaxHeight);
	const auto state = std::make_shared<State>();
	state->settingsOnly = settingsOnly;
	state->box = box;
	state->store = session->domainLocal().localData();
	state->account = {
		session->isTestMode() ? u"test"_q : u"production"_q,
		QString::number(session->userId().bare),
	};
	for (const auto &account : session->domain().accounts()) {
		if (const auto other = account.account->maybeSession()) {
			state->signedIn.insert(Identity({
				other->isTestMode() ? u"test"_q : u"production"_q,
				QString::number(other->userId().bare),
			}));
		}
	}
	box->lifetime().add([state] { state->cancel.store(true); });
	Initial(state);
}

} // namespace

void LocalTransferBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	PrepareTransferBox(box, session, false);
}

void LocalSettingsExportBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	PrepareTransferBox(box, session, true);
}
