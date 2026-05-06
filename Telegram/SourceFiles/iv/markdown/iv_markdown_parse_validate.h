#pragma once

#include "iv/markdown/iv_markdown_parse.h"

namespace Iv::Markdown {

[[nodiscard]] ParseResult Failure(QString sourceName, QString error);
[[nodiscard]] MarkdownSourceValidationResult ValidationFailure(
	QString sourceName,
	QString error);

} // namespace Iv::Markdown
