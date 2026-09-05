/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_experimental.h"

#include "settings/settings_common.h"
#include "data/components/passkeys.h"
#include "ui/layers/generic_box.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/search_field_controller.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/vertical_list.h"
#include "ui/gl/gl_detection.h"
#include "ui/chat/chat_style_radius.h"
#include "ui/controls/compose_ai_button_factory.h"
#include "base/options.h"
#include "boxes/moderate_messages_box.h"
#include "core/application.h"
#include "core/launcher.h"
#include "core/sandbox.h"
#include "core/vim_keymap.h"
#include "core/vim_keymap_bindings.h"
#include "core/vim_keymap_config.h"
#include "chat_helpers/tabbed_panel.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_widget.h"
#include "dialogs/ui/dialogs_layout.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "history/history_item_components.h"
#include "history/view/controls/compose_controls_common.h"
#include "history/view/history_view_message.h"
#include "info/profile/info_profile_actions.h"
#include "info/profile/tabs/adapters/info_profile_tab_media.h"
#include "info/profile/tabs/info_profile_tabs_host.h"
#include "lang/lang_keys.h"
#include "mainwindow.h"
#include "mainwidget.h"
#include "media/player/media_player_instance.h"
#include "mtproto/session_private.h"
#include "webview/webview_embed.h"
#include "window/main_window.h"
#include "window/window_filters_favorite.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "window/notifications_manager.h"
#include "info/info_flexible_scroll.h"
#include "chat_helpers/stickers_list_widget.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_settings.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>

#include <algorithm>
#include <functional>

namespace Settings {
namespace {

const auto kOptionsClipboardPrefix = u"tdesktop-flags:"_q;

struct DecodeOptionsResult {
	bool ok = false;
	QString json;
};

struct StringChoice {
	QString value;
	QString label;
};

class VimComposeCursorPreview final : public Ui::RpWidget {
public:
	explicit VimComposeCursorPreview(QWidget *parent)
	: Ui::RpWidget(parent) {
		resize(width(), kHeight);
		_blinkTimer.setTimerType(Qt::PreciseTimer);
		QObject::connect(&_blinkTimer, &QTimer::timeout, this, [=] {
			_blinkVisible = !_blinkVisible;
			update();
		});

		rpl::merge(
			base::options::lookup<QString>(
				Core::VimKeymap::kOptionVimKeymapComposeCursorStyle
			).changes(),
			base::options::lookup<int>(
				Core::VimKeymap::kOptionVimKeymapComposeCursorWidth
			).changes(),
			base::options::lookup<int>(
				Core::VimKeymap::kOptionVimKeymapComposeCursorHeight
			).changes(),
			base::options::lookup<int>(
				Core::VimKeymap::kOptionVimKeymapComposeCursorBlink
			).changes()
		) | rpl::on_next([=] {
			resetBlink();
		}, lifetime());

		refreshBlink();
	}

protected:
	void paintEvent(QPaintEvent*) override {
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing, true);

		const auto bg = palette().color(QPalette::Window);
		const auto base = palette().color(QPalette::Base);
		const auto text = palette().color(QPalette::Text);
		auto muted = palette().color(QPalette::Text);
		muted.setAlpha(150);
		auto border = palette().color(QPalette::Text);
		border.setAlpha(34);

		p.fillRect(rect(), bg);

		const auto left = st::boxRowPadding.left();
		const auto right = st::boxRowPadding.right();
		const auto titleRect = QRect(
			left,
			10,
			std::max(1, width() - left - right),
			24);

		auto titleFont = font();
		titleFont.setBold(true);
		p.setFont(titleFont);
		p.setPen(text);
		p.drawText(
			titleRect,
			Qt::AlignLeft | Qt::AlignVCenter,
			u"Cursor preview"_q);

		auto detailsFont = font();
		p.setFont(detailsFont);
		p.setPen(muted);
		p.drawText(
			QRect(left, 32, titleRect.width(), 22),
			Qt::AlignLeft | Qt::AlignVCenter,
			u"View mode composer test text"_q);

		const auto field = QRect(
			left,
			58,
			std::max(1, width() - left - right),
			54);
		p.setPen(Qt::NoPen);
		p.setBrush(base);
		p.drawRoundedRect(field, 8, 8);
		p.setBrush(Qt::NoBrush);
		p.setPen(border);
		p.drawRoundedRect(field.adjusted(0, 0, -1, -1), 8, 8);

