/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#ifdef _DEBUG

#include "base/unixtime.h"
#include "boxes/send_files_box.h"
#include "boxes/delete_messages_box.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "core/application.h"
#include "core/mime_type.h"
#include "core/vim_keymap.h"
#include "core/vim_keymap_options.h"
#include "data/data_groups.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/view/media/history_view_media_grouped.h"
#include "history/view/history_view_element.h"
#include "history/history.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mtproto/facade.h"
#include "storage/storage_media_prepare.h"
#include "test/test_capture.h"
#include "test/test_log.h"
#include "test/test_runner.h"
#include "test/test_widgets.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/widgets/elastic_scroll.h"
#include "window/main_window.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_vim_keymap.h"

#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtGui/QClipboard>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>

namespace Test {
namespace {

MTPUser FixtureUser(int id, bool self) {
	using Flag = MTPDuser::Flag;
	return MTP_user(
		MTP_flags(Flag::f_first_name | (self ? Flag::f_self : Flag::f_contact)),
		MTP_long(id),
		MTPlong(),
		MTP_string("Clipboard fixture"),
		MTPstring(),
		MTPstring(),
		MTPstring(),
		MTPUserProfilePhoto(),
		MTPUserStatus(),
		MTPint(),
		MTPVector<MTPRestrictionReason>(),
		MTPstring(),
		MTPstring(),
		MTPEmojiStatus(),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPint(),
		MTPlong(),
		MTPlong(),
		MTPlong());
}

struct AlbumFixture {
	std::vector<HistoryItem*> items;
	std::vector<std::shared_ptr<Data::PhotoMedia>> photos;
	QList<QUrl> urls;
	QStringList directories;
	QPointer<HistoryInner> inner;
};

QImage PhotoImage(int index) {
	auto result = QImage(480, 320, QImage::Format_RGB32);
	result.fill(index
		? QColor(30 + 15 * index, 180 - 9 * index, 70 + 8 * index)
		: QColor(200, 40, 30));
	return result;
}

void LoadPhoto(const std::shared_ptr<Data::PhotoMedia> &media, int index) {
	media->set(
		Data::PhotoSize::Large,
		Data::PhotoSize::Large,
		PhotoImage(index),
		{});
}

bool PhotoMatches(const QImage &image, int index) {
	if (image.isNull()) {
		return false;
	}
	const auto actual = image.pixelColor(
		image.width() / 2,
		image.height() / 2);
	const auto expected = PhotoImage(index).pixelColor(0, 0);
	return image.size() == QSize(480, 320)
		&& std::abs(actual.red() - expected.red()) <= 3
		&& std::abs(actual.green() - expected.green()) <= 3
		&& std::abs(actual.blue() - expected.blue()) <= 3;
}

void ChooseHint(not_null<HistoryInner*> inner, int index, int count) {
	using namespace Core::VimKeymap;
	ForceWindowActive(inner->window());
	inner->setFocus();
	SetNormalMode(true);
	PressKey(inner, Qt::Key_Y);
	const auto label = HintLabel(index, count);
	for (const auto character : label) {
		auto event = QKeyEvent(
			QEvent::KeyPress,
			character.toUpper().unicode(),
			Qt::NoModifier,
			QString(character));
		Settle([&] { QApplication::sendEvent(inner, &event); });
	}
}

void CheckAlbumClipboard(const std::shared_ptr<AlbumFixture> &fixture) {
	const auto mime = QApplication::clipboard()->mimeData();
	const auto urls = Core::ReadMimeUrls(mime);
	Check(urls.size() == fixture->photos.size() && !mime->hasImage(),
		u"album clipboard contains every photo as separate files"_q,
		u"files=%1"_q.arg(urls.size()));
	for (auto i = 0; i != urls.size(); ++i) {
		const auto path = urls[i].toLocalFile();
		Check(PhotoMatches(QImage(path), i),
			u"export preserves full resolution and album order"_q,
			u"photo=%1"_q.arg(i));
		const auto directory = QFileInfo(path).absolutePath();
		const auto permissions = QFileInfo(directory).permissions();
		Check(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
			| QFileDevice::ExeGroup | QFileDevice::ReadOther
			| QFileDevice::WriteOther | QFileDevice::ExeOther)),
			u"export directory is private to the user"_q);
		fixture->directories.push_back(directory);
	}
	const auto list = Storage::PrepareMediaList(
		urls,
		st::sendMediaPreviewSize,
		false);
	Check(list.error == Ui::PreparedList::Error::None
		&& list.files.size() == fixture->photos.size(),
		u"native paste prepares every photo as a separate attachment"_q);
	fixture->urls = urls;
}

