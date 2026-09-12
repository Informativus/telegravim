/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap.h"
#include "core/vim_keymap_options.h"
#include "core/vim_keymap_log.h"

#include "core/vim_keymap_bindings.h"
#include "core/vim_keymap_geometry.h"
#include "base/options.h"
#include "core/vim_keymap_widgets.h"
#include "core/application.h"
#include "core/shortcuts.h"
#include "core/version.h"
#include "lang/lang_keys.h"
#include "mainwidget.h"
#include "ui/abstract_button.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animations.h"
#include "ui/layers/box_layer_widget.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/elastic_scroll.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_vim_keymap.h"

#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QWidget>
#include <QtGui/QKeyEvent>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace Core::VimKeymap {
namespace {

constexpr auto kHoldScrollTickMs = 16;
constexpr auto kHoldScrollStartDelayMs = 90;
constexpr auto kSingleScrollDurationMs = 190;
constexpr auto kTelegraVimBuild = "2026.09.11-103";

bool NormalModeEnabled = false;
bool LegacyDefaultsMigrated = false;

KeyEventLog KeyLog;

struct ActionHandler {
	QPointer<QObject> owner;
	Fn<bool(Action)> handler;
};

std::vector<ActionHandler> ActionHandlers;

struct KeyHandler {
	QPointer<QObject> owner;
	Fn<bool(not_null<QKeyEvent*>)> handler;
	bool acceptShortcutOverride = false;
};

std::vector<KeyHandler> KeyHandlers;
std::vector<KeyHandler> PreLayerKeyHandlers;

struct TextInputPassthroughHandler {
	QPointer<QObject> owner;
	Fn<bool(not_null<QKeyEvent*>)> handler;
};

std::vector<TextInputPassthroughHandler> TextInputPassthroughHandlers;

struct ForcedNormalModeHandler {
	QPointer<QObject> owner;
	Fn<bool()> handler;
};

std::vector<ForcedNormalModeHandler> ForcedNormalModeHandlers;
std::vector<QPointer<QWidget>> ModeIndicatorWidgets;
QPointer<QWidget> LastKeyboardScope;

[[nodiscard]] Qt::KeyboardModifiers CleanModifiers(not_null<QKeyEvent*> e) {
	return e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
}

[[nodiscard]] QString PlainText(not_null<QKeyEvent*> e) {
	return e->text().toCaseFolded();
}

[[nodiscard]] bool ShouldLogIgnoredKey(not_null<QKeyEvent*> e) {
	const auto modifiers = CleanModifiers(e);
	if (modifiers != Qt::NoModifier
		|| e->key() == Qt::Key_Escape
		|| e->key() == Qt::Key_Tab
		|| e->key() == Qt::Key_Backtab
		|| e->key() == Qt::Key_Return
		|| e->key() == Qt::Key_Enter
		|| NormalMode()) {
		return true;
	}
	const auto text = PlainText(e);
	return text.size() == 1
		&& u"hjklfyro?/\u0440\u043E\u043B\u0434\u0430\u043D\u043A\u0449"_q
			.contains(text.front());
}

[[nodiscard]] bool IsModifierOnlyKey(not_null<QKeyEvent*> e) {
	switch (e->key()) {
	case Qt::Key_Control:
	case Qt::Key_Shift:
	case Qt::Key_Meta:
	case Qt::Key_Alt:
	case Qt::Key_AltGr:
		return true;
	default:
		return false;
	}
}

void RefreshModeIndicator();

void RecordKeyEvent(
		not_null<QKeyEvent*> e,
		const QString &status,
		bool force) {
	if (IsModifierOnlyKey(e)) {
		return;
	}
	if (!force && !ShouldLogIgnoredKey(e)) {
		return;
	}
	if (App().passcodeLocked()) {
		KeyLog.clear();
		return;
	}
	KeyLog.record(e, status);
}

[[nodiscard]] bool MatchesBindings(
		base::options::option<QString> &option,
		not_null<QKeyEvent*> e,
		bool allowExtraShift = false,
		bool realMacModifiers = false,
		bool allowMacControlCommandEquivalent = true) {
	return Bindings::Matches(option.value(), e, {
		.allowExtraShift = allowExtraShift,
		.realMacModifiers = realMacModifiers,
		.allowMacControlCommandEquivalent
			= allowMacControlCommandEquivalent,
	});
}

[[nodiscard]] QString BindingLabel(base::options::option<QString> &option) {
	MigrateLegacyDefaults();
	const auto value = option.value().trimmed();
	return value.isEmpty() ? u"-"_q : value;
}

[[nodiscard]] std::optional<Qt::Key> ScrollNavigationKey(not_null<QKeyEvent*> e) {
	if (MatchesBindings(VimKeymapKeyScrollDownOption, e)) {
		return Qt::Key_Down;
	} else if (MatchesBindings(VimKeymapKeyScrollUpOption, e)) {
		return Qt::Key_Up;
	}
	return std::nullopt;
}

[[nodiscard]] bool IsToggleModeKey(not_null<QKeyEvent*> e) {
	MigrateLegacyDefaults();
	return !e->isAutoRepeat()
		&& MatchesBindings(VimKeymapKeyToggleModeOption, e);
}

[[nodiscard]] bool IsPlainEscapeKey(not_null<QKeyEvent*> e) {
	return !e->isAutoRepeat()
		&& e->key() == Qt::Key_Escape
		&& CleanModifiers(e) == Qt::NoModifier;
}

[[nodiscard]] bool IsSearchKey(not_null<QKeyEvent*> e) {
	return MatchesBindings(VimKeymapKeySearchOption, e, true);
}

[[nodiscard]] bool IsGlobalSearchKey(not_null<QKeyEvent*> e) {
	return MatchesBindings(VimKeymapKeyGlobalSearchOption, e, true);
}

[[nodiscard]] bool IsHelpKey(not_null<QKeyEvent*> e) {
	return NormalMode()
		&& MatchesBindings(VimKeymapKeyHelpOption, e, true);
}

[[nodiscard]] bool IsTextInputObject(QObject *object) {
	for (auto current = object; current; current = current->parent()) {
		if (qobject_cast<QLineEdit*>(current)
			|| qobject_cast<QTextEdit*>(current)
			|| dynamic_cast<Ui::InputField*>(current)) {
			return true;
		}
	}
	if (const auto focus = QApplication::focusWidget()) {
		return qobject_cast<QLineEdit*>(focus)
			|| qobject_cast<QTextEdit*>(focus)
			|| dynamic_cast<Ui::InputField*>(focus);
	}
	return false;
}

[[nodiscard]] QWidget *WidgetFromObject(QObject *object) {
	return (object && object->isWidgetType())
		? static_cast<QWidget*>(object)
		: nullptr;
}

[[nodiscard]] bool InsideElasticScroll(QWidget *widget) {
	for (auto current = widget; current; current = current->parentWidget()) {
		if (dynamic_cast<Ui::ElasticScroll*>(current)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool ScrollAreaAvailable(Ui::ScrollArea *area) {
	return area
		&& area->isVisible()
		&& area->isEnabled()
		&& !InsideElasticScroll(area)
		&& area->scrollTopMax() > 0;
}

[[nodiscard]] Ui::ScrollArea *NearestScrollArea(QWidget *start) {
	for (auto current = start; current; current = current->parentWidget()) {
		if (dynamic_cast<Ui::ElasticScroll*>(current)) {
			return nullptr;
		} else if (const auto area = dynamic_cast<Ui::ScrollArea*>(current)) {
			return ScrollAreaAvailable(area) ? area : nullptr;
		}
	}
	return nullptr;
}

[[nodiscard]] Ui::ScrollArea *ContainedScrollArea(QWidget *root) {
	if (!root || !root->isVisible() || InsideElasticScroll(root)) {
		return nullptr;
	}
	auto result = (Ui::ScrollArea*)nullptr;
	const auto inspect = [&](QWidget *widget) {
		const auto area = dynamic_cast<Ui::ScrollArea*>(widget);
		if (ScrollAreaAvailable(area) && !result) {
			result = area;
		}
	};
	inspect(root);
	for (const auto widget : root->findChildren<QWidget*>()) {
		inspect(widget);
	}
	return result;
}

[[nodiscard]] QAbstractScrollArea *NearestQtScrollArea(QWidget *start) {
	for (auto current = start; current; current = current->parentWidget()) {
		if (dynamic_cast<Ui::ElasticScroll*>(current)
			|| dynamic_cast<Ui::ScrollArea*>(current)) {
			return nullptr;
		} else if (const auto area = dynamic_cast<QAbstractScrollArea*>(
				current)) {
			return area;
		}
	}
	return nullptr;
}

struct ScrollAreaAnimation {
	QPointer<Ui::ScrollArea> area;
	Ui::Animations::Simple animation;
	int target = 0;
};

struct ScrollBarAnimation {
	QPointer<QScrollBar> bar;
	Ui::Animations::Simple animation;
	int target = 0;
};

std::vector<std::unique_ptr<ScrollAreaAnimation>> ScrollAreaAnimations;
std::vector<std::unique_ptr<ScrollBarAnimation>> ScrollBarAnimations;

void CleanupScrollAnimations() {
	const auto removeArea = [](const auto &entry) {
		return !entry->area;
	};
	ScrollAreaAnimations.erase(
		std::remove_if(
			begin(ScrollAreaAnimations),
			end(ScrollAreaAnimations),
			removeArea),
		end(ScrollAreaAnimations));

	const auto removeBar = [](const auto &entry) {
		return !entry->bar;
	};
	ScrollBarAnimations.erase(
		std::remove_if(
			begin(ScrollBarAnimations),
			end(ScrollBarAnimations),
			removeBar),
		end(ScrollBarAnimations));
}

[[nodiscard]] ScrollAreaAnimation *AnimationFor(Ui::ScrollArea *area) {
	CleanupScrollAnimations();
	for (const auto &entry : ScrollAreaAnimations) {
		if (entry->area == area) {
			return entry.get();
		}
	}
	auto entry = std::make_unique<ScrollAreaAnimation>();
	entry->area = area;
	entry->target = area->scrollTop();
	const auto result = entry.get();
	ScrollAreaAnimations.push_back(std::move(entry));
	return result;
}

[[nodiscard]] ScrollBarAnimation *AnimationFor(QScrollBar *bar) {
	CleanupScrollAnimations();
	for (const auto &entry : ScrollBarAnimations) {
		if (entry->bar == bar) {
			return entry.get();
		}
	}
	auto entry = std::make_unique<ScrollBarAnimation>();
	entry->bar = bar;
	entry->target = bar->value();
	const auto result = entry.get();
	ScrollBarAnimations.push_back(std::move(entry));
	return result;
}

[[nodiscard]] bool SmoothScrollBy(
		Ui::ScrollArea *area,
		int delta,
		bool autoRepeat) {
	if (!area) {
		return false;
	}
	const auto current = area->scrollTop();
	const auto entry = AnimationFor(area);
	const auto base = (autoRepeat && entry->animation.animating())
		? entry->target
		: current;
	const auto target = std::clamp(base + delta, 0, area->scrollTopMax());
	if (target == current && target == base) {
		return false;
	}
	entry->target = target;
	entry->animation.stop();
	const auto weak = QPointer<Ui::ScrollArea>(area);
	entry->animation.start(
		[weak, entry] {
			if (weak) {
				weak->scrollToY(qRound(entry->animation.value(
					entry->target)));
			}
		},
		current,
		target,
		SingleScrollDurationMs(),
		anim::linear);
	return true;
}

[[nodiscard]] bool SmoothScrollBy(
		QScrollBar *bar,
		int delta,
		bool autoRepeat) {
	if (!bar) {
		return false;
	}
	const auto current = bar->value();
	const auto entry = AnimationFor(bar);
	const auto base = (autoRepeat && entry->animation.animating())
		? entry->target
		: current;
	const auto target = std::clamp(
		base + delta,
		bar->minimum(),
		bar->maximum());
	if (target == current && target == base) {
		return false;
	}
	entry->target = target;
	entry->animation.stop();
	const auto weak = QPointer<QScrollBar>(bar);
	entry->animation.start(
		[weak, entry] {
			if (weak) {
				weak->setValue(qRound(entry->animation.value(entry->target)));
			}
		},
		current,
		target,
		SingleScrollDurationMs(),
		anim::linear);
	return true;
}

[[nodiscard]] bool ScrollGenericArea(
		not_null<QObject*> object,
		not_null<QKeyEvent*> e) {
	const auto navigation = NavigationKey(e);
	if (!navigation) {
		return false;
	}
	const auto direction = (*navigation == Qt::Key_Down) ? 1 : -1;
	const auto delta = ScrollStep();
	const auto objectWidget = WidgetFromObject(object.get());
	const auto focus = QApplication::focusWidget();
	const auto active = QApplication::activeWindow();
	auto candidates = std::vector<QWidget*> {
		objectWidget,
		focus,
		active,
	};
	for (const auto candidate : candidates) {
		if (const auto area = NearestScrollArea(candidate)) {
			if (SmoothScrollBy(area, direction * delta, e->isAutoRepeat())) {
				return true;
			}
		}
	}
	for (const auto candidate : { objectWidget, focus }) {
		if (candidate == active) {
			continue;
		} else if (const auto area = ContainedScrollArea(candidate)) {
			if (SmoothScrollBy(area, direction * delta, e->isAutoRepeat())) {
				return true;
			}
		}
	}
	for (const auto candidate : candidates) {
		if (const auto area = NearestQtScrollArea(candidate)) {
			if (SmoothScrollBy(
					area->verticalScrollBar(),
					direction * delta,
					e->isAutoRepeat())) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] std::optional<Shortcuts::Command> LegacyCommand(
		not_null<QKeyEvent*> e) {
	if (const auto navigation = ChatNavigationKey(e)) {
		if (*navigation == ChatNavigation::Next) {
			return Shortcuts::Command::ChatNext;
		} else {
			return Shortcuts::Command::ChatPrevious;
		}
	} else if (MatchesBindings(VimKeymapKeyNextFolderOption, e)) {
		return Shortcuts::Command::FolderNext;
	} else if (MatchesBindings(VimKeymapKeyPreviousFolderOption, e)) {
		return Shortcuts::Command::FolderPrevious;
	}
	return std::nullopt;
}

[[nodiscard]] bool LaunchLegacyCommand(Shortcuts::Command command) {
	return Shortcuts::Launch(command);
}

[[nodiscard]] bool HandleRegisteredAction(Action action) {
	const auto remove = [](const ActionHandler &handler) {
		return !handler.owner;
	};
	ActionHandlers.erase(
		std::remove_if(begin(ActionHandlers), end(ActionHandlers), remove),
		end(ActionHandlers));
	const auto handlers = ActionHandlers;
	for (auto i = handlers.rbegin(); i != handlers.rend(); ++i) {
		if (i->owner && i->handler(action)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HandleRegisteredKey(
		not_null<QKeyEvent*> e,
		QWidget *scope = nullptr) {
	const auto guardedScope = QPointer<QWidget>(scope);
	const auto handle = [&](std::vector<KeyHandler> &handlers) {
		const auto remove = [](const KeyHandler &handler) {
			return !handler.owner;
		};
		handlers.erase(
			std::remove_if(begin(handlers), end(handlers), remove),
			end(handlers));
		const auto snapshot = handlers;
		for (auto i = snapshot.rbegin(); i != snapshot.rend(); ++i) {
			if (scope && !guardedScope) {
				return true;
			} else if (i->owner
				&& KeyHandlerInScope(i->owner, guardedScope)
				&& i->handler(e)) {
				return true;
			}
		}
		return scope && !guardedScope;
	};
	return handle(KeyHandlers);
}

[[nodiscard]] bool HandlePreLayerKey(
		not_null<QKeyEvent*> e,
		QWidget *scope = nullptr) {
	const auto guardedScope = QPointer<QWidget>(scope);
	const auto remove = [](const KeyHandler &handler) {
		return !handler.owner;
	};
	PreLayerKeyHandlers.erase(
		std::remove_if(
			begin(PreLayerKeyHandlers),
			end(PreLayerKeyHandlers),
			remove),
		end(PreLayerKeyHandlers));
	const auto handlers = PreLayerKeyHandlers;
	for (auto i = handlers.rbegin();
		i != handlers.rend();
		++i) {
		if (e->type() == QEvent::ShortcutOverride
			&& !i->acceptShortcutOverride) {
			continue;
		}
		if (scope && !guardedScope) {
			return true;
		} else if (i->owner
			&& KeyHandlerInScope(i->owner, guardedScope)
			&& i->handler(e)) {
			return true;
		}
	}
	return scope && !guardedScope;
}

[[nodiscard]] bool TextInputPassthroughRequested(not_null<QKeyEvent*> e) {
	const auto remove = [](const TextInputPassthroughHandler &handler) {
		return !handler.owner;
	};
	TextInputPassthroughHandlers.erase(
		std::remove_if(
			begin(TextInputPassthroughHandlers),
			end(TextInputPassthroughHandlers),
			remove),
		end(TextInputPassthroughHandlers));
	const auto handlers = TextInputPassthroughHandlers;
	for (auto i = handlers.rbegin(); i != handlers.rend(); ++i) {
		if (i->owner && i->handler(e)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool ForcedNormalMode() {
	const auto remove = [](const ForcedNormalModeHandler &handler) {
		return !handler.owner;
	};
	ForcedNormalModeHandlers.erase(
		std::remove_if(
			begin(ForcedNormalModeHandlers),
			end(ForcedNormalModeHandlers),
			remove),
		end(ForcedNormalModeHandlers));
	const auto handlers = ForcedNormalModeHandlers;
	for (const auto &handler : handlers) {
		if (handler.owner && handler.handler && handler.handler()) {
			return true;
		}
	}
	return false;
}

void FocusForCurrentMode() {
	if (const auto active = App().activeWindow()) {
		active->widget()->setInnerFocus();
	} else if (const auto primary = App().activePrimaryWindow()) {
		primary->widget()->setInnerFocus();
	}
}

void RefreshModeIndicator() {
	const auto remove = [](const auto &widget) {
		return !widget;
	};
	ModeIndicatorWidgets.erase(
		std::remove_if(
			begin(ModeIndicatorWidgets),
			end(ModeIndicatorWidgets),
			remove),
		end(ModeIndicatorWidgets));
	for (const auto &widget : ModeIndicatorWidgets) {
		if (widget->isVisible()) {
			widget->update();
		}
	}
}

[[nodiscard]] bool CanToggleByEscape() {
	if (QApplication::activeModalWidget()
		|| QApplication::activePopupWidget()) {
		return false;
	}
	const auto active = App().activeWindow();
	if (!active) {
		return false;
	} else if (active->locked() || active->isLayerShown()) {
		return false;
	}
	return active->widget()->isActiveWindow();
}

[[nodiscard]] bool TelegramLayerOrPopupShown() {
	if (QApplication::activeModalWidget()
		|| QApplication::activePopupWidget()) {
		return true;
	}
	const auto active = App().activeWindow();
	return active && (active->locked() || active->isLayerShown());
}

[[nodiscard]] bool TelegramModalLayerShown() {
	if (QApplication::activeModalWidget()) {
		return true;
	}
	const auto active = App().activeWindow();
	return active && (active->locked() || active->isLayerShown());
}

[[nodiscard]] QWidget *ActiveKeyboardScope() {
	if (const auto popup = QApplication::activePopupWidget()) {
		return popup;
	} else if (const auto modal = QApplication::activeModalWidget()) {
		if (const auto nested = FindKeyboardScope(modal)) {
			return nested;
		}
		return modal;
	}
	const auto active = QApplication::activeWindow();
	if (!active) {
		return nullptr;
	}
	const auto main = App().activeWindow();
	if (main && main->widget() == active && !main->isLayerShown()) {
		return nullptr;
	}
	if (const auto layer = FindKeyboardScope(active)) {
		return layer;
	}
	return (main && main->widget() == active) ? nullptr : active;
}

void UpdateKeyboardScope(QWidget *scope) {
	if (LastKeyboardScope == scope) {
		return;
	}
	if (LastKeyboardScope) {
		if (const auto navigation = KeyboardNavigation::Find(LastKeyboardScope)) {
			navigation->clearHints();
		}
	}
	LastKeyboardScope = scope;
	if (scope) {
		if (const auto navigation = KeyboardNavigation::Find(scope)) {
			navigation->restoreFocus();
		}
	}
}

void SetNormalModeValue(bool enabled) {
	const auto was = NormalMode();
	const auto value = enabled;
	if (NormalModeEnabled == value) {
		return;
	}
	NormalModeEnabled = value;
	if (was != NormalMode()) {
		RefreshModeIndicator();
	}
}

[[nodiscard]] bool OpenGlobalSearch() {
	const auto active = App().activeWindow();
	if (!active || active->locked() || active->isLayerShown()) {
		return false;
	}
	const auto controller = active->sessionController();
	return controller && controller->content()->focusDialogsSearch();
}

[[nodiscard]] bool HandleGlobalSearch(not_null<QKeyEvent*> e) {
	if (!GlobalSearchKey(e)) {
		return false;
	}
	if (OpenGlobalSearch()) {
		SetNormalModeValue(false);
		RecordKeyEvent(e, u"global chat search"_q, true);
	} else {
		RecordKeyEvent(e, u"global chat search unavailable"_q, true);
	}
	e->accept();
	return true;
}

[[nodiscard]] bool UseRussianHelp() {
	return Lang::LanguageIdOrDefault(
		Lang::Id()).startsWith(u"ru"_q, Qt::CaseInsensitive);
}

[[nodiscard]] bool UseEnglishHintAlphabet() {
	const auto value = VimKeymapHintAlphabetOption.value()
		.trimmed()
		.toCaseFolded();
	return value == QString::fromLatin1(kHintAlphabetEnglish)
		|| value == u"en"_q;
}

[[nodiscard]] QString EnglishHintAlphabet() {
	return u"asdfghjklqwertyuiopzxcvbnm,."_q;
}

[[nodiscard]] QString RussianHintAlphabet() {
	return u"фывапролдйцукенгшщзячсмитьбю"_q;
}

[[nodiscard]] QString CurrentHintAlphabet() {
	return UseEnglishHintAlphabet()
		? EnglishHintAlphabet()
		: RussianHintAlphabet();
}

struct HelpEntry {
	int section = 0;
	QString keys;
	QString description;
	QString detail;
	QString aliases;
};

[[nodiscard]] std::vector<HelpEntry> HelpEntries(bool russian) {
	auto result = std::vector<HelpEntry>();
	const auto add = [&](int section, QString keys, QString ru, QString en,
			QString ruDetail = {}, QString enDetail = {}) {
		result.push_back({ section, std::move(keys),
			russian ? std::move(ru) : std::move(en),
			russian ? std::move(ruDetail) : std::move(enDetail), {} });
	};
	const auto binding = [&](int section, auto &option, QString ru, QString en,
			QString ruDetail = {}, QString enDetail = {}) {
		if (option.value().trimmed().isEmpty()) {
			return;
		}
		const auto aliases = BindingLabel(option);
		add(section, aliases.section(QChar(','), 0, 0).trimmed(),
			std::move(ru), std::move(en), std::move(ruDetail), std::move(enDetail));
		result.back().aliases = aliases;
	};
	binding(0, VimKeymapKeyToggleModeOption,
		u"Ввод / навигация"_q,
		u"Typing / navigation"_q,
		u"Если открыто окно, меню или фото, Esc сначала закрывает его."_q,
		u"Esc closes an open dialog, menu or photo first."_q);
	binding(0, VimKeymapKeyScrollDownOption,
		u"Прокрутить вниз"_q,
		u"Scroll down"_q);
	binding(0, VimKeymapKeyScrollUpOption,
		u"Прокрутить вверх"_q,
		u"Scroll up"_q);
	add(0, u"Shift+U, Shift+D"_q,
		u"На экран вверх / вниз"_q,
		u"One screen up / down"_q);
	binding(0, VimKeymapKeyJumpBottomOption,
		u"К последним сообщениям"_q,
		u"Jump to the latest messages"_q);
	binding(0, VimKeymapKeyFocusHintsOption,
		u"Выбрать элемент по букве"_q,
		u"Choose a control by its letter"_q,
		u"Медиа, ссылки, опросы, ответы и авторы пересланных сообщений."_q,
		u"Media, links, polls, replies and forwarded authors."_q);
	add(0, u"Ctrl+A → L, Ctrl+A → H"_q,
		u"Правая панель / обратно в чат"_q,
		u"Right pane / back to chat"_q,
		u"В панели: f — метки, j/k — прокрутка, Tab — кнопки, Esc — в чат."_q,
		u"In the pane: f for hints, j/k to scroll, Tab for controls, Esc to return."_q);
	add(0, u"c"_q,
		u"Закрыть элемент по букве"_q,
		u"Close a control by its letter"_q,
		u"Кнопки закрытия и плавающие видеосообщения."_q,
		u"Close buttons and floating round videos."_q);
	add(0, u"f → hint, Space, Enter"_q,
		u"Пауза / пуск видеосообщения"_q,
		u"Pause / resume a round video"_q,
		u"Shift+F и буква кружка — перейти к сообщению."_q,
		u"Shift+F and its hint go to the original message."_q);
	binding(0, VimKeymapKeyHelpOption,
		u"Открыть эту справку"_q,
		u"Open this help"_q);
	binding(1, VimKeymapKeyNextChatOption,
		u"Следующий чат"_q,
		u"Next chat"_q,
		u"Добавьте Shift, чтобы перейти через один чат."_q,
		u"Add Shift to skip one chat."_q);
	binding(1, VimKeymapKeyPreviousChatOption,
		u"Предыдущий чат"_q,
		u"Previous chat"_q,
		u"Добавьте Shift, чтобы перейти через один чат."_q,
		u"Add Shift to skip one chat."_q);
	binding(1, VimKeymapKeyNextFolderOption,
		u"Следующая папка"_q,
		u"Next folder"_q);
	binding(1, VimKeymapKeyPreviousFolderOption,
		u"Предыдущая папка"_q,
		u"Previous folder"_q);
	binding(1, VimKeymapKeyOpenChatHintsOption,
		u"Открыть чат по букве"_q,
		u"Open a chat by its letter"_q);
	binding(1, VimKeymapKeyChatPreviewOption,
		u"Предпросмотр чата по букве"_q,
		u"Preview a chat by its letter"_q,
		u"Без открытия чата и отметки о прочтении."_q,
		u"Without opening the chat or marking it read."_q);
	add(1, u"Ctrl+V"_q,
		u"Предпросмотр выбранного чата"_q,
		u"Preview the selected chat"_q,
		u"В режиме навигации по чатам; не отмечает сообщения прочитанными."_q,
		u"In chat navigation mode; leaves messages unread."_q);
	binding(1, VimKeymapKeySearchOption,
		u"Поиск в режиме навигации"_q,
		u"Search in navigation mode"_q);
	binding(1, VimKeymapKeyGlobalSearchOption,
		u"Открыть поиск из любого режима"_q,
		u"Open search from any mode"_q);
	binding(1, VimKeymapKeyCallOption,
		u"Позвонить / войти в групповой звонок"_q,
		u"Call / join a group call"_q);
	binding(2, VimKeymapKeyCopyMessageOption,
		u"Копировать сообщение или фото"_q,
		u"Copy a message or photo"_q,
		u"В альбоме: своя буква у каждого фото и отдельная — для всех фото сразу."_q,
		u"Albums have a hint for each photo and another for all photos as separate files."_q);
	binding(2, VimKeymapKeyReplyToMessageOption,
		u"Ответить на сообщение по букве"_q,
		u"Reply to a message by its letter"_q);
	binding(2, VimKeymapKeyCancelReplyOption,
		u"Снять активный ответ"_q,
		u"Clear the active reply"_q);
	binding(2, VimKeymapKeyEditMessageOption,
		u"Редактировать своё сообщение"_q,
		u"Edit your message"_q);
	binding(2, VimKeymapKeyCancelEditOption,
		u"Отменить редактирование"_q,
		u"Cancel message editing"_q);
	binding(2, VimKeymapKeyDeleteMessageOption,
		u"Удалить сообщение или фото"_q,
		u"Delete a message or photo"_q,
		u"Выберите отдельное фото или весь альбом, затем подтвердите удаление."_q,
		u"Choose one photo or the whole album, then confirm deletion."_q);
	add(2, u"Shift+R"_q,
		u"Выбрать реакцию"_q,
		u"Choose a reaction"_q,
		u"Нажмите букву сообщения. h/j/k/l или стрелки — выбор, Enter — поставить."_q,
		u"Choose a message hint. h/j/k/l or arrows select; Enter applies."_q);
	add(2, u"Ctrl+F, Tab, Shift+Tab"_q,
		u"Поиск и выбор в реакциях"_q,
		u"Search and browse reactions"_q,
		u"Esc закрывает панель. Мышью: правый клик → эмодзи над меню; стрелка раскрывает остальные."_q,
		u"Esc closes the picker. With a mouse: right-click → emoji above the menu; the arrow reveals more."_q);
	add(2, u"s"_q,
		u"Выделить несколько сообщений"_q,
		u"Select several messages"_q,
		u"Нажмите букву первого сообщения, затем j/k для изменения диапазона."_q,
		u"Choose the first message by its letter, then extend the range with j/k."_q);
	add(2, u"d, f, y, Esc"_q,
		u"Действия с выделенными сообщениями"_q,
		u"Act on selected messages"_q,
		u"Удалить с подтверждением / переслать / копировать / выйти."_q,
		u"Confirm deletion / forward / copy / cancel."_q);
	binding(2, VimKeymapKeySelectMessageTextOption,
		u"Выделить текст сообщения"_q,
		u"Select message text"_q,
		u"В поиске: закрыть поиск и сохранить место в чате."_q,
		u"In search: close search and keep the current chat position."_q);
	add(2, u"gg, G, {, }"_q,
		u"Движение по тексту сообщения"_q,
		u"Move through message text"_q,
		u"Начало / конец / абзацы. Esc — выйти; остальные клавиши сохраняют выделение."_q,
		u"Start / end / paragraphs. Esc exits; other keys preserve the selection."_q);
	add(3, u"i, a, I, A, o, O"_q,
		u"Перейти к вводу текста"_q,
		u"Start typing"_q);
	add(3, u"h j k l"_q,
		u"Двигать курсор"_q,
		u"Move the cursor"_q,
		u"Влево / вниз / вверх / вправо."_q,
		u"Left / down / up / right."_q);
	add(3, u"w, b, e"_q,
		u"Перемещаться по словам"_q,
		u"Move by words"_q);
	add(3, u"0, ^, $, |"_q,
		u"Движение внутри строки"_q,
		u"Move within a line"_q,
		u"Начало, первый символ, конец или колонка."_q,
		u"Start, first character, end or column."_q);
	add(3, u"gg, G, {, }"_q,
		u"Начало, конец и абзацы"_q,
		u"Start, end and paragraphs"_q);
	add(3, u"x, X, dd, D, diw"_q,
		u"Удалять текст"_q,
		u"Delete text"_q);
	add(3, u"yy, Y, yiw, p, P"_q,
		u"Копировать и вставлять текст"_q,
		u"Yank and paste text"_q);
	binding(3, VimKeymapKeyUndoOption,
		u"Отменить изменение текста"_q,
		u"Undo a text change"_q);
	binding(3, VimKeymapKeyRedoOption,
		u"Повторить изменение текста"_q,
		u"Redo a text change"_q);
	add(3, u"c, cw, cc, C, ciw"_q,
		u"Заменять текст с переходом ко вводу"_q,
		u"Change text and start typing"_q);
	add(3, u"s, S, r<char>, J, ~"_q,
		u"Другие команды правки"_q,
		u"More editing commands"_q,
		u"Замена символа или строки, объединение строк, смена регистра."_q,
		u"Substitute a character or line, join lines, change case."_q);
	add(3, u"v, V"_q,
		u"Выделять текст посимвольно / построчно"_q,
		u"Select characters / lines"_q,
		u"y — копировать, d или x — удалить."_q,
		u"y copies; d or x deletes."_q);
	add(3, u"Ctrl+E"_q,
		u"Исправить слово под курсором"_q,
		u"Spelling suggestions at the cursor"_q);
	add(3, u"Option+H, Option+L"_q,
		u"Двигать курсор во время ввода"_q,
		u"Move the cursor while typing"_q,
		u"На macOS — на один символ влево / вправо."_q,
		u"On macOS: one character left / right."_q);
	binding(4, VimKeymapKeyEmojiPanelOption,
		u"Открыть панель"_q,
		u"Open the picker"_q);
	binding(4, VimKeymapKeyFocusEmojiOption,
		u"Открыть панель и перейти к выбору"_q,
		u"Open and focus the picker"_q);
	binding(4, VimKeymapKeyFocusChatOption,
		u"Вернуться в чат или к подписи фото"_q,
		u"Return to the chat or photo caption"_q);
	add(4, u"h j k l"_q,
		u"Выбрать эмодзи, стикер или GIF"_q,
		u"Choose an emoji, sticker or GIF"_q,
		u"Работает и в русской раскладке: р о л д."_q,
		u"Also works in the Russian layout: р о л д."_q);
	add(4, u"Enter, Space"_q,
		u"Вставить эмодзи / отправить стикер или GIF"_q,
		u"Insert an emoji / send a sticker or GIF"_q,
		u"В окне с фотографиями эмодзи вставляется в подпись; фотографии не отправляются."_q,
		u"In the photo preview, emoji go into the caption; photos remain unsent."_q);
	add(4, u"Tab, Shift+Tab"_q,
		u"Переключить раздел панели"_q,
		u"Switch picker tabs"_q,
		u"В окне с фотографиями — между поиском и сеткой."_q,
		u"In the photo preview: switch between search and the grid."_q);
	add(4, u"Ctrl+J, Ctrl+K"_q,
		u"Прокрутить панель / выйти из поиска к сетке"_q,
		u"Scroll the picker / leave search for the grid"_q);
	add(4, u"Ctrl+F"_q,
		u"Поиск внутри панели"_q,
		u"Search inside the picker"_q,
		u"В поле поиска j/k вводят буквы."_q,
		u"In search, j/k type letters."_q);
	add(4, u"Esc"_q,
		u"Вернуться из поиска / закрыть панель"_q,
		u"Leave search / close the picker"_q);
	add(3, u"Settings"_q,
		u"Настройка клавиш и курсора"_q, u"Customize keys and cursor"_q,
		u"Settings → Vim keymap. Основная комбинация показана в строке; все варианты — при наведении."_q,
		u"Settings → Vim keymap. Rows show the primary shortcut; hover to see every binding."_q);
	return result;
}

class HelpShortcutRow final : public Ui::RpWidget {
public:
	HelpShortcutRow(QWidget *parent, HelpEntry entry)
	: Ui::RpWidget(parent)
	, _entry(std::move(entry))
	, _title(Ui::CreateChild<Ui::FlatLabel>(this, _entry.description, st::vimHelpRowTitle))
	, _detail(Ui::CreateChild<Ui::FlatLabel>(this, _entry.detail, st::vimHelpRowDetail)) {
		_title->setSelectable(true);
		_detail->setSelectable(true);
		setToolTip(_entry.aliases.isEmpty() ? _entry.keys : _entry.aliases);
	}

	QAccessible::Role accessibilityRole() override {
		return QAccessible::Role::StaticText;
	}

	QString accessibilityName() override {
		return _entry.keys + u": "_q + _entry.description + u". "_q + _entry.detail;
	}

	int resizeGetHeight(int newWidth) override {
		const auto column = std::min(st::vimHelpKeyColumn, newWidth / 3);
		const auto left = column + st::vimHelpColumnGap;
		const auto top = st::vimHelpRowPadding;
		_title->resizeToWidth(std::max(1, newWidth - left));
		_detail->resizeToWidth(std::max(1, newWidth - left));
		_title->moveToLeft(left, top, newWidth);
		_detail->moveToLeft(left, top + _title->height() + st::vimHelpDetailGap, newWidth);
		auto x = 0;
		auto y = top;
		_badges.clear();
		for (const auto &key : _entry.keys.split(QChar(','), Qt::SkipEmptyParts)) {
			const auto text = key.trimmed();
			const auto w = std::min(column,
				st::vimHelpKeyFont->width(text) + 2 * st::vimHelpKeyPadding);
			if (x && x + w > column) {
				x = 0;
				y += st::vimHelpKeyHeight + st::vimHelpKeyGap;
			}
			_badges.push_back({ QRect(x, y, w, st::vimHelpKeyHeight), text });
			x += w + st::vimHelpKeyGap;
		}
		const auto textHeight = _title->height() + (_entry.detail.isEmpty()
			? 0 : st::vimHelpDetailGap + _detail->height());
		return top + std::max(textHeight, y - top + st::vimHelpKeyHeight) + top;
	}

protected:
	void paintEvent(QPaintEvent *event) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setFont(st::vimHelpKeyFont);
		for (const auto &badge : _badges) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(badge.rect, st::vimHelpKeyRadius, st::vimHelpKeyRadius);
			p.setPen(st::windowFg);
			const auto available = badge.rect.width() - 2 * st::vimHelpKeyPadding;
			p.drawText(badge.rect, Qt::AlignCenter,
				st::vimHelpKeyFont->elided(badge.text, available));
		}
		p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::boxDividerBg);
	}

private:
	struct Badge {
		QRect rect;
		QString text;
	};

	HelpEntry _entry;
	not_null<Ui::FlatLabel*> _title;
	not_null<Ui::FlatLabel*> _detail;
	std::vector<Badge> _badges;

};

class HelpTabsWidget final : public Ui::RpWidget {
public:
	HelpTabsWidget(
		QWidget *parent,
		std::vector<HelpEntry> entries,
		bool russian,
		not_null<Ui::InputField*> search,
		not_null<Ui::SettingsSlider*> slider,
		not_null<Ui::GenericBox*> box)
	: Ui::RpWidget(parent)
	, _entries(std::move(entries))
	, _russian(russian)
	, _search(search)
	, _slider(slider)
	, _box(box)
	, _content(Ui::CreateChild<Ui::VerticalLayout>(this)) {
		setFocusPolicy(Qt::StrongFocus);
		for (auto i = 0; i != kTabsCount; ++i) {
			_slider->addSection(tabTitle(i));
		}
		_slider->setActiveSectionFast(_activeTab);
		_slider->sectionActivated() | rpl::on_next([=](int index) {
			setActiveTab(index);
		}, lifetime());
		_search->changes() | rpl::on_next([=] {
			updateContent();
			_box->scrollTo({ 0, 0 }, anim::type::instant);
		}, lifetime());
		_search->cancelled() | rpl::on_next([=] {
			cancelSearch();
		}, lifetime());
		_search->submits() | rpl::on_next([=] {
			setFocus(Qt::ShortcutFocusReason);
		}, lifetime());
		updateContent();
	}

	QAccessible::Role accessibilityRole() override {
		return QAccessible::Role::Pane;
	}

	QString accessibilityName() override {
		return tabTitle(_activeTab);
	}

	bool handleSearchEscape(not_null<QKeyEvent*> e) {
		const auto focus = QApplication::focusWidget();
		if (!Bindings::IsPlainEscape(e)
			|| !isVisible()
			|| !window()->isActiveWindow()
			|| !focus
			|| (focus != _search && !_search->isAncestorOf(focus))) {
			return false;
		}
		if (e->type() != QEvent::ShortcutOverride && !e->isAutoRepeat()) {
			cancelSearch();
		}
		return true;
	}

	bool handleBoxKey(
			not_null<Ui::GenericBox*> box,
			not_null<QKeyEvent*> e) {
		const auto modifiers = CleanModifiers(e);
		const auto text = e->text().toCaseFolded();
		const auto focus = QApplication::focusWidget();
		const auto searching = focus
			&& (focus == _search || _search->isAncestorOf(focus));
		if (e->matches(QKeySequence::Find)
			|| (!searching && modifiers == Qt::NoModifier && text == u"/"_q)) {
			_search->setFocus();
			e->accept();
			return true;
		} else if (searching) {
			return false;
		}
		const auto previousTab = (e->key() == Qt::Key_Backtab)
			|| ((e->key() == Qt::Key_Tab)
				&& (modifiers == Qt::ShiftModifier))
			|| ((modifiers == Qt::NoModifier)
				&& ((e->key() == Qt::Key_Left)
					|| (text == u"h"_q)
					|| (text == u"\u0440"_q)));
		const auto nextTab = ((e->key() == Qt::Key_Tab)
				&& (modifiers == Qt::NoModifier))
			|| ((modifiers == Qt::NoModifier)
				&& ((e->key() == Qt::Key_Right)
					|| (text == u"l"_q)
					|| (text == u"\u0434"_q)));
		if (previousTab || nextTab) {
			RecordKeyEvent(
				e,
				previousTab ? u"help previous tab"_q : u"help next tab"_q,
				true);
			setActiveTab(_activeTab + (previousTab ? -1 : 1));
			e->accept();
			return true;
		}
		const auto plainScrollDirection = (modifiers == Qt::NoModifier)
			? ((text == u"j"_q || text == u"\u043E"_q)
				? 1
				: (text == u"k"_q || text == u"\u043B"_q)
				? -1
				: 0)
			: 0;
		const auto scrollDirection = plainScrollDirection
			? plainScrollDirection
			: MatchesBindings(
				VimKeymapKeyScrollDownOption,
				e)
			? 1
			: MatchesBindings(VimKeymapKeyScrollUpOption, e)
			? -1
			: 0;
		if (scrollDirection) {
			const auto target = box->scrollTop()
				+ scrollDirection * ScrollStep();
			box->scrollTo(
				{ target, target + box->scrollHeight() },
				anim::type::normal);
			RecordKeyEvent(
				e,
				(scrollDirection > 0)
					? u"help scroll down"_q
					: u"help scroll up"_q,
				true);
			if (_activeTab == kLogTab) {
				updateContent();
			}
			e->accept();
			return true;
		}
		return false;
	}

	int resizeGetHeight(int newWidth) override {
		_content->resizeToWidth(newWidth);
		return _content->height();
	}

protected:
	void keyPressEvent(QKeyEvent *e) override {
		for (auto current = parentWidget(); current; current = current->parentWidget()) {
			if (const auto box = dynamic_cast<Ui::GenericBox*>(current)) {
				if (handleBoxKey(not_null{ box }, not_null{ e })) {
					return;
				}
				break;
			}
		}
		Ui::RpWidget::keyPressEvent(e);
	}

public:
	bool handleGlobalKey(
			not_null<Ui::GenericBox*> box,
			not_null<QKeyEvent*> e) {
		const auto top = box->window();
		if (!top || !top->isActiveWindow() || !isVisibleTo(top)) {
			return false;
		}
		return handleBoxKey(box, e);
	}

private:
	static constexpr auto kTabsCount = 6;
	static constexpr auto kLogTab = kTabsCount - 1;

	void cancelSearch() {
		if (_search->getLastText().isEmpty()) {
			setFocus(Qt::ShortcutFocusReason);
		} else {
			_search->setText(QString());
		}
	}

	[[nodiscard]] QString tabTitle(int index) const {
		const auto ru = std::array{
			u"Основное"_q, u"Чаты"_q, u"Сообщения"_q,
			u"Текст"_q, u"Эмодзи"_q, u"Лог"_q };
		const auto en = std::array{
			u"Basics"_q, u"Chats"_q, u"Messages"_q,
			u"Text"_q, u"Emoji"_q, u"Key log"_q };
		return _russian ? ru[index] : en[index];
	}

	[[nodiscard]] QString sectionNote() const {
		const auto ru = std::array{
			u"Выбор сообщений, прокрутка и элементы интерфейса в режиме навигации."_q,
			u"Переключение чатов, папок и поиск доступны и во время ввода текста."_q,
			u"Нажмите команду, затем букву нужного сообщения или фотографии."_q,
			u"Команды редактора работают в режиме навигации внутри поля сообщения."_q,
			u"Управление панелью эмодзи, стикеров и GIF, в том числе над фотографиями."_q,
			u"История команд для диагностики. Вводимый текст скрыт."_q };
		const auto en = std::array{
			u"Navigate messages, scroll the chat and choose controls in navigation mode."_q,
			u"Switch chats, folders and search even while typing a message."_q,
			u"Press a command, then the hint letter on a message or photo."_q,
			u"Editor commands work in navigation mode inside the message field."_q,
			u"Control emoji, stickers and GIFs, including the picker above photo attachments."_q,
			u"Recent commands for troubleshooting. Typed text stays hidden."_q };
		return _russian ? ru[_activeTab] : en[_activeTab];
	}

	void setActiveTab(int tab) {
		const auto value = (tab % kTabsCount + kTabsCount) % kTabsCount;
		if (_activeTab == value) {
			return;
		}
		_activeTab = value;
		_slider->setActiveSectionFast(_activeTab);
		_search->setText(QString());
		updateContent();
		_box->scrollTo({ 0, 0 }, anim::type::instant);
		accessibilityNameChanged();
	}

	void updateContent() {
		_content->clear();
		const auto words = _search->getLastText().simplified().split(
			QChar(' '), Qt::SkipEmptyParts);
		const auto matches = [&](const QString &text) {
			return ranges::all_of(words, [&](const auto &word) {
				return text.contains(word, Qt::CaseInsensitive);
			});
		};
		if (_activeTab == kLogTab) {
			auto lines = keyLogText().split(QChar('\n'));
			lines.removeIf([&](const auto &line) { return !matches(line); });
			_content->add(object_ptr<Ui::FlatLabel>(
				_content, lines.join(QChar('\n')), st::vimHelpRowDetail))->setSelectable(true);
		} else {
			if (words.empty()) {
				_content->add(object_ptr<Ui::FlatLabel>(
					_content, sectionNote(), st::vimHelpRowDetail), st::vimHelpNotePadding);
			}
			auto section = -1;
			auto count = 0;
			for (const auto &entry : _entries) {
				if (words.empty() ? entry.section != _activeTab
					: !matches(entry.keys + u" "_q + entry.aliases + u" "_q
						+ entry.description + u" "_q + entry.detail + u" "_q
						+ tabTitle(entry.section))) {
					continue;
				}
				if (!words.empty() && section != entry.section) {
					section = entry.section;
					_content->add(object_ptr<Ui::FlatLabel>(
						_content, tabTitle(section), st::vimHelpSectionTitle), st::vimHelpNotePadding);
				}
				_content->add(object_ptr<HelpShortcutRow>(_content, entry));
				++count;
			}
			if (!count) {
				_content->add(object_ptr<Ui::FlatLabel>(_content, _russian
					? u"Ничего не найдено. Попробуйте название действия или клавишу."_q
					: u"No matches. Try an action name or a key."_q,
					st::vimHelpRowDetail), st::vimHelpNotePadding);
			}
		}
		resizeToWidth(std::max(1, width()));
	}

	[[nodiscard]] QString keyLogText() const {
		auto result = QString();
		if (_russian) {
			result += u"Последние "_q
				+ QString::number(KeyEventLog::kLimit)
				+ u" клавиш:\n"_q;
			result += u"Самое свежее нажатие находится внизу.\n\n"_q;
			if (const auto recent = RecentKeyLogText(); !recent.isEmpty()) {
				result += recent;
			} else {
				result += u"Лог пока пуст."_q;
			}
		} else {
			result += u"Last "_q
				+ QString::number(KeyEventLog::kLimit)
				+ u" keys:\n"_q;
			result += u"The latest key is at the bottom.\n\n"_q;
			if (const auto recent = RecentKeyLogText(); !recent.isEmpty()) {
				result += recent;
			} else {
				result += u"The log is empty."_q;
			}
		}
		return result;
	}

	std::vector<HelpEntry> _entries;
	bool _russian = false;
	not_null<Ui::InputField*> _search;
	not_null<Ui::SettingsSlider*> _slider;
	not_null<Ui::GenericBox*> _box;
	not_null<Ui::VerticalLayout*> _content;
	int _activeTab = 0;

};

void ShowHelpBox() {
	const auto active = App().activeWindow();
	const auto window = active ? active : App().activePrimaryWindow();
	if (!window) {
		return;
	}
	const auto russian = UseRussianHelp();
	const auto entries = HelpEntries(russian);
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(russian ? u"Горячие клавиши"_q : u"Keyboard shortcuts"_q);
		box->setAdditionalTitle(rpl::single(QString::fromLatin1(kTelegraVimBuild)));
		box->setWidth(st::vimHelpWidth);
		box->setMaxHeight(st::vimHelpMaxHeight);
		const auto top = box->setPinnedToTopContent(object_ptr<Ui::VerticalLayout>(box));
		const auto search = top->add(object_ptr<Ui::InputField>(top,
			st::defaultInputField, rpl::single(russian
				? u"Найти команду или клавишу…"_q : u"Find an action or a key…"_q)),
			st::boxRowPadding);
		const auto slider = top->add(object_ptr<Ui::SettingsSlider>(top, st::settingsSlider),
			st::boxRowPadding);
		const auto tabs = box->addRow(object_ptr<HelpTabsWidget>(
			box.get(), entries, russian, search, slider, box));
		RegisterKeyHandler(tabs, [=](not_null<QKeyEvent*> e) {
			return tabs->handleGlobalKey(box, e);
		});
		RegisterPreLayerKeyHandler(tabs, [=](not_null<QKeyEvent*> e) {
			return tabs->handleSearchEscape(e)
				|| (e->type() == QEvent::KeyPress
					&& !Ui::InFocusChain(search)
					&& tabs->handleGlobalKey(box, e));
		}, true);
		box->setFocusCallback([=] { tabs->setFocus(Qt::ShortcutFocusReason); });
		box->setShowFinishedCallback([=] { tabs->setFocus(Qt::ShortcutFocusReason); });
		const auto bottom = box->setPinnedToBottomContent(object_ptr<Ui::VerticalLayout>(box));
		bottom->add(object_ptr<Ui::FlatLabel>(bottom, russian
			? u"h / l  разделы    ·    j / k  прокрутка    ·    /  поиск"_q
			: u"h / l  sections    ·    j / k  scroll    ·    /  search"_q,
			st::vimHelpRowDetail), st::boxRowPadding);
		box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
	}));
}

} // namespace

void TraceKey(not_null<QKeyEvent*> e, const QString &status) {
	RecordKeyEvent(e, status, true);
}

QString RecentKeyLogText() {
	return App().passcodeLocked() ? QString() : KeyLog.text();
}

void ClearKeyLog() {
	KeyLog.clear();
}

QString NormalizeBindingToken(QString value) {
	value = value.trimmed().toCaseFolded();
	value.remove(QChar(' '));
	return value;
}

bool IsLegacyCtrlEToken(const QString &token) {
	return token == u"ctrl+e"_q
		|| token == u"cmd+e"_q
		|| token == u"command+e"_q
		|| token == u"meta+e"_q
		|| token == u"ctrl+\u0443"_q;
}

bool IsLegacyDeleteMessageDefault(const QString &bindings) {
	auto value = NormalizeBindingToken(bindings);
	return value == u"ctrl+d,ctrl+\u0432"_q
		|| value == u"ctrl+\u0432,ctrl+d"_q;
}

bool IsLegacyGlobalSearchDefault(const QString &bindings) {
	const auto value = NormalizeBindingToken(bindings);
	return value == u"ctrl+f,ctrl+\u0430"_q
		|| value == u"ctrl+\u0430,ctrl+f"_q;
}

QString WithoutLegacyCtrlE(const QString &bindings) {
	auto result = QStringList();
	const auto parts = bindings.split(',', Qt::SkipEmptyParts);
	for (const auto &part : parts) {
		if (!IsLegacyCtrlEToken(NormalizeBindingToken(part))) {
			result.push_back(part.trimmed());
		}
	}
	return result.join(u", "_q);
}

bool Enabled() {
	MigrateLegacyDefaults();
	return VimKeymapOption.value();
}

bool NormalMode() {
	MigrateLegacyDefaults();
	return Enabled() && (NormalModeEnabled || ForcedNormalMode());
}

bool EscapeClosesComposer() {
	MigrateLegacyDefaults();
	return Enabled() && VimKeymapEscapeClosesComposerOption.value();
}

void MigrateLegacyDefaults() {
	if (LegacyDefaultsMigrated || VimKeymapDefaultsMigratedOption.value()) {
		return;
	}
	LegacyDefaultsMigrated = true;
	VimKeymapDefaultsMigratedOption.set(true);

	auto value = NormalizeBindingToken(VimKeymapKeyToggleModeOption.value());
	if (IsLegacyCtrlEToken(value)) {
		VimKeymapKeyToggleModeOption.set(u"Esc"_q);
	}
	const auto emojiPanel = VimKeymapKeyEmojiPanelOption.value();
	const auto emojiPanelWithoutCtrlE = WithoutLegacyCtrlE(emojiPanel);
	if (emojiPanelWithoutCtrlE != emojiPanel.trimmed()) {
		VimKeymapKeyEmojiPanelOption.set(emojiPanelWithoutCtrlE);
	}
	const auto cancelEdit = VimKeymapKeyCancelEditOption.value();
	const auto cancelEditWithoutCtrlE = WithoutLegacyCtrlE(cancelEdit);
	if (cancelEditWithoutCtrlE != cancelEdit.trimmed()) {
		VimKeymapKeyCancelEditOption.set(cancelEditWithoutCtrlE);
	}
	if (IsLegacyDeleteMessageDefault(
			VimKeymapKeyDeleteMessageOption.value())) {
		VimKeymapKeyDeleteMessageOption.set(u"d, \u0432"_q);
	}
	if (VimKeymapScrollStepOption.value() == kLegacyScrollStepDefault) {
		VimKeymapScrollStepOption.set(kScrollStepDefault);
	}
	if (IsLegacyGlobalSearchDefault(VimKeymapKeyGlobalSearchOption.value())) {
		VimKeymapKeyGlobalSearchOption.set(
			u"Ctrl+F, Ctrl+\u0430, Cmd+Shift+F, "
			u"Cmd+Shift+\u0430, Ctrl+Shift+F, Ctrl+Shift+\u0430"_q);
	}
}

void SetNormalMode(bool enabled) {
	SetNormalModeValue(enabled);
}

int ScrollStep() {
	return std::clamp(
		VimKeymapScrollStepOption.value(),
		kScrollStepMin,
		kScrollStepMax);
}

int HoldScrollSpeed() {
	return std::clamp(
		VimKeymapHoldScrollSpeedOption.value(),
		kHoldScrollSpeedMin,
		kHoldScrollSpeedMax);
}

int HintSize() {
	return std::clamp(
		VimKeymapHintSizeOption.value(),
		kHintSizeMin,
		kHintSizeMax);
}

QString ComposeCursorStyle() {
	const auto value = VimKeymapComposeCursorStyleOption.value()
		.trimmed()
		.toCaseFolded();
	if (value == QString::fromLatin1(kComposeCursorStyleBar)
		|| value == QString::fromLatin1(kComposeCursorStyleUnderline)) {
		return value;
	}
	return QString::fromLatin1(kComposeCursorStyleBlock);
}

int ComposeCursorWidth() {
	return std::clamp(
		VimKeymapComposeCursorWidthOption.value(),
		kComposeCursorWidthMin,
		kComposeCursorWidthMax);
}

int ComposeCursorHeight() {
	return std::clamp(
		VimKeymapComposeCursorHeightOption.value(),
		kComposeCursorHeightMin,
		kComposeCursorHeightMax);
}

int ComposeCursorBlink() {
	return std::clamp(
		VimKeymapComposeCursorBlinkOption.value(),
		kComposeCursorBlinkMin,
		kComposeCursorBlinkMax);
}

int HoldScrollTickMs() {
	return kHoldScrollTickMs;
}

int HoldScrollStartDelayMs() {
	return kHoldScrollStartDelayMs;
}

int HoldScrollDelta() {
	return std::max(1, HoldScrollSpeed() * kHoldScrollTickMs / 1000);
}

int SingleScrollDurationMs() {
	return kSingleScrollDurationMs;
}

bool HandleApplicationShortcutOverride(not_null<QKeyEvent*> e) {
	const auto active = App().activeWindow();
	return Enabled()
		&& !App().passcodeLocked()
		&& (!active || !active->locked())
		&& !ActiveKeyboardScope()
		&& HandlePreLayerKey(e);
}

bool HandleApplicationKeyPress(
		not_null<QObject*> object,
		not_null<QKeyEvent*> e) {
	const auto active = App().activeWindow();
	if (App().passcodeLocked() || (active && active->locked())) {
		KeyLog.clear();
		return false;
	}
	if (!Enabled()) {
		return false;
	}
	const auto wasSuppressed = KeyLog.suppressed();
	KeyLog.setSuppressed(wasSuppressed || KeyboardInputActive(object));
	const auto restoreLogging = gsl::finally([&] {
		KeyLog.setSuppressed(wasSuppressed);
	});
	if (IsModifierOnlyKey(e)) {
		return false;
	}
	const auto scope = QPointer<QWidget>(ActiveKeyboardScope());
	UpdateKeyboardScope(scope);
	if (!scope && HandlePreLayerKey(e)) {
		RecordKeyEvent(e, u"pre-layer handler"_q, true);
		e->accept();
		return true;
	}
	const auto focus = QApplication::focusWidget();
	const auto hintScope = scope ? scope.data() : QApplication::activeWindow();
	if (hintScope) {
		if (const auto navigation = KeyboardNavigation::Find(hintScope)) {
			if (navigation->handleHintKey(e, HintInput(e))) {
				e->accept();
				return true;
			}
		}
	}
	const auto interfaceHints = [&] {
		if (!hintScope
			|| (!scope && (!NormalMode() || TextInputPassthroughRequested(e)))
			|| (scope && KeyboardScopeHasTextInput(scope, object))) {
			return false;
		}
		const auto close = Bindings::IsCloseHints(e);
		if (!close && !Bindings::IsShowMessageHints(e)) {
			return false;
		}
		if (!e->isAutoRepeat()) {
			KeyboardNavigation::Get(hintScope)->showHints(
				HintLabel,
				QFont(u"Menlo"_q, HintSize(), QFont::DemiBold),
				st::vimHintPadding,
				st::vimHintGap,
				close ? KeyboardHintMode::Close : KeyboardHintMode::ShowMessage);
		}
		e->accept();
		return true;
	};
	if (!scope && interfaceHints()) {
		return true;
	}

	const auto globalRoot = scope ? nullptr : GlobalFocusRoot(focus);
	if (globalRoot) {
		const auto navigation = KeyboardNavigation::Find(globalRoot);
		if (!navigation || !navigation->hasHints()) {
			if (CleanModifiers(e) == Qt::NoModifier
				&& (e->key() == Qt::Key_Return
					|| e->key() == Qt::Key_Enter
					|| e->key() == Qt::Key_Space)
				&& ActivateKeyboardHintTarget(focus, e->isAutoRepeat())) {
				e->accept();
				return true;
			} else if (const auto delta = Bindings::TabNavigationDelta(e)) {
				KeyboardNavigation::Get(globalRoot)->focusNext(delta > 0);
				e->accept();
				return true;
			} else if (dynamic_cast<Ui::AbstractButton*>(focus)
				&& CleanModifiers(e) == Qt::NoModifier
				&& (e->key() == Qt::Key_Return
					|| e->key() == Qt::Key_Enter
					|| e->key() == Qt::Key_Space)) {
				return false;
			}
		}
	}
	const auto controlScope = scope ? scope.data()
		: globalRoot ? globalRoot : QApplication::activeWindow();
	if (!scope && controlScope && HandleKeyboardControlKey(controlScope, e)) {
		e->accept();
		return true;
	}
	if (scope) {
		if (const auto navigation = KeyboardNavigation::Find(scope)) {
			if (navigation->handleHintKey(e, HintInput(e))) {
				e->accept();
				return true;
			}
		}
		if (HandleKeyboardControlKey(scope, e)) {
			e->accept();
			return true;
		}
		if (e->key() == Qt::Key_Escape
			&& CleanModifiers(e) == Qt::NoModifier
			&& e->isAutoRepeat()) {
			e->accept();
			return true;
		}
		const auto input = KeyboardScopeHasTextInput(scope, object);
		if (!QApplication::activePopupWidget()
			&& (!input || Bindings::IsPlainEscape(e))
			&& HandlePreLayerKey(e, scope)) {
			e->accept();
			return true;
		}
		if (const auto delta = Bindings::TabNavigationDelta(e)) {
			if (!dynamic_cast<Ui::PopupMenu*>(scope.data())) {
				FocusModalNextPrevChild(scope, delta > 0);
				e->accept();
				return true;
			} else if (HandleRegisteredKey(e, scope)) {
				e->accept();
				return true;
			}
			return false;
		}
		if (Bindings::IsPlainEscape(e)) {
			if ((object == scope || !KeyHandlerInScope(object, scope))
				&& CloseKeyboardScope(scope)) {
				e->accept();
				return true;
			}
			return false;
		}
		if (input) {
			return false;
		}
		if (interfaceHints()) {
			return true;
		}
		const auto focus = QApplication::focusWidget();
		if (KeyHandlerInScope(focus, scope)
			&& (dynamic_cast<Ui::AbstractButton*>(focus)
				|| dynamic_cast<Ui::FlatLabel*>(focus))
			&& CleanModifiers(e) == Qt::NoModifier
			&& (e->key() == Qt::Key_Return
				|| e->key() == Qt::Key_Enter
				|| e->key() == Qt::Key_Space)) {
			return false;
		}
		if (HandleRegisteredKey(e, scope)) {
			e->accept();
			return true;
		}
		if (FocusHintsKey(e) && !e->isAutoRepeat()) {
			KeyboardNavigation::Get(scope)->showHints(
				HintLabel,
				QFont(u"Menlo"_q, HintSize(), QFont::DemiBold),
				st::vimHintPadding,
				st::vimHintGap);
			e->accept();
			return true;
		}
		if (!QApplication::activePopupWidget()) {
			if (const auto navigation = ScrollNavigationKey(e)) {
				const auto down = *navigation == Qt::Key_Down;
				const auto handled = KeyboardNavigation::Get(scope)->scroll(
					(down ? 1 : -1) * ScrollStep(),
					e->isAutoRepeat(),
					SingleScrollDurationMs());
				if (!handled) {
					return false;
				}
				e->accept();
				return true;
			}
		}
		return false;
	}
	if (Bindings::IsPlainEscape(e)
		&& TelegramLayerOrPopupShown()) {
		RecordKeyEvent(e, u"Telegram layer/popup"_q, true);
		return false;
	}
	const auto logGeneration = KeyLog.generation();
	if (HandleRegisteredKey(e)) {
		if (KeyLog.generation() == logGeneration) {
			RecordKeyEvent(e, u"registered handler"_q, true);
		}
		e->accept();
		return true;
	}
	if (TextInputPassthroughRequested(e)) {
		RecordKeyEvent(e, u"text input passthrough"_q, false);
		return false;
	}
	if (TelegramModalLayerShown()) {
		RecordKeyEvent(e, u"Telegram layer"_q, false);
		return false;
	}
	const auto forcedNormalMode = ForcedNormalMode();
	if (const auto action = ActionKey(e)) {
		if (HandleRegisteredAction(*action)) {
			RecordKeyEvent(e, u"action handler"_q, true);
			e->accept();
			return true;
		}
		RecordKeyEvent(e, u"action without target"_q, true);
		e->accept();
		return true;
	}
	if (forcedNormalMode && IsTextInputObject(object.get())) {
		RecordKeyEvent(e, u"text input in forced view mode"_q, false);
		return false;
	} else if (IsToggleModeKey(e) && CanToggleByEscape()) {
		if (forcedNormalMode) {
			FocusForCurrentMode();
			RecordKeyEvent(e, u"focus current view mode"_q, true);
			e->accept();
			return true;
		}
		SetNormalModeValue(!NormalModeEnabled);
		FocusForCurrentMode();
		RecordKeyEvent(
			e,
			NormalModeEnabled ? u"navigation mode"_q : u"input mode"_q,
			true);
		e->accept();
		return true;
	} else if (HandleGlobalSearch(e)) {
		return true;
	} else if (const auto command = LegacyCommand(e)) {
		if (LaunchLegacyCommand(*command)) {
			if (*command == Shortcuts::Command::Search) {
				SetNormalModeValue(false);
			}
			RecordKeyEvent(e, u"Telegram shortcut"_q, true);
			e->accept();
			return true;
		}
	} else if (!NormalModeEnabled && !forcedNormalMode) {
		RecordKeyEvent(e, u"input mode ignored"_q, false);
		return false;
	} else if (HandleHelp(e) || HandleSearch(e)) {
		return true;
	} else if (ScrollGenericArea(object, e)) {
		RecordKeyEvent(e, u"generic scroll"_q, true);
		e->accept();
		return true;
	} else if (IsTextInputObject(object.get())) {
		RecordKeyEvent(e, u"normal mode consumed text"_q, false);
		e->accept();
		return true;
	}
	RecordKeyEvent(e, u"ignored"_q, false);
	return false;
}

bool InvokeAction(Action action) {
	const auto active = App().activeWindow();
	return Enabled()
		&& NormalMode()
		&& !App().passcodeLocked()
		&& (!active || !active->locked())
		&& !ActiveKeyboardScope()
		&& HandleRegisteredAction(action);
}

void RegisterActionHandler(
		not_null<QObject*> owner,
		Fn<bool(Action)> handler) {
	UnregisterActionHandler(owner);
	ActionHandlers.push_back({
		.owner = owner.get(),
		.handler = std::move(handler),
	});
}

void UnregisterActionHandler(not_null<QObject*> owner) {
	const auto raw = owner.get();
	const auto remove = [=](const ActionHandler &handler) {
		return !handler.owner || handler.owner == raw;
	};
	ActionHandlers.erase(
		std::remove_if(begin(ActionHandlers), end(ActionHandlers), remove),
		end(ActionHandlers));
}

void RegisterKeyHandler(
		not_null<QObject*> owner,
		Fn<bool(not_null<QKeyEvent*>)> handler) {
	UnregisterKeyHandler(owner);
	KeyHandlers.push_back({
		.owner = owner.get(),
		.handler = std::move(handler),
	});
}

void UnregisterKeyHandler(not_null<QObject*> owner) {
	const auto raw = owner.get();
	const auto remove = [=](const KeyHandler &handler) {
		return !handler.owner || handler.owner == raw;
	};
	KeyHandlers.erase(
		std::remove_if(begin(KeyHandlers), end(KeyHandlers), remove),
		end(KeyHandlers));
}

void RegisterPreLayerKeyHandler(
		not_null<QObject*> owner,
		Fn<bool(not_null<QKeyEvent*>)> handler,
		bool acceptShortcutOverride) {
	UnregisterPreLayerKeyHandler(owner);
	PreLayerKeyHandlers.push_back({
		.owner = owner.get(),
		.handler = std::move(handler),
		.acceptShortcutOverride = acceptShortcutOverride,
	});
}

void UnregisterPreLayerKeyHandler(not_null<QObject*> owner) {
	const auto raw = owner.get();
	const auto remove = [=](const KeyHandler &handler) {
		return !handler.owner || handler.owner == raw;
	};
	PreLayerKeyHandlers.erase(
		std::remove_if(
			begin(PreLayerKeyHandlers),
			end(PreLayerKeyHandlers),
			remove),
		end(PreLayerKeyHandlers));
}

void RegisterTextInputPassthroughHandler(
		not_null<QObject*> owner,
		Fn<bool(not_null<QKeyEvent*>)> handler) {
	UnregisterTextInputPassthroughHandler(owner);
	TextInputPassthroughHandlers.push_back({
		.owner = owner.get(),
		.handler = std::move(handler),
	});
}

void UnregisterTextInputPassthroughHandler(not_null<QObject*> owner) {
	const auto raw = owner.get();
	const auto remove = [=](const TextInputPassthroughHandler &handler) {
		return !handler.owner || handler.owner == raw;
	};
	TextInputPassthroughHandlers.erase(
		std::remove_if(
			begin(TextInputPassthroughHandlers),
			end(TextInputPassthroughHandlers),
			remove),
		end(TextInputPassthroughHandlers));
}

void RegisterForcedNormalModeHandler(
		not_null<QObject*> owner,
		Fn<bool()> handler) {
	UnregisterForcedNormalModeHandler(owner);
	ForcedNormalModeHandlers.push_back({
		.owner = owner.get(),
		.handler = std::move(handler),
	});
	RefreshForcedNormalMode();
}

void UnregisterForcedNormalModeHandler(not_null<QObject*> owner) {
	const auto raw = owner.get();
	const auto remove = [=](const ForcedNormalModeHandler &handler) {
		return !handler.owner || handler.owner == raw;
	};
	ForcedNormalModeHandlers.erase(
		std::remove_if(
			begin(ForcedNormalModeHandlers),
			end(ForcedNormalModeHandlers),
			remove),
		end(ForcedNormalModeHandlers));
	RefreshModeIndicator();
}

void RegisterModeIndicatorWidget(not_null<QWidget*> widget) {
	UnregisterModeIndicatorWidget(widget);
	ModeIndicatorWidgets.push_back(widget.get());
	RefreshModeIndicator();
}

void UnregisterModeIndicatorWidget(not_null<QWidget*> widget) {
	const auto raw = widget.get();
	const auto remove = [=](const QPointer<QWidget> &registered) {
		return !registered || registered == raw;
	};
	ModeIndicatorWidgets.erase(
		std::remove_if(
			begin(ModeIndicatorWidgets),
			end(ModeIndicatorWidgets),
			remove),
		end(ModeIndicatorWidgets));
}

void RefreshForcedNormalMode() {
	if (ForcedNormalMode()) {
		FocusForCurrentMode();
	}
	RefreshModeIndicator();
}

std::optional<ChatNavigation> ChatNavigationKey(not_null<QKeyEvent*> e) {
	if (MatchesBindings(VimKeymapKeyNextChatOption, e, true)) {
		return ChatNavigation::Next;
	} else if (MatchesBindings(VimKeymapKeyPreviousChatOption, e, true)) {
		return ChatNavigation::Previous;
	}
	return std::nullopt;
}

int ChatNavigationSteps(not_null<QKeyEvent*> e) {
	return (CleanModifiers(e) & Qt::ShiftModifier) ? 2 : 1;
}

bool ChatPreviewKey(not_null<QKeyEvent*> e) {
	return NormalMode()
		&& !Bindings::IsSystemPaste(e)
		&& MatchesBindings(
			VimKeymapKeyChatPreviewOption,
			e,
			false,
			false,
			false);
}

bool ChatHintsKey(not_null<QKeyEvent*> e) {
	return NormalMode()
		&& MatchesBindings(VimKeymapKeyOpenChatHintsOption, e, true);
}

bool FocusHintsKey(not_null<QKeyEvent*> e) {
	return Enabled()
		&& MatchesBindings(VimKeymapKeyFocusHintsOption, e, true);
}

bool CancelReplyKey(not_null<QKeyEvent*> e) {
	return Enabled()
		&& !e->isAutoRepeat()
		&& (!IsPlainEscapeKey(e) || EscapeClosesComposer())
		&& MatchesBindings(VimKeymapKeyCancelReplyOption, e);
}

bool CancelEditKey(not_null<QKeyEvent*> e) {
	return Enabled()
		&& !e->isAutoRepeat()
		&& (!IsPlainEscapeKey(e) || EscapeClosesComposer())
		&& MatchesBindings(VimKeymapKeyCancelEditOption, e);
}

bool EmojiPanelKey(not_null<QKeyEvent*> e) {
	return Enabled()
		&& !e->isAutoRepeat()
		&& MatchesBindings(VimKeymapKeyEmojiPanelOption, e);
}

bool FocusChatKey(not_null<QKeyEvent*> e) {
	return Enabled()
		&& !e->isAutoRepeat()
		&& MatchesBindings(VimKeymapKeyFocusChatOption, e);
}

bool FocusEmojiKey(not_null<QKeyEvent*> e) {
	return Enabled()
		&& !e->isAutoRepeat()
		&& MatchesBindings(VimKeymapKeyFocusEmojiOption, e);
}

bool CallKey(not_null<QKeyEvent*> e) {
	return Enabled()
		&& !e->isAutoRepeat()
		&& MatchesBindings(VimKeymapKeyCallOption, e);
}

bool GlobalSearchKey(not_null<QKeyEvent*> e) {
	const auto modifiers = CleanModifiers(e);
	return Enabled()
		&& !e->isAutoRepeat()
		&& (modifiers & Qt::ShiftModifier)
		&& IsGlobalSearchKey(e);
}

bool UndoKey(not_null<QKeyEvent*> e) {
	return NormalMode()
		&& !e->isAutoRepeat()
		&& MatchesBindings(VimKeymapKeyUndoOption, e, true);
}

bool RedoKey(not_null<QKeyEvent*> e) {
	return NormalMode()
		&& !e->isAutoRepeat()
		&& MatchesBindings(VimKeymapKeyRedoOption, e, true);
}

std::optional<Qt::Key> NavigationKey(not_null<QKeyEvent*> e) {
	return NormalMode() ? ScrollNavigationKey(e) : std::nullopt;
}

bool SelectMessageTextKey(not_null<QKeyEvent*> e) {
	return MatchesBindings(VimKeymapKeySelectMessageTextOption, e);
}

std::optional<Action> ActionKey(not_null<QKeyEvent*> e) {
	if (!NormalMode()) {
		return std::nullopt;
	} else if (Bindings::IsMessageReaction(e)) {
		return e->isAutoRepeat()
			? std::nullopt
			: std::make_optional(Action::ReactToMessage);
	} else if (MatchesBindings(VimKeymapKeyCopyMessageOption, e, true)) {
		return Action::CopyMessage;
	} else if (ChatPreviewKey(e)) {
		return Action::ChatPreview;
	} else if (SelectMessageTextKey(e)) {
		return Action::SelectMessageText;
	} else if (MatchesBindings(VimKeymapKeyReplyToMessageOption, e, true)) {
		return Action::ReplyToMessage;
	} else if (MatchesBindings(VimKeymapKeyEditMessageOption, e, true)) {
		return Action::EditMessage;
	} else if (MatchesBindings(VimKeymapKeyDeleteMessageOption, e, true)) {
		return Action::DeleteMessage;
	} else if (MatchesBindings(VimKeymapKeyFocusHintsOption, e, true)) {
		return Action::LinkHints;
	} else if (Bindings::IsMessageSelection(e)) {
		return Action::SelectMessages;
	}
	return std::nullopt;
}

std::optional<TextMotion> TextMotionKey(
		not_null<QKeyEvent*> e,
		bool &pendingStart) {
	return NormalMode()
		? Bindings::TextMotionKey(e, pendingStart)
		: std::nullopt;
}

bool TextVisualKey(not_null<QKeyEvent*> e) {
	return NormalMode()
		&& !e->isAutoRepeat()
		&& CleanModifiers(e) == Qt::NoModifier
		&& Bindings::KeyIs(e, Qt::Key_V, u"v"_q, u"\u043C"_q);
}

bool IsJumpToBottomKey(not_null<QKeyEvent*> e) {
	return NormalMode()
		&& MatchesBindings(VimKeymapKeyJumpBottomOption, e);
}

QString HintLabel(int index, int total) {
	return Bindings::ShortestHintLabel(index, total, CurrentHintAlphabet());
}

QString HintInput(not_null<QKeyEvent*> e) {
	const auto text = Bindings::HintCharacter(e);
	if (text.isEmpty()) {
		return QString();
	}
	const auto english = EnglishHintAlphabet();
	const auto russian = RussianHintAlphabet();
	const auto target = CurrentHintAlphabet();
	const auto source = UseEnglishHintAlphabet() ? russian : english;
	const auto ch = text.front();
	const auto sourceIndex = source.indexOf(ch);
	if (sourceIndex >= 0 && sourceIndex < target.size()) {
		return target.mid(sourceIndex, 1);
	}
	const auto targetIndex = target.indexOf(ch);
	if (targetIndex >= 0) {
		return target.mid(targetIndex, 1);
	}
	return text.mid(0, 1);
}

void ShowHelp() {
	ShowHelpBox();
}

bool HandleHelp(not_null<QKeyEvent*> e) {
	if (!Enabled() || !IsHelpKey(e)) {
		return false;
	}
	RecordKeyEvent(e, u"help"_q, true);
	ShowHelp();
	e->accept();
	return true;
}

bool HandleSearch(not_null<QKeyEvent*> e) {
	if (HandleGlobalSearch(e)) {
		return true;
	}
	if (!NormalMode() || !IsSearchKey(e)) {
		return false;
	}
	if (Shortcuts::Launch(Shortcuts::Command::Search)) {
		SetNormalModeValue(false);
		RecordKeyEvent(e, u"search"_q, true);
	}
	e->accept();
	return true;
}

} // namespace Core::VimKeymap