		const auto prefix = u"Test text: vim "_q;
		const auto suffix = u"cursor preview"_q;
		const auto full = prefix + suffix;
		const auto textRect = field.adjusted(14, 0, -14, 0);

		p.setFont(detailsFont);
		p.setPen(text);
		p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, full);

		if (!_blinkVisible) {
			return;
		}

		const auto metrics = QFontMetrics(detailsFont);
		const auto lineHeight = std::max(1, metrics.height());
		const auto baseline = field.top()
			+ (field.height() + metrics.ascent() - metrics.descent()) / 2;
		const auto cursorX = textRect.left()
			+ metrics.horizontalAdvance(prefix);
		const auto cursorTop = baseline - metrics.ascent();
		const auto cursorText = suffix.left(1);
		const auto requestedWidth = Core::VimKeymap::ComposeCursorWidth();
		const auto characterWidth = std::max(
			requestedWidth,
			metrics.horizontalAdvance(cursorText));
		const auto requestedHeight = std::clamp(
			lineHeight * Core::VimKeymap::ComposeCursorHeight() / 100,
			1,
			lineHeight);
		const auto style = Core::VimKeymap::ComposeCursorStyle();
		auto cursor = QRect();

		if (style == u"underline"_q) {
			const auto thickness = std::clamp(requestedWidth, 1, lineHeight);
			cursor = QRect(
				cursorX,
				cursorTop + lineHeight - thickness,
				characterWidth,
				thickness);
		} else {
			const auto top = cursorTop + (lineHeight - requestedHeight) / 2;
			cursor = QRect(
				cursorX,
				top,
				(style == u"block"_q) ? characterWidth : requestedWidth,
				requestedHeight);
		}
		cursor = cursor.intersected(field.adjusted(6, 4, -6, -4));
		if (cursor.isEmpty()) {
			return;
		}

		auto cursorColor = text;
		if (style == u"block"_q) {
			cursorColor.setAlpha(210);
		}
		p.fillRect(cursor, cursorColor);
		if (style == u"block"_q) {
			p.save();
			p.setClipRect(cursor);
			p.setPen(base);
			p.drawText(
				QRect(
					cursorX,
					field.top(),
					characterWidth,
					field.height()),
				Qt::AlignLeft | Qt::AlignVCenter,
				cursorText);
			p.restore();
		}
	}

private:
	static constexpr auto kHeight = 124;

	void refreshBlink() {
		const auto blink = Core::VimKeymap::ComposeCursorBlink();
		if (blink <= 0) {
			_blinkTimer.stop();
			_blinkVisible = true;
		} else if (!_blinkTimer.isActive()
			|| _blinkTimer.interval() != blink) {
			_blinkTimer.start(blink);
		}
	}

	void resetBlink() {
		_blinkVisible = true;
		refreshBlink();
		update();
	}

	QTimer _blinkTimer;
	bool _blinkVisible = true;
};

[[nodiscard]] QString EncodeOptionsToText(const QString &json) {
	const auto flags = QByteArray::Base64UrlEncoding
		| QByteArray::OmitTrailingEquals;
	return kOptionsClipboardPrefix
		+ qs(qCompress(json.toLatin1(), 9).toBase64(flags));
}

[[nodiscard]] DecodeOptionsResult DecodeOptionsFromText(const QString &text) {
	auto result = DecodeOptionsResult();
	if (!text.startsWith(kOptionsClipboardPrefix)) {
		return result;
	}
	auto encoded = QStringView(text).mid(
		kOptionsClipboardPrefix.size()).toLatin1();
	const auto compressed = QByteArray::fromBase64Encoding(
		std::move(encoded),
		QByteArray::Base64UrlEncoding
			| QByteArray::AbortOnBase64DecodingErrors);
	if (!compressed || (*compressed).isEmpty()) {
		return result;
	}
	const auto decoded = qUncompress(*compressed);
	if (decoded.isEmpty()) {
		return result;
	}

	auto error = QJsonParseError();
	const auto parsed = QJsonDocument::fromJson(decoded, &error);
	if ((error.error != QJsonParseError::NoError) || !parsed.isObject()) {
		return result;
	}
	result.ok = true;
	result.json = QString::fromUtf8(decoded);
	return result;
}

