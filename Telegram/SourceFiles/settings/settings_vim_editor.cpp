/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_vim_editor.h"

#include "base/options.h"
#include "core/vim_keymap_config.h"
#include "core/vim_keymap_widgets.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_widgets.h"
#include "styles/style_vim_keymap.h"

#include <QtCore/QJsonDocument>
#include <QtGui/QPalette>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTextBrowser>

namespace Settings {
namespace {

void SetEditorPalette(not_null<QWidget*> widget) {
	auto palette = widget->palette();
	palette.setColor(QPalette::Base, st::windowBg->c);
	palette.setColor(QPalette::Text, st::windowFg->c);
	palette.setColor(QPalette::Highlight, st::windowBgActive->c);
	palette.setColor(QPalette::HighlightedText, st::windowFgActive->c);
	widget->setPalette(palette);
}

} // namespace

VimKeymapEditor::VimKeymapEditor(
	QWidget *parent,
	Fn<void(not_null<Ui::VerticalLayout*>)> fillUi,
	Fn<void(Fn<void()>)> confirmDiscard,
	Fn<void()> showGuide)
: Ui::RpWidget(parent)
, _confirmDiscard(std::move(confirmDiscard)) {
	const auto layout = Ui::CreateChild<Ui::VerticalLayout>(this);
	_tabs = layout->add(object_ptr<Ui::SettingsSlider>(layout));
	_tabs->addSection(u"UI"_q);
	_tabs->addSection(u"JSON"_q);
	_tabs->setActiveSectionFast(0);
	_tabs->sectionActivated() | rpl::on_next([=](int index) {
		if (!_updatingTabs) {
			chooseMode(index);
		}
	}, lifetime());
	for (auto i = 0; i != 2; ++i) {
		const auto target = Core::VimKeymap::CreateKeyboardTabTarget(_tabs, [=] { chooseMode(i); });
		target->setObjectName(i ? u"vim-mode-json"_q : u"vim-mode-ui"_q);
		target->setAccessibleName(i ? u"JSON"_q : u"UI"_q);
		_tabs->sizeValue() | rpl::on_next([=](QSize size) {
			const auto left = size.width() * i / 2;
			target->setGeometry(left, 0, size.width() * (i + 1) / 2 - left, size.height());
		}, target->lifetime());
	}
	const auto guide = layout->add(object_ptr<Ui::SettingsButton>(layout, rpl::single(u"Руководство"_q)));
	guide->setObjectName(u"vim-guide"_q);
	guide->setClickedCallback(std::move(showGuide));
	_ui = layout->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		layout, object_ptr<Ui::VerticalLayout>(layout)));
	fillUi(_ui->entity());
	_json = layout->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		layout, object_ptr<Ui::VerticalLayout>(layout)));
	const auto json = _json->entity();
	const auto field = json->add(object_ptr<Ui::RpWidget>(json));
	field->resize(0, st::vimJsonEditorHeight);
	_text = new QPlainTextEdit(field);
	_text->setObjectName(u"vim-json-editor"_q);
	_text->setAccessibleName(u"Конфигурация Vim JSON"_q);
	_text->setFont(st::normalFont->monospace()->f);
	_text->setTabChangesFocus(true);
	_text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	SetEditorPalette(_text);
	style::PaletteChanged() | rpl::on_next([=] {
		SetEditorPalette(_text);
	}, lifetime());
	field->sizeValue() | rpl::on_next([=](QSize size) {
		_text->setGeometry(QRect(QPoint(), size));
	}, field->lifetime());
	_status = json->add(object_ptr<Ui::FlatLabel>(json));
	_status->setObjectName(u"vim-json-status"_q);
	_status->setBreakEverywhere(true);
	_apply = json->add(object_ptr<Ui::SettingsButton>(json, rpl::single(u"Применить"_q)));
	_apply->setObjectName(u"vim-json-apply"_q);
	_apply->setClickedCallback([=] { apply(); });
	const auto reread = json->add(object_ptr<Ui::SettingsButton>(json, rpl::single(u"Перечитать"_q)));
	reread->setObjectName(u"vim-json-reload"_q);
	reread->setClickedCallback([=] { checkBeforeClose([=] { reload(); }); });
	QObject::connect(_text, &QPlainTextEdit::textChanged, this, [=] {
		_status->setText({});
		updateState();
	});
	for (const auto &field : Core::VimKeymap::ConfigOptions()) {
		base::options::details::Lookup(field.id).changes() | rpl::on_next([=] {
			if (_mode == 1 && !_applying) {
				if (!dirty()) {
					reload();
				} else {
					_status->setText(u"Настройки изменены в другом окне. Перечитайте конфигурацию перед применением."_q);
				}
			}
		}, lifetime());
	}
	displayMode(0);
	Ui::ResizeFitChild(this, layout);
}

