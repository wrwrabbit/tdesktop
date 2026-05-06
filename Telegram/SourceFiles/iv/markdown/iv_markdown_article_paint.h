#pragma once

#include "iv/markdown/iv_markdown_article_selection.h"

namespace Iv::Markdown {

void PaintBlocks(
	Painter &p,
	const std::vector<LaidOutBlock> &blocks,
	std::vector<PreparedFormulaSlot> *formulas,
	std::vector<RenderedFormula> *renderedFormulas,
	MathRenderer *renderer,
	int devicePixelRatio,
	int outerWidth,
	const MarkdownArticlePaintCaches &caches,
	const PaintSelectionState &selectionState,
	QRect clip);

} // namespace Iv::Markdown
