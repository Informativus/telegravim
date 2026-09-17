if (APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 10.15)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15)
    set_target_properties(Telegram PROPERTIES
        XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET 10.15)
endif()

function(telegram_add_whisper)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.16)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(WHISPER_ALL_WARNINGS OFF)
    set(WHISPER_BUILD_TESTS OFF)
    set(WHISPER_BUILD_EXAMPLES OFF)
    set(WHISPER_BUILD_SERVER OFF)
    set(GGML_NATIVE OFF)
    set(GGML_BLAS OFF)
    set(GGML_OPENMP OFF)
    set(GGML_CCACHE OFF)
    set(GGML_BACKEND_DL OFF)
    set(GGML_METAL_EMBED_LIBRARY ON)
    add_subdirectory(ThirdParty/whisper.cpp EXCLUDE_FROM_ALL)
    foreach(target whisper ggml ggml-base ggml-cpu ggml-metal)
        if (TARGET ${target})
            if (APPLE)
                set_target_properties(${target} PROPERTIES
                    XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET
                        ${CMAKE_OSX_DEPLOYMENT_TARGET})
            endif()
            if (MSVC)
                target_compile_options(${target} PRIVATE /w)
            else()
                target_compile_options(${target} PRIVATE -w)
            endif()
        endif()
    endforeach()
endfunction()
telegram_add_whisper()

add_library(lib_transcription STATIC)
add_library(tdesktop::lib_transcription ALIAS lib_transcription)
init_target(lib_transcription)
target_sources(lib_transcription PRIVATE
    ${src_loc}/media/transcription/transcription_engine.cpp
    ${src_loc}/media/transcription/transcription_engine.h
)
target_include_directories(lib_transcription PUBLIC ${src_loc})
target_link_libraries(lib_transcription
    PUBLIC desktop-app::lib_base desktop-app::external_qt
    PRIVATE whisper desktop-app::external_ffmpeg
)

if (DESKTOP_APP_TEST_APPS)
    add_executable(test_local_transcription
        ${src_loc}/tests/test_local_transcription.cpp)
    init_target(test_local_transcription "(tests)")
    target_link_libraries(test_local_transcription PRIVATE tdesktop::lib_transcription)
    set_target_properties(test_local_transcription PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

if (APPLE)
    add_custom_command(TARGET Telegram POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:Telegram>/../Resources/licenses/whisper"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/whisper.cpp/LICENSE"
            "$<TARGET_FILE_DIR:Telegram>/../Resources/licenses/whisper/LICENSE"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/licenses/whisper-model.txt"
            "$<TARGET_FILE_DIR:Telegram>/../Resources/licenses/whisper/model-LICENSE")
endif()
