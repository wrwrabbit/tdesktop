#include "iv/markdown/iv_markdown_parse.h"

#include <utility>

namespace Iv::Markdown {

ParseResult ParseMarkdownForIv(const QByteArray &, ParseOptions options) {
	auto result = ParseResult();
	result.document = EmptyDocument(std::move(options.sourceName));
	return result;
}

} // namespace Iv::Markdown
