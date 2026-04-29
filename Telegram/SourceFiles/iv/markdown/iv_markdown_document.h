#pragma once

#include "iv/markdown/iv_markdown_common.h"

namespace Iv::Markdown {

struct PreparedDocument {
	QString sourceName;
	QString title;
	bool empty = true;
};

struct ParseResult {
	PreparedDocument document;
	QString error;
	bool ok = true;
};

[[nodiscard]] PreparedDocument EmptyDocument(QString sourceName = QString());

} // namespace Iv::Markdown
