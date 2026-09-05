/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"
#include <QtCore/QJsonObject>

class QPlainTextEdit;

namespace Ui {
class VerticalLayout;
class SettingsSlider;
class RoundButton;
class FlatLabel;
template <typename Widget> class SlideWrap;
} // namespace Ui

namespace Settings {

class VimKeymapEditor final : public Ui::RpWidget {
public:
	VimKeymapEditor(
		QWidget *parent,
		Fn<void(not_null<Ui::VerticalLayout*>)> fillUi,
		Fn<void(Fn<void()>)> confirmDiscard);
	[[nodiscard]] bool dirty() const;
	void checkBeforeClose(Fn<void()> close);

private:
	void chooseMode(int mode);
	void displayMode(int mode);
	void reload();
	void apply();
	void updateState();

	Ui::SettingsSlider *_tabs = nullptr;
	Ui::SlideWrap<Ui::VerticalLayout> *_ui = nullptr;
	Ui::SlideWrap<Ui::VerticalLayout> *_json = nullptr;
	QPlainTextEdit *_text = nullptr;
	Ui::FlatLabel *_status = nullptr;
	Ui::RoundButton *_apply = nullptr;
	Fn<void(Fn<void()>)> _confirmDiscard;
	QJsonObject _baseline;
	QString _cleanText;
	int _mode = 0;
	bool _updatingTabs = false;
	bool _applying = false;

};

} // namespace Settings
