# Guards the application call sites that the standalone Qt harness cannot link.
get_filename_component(src "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set_property(GLOBAL PROPERTY vim_wiring_count 0)

function(expect path pattern reason)
    file(READ "${src}/${path}" source)
    if (NOT source MATCHES "${pattern}")
        message(FATAL_ERROR "${reason}: ${path}")
    endif()
    get_property(count GLOBAL PROPERTY vim_wiring_count)
    math(EXPR count "${count} + 1")
    set_property(GLOBAL PROPERTY vim_wiring_count ${count})
endfunction()

expect(history/history_widget.cpp
    "_cornerButtons.downClick\\(false\\)"
    "Ctrl G must call the corner button action without treating Ctrl as a mouse modifier")
expect(history/view/history_view_corner_buttons.cpp
    "void CornerButtons::downClick\\(\\) [^{]*\\{[^}]*downClick\\(base::IsCtrlPressed\\(\\)\\)"
    "Mouse and Vim must use the same down-button implementation")
expect(history/history_inner_widget.cpp
    "showCopyMediaRestriction\\(item\\)\\) \\{[^}]*\\}[\n\t ]*const auto media = photo->createMediaView\\(\\);[\n\t ]*if \\(media->setToClipboard\\(\\)"
    "Photo yank must enforce media restrictions before copying an image")
expect(history/history_inner_widget.cpp
    "media->wanted\\(Data::PhotoSize::Large, id\\)"
    "Photo yank must request full-resolution media when not downloaded")
expect(history/history_inner_widget.cpp
    "QClipboard::dataChanged"
    "A delayed photo copy must not replace a newer clipboard selection")
expect(history/history_inner_widget.cpp
    "if \\(current && !showCopyRestriction\\(current\\)[\n\t ]*&& !showCopyMediaRestriction\\(current\\)\\) \\{[\n\t ]*media->setToClipboard\\(\\)"
    "Delayed image copying must recheck restrictions after the download")
expect(history/view/history_view_list_widget.cpp
    "showCopyMediaRestriction\\(item\\)\\) \\{[^}]*\\}[\n\t ]*const auto media = photo->createMediaView\\(\\);[\n\t ]*if \\(media->setToClipboard\\(\\)"
    "Auxiliary histories must enforce media restrictions before photo copying")
expect(history/view/history_view_list_widget.cpp
    "media->wanted\\(Data::PhotoSize::Large, id\\)"
    "Auxiliary histories must also download missing photo content")
expect(history/view/history_view_list_widget.cpp
    "QClipboard::dataChanged"
    "Auxiliary photo copies must not replace newer clipboard content")
expect(history/view/history_view_list_widget.cpp
    "if \\(current && !showCopyMediaRestriction\\(current\\)\\) \\{[\n\t ]*media->setToClipboard\\(\\)"
    "Auxiliary photo copies must recheck restrictions after the download")
expect(history/view/history_view_chat_section.cpp
    "bool ChatWidget::listJumpToBottom\\(\\) \\{[^}]*_cornerButtons.downClick\\(false\\)"
    "List-based chats must use their actual down-button action")
expect(history/view/history_view_list_widget.cpp
    "if \\(!_delegate->listJumpToBottom\\(\\)\\) \\{[^}]*showAtPosition\\(Data::MaxMessagePosition"
    "Other histories must request the newest slice instead of scrolling the loaded page")
expect(history/history_inner_widget.cpp
    "gif->roundThumbRect\\(\\)[^;]*;[^}]*addClickPoint\\([\n\t ]*itemId,[\n\t ]*thumb.center\\(\\),"
    "Round-video hints must click the real thumbnail rather than its share action")
expect(history/history_inner_widget.cpp
    "mouseActionStart\\(globalPoint, Qt::LeftButton\\);[\n\t ]*mouseActionFinish\\(globalPoint, Qt::LeftButton\\)"
    "Round-video playback must retain native press/release and spoiler handling")
expect(history/history_inner_widget.cpp
    "bool HistoryInner::vimKeymapTriggerHint\\(VimKeymapHint hint\\)"
    "The selected hint must survive clearing the hint vector")
expect(history/history_inner_widget.cpp
    "link->property\\(kFastShareProperty\\).value<bool>\\(\\)"
    "Focus hints must exclude the independent share action")
expect(history/history_inner_widget.cpp
    "transcribe->geometry\\(\\)[^;]*;[^}]*add\\(transcribe->link\\(\\), rect.topLeft\\(\\), rect\\)"
    "Transcription hints must use the painted button geometry and its own action")
expect(history/history_inner_widget.cpp
    "mode == VimKeymapHintMode::ShareMessage && !item->allowsForward\\(\\)"
    "Share hints must respect message forwarding restrictions")
expect(history/history_inner_widget.cpp
    "FastShareMessage\\(_controller, item\\)"
    "Share hints must open the folder-based share grid, not the peer-list dialog")
expect(boxes/share_box.cpp
    "RegisterPreLayerKeyHandler"
    "Share search Escape must run before the layer closes")
expect(boxes/share_box.cpp
    "LeaveKeyboardInput\\(this, _select, e\\)"
    "Share search must relinquish focus before Escape closes the dialog")
expect(boxes/share_box.cpp
    "KeyboardScopeHasTextInput\\(this, nullptr\\)\\) \\{[\n\t ]*return false;[\n\t ]*\\}[\n\t ]*switch"
    "Share-dialog Vim navigation must not consume characters while typing")
expect(boxes/share_box.cpp
    "StickerGridAction::MoveDown:[\n\t ]*_inner->activateSkipRow\\(1\\)"
    "Share-dialog j must move its own recipient grid")
expect(boxes/share_box.cpp
    "QApplication::focusWidget\\(\\) != this\\) \\{[\n\t ]*return false;"
    "Enter on a footer button must not choose a recipient instead")
expect(ui/widgets/chat_filters_tabs_strip.cpp
    "CreateKeyboardTabTarget\\([\n\t ]*slider"
    "Folder sections must participate in the shared Tab focus cycle")
expect(ui/widgets/chat_filters_tabs_strip.cpp
    "if \\(i >= count\\) \\{[\n\t ]*return;"
    "Folder target resizing must tolerate removal of a section")
expect(media/view/media_view_overlay_widget.cpp
    "Bindings::MediaPlaybackSpeed"
    "Video speed key bindings must reach the actual media viewer")
expect(media/view/media_view_overlay_widget.cpp
    "controls->updatePlaybackSpeed\\(\\*speed\\)"
    "Video speed shortcuts must update the existing playback controls")
expect(media/player/media_player_widget.cpp
    "RegisterGlobalFocusRoot\\(this\\)"
    "The top playback bar must contribute controls to global focus hints")
expect(core/vim_keymap.cpp
    "return Bindings::ShortestHintLabel\\(index, total, CurrentHintAlphabet\\(\\)\\)"
    "All hint consumers must share the shortest prefix-free label allocator")
expect(history/history_inner_widget.cpp
    "vimKeymapAddWidgetHints\\(\\);[\n\t ]*for \\(const auto view : accessibleElements\\(\\)\\)"
    "Player controls must receive short labels before message links")
expect(history/history_inner_widget.cpp
    "HintLabel\\(offset \\+ index, total\\)"
    "Player and message targets must share one alphabet without collisions")
expect(history/history_inner_widget.cpp
    "navigation->setHintPrefix\\(_vimKeymapHintPrefix\\)"
    "Partial multi-letter input must filter player and message badges together")
expect(history/history_inner_widget.cpp
    "KeyboardNavigation::Get\\(root\\)->focusTarget\\(target\\)"
    "Choosing a player hint must focus its actual control")
expect(core/vim_keymap.cpp
    "if \\(globalRoot\\)[^{]*\\{[^;]*KeyboardNavigation::Find\\(globalRoot\\)"
    "Native player control keys must be routed before chat handlers")
expect(core/vim_keymap.cpp
    "if \\(const auto navigation = ScrollNavigationKey\\(e\\)\\)"
    "Modal scrolling must not depend on the composer mode behind the layer")
expect(core/vim_keymap.cpp
    "return NormalMode\\(\\) \\? ScrollNavigationKey\\(e\\) : std::nullopt"
    "Chat j/k must keep their normal-mode guard for insert-mode typing")
expect(settings/sections/settings_main.cpp
    "_editor->checkBeforeClose"
    "The settings section must protect unsaved JSON on close")
expect(settings/sections/settings_main.cpp
    "return !_editor->dirty"
    "Outside clicks must not discard JSON drafts")
expect(settings/sections/settings_main.cpp
    "close\\(\\);[\n\t ]*discard\\(\\);"
    "Discard must close the native confirmation before changing mode")
expect(settings/settings_experimental.cpp
    "for \\(const auto &field : ConfigOptions\\(\\)\\)"
    "UI and JSON must expose the same settings registry")
expect(settings/settings_experimental.cpp
    "Bindings::ValidBindings\\(value\\)"
    "UI edits must validate the same shortcut grammar as JSON")
expect(core/vim_keymap.cpp
    "LegacyDefaultsMigrated \\|\\| VimKeymapDefaultsMigratedOption.value\\(\\)"
    "Explicit JSON values must survive migration on the next launch")

file(READ "${src}/core/vim_keymap_options.cpp" options)
string(REGEX MATCHALL "const char (kOptionVimKeymap[a-zA-Z]*)\\[\\]" keys "${options}")
foreach(key IN LISTS keys)
    string(REGEX REPLACE "const char ([a-zA-Z]*)\\[\\]" "\\1" key "${key}")
    expect(core/vim_keymap_config.cpp
        "\\{ ${key},"
        "Every registered Vim setting must have JSON support and documentation")
endforeach()

expect(core/vim_keymap.cpp
    "App\\(\\).passcodeLocked\\(\\) \\|\\| \\(active && active->locked\\(\\)\\)"
    "Vim dispatch must reject locked application and session windows")
expect(core/vim_keymap.cpp
    "KeyLog.setSuppressed\\(wasSuppressed \\|\\| KeyboardInputActive\\(object\\)\\)"
    "Input privacy must be captured before handlers can move focus")
expect(core/application.cpp
    "void Application::lockByPasscode\\(\\) \\{[\n\t ]*VimKeymap::ClearKeyLog\\(\\)"
    "Locking must clear prior keyboard diagnostics")
expect(core/application.cpp
    "void Application::logout\\(Main::Account \\*account\\) \\{[\n\t ]*VimKeymap::ClearKeyLog\\(\\)"
    "Logout must clear keyboard diagnostics")
foreach(history IN ITEMS history/history_inner_widget.cpp history/view/history_view_list_widget.cpp)
    expect(${history}
        "Core::App\\(\\).passcodeLockChanges\\(\\)[^{]*\\{[^}]*_vimKeymapPhotoCopyLifetime.destroy\\(\\)"
        "Locking must cancel delayed image copies permanently")
    expect(${history}
        "currentMedia->photo\\(\\) != photo"
        "A changed message must not copy its old photo after download")
endforeach()
expect(history/history_inner_widget.cpp
    "void HistoryInner::viewRemoved[^{]*[{][^}]*}[^{]*[{][^}]*vimKeymapClearHints"
    "Destroyed message views must invalidate captured hint actions")
expect(core/external_control.cpp
    "App\\(\\).passcodeLocked\\(\\)[^{]*\\{[^}]*application is locked"
    "Local control must not expose settings while locked")
expect(window/window_session_controller.cpp
    "item->forbidsSaving\\(\\)[^;]*allowsForwarding"
    "External viewers must respect protected and expiring media")
expect(menu/menu_item_download_files.cpp
    "item->forbidsSaving\\(\\)"
    "Bulk exports must exclude self-destructing media")
expect(core/sandbox.cpp
    "if [(]!LocalSocketPeerAllowed[(]client[)][)] [^{]*[{][^}]*client->abort[(][)];[^}]*continue;"
    "Unauthenticated local peers must be rejected before command processing")
expect(core/sandbox.cpp
    "QLocalServer::UserAccessOption"
    "Filesystem and Windows local sockets must restrict access to the user")
expect(core/sandbox.cpp
    "cmd == \"quit\"[^}]*}[ \t,\n]*Qt::QueuedConnection"
    "Local quit must finish its socket dispatch before application teardown")
expect(storage/file_download.cpp
    "_data.left[(]_loadSize[)]"
    "Image reads must be bounded by the available byte array")
get_property(count GLOBAL PROPERTY vim_wiring_count)
message(STATUS "${count} Vim application wiring checks passed (source-level).")