void AddDeletionScenarios(not_null<Runner*> runner) {
	using namespace Core::VimKeymap;
	const auto cases = QStringList{
		u"first"_q, u"last"_q, u"all"_q, u"cancel"_q,
		u"removed"_q, u"replaced"_q, u"added"_q, u"revoked"_q,
		u"mixed"_q, u"clipped"_q,
	};
	for (auto test = 0; test != cases.size(); ++test) {
		const auto name = cases[test];
		const auto count = name == u"clipped"_q ? 10 : 3;
		const auto fixture = std::make_shared<AlbumFixture>();
		const auto ids = std::make_shared<MessageIdsList>();
		runner->add({
			.name = u"build deletion album: "_q + name,
			.run = [=] {
				auto &session = Core::App().domain().active().session();
				auto peer = static_cast<PeerData*>(session.data().processUser(
					FixtureUser(100 + test, false)).get());
				if (name == u"revoked"_q) {
					const auto channel = session.data().channel(ChannelId(100 + test));
					channel->setName(u"Deletion fixture"_q, QString());
					channel->setFlags(ChannelDataFlag::Megagroup | ChannelDataFlag::Creator);
					peer = channel;
				}
				const auto history = session.data().history(peer);
				history->clearFolder();
				for (auto i = 0; i != count; ++i) {
					const auto photo = session.data().processPhoto(MTP_photo(
						MTP_flags(0),
						MTP_long(5000 + 100 * test + i),
						MTP_long(1),
						MTP_bytes(QByteArray()),
						MTP_int(base::unixtime::now()),
						MTP_vector<MTPPhotoSize>({ MTP_photoSize(
							MTP_string("x"),
							MTP_int(480),
							MTP_int(320),
							MTP_int(1000)) }),
						MTPVector<MTPVideoSize>(),
						MTP_int(2)));
					const auto media = photo->createMediaView();
					media->set(
						Data::PhotoSize::Small,
						Data::PhotoSize::Small,
						PhotoImage(i),
						{});
					LoadPhoto(media, i);
					const auto item = history->addNewLocalMessage({
						.id = session.data().nextLocalMessageId(),
						.flags = MessageFlag::Local | MessageFlag::BeingSent
							| (name == u"revoked"_q ? MessageFlags() : MessageFlag::Outgoing),
						.from = session.user()->id,
						.date = base::unixtime::now(),
						.groupedId = uint64(500 + test),
					}, photo, { u"Deletion fixture"_q });
					item->setRealId(2000 + 100 * test + i);
					fixture->items.push_back(item);
					fixture->photos.push_back(media);
					ids->push_back(item->fullId());
				}
				if (name == u"mixed"_q) {
					const auto document = session.data().processDocument(MTP_document(
						MTP_flags(0), MTP_long(9999), MTP_long(1), MTP_bytes(QByteArray()),
						MTP_int(base::unixtime::now()), MTP_string("video/mp4"), MTP_long(1000),
						MTPVector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(), MTP_int(2),
						MTP_vector<MTPDocumentAttribute>({ MTP_documentAttributeVideo(
							MTP_flags(0), MTP_double(1.), MTP_int(480), MTP_int(320),
							MTPint(), MTPdouble(), MTPstring()) })));
					const auto item = history->addNewLocalMessage({
						.id = session.data().nextLocalMessageId(),
						.flags = MessageFlag::Local | MessageFlag::BeingSent | MessageFlag::Outgoing,
						.from = session.user()->id,
						.date = base::unixtime::now(),
						.groupedId = uint64(500 + test),
					}, document, {});
					item->setRealId(2000 + 100 * test + count);
					ids->push_back(item->fullId());
				}
				const auto active = Core::App().activePrimaryWindow();
				active->widget()->resize(1100, name == u"clipped"_q ? 550 : 850);
				active->sessionController()->showPeerHistory(history,
					Window::SectionShow::Way::ClearStack, ShowAtTheEndMsgId);
			},
		});
		runner->actOnWidget(u"choose deletion hint: "_q + name,
			[=]() -> QWidget* {
				const auto active = Core::App().activePrimaryWindow();
				for (const auto inner : FindVisible<HistoryInner>(active->widget())) {
					if (inner->viewByItem(fixture->items.front())) {
						fixture->inner = inner;
						return inner;
					}
				}
				return nullptr;
			}, [=](QWidget*) {
				const auto inner = fixture->inner.data();
				ForceWindowActive(inner->window());
				inner->setFocus();
				SetNormalMode(true);
				if (name == u"clipped"_q) {
					for (auto parent = inner->parentWidget(); parent; parent = parent->parentWidget()) {
						if (const auto scroll = dynamic_cast<Ui::ElasticScroll*>(parent)) {
							scroll->resize(scroll->width(), scroll->height() / 2);
							SettlePostponedCalls();
							break;
						}
					}
				}
				PressKey(inner, Qt::Key_D);
				if (name == u"all"_q) {
					CaptureWidget(inner->window(), u"album-delete-hints"_q);
				}
				auto &session = Core::App().domain().active().session();
				if (name == u"removed"_q) {
					fixture->items.back()->destroy();
				} else if (name == u"replaced"_q) {
					const auto empty = MTPMessageMedia(MTP_messageMediaEmpty());
					fixture->items.back()->updateSentContent({}, &empty, nullptr);
				} else if (name == u"added"_q) {
					const auto history = fixture->items.front()->history();
					const auto item = history->addNewLocalMessage({
						.id = session.data().nextLocalMessageId(),
						.flags = MessageFlag::Local | MessageFlag::BeingSent | MessageFlag::Outgoing,
						.from = session.user()->id,
						.date = base::unixtime::now(),
						.groupedId = uint64(500 + test),
					}, fixture->photos.front()->owner(), {});
					item->setRealId(2000 + 100 * test + count);
					ids->push_back(item->fullId());
				} else if (name == u"revoked"_q) {
					fixture->items.front()->history()->peer->asChannel()->removeFlags(
						ChannelDataFlag::Creator);
					Check(!fixture->items.front()->canDelete(), u"fixture revokes delete permission"_q);
				}
				auto visiblePhotos = count;
				if (name == u"clipped"_q) {
					const auto view = inner->viewByItem(fixture->items.front());
					const auto grouped = dynamic_cast<HistoryView::GroupedMedia*>(view->media());
					visiblePhotos = 0;
					for (auto i = 0; i != count; ++i) {
						const auto rect = grouped->groupItemRect(i).translated(view->innerGeometry().topLeft());
						const auto global = QRect(inner->mapToGlobal(
							rect.topLeft() + QPoint(0, inner->itemTop(view))), rect.size());
						auto viewport = inner->parentWidget();
						while (viewport->parentWidget() && !dynamic_cast<Ui::ElasticScroll*>(viewport)) {
							viewport = viewport->parentWidget();
						}
						const auto clipped = global.intersected(QRect(
							viewport->mapToGlobal(QPoint()), viewport->size()));
						if (clipped.width() >= st::vimHintMinVisibleHeight
							&& clipped.height() >= st::vimHintMinVisibleHeight) {
							++visiblePhotos;
						}
					}
					Check(visiblePhotos > 0 && visiblePhotos < count,
						u"deletion fixture includes clipped photos"_q,
						u"visible=%1 count=%2"_q.arg(visiblePhotos).arg(count));
				}
				const auto index = name == u"first"_q ? 0
					: name == u"last"_q ? count - 1 : visiblePhotos;
				const auto label = HintLabel(index, visiblePhotos + 1);
				for (const auto character : label) {
					auto event = QKeyEvent(QEvent::KeyPress,
						character.toUpper().unicode(), Qt::NoModifier, QString(character));
					Settle([&] { QApplication::sendEvent(inner, &event); });
				}
			}, [=](QWidget*) { return !fixture->items.front()->history()->hasPendingResizedItems(); });
		const auto invalid = name == u"removed"_q || name == u"replaced"_q
			|| name == u"added"_q || name == u"revoked"_q;
		if (invalid) {
			runner->add({
				.name = u"reject changed deletion album: "_q + name,
				.run = [=] {
					const auto boxes = FindVisible<DeleteMessagesBox>(fixture->inner->window());
					Check(boxes.empty(), u"changed album cannot open deletion confirmation: "_q + name);
					for (const auto box : boxes) {
						box->closeBox();
					}
					PressKey(fixture->inner, Qt::Key_Escape);
				},
			});
		} else {
			runner->actOnWidget(u"confirm deletion scope: "_q + name,
				[=]() -> QWidget* {
					const auto boxes = FindVisible<DeleteMessagesBox>(fixture->inner->window());
					return boxes.empty() ? nullptr : boxes.front();
				}, [=](QWidget *box) {
					auto &data = Core::App().domain().active().session().data();
					Check(ranges::all_of(*ids, [&](FullMsgId id) { return data.message(id); }),
						u"hint waits for confirmation before deleting: "_q + name);
					if (name == u"cancel"_q) {
						static_cast<DeleteMessagesBox*>(box)->closeBox();
					} else {
						PressKey(box, Qt::Key_Return);
					}
					for (auto i = 0; i != ids->size(); ++i) {
						const auto deleted = name == u"first"_q ? i == 0
							: name == u"last"_q ? i == count - 1
							: name != u"cancel"_q && i < count;
						Check(bool(data.message((*ids)[i])) != deleted,
							u"deletion preserves exactly the intended messages: "_q + name,
							u"member=%1 expectedDeleted=%2"_q.arg(i).arg(deleted));
					}
				});
		}
	}
}

} // namespace

