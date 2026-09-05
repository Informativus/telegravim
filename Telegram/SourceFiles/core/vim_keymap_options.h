/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/options.h"
#include "core/vim_keymap.h"

namespace Core::VimKeymap {

inline constexpr auto kScrollStepMin = 80;
inline constexpr auto kScrollStepMax = 1800;
inline constexpr auto kLegacyScrollStepDefault = 360;
inline constexpr auto kScrollStepDefault = 180;
inline constexpr auto kHoldScrollSpeedMin = 600;
inline constexpr auto kHoldScrollSpeedMax = 16000;
inline constexpr auto kHoldScrollSpeedDefault = 3200;
inline constexpr auto kHintSizeMin = 10;
inline constexpr auto kHintSizeMax = 24;
inline constexpr auto kHintSizeDefault = 13;
inline constexpr auto kComposeCursorWidthMin = 1;
inline constexpr auto kComposeCursorWidthMax = 30;
inline constexpr auto kComposeCursorWidthDefault = 5;
inline constexpr auto kComposeCursorHeightMin = 20;
inline constexpr auto kComposeCursorHeightMax = 100;
inline constexpr auto kComposeCursorHeightDefault = 100;
inline constexpr auto kComposeCursorBlinkMin = 0;
inline constexpr auto kComposeCursorBlinkMax = 2000;
inline constexpr auto kComposeCursorBlinkDefault = 0;
inline constexpr auto kHintAlphabetRussian = "russian";
inline constexpr auto kHintAlphabetEnglish = "english";
inline constexpr auto kComposeCursorStyleBlock = "block";
inline constexpr auto kComposeCursorStyleBar = "bar";
inline constexpr auto kComposeCursorStyleUnderline = "underline";

extern base::options::toggle VimKeymapOption;
extern base::options::toggle VimKeymapDefaultsMigratedOption;
extern base::options::toggle VimKeymapEscapeClosesComposerOption;
extern base::options::option<int> VimKeymapScrollStepOption;
extern base::options::option<int> VimKeymapHoldScrollSpeedOption;
extern base::options::option<int> VimKeymapHintSizeOption;
extern base::options::option<QString> VimKeymapHintAlphabetOption;
extern base::options::option<QString> VimKeymapComposeCursorStyleOption;
extern base::options::option<int> VimKeymapComposeCursorWidthOption;
extern base::options::option<int> VimKeymapComposeCursorHeightOption;
extern base::options::option<int> VimKeymapComposeCursorBlinkOption;
extern base::options::option<QString> VimKeymapKeyToggleModeOption;
extern base::options::option<QString> VimKeymapKeyCancelReplyOption;
extern base::options::option<QString> VimKeymapKeyCancelEditOption;
extern base::options::option<QString> VimKeymapKeyHelpOption;
extern base::options::option<QString> VimKeymapKeyScrollDownOption;
extern base::options::option<QString> VimKeymapKeyScrollUpOption;
extern base::options::option<QString> VimKeymapKeyJumpBottomOption;
extern base::options::option<QString> VimKeymapKeyCopyMessageOption;
extern base::options::option<QString> VimKeymapKeySelectMessageTextOption;
extern base::options::option<QString> VimKeymapKeyReplyToMessageOption;
extern base::options::option<QString> VimKeymapKeyEditMessageOption;
extern base::options::option<QString> VimKeymapKeyDeleteMessageOption;
extern base::options::option<QString> VimKeymapKeyFocusHintsOption;
extern base::options::option<QString> VimKeymapKeyOpenChatHintsOption;
extern base::options::option<QString> VimKeymapKeyChatPreviewOption;
extern base::options::option<QString> VimKeymapKeySearchOption;
extern base::options::option<QString> VimKeymapKeyGlobalSearchOption;
extern base::options::option<QString> VimKeymapKeyUndoOption;
extern base::options::option<QString> VimKeymapKeyRedoOption;
extern base::options::option<QString> VimKeymapKeyNextChatOption;
extern base::options::option<QString> VimKeymapKeyPreviousChatOption;
extern base::options::option<QString> VimKeymapKeyNextFolderOption;
extern base::options::option<QString> VimKeymapKeyPreviousFolderOption;
extern base::options::option<QString> VimKeymapKeyEmojiPanelOption;
extern base::options::option<QString> VimKeymapKeyFocusChatOption;
extern base::options::option<QString> VimKeymapKeyFocusEmojiOption;
extern base::options::option<QString> VimKeymapKeyCallOption;

} // namespace Core::VimKeymap