void SetupCopyDeepLink(
		not_null<Window::Controller*> window,
		not_null<Button*> button,
		const QString &id) {
	const auto link = u"tg://settings/experimental/"_q + id;
	const auto menu
		= button->lifetime().make_state<base::unique_qptr<Ui::PopupMenu>>();
	button->events(
	) | rpl::filter([](not_null<QEvent*> e) {
		return e->type() == QEvent::ContextMenu;
	}) | rpl::on_next([=](not_null<QEvent*> e) {
		*menu = base::make_unique_q<Ui::PopupMenu>(
			button,
			st::popupMenuWithIcons);
		(*menu)->addAction(u"Copy deep link"_q, [=] {
			TextUtilities::SetClipboardText({ link });
			window->showToast({
				.text = { u"Deep link copied to clipboard."_q },
				.iconLottie = u"toast/voip_invite"_q,
				.iconLottieSize = st::toastLottieIconSize,
			});
		}, &st::menuIconCopy);
		(*menu)->popup(QCursor::pos());
		e->accept();
	}, button->lifetime());
}

[[nodiscard]] not_null<Button*> AddOptionRow(
		not_null<Ui::VerticalLayout*> container,
		const QString &name,
		const QString &description,
		const style::SettingsButton &st) {
	if (description.isEmpty()) {
		return container->add(object_ptr<Button>(
			container,
			rpl::single(name),
			st));
	}
	const auto &titlePadding = st::settingsExperimentalTitlePadding;
	const auto &aboutPadding = st::settingsExperimentalAboutPadding;
	const auto button = Ui::CreateChild<Button>(
		container.get(),
		rpl::single(QString()),
		st);
	const auto title = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			name,
			st::settingsExperimentalTitle),
		titlePadding);
	const auto about = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			description,
			st::settingsExperimentalAbout),
		aboutPadding);
	title->setAttribute(Qt::WA_TransparentForMouseEvents);
	about->setAttribute(Qt::WA_TransparentForMouseEvents);
	rpl::combine(
		container->widthValue(),
		title->heightValue(),
		about->heightValue()
	) | rpl::on_next([=](int width, int titleHeight, int aboutHeight) {
		button->resize(width, titlePadding.top()
			+ titleHeight
			+ titlePadding.bottom()
			+ aboutPadding.top()
			+ aboutHeight
			+ aboutPadding.bottom());
	}, button->lifetime());
	title->topValue(
	) | rpl::on_next([=](int top) {
		button->moveToLeft(0, top - titlePadding.top());
	}, button->lifetime());
	button->show();
	return button;
}

QString AddOption(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		base::options::option<bool> &option,
		rpl::producer<> resetClicks,
		rpl::producer<> reloadOptionsRequests,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto name = option.name().isEmpty() ? option.id() : option.name();
	const auto &description = option.description();

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		style::margins(),
		style::al_justify);
	const auto inner = wrap->entity();

	auto &lifetime = inner->lifetime();
	const auto toggles = lifetime.make_state<rpl::event_stream<bool>>();
	std::move(
		resetClicks
	) | rpl::map_to(
		option.defaultValue()
	) | rpl::start_to_stream(*toggles, lifetime);
	std::move(reloadOptionsRequests) | rpl::on_next([=, &option] {
		toggles->fire_copy(option.value());
	}, lifetime);

	const auto button = AddOptionRow(
		inner,
		name,
		description,
		(option.relevant()
			? st::settingsButtonNoIcon
			: st::settingsOptionDisabled)
	)->toggleOn(toggles->events_starting_with(option.value()));

	if (registerHighlight) {
		registerHighlight(u"experimental/"_q + option.id(), button);
	}

	SetupCopyDeepLink(window, button, option.id());

	const auto restarter = (option.relevant() && option.restartRequired())
		? button->lifetime().make_state<base::Timer>()
		: nullptr;
	if (restarter) {
		restarter->setCallback([=] {
			window->show(Ui::MakeConfirmBox({
				.text = tr::lng_settings_need_restart(),
				.confirmed = [] { Core::Restart(); },
				.confirmText = tr::lng_settings_restart_now(),
				.cancelText = tr::lng_settings_restart_later(),
			}));
		});
	}
	button->toggledChanges(
	) | rpl::on_next([=, &option](bool toggled) {
		if (!option.relevant() && toggled != option.defaultValue()) {
			toggles->fire_copy(option.defaultValue());
			window->showToast(
				tr::lng_settings_experimental_irrelevant(tr::now));
			return;
		}
		option.set(toggled);
		if (restarter) {
			restarter->callOnce(st::settingsButtonNoIcon.toggle.duration);
		}
	}, inner->lifetime());

	const auto searchable = name + ' ' + description;
	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

