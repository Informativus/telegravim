# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_vim_keymap)
init_target(test_vim_keymap "(tests)")

target_include_directories(test_vim_keymap PRIVATE
    ${src_loc}
    ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_precompile_headers(test_vim_keymap PRIVATE ${src_loc}/ui/ui_pch.h)

nice_target_sources(test_vim_keymap ${src_loc}
PRIVATE
    core/vim_keymap_bindings.cpp
    core/vim_keymap_bindings.h
    core/vim_keymap_geometry.cpp
    core/vim_keymap_geometry.h
    core/vim_keymap_widgets.cpp
    core/vim_keymap_widgets.h
    tests/test_vim_keymap.cpp
    tests/vim_focus_labels_tests.h
    ui/widgets/continuous_sliders.cpp
    ui/widgets/continuous_sliders.h
)

target_sources(test_vim_keymap PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/gen/styles/style_vim_keymap.cpp)
add_dependencies(test_vim_keymap td_ui_styles)

target_link_libraries(test_vim_keymap
PRIVATE
    desktop-app::lib_base
    desktop-app::lib_crl
    desktop-app::lib_ui
    desktop-app::external_qt
)

set_target_properties(
    test_vim_keymap
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_vim_keymap)
