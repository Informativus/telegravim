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
#include <memory>
#include <vector>

namespace Core::VimKeymap {
namespace {

constexpr auto kHoldScrollTickMs = 16;
constexpr auto kHoldScrollStartDelayMs = 90;
constexpr auto kSingleScrollDurationMs = 190;
constexpr auto kTelegraVimBuild = "2026.09.07-102-beta.1";

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

class HelpTabsWidget final : public Ui::RpWidget {
public:
	HelpTabsWidget(
		QWidget *parent,
		QString hints,
		bool russian)
	: Ui::RpWidget(parent)
	, _hints(std::move(hints))
	, _russian(russian)
	, _slider(Ui::CreateChild<Ui::SettingsSlider>(this, st::settingsSlider))
	, _label(Ui::CreateChild<Ui::FlatLabel>(this, st::boxLabel)) {
		setFocusPolicy(Qt::StrongFocus);
		_slider->addSection(tabTitle(0));
		_slider->addSection(tabTitle(1));
		_slider->setActiveSectionFast(_activeTab);
		_slider->sectionActivated(
		) | rpl::on_next([=](int index) {
			setActiveTab(index);
		}, _slider->lifetime());
		_label->setSelectable(true);
		updateLabel();
	}

	QAccessible::Role accessibilityRole() override {
		return QAccessible::Role::PageTabList;
	}

	QString accessibilityName() override {
		return currentTitle() + u"\n"_q + currentText();
	}

	bool handleBoxKey(
			not_null<Ui::GenericBox*> box,
			not_null<QKeyEvent*> e) {
		const auto modifiers = CleanModifiers(e);
		const auto text = e->text().toCaseFolded();
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
			if (_activeTab == 1) {
				updateLabel();
			}
			e->accept();
			return true;
		}
		return false;
	}