void SetupScenario(not_null<Runner*> runner) {
	using namespace Core::VimKeymap;
	runner->waitEvent(u"launch_finished"_q);
	auto fixture = std::make_shared<AlbumFixture>();
	runner->add({
		.name = u"create a disposable account with networking paused"_q,
		.run = [] {
			auto &account = Core::App().domain().active();
			Check(!account.sessionExists(), u"test never uses a signed-in account"_q);
			if (account.sessionExists()) {
				return;
			}
			MTP::details::pause();
			account.createSession(FixtureUser(1, true));
			VimKeymapOption.set(true);
			VimKeymapHintAlphabetOption.set(u"en"_q);
			const auto saved = std::make_shared<QMimeData>();
			const auto original = QApplication::clipboard()->mimeData();
			for (const auto &format : original->formats()) {
				saved->setData(format, original->data(format));
			}
			QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [saved] {
				auto restored = std::make_unique<QMimeData>();
				for (const auto &format : saved->formats()) {
					restored->setData(format, saved->data(format));
				}
				QApplication::clipboard()->setMimeData(restored.release());
			});
		},
	});
	const auto cases = QStringList{
		u"ready"_q,
		u"pending"_q,
		u"new clipboard"_q,
		u"revoked"_q,
		u"removed"_q,
		u"replaced"_q,
		u"protected member"_q,
		u"ten photos"_q,
	};
	for (auto test = 0; test != cases.size(); ++test) {
		const auto name = cases[test];
		const auto count = (name == u"ten photos"_q) ? 10 : 2;
		runner->add({
			.name = u"build album: "_q + name,
			.run = [=] {
				auto &session = Core::App().domain().active().session();
				fixture->inner = nullptr;
				fixture->items.clear();
				fixture->photos.clear();
				const auto peer = session.data().processUser(FixtureUser(20 + test, false));
				const auto history = session.data().history(peer);
				history->clearFolder();
				for (auto i = 0; i != count; ++i) {
					const auto photo = session.data().processPhoto(MTP_photo(
						MTP_flags(0),
						MTP_long(1000 + 100 * test + i),
						MTP_long(1),
						MTP_bytes(QByteArray()),
						MTP_int(base::unixtime::now()),
						MTP_vector<MTPPhotoSize>({ MTP_photoSize(
							MTP_string("x"),
							MTP_int(480),
							MTP_int(320),
							MTP_int(1000)) }),
						MTPVector<MTPVideoSize>(),
						MTP_int(2)));
					const auto media = photo->createMediaView();
					media->set(
						Data::PhotoSize::Small,
						Data::PhotoSize::Small,
						PhotoImage(i),
						{});
					if (!i
						|| name == u"ready"_q
						|| name == u"protected member"_q
						|| name == u"ten photos"_q) {
						LoadPhoto(media, i);
					}
					const auto message = history->addNewLocalMessage({
						.id = session.data().nextLocalMessageId(),
						.flags = MessageFlag::Local | MessageFlag::Outgoing | MessageFlag::BeingSent
							| ((name == u"protected member"_q && i) ? MessageFlag::NoForwards : MessageFlags()),
						.from = session.user()->id,
						.date = base::unixtime::now(),
						.groupedId = uint64(100 + test),
					}, photo, { u"Album fixture"_q });
					message->setRealId(100 * (test + 1) + i);
					fixture->items.push_back(message);
					fixture->photos.push_back(media);
				}
				const auto active = Core::App().activePrimaryWindow();
				active->widget()->resize(1100, 850);
				active->sessionController()->showPeerHistory(
					history,
					Window::SectionShow::Way::ClearStack,
					ShowAtTheEndMsgId);
			},
		});
		const auto resolve = [=]() -> QWidget* {
			const auto active = Core::App().activePrimaryWindow();
			if (!active || fixture->items.empty()) {
				return nullptr;
			}
			ForceWindowActive(active->widget());
			for (const auto inner : FindVisible<HistoryInner>(active->widget())) {
				const auto view = inner->viewByItem(fixture->items.front());
				if (view && dynamic_cast<HistoryView::GroupedMedia*>(view->media())) {
					fixture->inner = inner;
					return inner;
				}
			}
			return nullptr;
		};
		runner->actOnWidget(u"copy album: "_q + name, resolve,
			[=](QWidget*) {
				const auto inner = fixture->inner.data();
				if (name == u"ready"_q) {
					ChooseHint(inner, 0, 3);
					Check(PhotoMatches(qvariant_cast<QImage>(
						QApplication::clipboard()->mimeData()->imageData()), 0),
						u"individual photo hint still copies just its image"_q);
					SetNormalMode(true);
					inner->setFocus();
					PressKey(inner, Qt::Key_Y);
					CaptureWidget(inner->window(), u"album-copy-hints"_q);
					PressKey(inner, Qt::Key_Escape);
				}
				QApplication::clipboard()->setText(u"previous clipboard"_q);
				ChooseHint(inner, count, count + 1);
				if (name == u"ready"_q || name == u"ten photos"_q) {
					CheckAlbumClipboard(fixture);
				} else if (name == u"protected member"_q) {
					Check(QApplication::clipboard()->text() == u"previous clipboard"_q,
						u"one protected photo blocks the entire album"_q);
				} else {
					Check(QApplication::clipboard()->text() == u"previous clipboard"_q,
						u"partial download never publishes a partial album"_q);
					if (name == u"new clipboard"_q) {
						QApplication::clipboard()->setText(u"new clipboard"_q);
					} else if (name == u"revoked"_q) {
						fixture->items.front()->history()->peer->asUser()->setNoForwardsFlags(true, false);
					} else if (name == u"removed"_q) {
						fixture->items.back()->destroy();
						fixture->items.pop_back();
					} else if (name == u"replaced"_q) {
						const auto empty = MTPMessageMedia(MTP_messageMediaEmpty());
						fixture->items.back()->updateSentContent({}, &empty, nullptr);
					}
					LoadPhoto(fixture->photos.back(), 1);
					Core::App().domain().active().session().notifyDownloaderTaskFinished();
					if (name == u"pending"_q) {
						CheckAlbumClipboard(fixture);
					} else {
						Check(QApplication::clipboard()->text()
							== (name == u"new clipboard"_q ? name : u"previous clipboard"_q),
							u"pending album cannot overwrite clipboard after "_q + name);
					}
				}
				if (name == u"ready"_q) {
					SetNormalMode(true);
					inner->setFocus();
					PressKey(inner, Qt::Key_I);
					PressKey(QApplication::focusWidget(), Qt::Key_V, Qt::ControlModifier);
				}
			},
			[=](QWidget*) { return !fixture->items.front()->history()->hasPendingResizedItems(); });
		if (name == u"ready"_q) {
			runner->actOnWidget(u"verify actual paste preview"_q,
				[]() -> QWidget* {
					const auto active = Core::App().activePrimaryWindow();
					return active ? FindFirst<SendFilesBox>(active->widget()) : nullptr;
				},
				[=](QWidget *widget) {
					CaptureViaWindow(widget, u"album-paste-preview"_q);
					Check(true, u"system paste opens the native multi-photo send preview"_q);
					QApplication::clipboard()->setText(u"new text while preview is open"_q);
					Check(ranges::all_of(fixture->urls, [](const QUrl &url) {
						return QFileInfo::exists(url.toLocalFile());
					}), u"clipboard changes retain files needed by an open paste preview"_q);
					static_cast<SendFilesBox*>(widget)->closeBox();
				},
				[](QWidget *widget) { return widget->isVisible(); });
		}
	}
	AddDeletionScenarios(runner);
	runner->add({
		.name = u"record exports for cleanup verification"_q,
		.run = [=] {
			fixture->directories.removeDuplicates();
			for (const auto &directory : fixture->directories) {
				LogRaw(u"ALBUM_EXPORT_DIRECTORY: "_q + directory);
			}
		},
	});
}

} // namespace Test

#endif // _DEBUG
