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
#include <QtCore/QRegularExpression>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QSyntaxHighlighter>
#include <QtGui/QTextBlock>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollBar>

namespace Settings {
namespace {

class JsonHighlighter final : public QSyntaxHighlighter {
public:
	explicit JsonHighlighter(QTextDocument *document)
	: QSyntaxHighlighter(document) {
	}

protected:
	void highlightBlock(const QString &text) override {
		static const auto tokens = QRegularExpression(
			uR"json("(?:[^"\\]|\\.)*"|\b(?:true|false|null)\b|-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)json"_q);
		auto matches = tokens.globalMatch(text);
		while (matches.hasNext()) {
			const auto match = matches.next();
			const auto token = match.captured();
			const auto quoted = token.startsWith('"');
			const auto key = quoted
				&& text.mid(match.capturedEnd()).trimmed().startsWith(':');
			const auto color = key ? st::windowFg->c
				: quoted ? st::boxTextFgGood->c : st::windowActiveTextFg->c;
			setFormat(match.capturedStart(), match.capturedLength(), color);
		}
	}

};

class JsonEditor final : public QPlainTextEdit {
public:
	explicit JsonEditor(QWidget *parent)
	: QPlainTextEdit(parent)
	, _gutter(Ui::CreateChild<Ui::RpWidget>(this))
	, _highlighter(new JsonHighlighter(document())) {
		setFrameShape(QFrame::NoFrame);
		setFont(st::normalFont->monospace()->f);
		setTabChangesFocus(true);
		setLineWrapMode(QPlainTextEdit::NoWrap);
		document()->setDocumentMargin(st::vimJsonPadding);
		_gutter->setObjectName(u"vim-json-lines"_q);
		_gutter->setAttribute(Qt::WA_TransparentForMouseEvents);
		_gutter->paintRequest() | rpl::on_next([=] { paintGutter(); }, _gutter->lifetime());
		QObject::connect(this, &QPlainTextEdit::blockCountChanged, this, [=] {
			updateGutter();
		});
		QObject::connect(this, &QPlainTextEdit::updateRequest, this, [=] {
			_gutter->update();
		});
		QObject::connect(this, &QPlainTextEdit::cursorPositionChanged, this, [=] {
			updateCurrentLine();
			_gutter->update();
		});
		updateGutter();
		refreshPalette();
	}

	void refreshPalette() {
		auto colors = palette();
		colors.setColor(QPalette::Base, st::windowBg->c);
		colors.setColor(QPalette::Text, st::windowFg->c);
		colors.setColor(QPalette::Highlight, st::windowBgActive->c);
		colors.setColor(QPalette::HighlightedText, st::windowFgActive->c);
		setStyleSheet(uR"(
QScrollBar { background: transparent; border: none; margin: 0; }
QScrollBar:vertical { width: %1px; }
QScrollBar:horizontal { height: %1px; }
QScrollBar::handle { background: %2; border-radius: %3px; }
QScrollBar::handle:vertical { min-height: %4px; }
QScrollBar::handle:horizontal { min-width: %4px; }
QScrollBar::handle:hover { background: %5; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QAbstractScrollArea::corner { background: transparent; }
)"_q.arg(st::vimJsonScrollWidth)
			.arg(st::scrollBarBg->c.name(QColor::HexArgb))
			.arg(st::vimJsonScrollRadius)
			.arg(st::vimJsonScrollMin)
			.arg(st::scrollBarBgOver->c.name(QColor::HexArgb)));
		setPalette(colors);
		viewport()->setPalette(colors);
		_highlighter->rehighlight();
		updateCurrentLine();
		_gutter->update();
	}

protected:
	void resizeEvent(QResizeEvent *event) override {
		QPlainTextEdit::resizeEvent(event);
		updateGutter();
	}

private:
	void updateGutter() {
		const auto digits = QString::number(blockCount()).size();
		const auto width = st::vimJsonGutterPadding * 2
			+ fontMetrics().horizontalAdvance('9') * digits;
		setViewportMargins(width, 0, 0, 0);
		_gutter->setGeometry(0, viewport()->y(), width, viewport()->height());
	}

	void updateCurrentLine() {
		auto line = QTextEdit::ExtraSelection();
		line.format.setBackground(st::windowBgOver->c);
		line.format.setProperty(QTextFormat::FullWidthSelection, true);
		line.cursor = textCursor();
		line.cursor.clearSelection();
		setExtraSelections({ line });
	}

	void paintGutter() {
		auto painter = QPainter(_gutter);
		painter.fillRect(_gutter->rect(), st::windowBg->c);
		painter.setFont(font());
		auto block = firstVisibleBlock();
		while (block.isValid()) {
			const auto top = blockBoundingGeometry(block).translated(contentOffset()).top();
			if (top >= _gutter->height()) {
				break;
			}
			if (block.isVisible()) {
				painter.setPen(block.blockNumber() == textCursor().blockNumber()
					? st::windowActiveTextFg->c : st::windowSubTextFg->c);
				painter.drawText(QRectF(0, top,
					_gutter->width() - st::vimJsonGutterPadding,
					fontMetrics().height()), Qt::AlignRight,
					QString::number(block.blockNumber() + 1));
			}
			block = block.next();
		}
	}

	Ui::RpWidget *_gutter = nullptr;
	JsonHighlighter *_highlighter = nullptr;

};

} // namespace

VimKeymapEditor::VimKeymapEditor(
	QWidget *parent,
	Fn<void(not_null<Ui::VerticalLayout*>)> fillUi,
	Fn<void(Fn<void()>)> confirmDiscard)
: Ui::RpWidget(parent)
, _confirmDiscard(std::move(confirmDiscard)) {
	paintRequest() | rpl::on_next([=] {
		auto painter = QPainter(this);
		painter.fillRect(rect(), st::windowBg->c);
	}, lifetime());
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
	_ui = layout->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		layout, object_ptr<Ui::VerticalLayout>(layout)));
	fillUi(_ui->entity());
	_json = layout->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		layout, object_ptr<Ui::VerticalLayout>(layout)));
	const auto json = _json->entity();
	const auto header = json->add(object_ptr<Ui::RpWidget>(json));
	header->resize(0, st::vimJsonHeaderHeight);
	const auto filename = Ui::CreateChild<Ui::FlatLabel>(
		header, u"vim-keymap.json"_q, st::defaultSubTextLabel);
	filename->move(st::vimJsonPadding, (header->height() - filename->height()) / 2);
	const auto field = json->add(object_ptr<Ui::RpWidget>(json));
	field->resize(0, st::vimJsonEditorHeight);
	const auto editor = new JsonEditor(field);
	_text = editor;
	_text->setObjectName(u"vim-json-editor"_q);
	_text->setAccessibleName(u"Конфигурация Vim JSON"_q);
	style::PaletteChanged() | rpl::on_next([=] {
		editor->refreshPalette();
		updateState();
		update();
	}, lifetime());
	field->sizeValue() | rpl::on_next([=](QSize size) {
		_text->setGeometry(QRect(QPoint(), size));
	}, field->lifetime());
	_status = json->add(object_ptr<Ui::FlatLabel>(json),
		style::margins(st::vimJsonPadding, 0, st::vimJsonPadding, 0));
	_status->setObjectName(u"vim-json-status"_q);
	_status->setBreakEverywhere(true);
	_status->setTextColorOverride(st::boxTextFgError->c);
	const auto footer = json->add(object_ptr<Ui::RpWidget>(json));
	footer->resize(0, st::vimJsonFooterHeight);
	_apply = Ui::CreateChild<Ui::RoundButton>(footer,
		rpl::single(u"Применить"_q), st::vimJsonButton);
	_apply->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_apply->setObjectName(u"vim-json-apply"_q);
	_apply->setClickedCallback([=] { apply(); });
	const auto reread = Ui::CreateChild<Ui::RoundButton>(footer,
		rpl::single(u"Перечитать"_q), st::vimJsonSecondaryButton);
	reread->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	reread->setObjectName(u"vim-json-reload"_q);
	reread->setClickedCallback([=] { checkBeforeClose([=] { reload(); }); });
	footer->sizeValue() | rpl::on_next([=](QSize size) {
		_apply->move(size.width() - st::vimJsonPadding - _apply->width(),
			(size.height() - _apply->height()) / 2);
		reread->move(st::vimJsonPadding, (size.height() - reread->height()) / 2);
	}, footer->lifetime());
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
	_apply->setTextFgOverride(dirty()
		? std::nullopt : std::make_optional(st::windowSubTextFg->c));
	_apply->setBrushOverride(dirty()
		? std::nullopt : std::make_optional(QBrush(st::windowBgOver->c)));
}

void VimKeymapEditor::apply() {
	if (_baseline != Core::VimKeymap::ConfigSnapshot()) {
		_status->setTextColorOverride(st::boxTextFgError->c);
		_status->setText(u"Настройки изменены в другом окне. Перечитайте конфигурацию перед применением."_q);
		return;
	}
	_applying = true;
	const auto error = Core::VimKeymap::ApplyConfig(_text->toPlainText().toUtf8());
	_applying = false;
	if (!error.isEmpty()) {
		_status->setTextColorOverride(st::boxTextFgError->c);
		_status->setText(error);
		return;
	}
	reload();
	_status->setTextColorOverride(st::boxTextFgGood->c);
	_status->setText(u"Настройки применены."_q);
}

} // namespace Settings