QString AddFavoriteLinkButton(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto option = &base::options::lookup<QString>(
		Window::kOptionFolderFavoriteLink);
	const auto name = option->name().isEmpty()
		? option->id()
		: option->name();
	const auto &description = option->description();

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		style::margins(),
		style::al_justify);
	const auto inner = wrap->entity();

	auto label = rpl::single(
		rpl::empty
	) | rpl::then(
		option->changes()
	) | rpl::map([option] {
		return option->value();
	});
	const auto button = AddButtonWithLabel(
		inner,
		rpl::single(name),
		std::move(label),
		st::settingsButtonNoIcon);
	button->setClickedCallback([=] {
		window->show(Box(Window::EditFolderFavoriteLinkBox));
	});

	if (registerHighlight) {
		registerHighlight(u"experimental/"_q + option->id(), button);
	}

	SetupCopyDeepLink(window, button, option->id());

	const auto searchable = name + ' ' + description;
	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

void EditIntegerOptionBox(
		not_null<Ui::GenericBox*> box,
		base::options::option<int> &option,
		Core::VimKeymap::IntOptionBounds bounds) {
	const auto name = option.name().isEmpty() ? option.id() : option.name();
	box->setTitle(name);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Enter a value from %1 to %2."_q
			.arg(bounds.min)
			.arg(bounds.max),
		st::boxLabel));

	const auto height = st::boxPadding.bottom()
		+ st::defaultInputField.heightMin
		+ st::boxPadding.bottom();
	const auto wrap = box->addRow(object_ptr<Ui::FixedHeightWidget>(
		box,
		height));
	const auto input = Ui::CreateChild<Ui::NumberInput>(
		wrap,
		st::defaultInputField,
		rpl::single(name),
		QString::number(std::clamp(option.value(), bounds.min, bounds.max)),
		bounds.max);
	wrap->widthValue(
	) | rpl::on_next([=](int width) {
		input->resize(width, input->height());
		input->moveToLeft(0, st::boxPadding.bottom());
	}, input->lifetime());
	box->setFocusCallback([=] {
		input->setFocusFast();
	});
	input->selectAll();

	const auto save = [=, &option] {
		const auto value = input->getLastText().toInt();
		if (value < bounds.min || value > bounds.max) {
			input->showError();
			return;
		}
		option.set(value);
		box->closeBox();
	};
	QObject::connect(input, &Ui::NumberInput::submitted, save);
	QObject::connect(input, &Ui::NumberInput::cancelled, [=] {
		box->closeBox();
	});
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

QString AddIntegerOption(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		base::options::option<int> &option,
		Core::VimKeymap::IntOptionBounds bounds,
		rpl::producer<> reloadOptionsRequests,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto name = option.name().isEmpty() ? option.id() : option.name();
	const auto &description = option.description();

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		style::margins(),
		style::al_justify);
	const auto inner = wrap->entity();

	auto valueChanges = rpl::merge(
		option.changes(),
		std::move(reloadOptionsRequests));
	auto label = rpl::single(
		rpl::empty
	) | rpl::then(
		std::move(valueChanges)
	) | rpl::map([=, &option] {
		return QString::number(std::clamp(
			option.value(),
			bounds.min,
			bounds.max));
	});
	const auto button = AddButtonWithLabel(
		inner,
		rpl::single(name),
		std::move(label),
		st::settingsButtonNoIcon);

	if (registerHighlight) {
		registerHighlight(u"experimental/"_q + option.id(), button);
	}

	SetupCopyDeepLink(window, button, option.id());
	button->setClickedCallback([=, &option] {
		window->show(Box(
			EditIntegerOptionBox,
			std::ref(option),
			bounds));
	});

	const auto searchable = name + ' ' + description;
	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

