/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/Qt>

#include <optional>

class QKeyEvent;
class QObject;
class QWidget;

namespace Core::VimKeymap {

extern const char kOptionVimKeymap[];
extern const char kOptionVimKeymapScrollStep[];
extern const char kOptionVimKeymapHoldScrollSpeed[];
extern const char kOptionVimKeymapHintSize[];
extern const char kOptionVimKeymapHintAlphabet[];
extern const char kOptionVimKeymapComposeCursorStyle[];
extern const char kOptionVimKeymapComposeCursorWidth[];
extern const char kOptionVimKeymapComposeCursorHeight[];
extern const char kOptionVimKeymapComposeCursorBlink[];
extern const char kOptionVimKeymapKeyToggleMode[];
extern const char kOptionVimKeymapKeyCancelReply[];
extern const char kOptionVimKeymapKeyCancelEdit[];
extern const char kOptionVimKeymapKeyHelp[];
extern const char kOptionVimKeymapKeyScrollDown[];
extern const char kOptionVimKeymapKeyScrollUp[];
extern const char kOptionVimKeymapKeyJumpBottom[];
extern const char kOptionVimKeymapKeyCopyMessage[];
extern const char kOptionVimKeymapKeyReplyToMessage[];
extern const char kOptionVimKeymapKeyEditMessage[];
extern const char kOptionVimKeymapKeyDeleteMessage[];
extern const char kOptionVimKeymapKeyFocusHints[];
extern const char kOptionVimKeymapKeyOpenChatHints[];
extern const char kOptionVimKeymapKeyChatPreview[];
extern const char kOptionVimKeymapKeySearch[];
extern const char kOptionVimKeymapKeyGlobalSearch[];
extern const char kOptionVimKeymapKeyUndo[];
extern const char kOptionVimKeymapKeyRedo[];
extern const char kOptionVimKeymapKeyNextChat[];
extern const char kOptionVimKeymapKeyPreviousChat[];
extern const char kOptionVimKeymapKeyNextFolder[];
extern const char kOptionVimKeymapKeyPreviousFolder[];
extern const char kOptionVimKeymapKeyEmojiPanel[];
extern const char kOptionVimKeymapKeyFocusChat[];
extern const char kOptionVimKeymapKeyFocusEmoji[];
extern const char kOptionVimKeymapKeyCall[];

struct IntOptionBounds {
	int min = 0;
	int max = 0;
};

enum class Action {
	CopyMessage,
	ReplyToMessage,
	EditMessage,
	DeleteMessage,
	LinkHints,
};

enum class ChatNavigation {
	Previous,
	Next,
};

[[nodiscard]] bool Enabled();
[[nodiscard]] bool NormalMode();
void MigrateLegacyDefaults();
void SetNormalMode(bool enabled);
[[nodiscard]] IntOptionBounds ScrollStepBounds();
[[nodiscard]] IntOptionBounds HoldScrollSpeedBounds();
[[nodiscard]] IntOptionBounds HintSizeBounds();
[[nodiscard]] IntOptionBounds ComposeCursorWidthBounds();
[[nodiscard]] IntOptionBounds ComposeCursorHeightBounds();
[[nodiscard]] IntOptionBounds ComposeCursorBlinkBounds();
[[nodiscard]] int ScrollStep();
[[nodiscard]] int HoldScrollSpeed();
[[nodiscard]] int HintSize();
[[nodiscard]] QString ComposeCursorStyle();
[[nodiscard]] int ComposeCursorWidth();
[[nodiscard]] int ComposeCursorHeight();
[[nodiscard]] int ComposeCursorBlink();
[[nodiscard]] int HoldScrollTickMs();
[[nodiscard]] int HoldScrollStartDelayMs();
[[nodiscard]] int HoldScrollDelta();
[[nodiscard]] int SingleScrollDurationMs();

[[nodiscard]] bool HandleApplicationKeyPress(
	not_null<QObject*> object,
	not_null<QKeyEvent*> e);
void RegisterActionHandler(not_null<QObject*> owner, Fn<bool(Action)> handler);
void UnregisterActionHandler(not_null<QObject*> owner);
void RegisterKeyHandler(
	not_null<QObject*> owner,
	Fn<bool(not_null<QKeyEvent*>)> handler);
void UnregisterKeyHandler(not_null<QObject*> owner);
void RegisterTextInputPassthroughHandler(
	not_null<QObject*> owner,
	Fn<bool(not_null<QKeyEvent*>)> handler);
void UnregisterTextInputPassthroughHandler(not_null<QObject*> owner);
void RegisterForcedNormalModeHandler(
	not_null<QObject*> owner,
	Fn<bool()> handler);
void UnregisterForcedNormalModeHandler(not_null<QObject*> owner);
void RegisterModeIndicatorWidget(not_null<QWidget*> widget);
void UnregisterModeIndicatorWidget(not_null<QWidget*> widget);
void RefreshForcedNormalMode();
void TraceKey(not_null<QKeyEvent*> e, const QString &status);
[[nodiscard]] QString RecentKeyLogText();
[[nodiscard]] std::optional<ChatNavigation> ChatNavigationKey(
	not_null<QKeyEvent*> e);
[[nodiscard]] int ChatNavigationSteps(not_null<QKeyEvent*> e);
[[nodiscard]] bool ChatPreviewKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool ChatHintsKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool FocusHintsKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool CancelReplyKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool CancelEditKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool EmojiPanelKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool FocusChatKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool FocusEmojiKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool CallKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool GlobalSearchKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool UndoKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool RedoKey(not_null<QKeyEvent*> e);
[[nodiscard]] std::optional<Qt::Key> NavigationKey(not_null<QKeyEvent*> e);
[[nodiscard]] std::optional<Action> ActionKey(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsJumpToBottomKey(not_null<QKeyEvent*> e);
[[nodiscard]] QString HintLabel(int index, int total);
[[nodiscard]] QString HintInput(not_null<QKeyEvent*> e);

void ShowHelp();
[[nodiscard]] bool HandleHelp(not_null<QKeyEvent*> e);
[[nodiscard]] bool HandleSearch(not_null<QKeyEvent*> e);

} // namespace Core::VimKeymap
