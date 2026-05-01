# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_library(td_iv OBJECT)
init_non_host_target(td_iv)
add_library(tdesktop::td_iv ALIAS td_iv)

target_precompile_headers(td_iv PRIVATE ${src_loc}/iv/iv_pch.h)
nice_target_sources(td_iv ${src_loc}
PRIVATE
    iv/iv_controller.cpp
    iv/iv_controller.h
    iv/iv_data.cpp
    iv/iv_data.h
    iv/iv_delegate.h
    iv/iv_pch.h
    iv/iv_prepare.cpp
    iv/iv_prepare.h
)

nice_target_sources(td_iv ${src_loc}
PRIVATE
    iv/markdown/iv_markdown_common.cpp
    iv/markdown/iv_markdown_common.h
    iv/markdown/iv_markdown_controller.cpp
    iv/markdown/iv_markdown_controller.h
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
    iv/markdown/iv_markdown_view.cpp
    iv/markdown/iv_markdown_view.h
)

target_link_libraries(td_iv
PRIVATE
    desktop-app::external_cmark_gfm
    desktop-app::external_microtex
)

nice_target_sources(td_iv ${res_loc}
PRIVATE
    iv_html/page.css
    iv_html/page.js
)

target_include_directories(td_iv
PUBLIC
    ${src_loc}
)

target_link_libraries(td_iv
PUBLIC
    desktop-app::lib_ui
    tdesktop::td_scheme
PRIVATE
    desktop-app::lib_webview
    desktop-app::external_ada
    tdesktop::td_lang
    tdesktop::td_ui
)
