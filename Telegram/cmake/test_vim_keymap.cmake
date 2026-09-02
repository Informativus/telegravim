# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_vim_keymap)
init_target(test_vim_keymap "(tests)")

target_include_directories(test_vim_keymap PRIVATE ${src_loc})

nice_target_sources(test_vim_keymap ${src_loc}
PRIVATE
    core/vim_keymap_bindings.cpp
    core/vim_keymap_bindings.h
    tests/test_vim_keymap.cpp
)

target_link_libraries(test_vim_keymap
PRIVATE
    desktop-app::lib_base
    desktop-app::external_qt
)

set_target_properties(
    test_vim_keymap
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_vim_keymap)