void EditStringOptionBox(
		not_null<Ui::GenericBox*> box,
		base::options::option<QString> &option) {
	const auto name = option.name().isEmpty() ? option.id() : option.name();
	box->setTitle(name);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Use comma-separated bindings. Examples: j, Ctrl+J, Esc, ?, "
		u"Tab, Shift+Tab. Empty disables this binding."_q,
		st::boxLabel));

	const auto input = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		rpl::single(name),
		option.value()));
	box->setFocusCallback([=] {
		input->setFocusFast();
	});
	input->selectAll();
	input->setMaxLength(1024);

	const auto save = [=, &option] {
		const auto value = input->getLastText().trimmed();
		if (!Core::VimKeymap::Bindings::ValidBindings(value)) {
			input->showError();
			return;
		}
		option.set(value);
		box->closeBox();
	};
	input->submits(
	) | rpl::on_next([=] { save(); }, input->lifetime());
	input->cancelled(
	) | rpl::on_next([=] {
		box->closeBox();
	}, input->lifetime());
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

[[nodiscard]] QString StringChoiceLabel(
		const QString &value,
		const std::vector<StringChoice> &choices) {
	const auto normalized = value.trimmed().toCaseFolded();
	for (const auto &choice : choices) {
		if (choice.value.trimmed().toCaseFolded() == normalized) {
			return choice.label;
		}
	}
	return value.trimmed().isEmpty() ? u"-"_q : value.trimmed();
}

QString AddStringChoiceOption(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		base::options::option<QString> &option,
		std::vector<StringChoice> choices,
		rpl::producer<> reloadOptionsRequests,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto name = option.name().isEmpty() ? option.id() : option.name();
	const auto &description = option.description();

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		style::margins(),
		style::al_justify);
	const auto inner = wrap->entity();

	auto valueChanges = rpl::merge(
		option.changes(),
		std::move(reloadOptionsRequests));
	auto label = rpl::single(
		rpl::empty
	) | rpl::then(
		std::move(valueChanges)
	) | rpl::map([=, &option] {
		return StringChoiceLabel(option.value(), choices);
	});
	const auto button = AddButtonWithLabel(
		inner,
		rpl::single(name),
		std::move(label),
		st::settingsButtonNoIcon);

	if (registerHighlight) {
		registerHighlight(u"experimental/"_q + option.id(), button);
	}

	SetupCopyDeepLink(window, button, option.id());
	const auto menu
		= button->lifetime().make_state<base::unique_qptr<Ui::PopupMenu>>();
	button->setClickedCallback([=, &option] {
		*menu = base::make_unique_q<Ui::PopupMenu>(
			button,
			st::popupMenuWithIcons);
		for (const auto &choice : choices) {
			(*menu)->addAction(choice.label, [=, &option] {
				option.set(choice.value);
			});
		}
		(*menu)->popup(QCursor::pos());
	});

	const auto searchable = name + ' ' + description;
	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

QString AddStringOption(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		base::options::option<QString> &option,
		rpl::producer<> reloadOptionsRequests,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto name = option.name().isEmpty() ? option.id() : option.name();
	const auto &description = option.description();

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		style::margins(),
		style::al_justify);
	const auto inner = wrap->entity();

	auto valueChanges = rpl::merge(
		option.changes(),
		std::move(reloadOptionsRequests));
	auto label = rpl::single(
		rpl::empty
	) | rpl::then(
		std::move(valueChanges)
	) | rpl::map([=, &option] {
		const auto value = option.value().trimmed();
		return value.isEmpty() ? u"-"_q : value;
	});
	const auto button = AddButtonWithLabel(
		inner,
		rpl::single(name),
		std::move(label),
		st::settingsButtonNoIcon);

	if (registerHighlight) {
		registerHighlight(u"experimental/"_q + option.id(), button);
	}

	SetupCopyDeepLink(window, button, option.id());
	button->setClickedCallback([=, &option] {
		window->show(Box(EditStringOptionBox, std::ref(option)));
	});

	const auto searchable = name + ' ' + description;
	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

QString AddComposeCursorPreview(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> query = rpl::producer<QString>()) {
	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<VimComposeCursorPreview>>(
			container,
			object_ptr<VimComposeCursorPreview>(container)),
		style::margins(),
		style::al_justify);

	const auto searchable = u"Cursor preview Vim compose cursor "
		u"View mode composer test text"_q;
	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

