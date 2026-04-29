#include "iv/markdown/iv_markdown_document.h"

#include <utility>

namespace Iv::Markdown {

PreparedDocument EmptyDocument(QString sourceName) {
	auto document = PreparedDocument();
	document.sourceName = std::move(sourceName);
	return document;
}

} // namespace Iv::Markdown