	int resizeGetHeight(int newWidth) override {
		_slider->resizeToWidth(newWidth);
		_label->resizeToWidth(newWidth);
		layoutChildren(newWidth);
		return _slider->height() + kTabGap + _label->height();
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

protected:
	void resizeEvent(QResizeEvent *e) override {
		layoutChildren(e->size().width());
		Ui::RpWidget::resizeEvent(e);
	}

private:
	static constexpr auto kTabsCount = 2;
	static constexpr auto kTabGap = 10;

	[[nodiscard]] QString tabTitle(int index) const {
		if (_russian) {
			return index == 0 ? u"Подсказки"_q : u"Лог клавиш"_q;
		}
		return index == 0 ? u"Hints"_q : u"Key log"_q;
	}

	[[nodiscard]] QString currentTitle() const {
		return tabTitle(_activeTab);
	}

	[[nodiscard]] QString currentText() const {
		return (_activeTab == 0) ? _hints : keyLogText();
	}

	void setActiveTab(int tab) {
		const auto value = (tab % kTabsCount + kTabsCount) % kTabsCount;
		if (_activeTab == value) {
			return;
		}
		_activeTab = value;
		_slider->setActiveSectionFast(_activeTab);
		updateLabel();
		accessibilityNameChanged();
		update();
	}

	void updateLabel() {
		_label->setText(currentText());
		_label->resizeToWidth(std::max(1, width()));
		layoutChildren(width());
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

	void layoutChildren(int newWidth) {
		_slider->moveToLeft(0, 0, newWidth);
		_label->moveToLeft(0, _slider->height() + kTabGap, newWidth);
	}

	QString _hints;
	bool _russian = false;
	not_null<Ui::SettingsSlider*> _slider;
	not_null<Ui::FlatLabel*> _label;
	int _activeTab = 0;

};

void ShowHelpBox() {
	const auto active = App().activeWindow();
	const auto window = active ? active : App().activePrimaryWindow();
	if (!window) {
		return;
	}
	const auto russian = UseRussianHelp();
	const auto toggle = BindingLabel(VimKeymapKeyToggleModeOption);
	const auto cancelReply = BindingLabel(VimKeymapKeyCancelReplyOption);
	const auto cancelEdit = BindingLabel(VimKeymapKeyCancelEditOption);
	const auto cancelEditEnabled
		= !VimKeymapKeyCancelEditOption.value().trimmed().isEmpty();
	const auto help = BindingLabel(VimKeymapKeyHelpOption);
	const auto scrollDown = BindingLabel(VimKeymapKeyScrollDownOption);
	const auto scrollUp = BindingLabel(VimKeymapKeyScrollUpOption);
	const auto jumpBottom = BindingLabel(VimKeymapKeyJumpBottomOption);
	const auto copy = BindingLabel(VimKeymapKeyCopyMessageOption);
	const auto selectMessageText = BindingLabel(
		VimKeymapKeySelectMessageTextOption);
	const auto reply = BindingLabel(VimKeymapKeyReplyToMessageOption);
	const auto edit = BindingLabel(VimKeymapKeyEditMessageOption);
	const auto deleteMessage = BindingLabel(VimKeymapKeyDeleteMessageOption);
	const auto focus = BindingLabel(VimKeymapKeyFocusHintsOption);
	const auto openChats = BindingLabel(VimKeymapKeyOpenChatHintsOption);
	const auto preview = BindingLabel(VimKeymapKeyChatPreviewOption);
	const auto search = BindingLabel(VimKeymapKeySearchOption);
	const auto globalSearch = BindingLabel(VimKeymapKeyGlobalSearchOption);
	const auto undo = BindingLabel(VimKeymapKeyUndoOption);
	const auto redo = BindingLabel(VimKeymapKeyRedoOption);
	const auto nextChat = BindingLabel(VimKeymapKeyNextChatOption);
	const auto previousChat = BindingLabel(VimKeymapKeyPreviousChatOption);
	const auto nextFolder = BindingLabel(VimKeymapKeyNextFolderOption);
	const auto previousFolder = BindingLabel(VimKeymapKeyPreviousFolderOption);
	const auto emojiPanel = BindingLabel(VimKeymapKeyEmojiPanelOption);
	const auto emojiPanelEnabled
		= !VimKeymapKeyEmojiPanelOption.value().trimmed().isEmpty();
	const auto focusChat = BindingLabel(VimKeymapKeyFocusChatOption);
	const auto focusEmoji = BindingLabel(VimKeymapKeyFocusEmojiOption);
	const auto call = BindingLabel(VimKeymapKeyCallOption);
	const auto version = u"TelegraVim "_q
		+ QString::fromLatin1(kTelegraVimBuild)
		+ u" / Telegram "_q
		+ QString::fromLatin1(AppVersionStr);
	const auto hints = [&] {
		auto result = QString();
		if (russian) {
			result += u"Версия: "_q + version + u"\n\n"_q;
			result += toggle
				+ u" - режим ввода / навигации, если не открыт слой Telegram\n"_q;
			result += u"Если открыто фото, меню или окно - Esc сначала закрывает его.\n\n"_q;
			result += cancelReply
				+ u" - снять активный reply у сообщения\n\n"_q;
			if (cancelEditEnabled) {
				result += cancelEdit
					+ u" - отменить редактирование сообщения\n\n"_q;
			}
			result += u"Индикатор режима:\n"_q;
			result += u"\U0001F7E2 ввод\n"_q;
			result += u"\U0001F7E1 навигация\n"_q;
			result += u"\U0001F7E3 visual selection\n\n"_q;
			result += u"Режим навигации:\n"_q;
			result += scrollDown + u" - скролл вниз\n"_q;
			result += scrollUp + u" - скролл вверх\n"_q;
			result += jumpBottom + u" - перейти вниз\n"_q;
			result += copy + u" - подсказки для копирования сообщений\n"_q;
			result += selectMessageText
				+ u" - подсказки для visual-выделения текста сообщения\n"_q;
			result += u"В тексте сообщения: gg/G - начало/конец, {/} - абзацы\n"_q;
			result += u"Лишние клавиши сохраняют курсор и выделение; Esc - выход\n"_q;
			result += reply + u" - подсказки для ответа на сообщение\n"_q;
			result += edit
				+ u" - подсказки для редактирования своих сообщений\n"_q;
			result += deleteMessage
				+ u" - подсказки для удаления сообщений\n"_q;
			result += focus
				+ u" - подсказки для медиа, ссылок, голосований, ответов и пересланных авторов\n"_q;
			result += openChats + u" - подсказки для открытия чатов\n"_q;
			result += preview
				+ u" - буквы для предпросмотра чата без прочтения\n"_q;
			result += u"Ctrl+V - preview чата в view mode, без открытия и прочтения\n"_q;
			result += search + u" - поиск\n"_q;
			result += undo + u" - откатить последнее изменение текста\n"_q;
			result += redo + u" - вернуть откатанное изменение текста\n"_q;
			result += u"h/j/k/l, w/b/e, 0/^/$/|, gg/G, {/} - движение в composer\n"_q;
			result += u"x/X, dd/D, diw, yy/Y, yiw, p/P, u/Ctrl+R - правка текста\n"_q;
			result += u"Ctrl+E - исправления слова под курсором\n"_q;
			result += u"c/cw/cc/C/ciw, s/S, r<char>, J, ~ - Vim-команды composer\n"_q;
			result += u"v/V - visual/visual line заготовка: y копирует, d/x удаляет\n"_q;
			result += u"i/a/I/A/o/O - вернуться к вводу текста\n"_q;
			result += u"Option+h/l - двигать курсор на одну букву во время ввода\n"_q;
			result += u"Cursor style/width/height/blink меняются в Settings > Vim keymap\n"_q;
			result += help + u" - эта подсказка\n\n"_q;
			result += u"Всегда доступно:\n"_q;
			result += nextChat + u" - следующий чат\n"_q;
			result += previousChat + u" - предыдущий чат\n"_q;
			result += nextChat + u" + Shift - перейти через один чат\n"_q;
			result += previousChat + u" + Shift - перейти через один чат\n"_q;
			result += globalSearch + u" - поиск\n"_q;
			result += call + u" - звонок / войти в групповой звонок\n"_q;
			result += nextFolder + u" - следующая папка\n"_q;
			result += previousFolder + u" - предыдущая папка\n\n"_q;
			result += u"Emoji / stickers:\n"_q;
			if (emojiPanelEnabled) {
				result += emojiPanel + u" - открыть и сфокусировать панель\n"_q;
			}
			result += focusEmoji + u" - открыть и сфокусировать панель\n"_q;
			result += focusChat + u" - фокус обратно в чат\n\n"_q;
			result += u"h/j/k/l или р/о/л/д - выбор emoji/sticker/gif\n"_q;
			result += u"Enter или Space - отправить выбранное\n"_q;
			result += u"Tab / Shift+Tab - emoji, stickers, GIFs\n"_q;
			result += u"Ctrl+J/K - скролл внутри панели\n"_q;
			result += u"Ctrl+F - поиск внутри панели\n\n"_q;
			result += u"Настройки: Settings > Vim keymap"_q;
		} else {
			result += u"Version: "_q + version + u"\n\n"_q;
			result += toggle
				+ u" - input / navigation mode when no Telegram layer is open\n"_q;
			result += u"If a photo, menu or dialog is open, Esc closes it first.\n\n"_q;
			result += cancelReply + u" - clear the active message reply\n\n"_q;
			if (cancelEditEnabled) {
				result += cancelEdit + u" - cancel message editing\n\n"_q;
			}
			result += u"Mode indicator:\n"_q;
			result += u"\U0001F7E2 input\n"_q;
			result += u"\U0001F7E1 navigation\n"_q;
			result += u"\U0001F7E3 visual selection\n\n"_q;
			result += u"Navigation mode:\n"_q;
			result += scrollDown + u" - scroll down\n"_q;
			result += scrollUp + u" - scroll up\n"_q;
			result += jumpBottom + u" - jump to bottom\n"_q;
			result += copy + u" - show copy message hints\n"_q;
			result += selectMessageText
				+ u" - show message text visual selection hints\n"_q;
			result += u"Message text: gg/G - start/end, {/} - paragraphs\n"_q;
			result += u"Other keys keep the cursor and selection; Esc exits\n"_q;
			result += reply + u" - show reply message hints\n"_q;
			result += edit + u" - show edit message hints\n"_q;
			result += deleteMessage + u" - show delete message hints\n"_q;
			result += focus
				+ u" - show media, link, poll, reply and forwarded-source hints\n"_q;
			result += openChats + u" - show chat open hints\n"_q;
			result += preview
				+ u" - show chat preview hints without marking read\n"_q;
			result += u"Ctrl+V - chat preview in view mode, without opening or marking read\n"_q;
			result += search + u" - focus search\n"_q;
			result += undo + u" - undo the last compose text change\n"_q;
			result += redo + u" - redo the last compose text change\n"_q;
			result += u"h/j/k/l, w/b/e, 0/^/$/|, gg/G, {/} - compose motions\n"_q;
			result += u"x/X, dd/D, diw, yy/Y, yiw, p/P, u/Ctrl+R - edit text\n"_q;
			result += u"Ctrl+E - spelling suggestions at the cursor\n"_q;
			result += u"c/cw/cc/C/ciw, s/S, r<char>, J, ~ - compose Vim commands\n"_q;
			result += u"v/V - visual/visual line groundwork: y copies, d/x deletes\n"_q;
			result += u"i/a/I/A/o/O - return to text input\n"_q;
			result += u"Option+h/l - move one character while typing\n"_q;
			result += u"Cursor style/width/height/blink are in Settings > Vim keymap\n"_q;
			result += help + u" - show this help\n\n"_q;
			result += u"Always available:\n"_q;
			result += nextChat + u" - next chat\n"_q;
			result += previousChat + u" - previous chat\n"_q;
			result += nextChat + u" + Shift - skip one chat\n"_q;
			result += previousChat + u" + Shift - skip one chat\n"_q;
			result += globalSearch + u" - search\n"_q;
			result += call + u" - call / join group call\n"_q;
			result += nextFolder + u" - next folder\n"_q;
			result += previousFolder + u" - previous folder\n\n"_q;
			result += u"Emoji / stickers:\n"_q;
			if (emojiPanelEnabled) {
				result += emojiPanel + u" - open and focus the panel\n"_q;
			}
			result += focusEmoji + u" - open and focus the panel\n"_q;
			result += focusChat + u" - focus back to chat\n\n"_q;
			result += u"h/j/k/l or р/о/л/д - select emoji/sticker/gif\n"_q;
			result += u"Enter or Space - send selected item\n"_q;
			result += u"Tab / Shift+Tab - emoji, stickers, GIFs\n"_q;
			result += u"Ctrl+J/K - scroll inside the panel\n"_q;
			result += u"Ctrl+F - search inside the panel\n\n"_q;
			result += u"Settings: Settings > Vim keymap"_q;
		}
		return result;
	}();
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(russian ? u"Vim-навигация"_q : u"Vim navigation"_q);
		box->setWidth(560);
		box->setMaxHeight(640);
		const auto tabs = box->addRow(
			object_ptr<HelpTabsWidget>(
				box.get(),
				hints,
				russian));
		RegisterKeyHandler(tabs, [=](not_null<QKeyEvent*> e) {
			return tabs->handleGlobalKey(box, e);
		});
		box->setFocusCallback([=] {
			tabs->setFocus(Qt::ShortcutFocusReason);
		});
		box->setShowFinishedCallback([=] {
			tabs->setFocus(Qt::ShortcutFocusReason);
		});
		box->addButton(tr::lng_box_ok(), [=] {
			box->closeBox();
		});
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
	const auto globalRoot = scope ? nullptr : GlobalFocusRoot(focus);
	if (globalRoot) {
		const auto navigation = KeyboardNavigation::Find(globalRoot);
		if (!navigation || !navigation->hasHints()) {
			if (const auto delta = Bindings::TabNavigationDelta(e)) {
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

std::optional<Action> ActionKey(not_null<QKeyEvent*> e) {
	if (!NormalMode()) {
		return std::nullopt;
	} else if (MatchesBindings(VimKeymapKeyCopyMessageOption, e, true)) {
		return Action::CopyMessage;
	} else if (ChatPreviewKey(e)) {
		return Action::ChatPreview;
	} else if (MatchesBindings(VimKeymapKeySelectMessageTextOption, e)) {
		return Action::SelectMessageText;
	} else if (MatchesBindings(VimKeymapKeyReplyToMessageOption, e, true)) {
		return Action::ReplyToMessage;
	} else if (MatchesBindings(VimKeymapKeyEditMessageOption, e, true)) {
		return Action::EditMessage;
	} else if (MatchesBindings(VimKeymapKeyDeleteMessageOption, e, true)) {
		return Action::DeleteMessage;
	} else if (MatchesBindings(VimKeymapKeyFocusHintsOption, e, true)) {
		return Action::LinkHints;
	} else if (Bindings::IsMessageShare(e)) {
		return Action::ShareMessage;
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