void SetupExperimental(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<> reloadOptionsRequests,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto headerWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto header = headerWrap->entity();

	Ui::AddSkip(header, st::settingsCheckboxesSkip);

	header->add(
		object_ptr<Ui::FlatLabel>(
			header,
			tr::lng_settings_experimental_about(),
			st::boxLabel),
		st::defaultBoxDividerLabelPadding);

	auto reset = (Button*)nullptr;
	if (base::options::changed()) {
		const auto wrap = header->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				header,
				object_ptr<Ui::VerticalLayout>(header)));
		const auto inner = wrap->entity();
		Ui::AddDivider(inner);
		Ui::AddSkip(inner, st::settingsCheckboxesSkip);
		reset = inner->add(object_ptr<Button>(
			inner,
			tr::lng_settings_experimental_restore(),
			st::settingsButtonNoIcon));
		reset->addClickHandler([=] {
			base::options::reset();
			wrap->hide(anim::type::normal);
		});
		Ui::AddSkip(inner, st::settingsCheckboxesSkip);
	}

	Ui::AddDivider(header);

	rpl::duplicate(
		query
	) | rpl::on_next([=](const QString &text) {
		headerWrap->toggle(text.trimmed().isEmpty(), anim::type::instant);
	}, headerWrap->lifetime());

	struct Category {
		QString title;
		std::vector<const char*> options;
	};
	const auto categories = std::vector<Category>{
		{
			u"Chats"_q,
			{
				Dialogs::kOptionForumHideChatsList,
				Dialogs::kOptionDialogsUnreadOnTop,
				Dialogs::Ui::kOptionDialogsMuteIcon,
				kOptionUseNewChatView,
				kOptionAutoScrollInactiveChat,
				kModerateCommonGroups,
				Info::kClassicProfileScroll,
			}
		},
		{
			u"Messages"_q,
			{
				Ui::kOptionUseSmallMsgBubbleRadius,
				HistoryView::kOptionUnlimitedMessageWidth,
				HistoryView::Controls::kOptionMacCmdReplyImmediately,
				Ui::kOptionHideAiButton,
				kForceComposeSearchOneColumn,
			}
		},
		{
			u"Profile"_q,
			{
				Window::kOptionViewProfileInChatsListContextMenu,
				Info::Profile::kOptionShowPeerIdBelowAbout,
				Info::Profile::kOptionShowChannelJoinedBelowAbout,
				Info::Profile::kOptionProfileMediaTabs,
				Info::Profile::kOptionProfileMediaTabsExpanded,
			}
		},
		{
			u"Stickers and emoji"_q,
			{
				ChatHelpers::kOptionTabbedPanelShowOnClick,
				ChatHelpers::kOptionUnlimitedRecentStickers,
			}
		},
		{
			u"Media"_q,
			{
				Media::Player::kOptionDisableAutoplayNext,
				Window::kOptionExternalMediaViewer,
				FFmpeg::kOptionFFmpegMultiThread,
			}
		},
		{
			u"Notifications"_q,
			{
				Window::Notifications::kOptionHideReplyButton,
				Window::Notifications::kOptionCustomNotification,
				Window::Notifications::kOptionGNotification,
				Window::Notifications::kOptionMacModernNotifications,
			}
		},
		{
			u"Interface"_q,
			{
				Core::kOptionFractionalScalingEnabled,
				Core::kOptionHighDpiDownscale,
				Ui::GL::kOptionUseQtRhi,
				Ui::GL::kOptionEnableVulkanRhi,
				Core::kOptionFreeType,
				Ui::kOptionQScroller,
				Window::kOptionDisableTouchbar,
				Window::kOptionNewWindowsSizeAsFirst,
			}
		},
		{
			u"System"_q,
			{
				MTP::details::kOptionPreferIPv6,
				Core::kOptionSkipUrlSchemeRegister,
				Core::kOptionDeadlockDetector,
				Webview::kOptionWebviewDebugEnabled,
				Webview::kOptionWebviewLegacyEdge,
			}
		},
	};

	const auto addOption = [&](
			not_null<Ui::VerticalLayout*> inner,
			const char name[]) {
		return AddOption(
			window,
			inner,
			base::options::lookup<bool>(name),
			(reset
				? (reset->clicks() | rpl::to_empty)
				: rpl::producer<>()),
			rpl::duplicate(reloadOptionsRequests),
			rpl::duplicate(query),
			registerHighlight);
	};
	const auto addIntegerOption = [&](
			not_null<Ui::VerticalLayout*> inner,
			const char name[],
			Core::VimKeymap::IntOptionBounds bounds) {
		return AddIntegerOption(
			window,
			inner,
			base::options::lookup<int>(name),
			bounds,
			rpl::duplicate(reloadOptionsRequests),
			rpl::duplicate(query),
			registerHighlight);
	};
	const auto addStringOption = [&](
			not_null<Ui::VerticalLayout*> inner,
			const char name[]) {
		return AddStringOption(
			window,
			inner,
			base::options::lookup<QString>(name),
			rpl::duplicate(reloadOptionsRequests),
			rpl::duplicate(query),
			registerHighlight);
	};
	const auto addHintAlphabetOption = [&](
			not_null<Ui::VerticalLayout*> inner) {
		return AddStringChoiceOption(
			window,
			inner,
			base::options::lookup<QString>(
				Core::VimKeymap::kOptionVimKeymapHintAlphabet),
			{
				{ u"russian"_q, u"Russian"_q },
				{ u"english"_q, u"English"_q },
			},
			rpl::duplicate(reloadOptionsRequests),
			rpl::duplicate(query),
			registerHighlight);
	};
	const auto addComposeCursorStyleOption = [&](
			not_null<Ui::VerticalLayout*> inner) {
		return AddStringChoiceOption(
			window,
			inner,
			base::options::lookup<QString>(
				Core::VimKeymap::kOptionVimKeymapComposeCursorStyle),
			{
				{ u"block"_q, u"Block"_q },
				{ u"bar"_q, u"Bar"_q },
				{ u"underline"_q, u"Underline"_q },
			},
			rpl::duplicate(reloadOptionsRequests),
			rpl::duplicate(query),
			registerHighlight);
	};
	const auto addCategory = [&](
			const QString &title,
			auto &&fill) {
		const auto wrap = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		const auto inner = wrap->entity();
		Ui::AddSkip(inner);
		Ui::AddSubsectionTitle(inner, rpl::single(title));
		auto searchable = std::vector<QString>();
		fill(inner, searchable);
		Ui::AddSkip(inner);
		Ui::AddDivider(inner);

		auto terms = std::vector<QStringList>();
		for (const auto &entry : searchable) {
			terms.push_back(SearchWords(entry));
		}
		rpl::duplicate(
			query
		) | rpl::on_next([=](const QString &text) {
			const auto words = SearchWords(text);
			const auto matches = words.isEmpty()
				|| ranges::any_of(terms, [&](const QStringList &entry) {
					return MatchesWords(entry, words);
				});
			wrap->toggle(matches, anim::type::instant);
		}, wrap->lifetime());
	};

	for (const auto &category : categories) {
		addCategory(category.title, [&](
				not_null<Ui::VerticalLayout*> inner,
				std::vector<QString> &searchable) {
			for (const auto name : category.options) {
				searchable.push_back(addOption(inner, name));
			}
		});
	}

	addCategory(u"Vim keymap"_q, [&](
			not_null<Ui::VerticalLayout*> inner,
			std::vector<QString> &searchable) {
		searchable.push_back(addOption(
			inner,
			Core::VimKeymap::kOptionVimKeymap));
		searchable.push_back(addOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapEscapeClosesComposer));
		searchable.push_back(addIntegerOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapScrollStep,
			Core::VimKeymap::ScrollStepBounds()));
		searchable.push_back(addIntegerOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapHoldScrollSpeed,
			Core::VimKeymap::HoldScrollSpeedBounds()));
		searchable.push_back(addIntegerOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapHintSize,
			Core::VimKeymap::HintSizeBounds()));
		searchable.push_back(addHintAlphabetOption(inner));
		searchable.push_back(addComposeCursorStyleOption(inner));
		searchable.push_back(addIntegerOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapComposeCursorWidth,
			Core::VimKeymap::ComposeCursorWidthBounds()));
		searchable.push_back(addIntegerOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapComposeCursorHeight,
			Core::VimKeymap::ComposeCursorHeightBounds()));
		searchable.push_back(addIntegerOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapComposeCursorBlink,
			Core::VimKeymap::ComposeCursorBlinkBounds()));
		searchable.push_back(AddComposeCursorPreview(
			inner,
			rpl::duplicate(query)));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyToggleMode));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyCancelReply));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyCancelEdit));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyHelp));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyScrollDown));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyScrollUp));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyJumpBottom));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyCopyMessage));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeySelectMessageText));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyReplyToMessage));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyEditMessage));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyDeleteMessage));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyFocusHints));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyOpenChatHints));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyChatPreview));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeySearch));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyGlobalSearch));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyNextChat));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyPreviousChat));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyNextFolder));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyPreviousFolder));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyEmojiPanel));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyFocusEmoji));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyFocusChat));
		searchable.push_back(addStringOption(
			inner,
			Core::VimKeymap::kOptionVimKeymapKeyCall));
	});

	addCategory(u"Other"_q, [&](
			not_null<Ui::VerticalLayout*> inner,
			std::vector<QString> &searchable) {
		if (base::options::lookup<bool>(kOptionFastButtonsMode).value()) {
			searchable.push_back(addOption(inner, kOptionFastButtonsMode));
		}
		searchable.push_back(AddFavoriteLinkButton(
			window,
			inner,
			rpl::duplicate(query),
			registerHighlight));
	});
}

} // namespace

