#pragma once

#include "iv/markdown/iv_markdown_document.h"

#include <QtCore/QByteArray>

namespace Iv::Markdown {

[[nodiscard]] ParseResult ParseMarkdownForIv(
	const QByteArray &source,
	ParseOptions options = {});

} // namespace Iv::Markdown
