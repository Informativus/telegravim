# Guards the application call sites that the standalone Qt harness cannot link.
get_filename_component(src "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(expect path pattern reason)
    file(READ "${src}/${path}" source)
    if (NOT source MATCHES "${pattern}")
        message(FATAL_ERROR "${reason}: ${path}")
    endif()
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
message(STATUS "28 Vim application wiring checks passed (source-level).")
