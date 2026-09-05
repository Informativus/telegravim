/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_options.h"

namespace Core::VimKeymap {

const char kOptionVimKeymap[] = "vim-keymap";
const char kOptionVimKeymapEscapeClosesComposer[] =
	"vim-keymap-escape-closes-composer";
const char kOptionVimKeymapScrollStep[] = "vim-keymap-scroll-step";
const char kOptionVimKeymapHoldScrollSpeed[] = "vim-keymap-hold-scroll-speed";
const char kOptionVimKeymapHintSize[] = "vim-keymap-hint-size";
const char kOptionVimKeymapHintAlphabet[] = "vim-keymap-hint-alphabet";
const char kOptionVimKeymapComposeCursorStyle[]
	= "vim-keymap-compose-cursor-style";
const char kOptionVimKeymapComposeCursorWidth[]
	= "vim-keymap-compose-cursor-width";
const char kOptionVimKeymapComposeCursorHeight[]
	= "vim-keymap-compose-cursor-height";
const char kOptionVimKeymapComposeCursorBlink[]
	= "vim-keymap-compose-cursor-blink";
const char kOptionVimKeymapKeyToggleMode[] = "vim-keymap-key-toggle-mode";
const char kOptionVimKeymapKeyCancelReply[] = "vim-keymap-key-cancel-reply";
const char kOptionVimKeymapKeyCancelEdit[] = "vim-keymap-key-cancel-edit";
const char kOptionVimKeymapKeyHelp[] = "vim-keymap-key-help";
const char kOptionVimKeymapKeyScrollDown[] = "vim-keymap-key-scroll-down";
const char kOptionVimKeymapKeyScrollUp[] = "vim-keymap-key-scroll-up";
const char kOptionVimKeymapKeyJumpBottom[] = "vim-keymap-key-jump-bottom";
const char kOptionVimKeymapKeyCopyMessage[] = "vim-keymap-key-copy-message";
const char kOptionVimKeymapKeySelectMessageText[] =
	"vim-keymap-key-select-message-text";
const char kOptionVimKeymapKeyReplyToMessage[] =
	"vim-keymap-key-reply-to-message";
const char kOptionVimKeymapKeyEditMessage[] =
	"vim-keymap-key-edit-message";
const char kOptionVimKeymapKeyDeleteMessage[] =
	"vim-keymap-key-delete-message";
const char kOptionVimKeymapKeyFocusHints[] = "vim-keymap-key-focus-hints";
const char kOptionVimKeymapKeyOpenChatHints[] =
	"vim-keymap-key-open-chat-hints";
const char kOptionVimKeymapKeyChatPreview[] = "vim-keymap-key-chat-preview";
const char kOptionVimKeymapKeySearch[] = "vim-keymap-key-search";
const char kOptionVimKeymapKeyGlobalSearch[] =
	"vim-keymap-key-global-search";
const char kOptionVimKeymapKeyUndo[] = "vim-keymap-key-undo";
const char kOptionVimKeymapKeyRedo[] = "vim-keymap-key-redo";
const char kOptionVimKeymapKeyNextChat[] = "vim-keymap-key-next-chat";
const char kOptionVimKeymapKeyPreviousChat[] =
	"vim-keymap-key-previous-chat";
const char kOptionVimKeymapKeyNextFolder[] = "vim-keymap-key-next-folder";
const char kOptionVimKeymapKeyPreviousFolder[] =
	"vim-keymap-key-previous-folder";
const char kOptionVimKeymapKeyEmojiPanel[] = "vim-keymap-key-emoji-panel";
const char kOptionVimKeymapKeyFocusChat[] = "vim-keymap-key-focus-chat";
const char kOptionVimKeymapKeyFocusEmoji[] = "vim-keymap-key-focus-emoji";
const char kOptionVimKeymapKeyCall[] = "vim-keymap-key-call";

base::options::toggle VimKeymapDefaultsMigratedOption({
	.id = "vim-keymap-defaults-migrated",
});

base::options::toggle VimKeymapOption({
	.id = kOptionVimKeymap,
	.name = "Vim keymap",
	.description = "Enable Vim-style navigation outside text fields.",
	.defaultValue = true,
});

base::options::toggle VimKeymapEscapeClosesComposerOption({
	.id = kOptionVimKeymapEscapeClosesComposer,
	.name = "Vim Esc closes composer state",
	.description = "Let Esc close reply, edit, forward and similar composer state before changing Vim mode.",
	.defaultValue = true,
});

base::options::option<int> VimKeymapScrollStepOption({
	.id = kOptionVimKeymapScrollStep,
	.name = "Vim scroll step",
	.description = "Pixels moved by one j/k press in navigation mode.",
	.defaultValue = kScrollStepDefault,
});

base::options::option<int> VimKeymapHoldScrollSpeedOption({
	.id = kOptionVimKeymapHoldScrollSpeed,
	.name = "Vim hold scroll speed",
	.description = "Pixels per second while holding j/k in navigation mode.",
	.defaultValue = kHoldScrollSpeedDefault,
});

base::options::option<int> VimKeymapHintSizeOption({
	.id = kOptionVimKeymapHintSize,
	.name = "Vim hint letter size",
	.description = "Font size for Vim-style hint badges.",
	.defaultValue = kHintSizeDefault,
});

base::options::option<QString> VimKeymapHintAlphabetOption({
	.id = kOptionVimKeymapHintAlphabet,
	.name = "Vim hint buttons language",
	.description = "Language used for Vim-style hint badges.",
	.defaultValue = QString::fromLatin1(kHintAlphabetRussian),
});

base::options::option<QString> VimKeymapComposeCursorStyleOption({
	.id = kOptionVimKeymapComposeCursorStyle,
	.name = "Vim compose cursor style",
	.description = "Cursor shape in compose normal mode: block, bar or underline.",
	.defaultValue = QString::fromLatin1(kComposeCursorStyleBlock),
});

base::options::option<int> VimKeymapComposeCursorWidthOption({
	.id = kOptionVimKeymapComposeCursorWidth,
	.name = "Vim compose cursor width",
	.description = "Cursor width in pixels in compose normal mode.",
	.defaultValue = kComposeCursorWidthDefault,
});

base::options::option<int> VimKeymapComposeCursorHeightOption({
	.id = kOptionVimKeymapComposeCursorHeight,
	.name = "Vim compose cursor height",
	.description = "Cursor height percent in compose normal mode.",
	.defaultValue = kComposeCursorHeightDefault,
});

base::options::option<int> VimKeymapComposeCursorBlinkOption({
	.id = kOptionVimKeymapComposeCursorBlink,
	.name = "Vim compose cursor blink",
	.description = "Blink interval in milliseconds. 0 keeps the cursor always visible.",
	.defaultValue = kComposeCursorBlinkDefault,
});

constexpr auto kBindingDescription =
	"Comma-separated bindings. Examples: j, Ctrl+J, Esc, ?, Tab, "
	"Shift+Tab. Empty disables this binding.";

base::options::option<QString> VimKeymapKeyToggleModeOption({
	.id = kOptionVimKeymapKeyToggleMode,
	.name = "Vim key: toggle mode",
	.description = kBindingDescription,
	.defaultValue = u"Esc"_q,
});

base::options::option<QString> VimKeymapKeyCancelReplyOption({
	.id = kOptionVimKeymapKeyCancelReply,
	.name = "Vim key: cancel reply",
	.description = kBindingDescription,
	.defaultValue = u"Esc"_q,
});

base::options::option<QString> VimKeymapKeyCancelEditOption({
	.id = kOptionVimKeymapKeyCancelEdit,
	.name = "Vim key: cancel edit",
	.description = kBindingDescription,
	.defaultValue = QString(),
});

base::options::option<QString> VimKeymapKeyHelpOption({
	.id = kOptionVimKeymapKeyHelp,
	.name = "Vim key: help",
	.description = kBindingDescription,
	.defaultValue = u"?"_q,
});

base::options::option<QString> VimKeymapKeyScrollDownOption({
	.id = kOptionVimKeymapKeyScrollDown,
	.name = "Vim key: scroll down",
	.description = kBindingDescription,
	.defaultValue = u"j, \u043E"_q,
});

base::options::option<QString> VimKeymapKeyScrollUpOption({
	.id = kOptionVimKeymapKeyScrollUp,
	.name = "Vim key: scroll up",
	.description = kBindingDescription,
	.defaultValue = u"k, \u043B"_q,
});

base::options::option<QString> VimKeymapKeyJumpBottomOption({
	.id = kOptionVimKeymapKeyJumpBottom,
	.name = "Vim key: jump to bottom",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+G, Ctrl+\u043F"_q,
});

base::options::option<QString> VimKeymapKeyCopyMessageOption({
	.id = kOptionVimKeymapKeyCopyMessage,
	.name = "Vim key: copy message hints",
	.description = kBindingDescription,
	.defaultValue = u"y, \u043D"_q,
});

base::options::option<QString> VimKeymapKeySelectMessageTextOption({
	.id = kOptionVimKeymapKeySelectMessageText,
	.name = "Vim key: select message text hints",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+Shift+V, Ctrl+Shift+\u043C"_q,
});

base::options::option<QString> VimKeymapKeyReplyToMessageOption({
	.id = kOptionVimKeymapKeyReplyToMessage,
	.name = "Vim key: reply hints",
	.description = kBindingDescription,
	.defaultValue = u"r, \u043A"_q,
});

base::options::option<QString> VimKeymapKeyEditMessageOption({
	.id = kOptionVimKeymapKeyEditMessage,
	.name = "Vim key: edit message hints",
	.description = kBindingDescription,
	.defaultValue = u"e, \u0443"_q,
});

base::options::option<QString> VimKeymapKeyDeleteMessageOption({
	.id = kOptionVimKeymapKeyDeleteMessage,
	.name = "Vim key: delete message hints",
	.description = kBindingDescription,
	.defaultValue = u"d, \u0432"_q,
});

base::options::option<QString> VimKeymapKeyFocusHintsOption({
	.id = kOptionVimKeymapKeyFocusHints,
	.name = "Vim key: focus/link hints",
	.description = kBindingDescription,
	.defaultValue = u"f, \u0430"_q,
});

base::options::option<QString> VimKeymapKeyOpenChatHintsOption({
	.id = kOptionVimKeymapKeyOpenChatHints,
	.name = "Vim key: open chat hints",
	.description = kBindingDescription,
	.defaultValue = u"o, \u0449"_q,
});

base::options::option<QString> VimKeymapKeyChatPreviewOption({
	.id = kOptionVimKeymapKeyChatPreview,
	.name = "Vim key: chat preview hints",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+V, Ctrl+\u043C"_q,
});

base::options::option<QString> VimKeymapKeySearchOption({
	.id = kOptionVimKeymapKeySearch,
	.name = "Vim key: search in navigation mode",
	.description = kBindingDescription,
	.defaultValue = u"/"_q,
});

base::options::option<QString> VimKeymapKeyGlobalSearchOption({
	.id = kOptionVimKeymapKeyGlobalSearch,
	.name = "Vim key: global search",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+F, Ctrl+\u0430, Cmd+Shift+F, "
		u"Cmd+Shift+\u0430, Ctrl+Shift+F, Ctrl+Shift+\u0430"_q,
});

base::options::option<QString> VimKeymapKeyUndoOption({
	.id = kOptionVimKeymapKeyUndo,
	.name = "Vim key: undo compose text",
	.description = kBindingDescription,
	.defaultValue = u"u, \u0433"_q,
});

base::options::option<QString> VimKeymapKeyRedoOption({
	.id = kOptionVimKeymapKeyRedo,
	.name = "Vim key: redo compose text",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+R, Ctrl+\u043A"_q,
});

base::options::option<QString> VimKeymapKeyNextChatOption({
	.id = kOptionVimKeymapKeyNextChat,
	.name = "Vim key: next chat",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+J, Ctrl+\u043E"_q,
});

base::options::option<QString> VimKeymapKeyPreviousChatOption({
	.id = kOptionVimKeymapKeyPreviousChat,
	.name = "Vim key: previous chat",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+K, Ctrl+\u043B"_q,
});

base::options::option<QString> VimKeymapKeyNextFolderOption({
	.id = kOptionVimKeymapKeyNextFolder,
	.name = "Vim key: next folder",
	.description = kBindingDescription,
	.defaultValue = u"Tab"_q,
});

base::options::option<QString> VimKeymapKeyPreviousFolderOption({
	.id = kOptionVimKeymapKeyPreviousFolder,
	.name = "Vim key: previous folder",
	.description = kBindingDescription,
	.defaultValue = u"Shift+Tab"_q,
});

base::options::option<QString> VimKeymapKeyEmojiPanelOption({
	.id = kOptionVimKeymapKeyEmojiPanel,
	.name = "Vim key: emoji panel",
	.description = kBindingDescription,
	.defaultValue = QString(),
});

base::options::option<QString> VimKeymapKeyFocusChatOption({
	.id = kOptionVimKeymapKeyFocusChat,
	.name = "Vim key: focus chat",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+H, Ctrl+\u0440"_q,
});

base::options::option<QString> VimKeymapKeyFocusEmojiOption({
	.id = kOptionVimKeymapKeyFocusEmoji,
	.name = "Vim key: focus emoji",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+L, Ctrl+\u0434"_q,
});

base::options::option<QString> VimKeymapKeyCallOption({
	.id = kOptionVimKeymapKeyCall,
	.name = "Vim key: call",
	.description = kBindingDescription,
	.defaultValue = u"Ctrl+T, Ctrl+\u0435"_q,
});

IntOptionBounds ScrollStepBounds() {
	return { kScrollStepMin, kScrollStepMax };
}

IntOptionBounds HoldScrollSpeedBounds() {
	return { kHoldScrollSpeedMin, kHoldScrollSpeedMax };
}

IntOptionBounds HintSizeBounds() {
	return { kHintSizeMin, kHintSizeMax };
}

IntOptionBounds ComposeCursorWidthBounds() {
	return { kComposeCursorWidthMin, kComposeCursorWidthMax };
}

IntOptionBounds ComposeCursorHeightBounds() {
	return { kComposeCursorHeightMin, kComposeCursorHeightMax };
}

IntOptionBounds ComposeCursorBlinkBounds() {
	return { kComposeCursorBlinkMin, kComposeCursorBlinkMax };
}

} // namespace Core::VimKeymap
