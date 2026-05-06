#pragma once

#include "iv/markdown/iv_markdown_parse_convert.h"

namespace Iv::Markdown {

void NormalizeDisplayMathBlocks(
	PreparedDocument *document,
	const QByteArray &source,
	const std::vector<int> &lineStarts);
void FinalizeDocumentSemantics(PreparedDocument *document);

} // namespace Iv::Markdown
