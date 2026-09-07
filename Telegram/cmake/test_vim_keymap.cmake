# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_vim_keymap)
add_custom_target(test_vim_keymap_wiring
    COMMAND ${CMAKE_COMMAND} -P ${src_loc}/tests/vim_keymap_wiring_tests.cmake)
add_dependencies(test_vim_keymap test_vim_keymap_wiring)
init_target(test_vim_keymap "(tests)")

target_include_directories(test_vim_keymap PRIVATE
    ${src_loc}
    ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_precompile_headers(test_vim_keymap PRIVATE ${src_loc}/ui/ui_pch.h)
target_compile_definitions(test_vim_keymap PRIVATE
    VIM_KEYMAP_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs")

nice_target_sources(test_vim_keymap ${src_loc}
PRIVATE
    core/vim_keymap_log.cpp
    core/vim_keymap_log.h
    core/local_socket_security.cpp
    core/local_socket_security.h
    core/vim_keymap_bindings.cpp
    core/vim_keymap_bindings.h
    chat_helpers/spellchecker_bundled.cpp
    chat_helpers/spellchecker_bundled.h
    chat_helpers/spellchecker_menu.cpp
    chat_helpers/spellchecker_menu.h
    core/vim_keymap_options.cpp
    core/vim_keymap_options.h
    core/vim_keymap_config.cpp
    core/vim_keymap_config.h
    settings/settings_vim_editor.cpp
    settings/settings_vim_editor.h
    core/vim_keymap_geometry.cpp
    core/vim_keymap_geometry.h
    core/vim_keymap_widgets.cpp
    core/vim_keymap_widgets.h
    tests/test_vim_keymap.cpp
    tests/vim_config_tests.h
    tests/vim_security_tests.h
    tests/spellchecker_tests.h
    tests/vim_focus_labels_tests.h
    ui/widgets/continuous_sliders.cpp
    ui/widgets/continuous_sliders.h
    ui/widgets/discrete_sliders.cpp
    ui/widgets/discrete_sliders.h
)

target_sources(test_vim_keymap PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/gen/styles/style_vim_keymap.cpp)
add_dependencies(test_vim_keymap td_ui_styles)

qt_add_resources(test_vim_keymap spellcheck_test_resources
    PREFIX "/dictionaries"
    BASE ${res_loc}/dictionaries
    FILES
        ${res_loc}/dictionaries/ru_RU.aff
        ${res_loc}/dictionaries/ru_RU.dic.1
        ${res_loc}/dictionaries/ru_RU.dic.2
        ${res_loc}/dictionaries/en_US.aff
        ${res_loc}/dictionaries/en_US.dic)

target_link_libraries(test_vim_keymap
PRIVATE
    desktop-app::lib_base
    desktop-app::lib_crl
    desktop-app::lib_ui
    desktop-app::lib_spellcheck
    desktop-app::external_qt
)

set_target_properties(
    test_vim_keymap
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_vim_keymap)
