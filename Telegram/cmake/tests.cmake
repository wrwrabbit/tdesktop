# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_text WIN32)
init_target(test_text "(tests)")

target_include_directories(test_text PRIVATE ${src_loc})

nice_target_sources(test_text ${src_loc}
PRIVATE
    tests/test_main.cpp
    tests/test_main.h
    tests/test_text.cpp
)

nice_target_sources(test_text ${res_loc}
PRIVATE
    qrc/emoji_1.qrc
    qrc/emoji_2.qrc
    qrc/emoji_3.qrc
    qrc/emoji_4.qrc
    qrc/emoji_5.qrc
    qrc/emoji_6.qrc
    qrc/emoji_7.qrc
    qrc/emoji_8.qrc
)

target_link_libraries(test_text
PRIVATE
    desktop-app::lib_base
    desktop-app::lib_crl
    desktop-app::lib_ui
    desktop-app::external_qt
    desktop-app::external_qt_static_plugins
)

set_target_properties(test_text PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_text)

target_prepare_qrc(test_text)

if (TDESKTOP_NATIVE_MARKDOWN_IV)
    add_executable(test_markdown_iv)
    init_target(test_markdown_iv "(tests)")

    target_precompile_headers(test_markdown_iv PRIVATE ${src_loc}/iv/iv_pch.h)

    target_include_directories(test_markdown_iv PRIVATE
        ${src_loc}
        ${CMAKE_BINARY_DIR}/Telegram/gen
        ${CMAKE_BINARY_DIR}/Telegram/lib_ui/gen
    )

    nice_target_sources(test_markdown_iv ${src_loc}
    PRIVATE
        tests/test_markdown_iv.cpp
        iv/markdown/iv_markdown_common.cpp
        iv/markdown/iv_markdown_common.h
        iv/markdown/iv_markdown_document.cpp
        iv/markdown/iv_markdown_document.h
        iv/markdown/iv_markdown_math.cpp
        iv/markdown/iv_markdown_math.h
        iv/markdown/iv_markdown_math_renderer.cpp
        iv/markdown/iv_markdown_math_renderer.h
        iv/markdown/iv_markdown_microtex.cpp
        iv/markdown/iv_markdown_microtex.h
        iv/markdown/iv_markdown_parse.cpp
        iv/markdown/iv_markdown_parse.h
        iv/markdown/iv_markdown_prepare.cpp
        iv/markdown/iv_markdown_prepare.h
    )

    target_sources(test_markdown_iv PRIVATE
        ${CMAKE_BINARY_DIR}/Telegram/gen/styles/style_iv.cpp
        ${CMAKE_BINARY_DIR}/Telegram/lib_ui/gen/styles/palette.cpp
        ${CMAKE_BINARY_DIR}/Telegram/lib_ui/gen/styles/style_basic.cpp
        ${CMAKE_BINARY_DIR}/Telegram/lib_ui/gen/styles/style_widgets.cpp
    )

    target_compile_definitions(test_markdown_iv
    PRIVATE
        TDESKTOP_NATIVE_MARKDOWN_IV
    )

    target_link_libraries(test_markdown_iv
    PRIVATE
        desktop-app::external_cmark_gfm
        desktop-app::external_microtex
        desktop-app::external_qt
        desktop-app::external_qt_static_plugins
        desktop-app::lib_base
        desktop-app::lib_crl
        desktop-app::lib_tl
        desktop-app::lib_ui
    )

    set_target_properties(test_markdown_iv PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

    add_dependencies(Telegram test_markdown_iv)
    add_dependencies(test_markdown_iv lib_ui_styles td_scheme_scheme td_ui_styles)
endif()