void SetupVimKeymapOptions(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container) {
	using namespace Core::VimKeymap;
	MigrateLegacyDefaults();
	for (const auto &field : ConfigOptions()) {
		auto &option = base::options::details::Lookup(field.id);
		if (v::is<bool>(option.value())) {
			AddOption(window, container, base::options::lookup<bool>(field.id),
				{}, option.changes(), {}, nullptr);
		} else if (field.bounds) {
			AddIntegerOption(window, container, base::options::lookup<int>(field.id),
				*field.bounds, option.changes(), {}, nullptr);
		} else if (!field.choices.empty()) {
			auto choices = std::vector<StringChoice>();
			for (const auto &value : field.choices) {
				auto label = value;
				label[0] = label.front().toUpper();
				choices.push_back({ value, label });
			}
			AddStringChoiceOption(window, container,
				base::options::lookup<QString>(field.id),
				std::move(choices), option.changes(), {}, nullptr);
		} else {
			AddStringOption(window, container, base::options::lookup<QString>(field.id),
				option.changes(), {}, nullptr);
		}
		if (QString::fromLatin1(field.id) == QLatin1String(kOptionVimKeymapComposeCursorBlink)) {
			AddComposeCursorPreview(container);
		}
	}
}

Experimental::Experimental(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

Experimental::~Experimental() = default;

rpl::producer<QString> Experimental::title() {
	return tr::lng_settings_experimental();
}

void Experimental::fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) {
	const auto window = &controller()->window();
	addAction(
		u"Export"_q,
		[=] {
			TextUtilities::SetClipboardText(
				{ EncodeOptionsToText(base::options::serialize()) });
			window->showToast({
				.text = { u"Experimental settings code copied to clipboard."_q },
				.iconLottie = u"toast/copy"_q,
				.iconLottieSize = st::toastLottieIconSize,
			});
		},
		&st::menuIconCopy);
	if (!DecodeOptionsFromText(QGuiApplication::clipboard()->text()).ok) {
		return;
	}
	addAction(
		u"Import"_q,
		[=] {
			const auto decoded = DecodeOptionsFromText(
				QGuiApplication::clipboard()->text());
			if (!decoded.ok) {
				window->showToast(u"Clipboard does not contain "
					"a valid experimental settings code."_q);
				return;
			}
			if (!base::options::deserialize(decoded.json)) {
				window->showToast(u"Experimental settings code is valid"
					", but data format is not supported."_q);
				return;
			}
			_reloadOptionsRequests.fire({});
			window->showToast(u"Experimental settings imported "
				"from code in clipboard."_q);
		},
		&st::menuIconImportTheme);
}

void Experimental::setInnerFocus() {
	if (_searchField) {
		_searchField->setFocus();
	} else {
		setFocus();
	}
}

void Experimental::showFinished() {
	AbstractSection::showFinished();
	for (const auto &[id, widget] : _highlights) {
		if (widget) {
			controller()->checkHighlightControl(id, widget);
		}
	}
}

base::weak_qptr<Ui::RpWidget> Experimental::createPinnedToTop(
		not_null<QWidget*> parent) {
	auto search = CreateSectionSearchRow(parent, _query.current());
	_searchController = std::move(search.controller);
	const auto row = search.row;
	_searchField = search.field;

	_searchController->queryValue(
	) | rpl::on_next([=](QString text) {
		_query = std::move(text);
	}, row->lifetime());

	return base::make_weak(row);
}

void Experimental::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	SetupExperimental(
		&controller()->window(),
		content,
		_reloadOptionsRequests.events(),
		_query.value(),
		[this](const QString &id, not_null<QWidget*> widget) {
			_highlights.push_back({ id, widget.get() });
		});

	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