bool VimKeymapEditor::dirty() const {
	return _mode == 1 && _text->toPlainText() != _cleanText;
}

void VimKeymapEditor::checkBeforeClose(Fn<void()> close) {
	const auto weak = QPointer<VimKeymapEditor>(this);
	const auto proceed = [=] {
		if (weak) {
			reload();
			close();
		}
	};
	if (dirty()) {
		_confirmDiscard(proceed);
	} else {
		proceed();
	}
}

void VimKeymapEditor::chooseMode(int mode) {
	if (mode == _mode) {
		return;
	}
	_updatingTabs = true;
	_tabs->setActiveSectionFast(_mode);
	_updatingTabs = false;
	if (dirty()) {
		_text->setFocus();
	}
	checkBeforeClose([=] { displayMode(mode); });
}

void VimKeymapEditor::displayMode(int mode) {
	_mode = mode;
	if (_mode == 1) {
		reload();
	}
	_updatingTabs = true;
	_tabs->setActiveSectionFast(mode);
	_updatingTabs = false;
	_ui->toggle(mode == 0, anim::type::instant);
	_json->toggle(mode == 1, anim::type::instant);
}

void VimKeymapEditor::reload() {
	_baseline = Core::VimKeymap::ConfigSnapshot();
	_cleanText = QString::fromUtf8(QJsonDocument(_baseline).toJson(QJsonDocument::Indented));
	_text->setPlainText(_cleanText);
	_status->setText({});
	updateState();
}

void VimKeymapEditor::updateState() {
	_apply->setDisabled(!dirty());
}

void VimKeymapEditor::apply() {
	if (_baseline != Core::VimKeymap::ConfigSnapshot()) {
		_status->setText(u"Настройки изменены в другом окне. Перечитайте конфигурацию перед применением."_q);
		return;
	}
	_applying = true;
	const auto error = Core::VimKeymap::ApplyConfig(_text->toPlainText().toUtf8());
	_applying = false;
	if (!error.isEmpty()) {
		_status->setText(error);
		return;
	}
	reload();
	_status->setText(u"Настройки применены."_q);
}

object_ptr<Ui::RpWidget> CreateVimKeymapGuide(QWidget *parent) {
	auto result = object_ptr<Ui::RpWidget>(parent);
	const auto view = new QTextBrowser(result);
	view->setObjectName(u"vim-guide-text"_q);
	view->setAccessibleName(u"Руководство Vim keymap"_q);
	view->setReadOnly(true);
	view->setOpenExternalLinks(false);
	view->setOpenLinks(false);
	view->setMarkdown(Core::VimKeymap::ConfigGuide());
	SetEditorPalette(view);
	style::PaletteChanged() | rpl::on_next([=] {
		SetEditorPalette(view);
	}, result->lifetime());
	result->resize(0, st::vimGuideHeight);
	result->sizeValue() | rpl::on_next([=](QSize size) {
		view->setGeometry(QRect(QPoint(), size));
	}, result->lifetime());
	return result;
}

} // namespace Settings
