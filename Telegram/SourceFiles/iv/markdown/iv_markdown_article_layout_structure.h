#pragma once

#include "iv/markdown/iv_markdown_article_layout_blocks.h"

namespace Iv::Markdown {

[[nodiscard]] int LayoutBlocks(
	const std::vector<PreparedBlock> &prepared,
	std::vector<PreparedFormulaSlot> *formulas,
	std::vector<RenderedFormula> *renderedFormulas,
	MathRenderer *renderer,
	InlineFormulaObjectCache *inlineFormulaObjects,
	const std::shared_ptr<MediaRuntime> &mediaRuntime,
	std::vector<LaidOutBlock> *blocks,
	const style::Markdown &markdown,
	int left,
	int top,
	int width,
	LayoutContext context);

} // namespace Iv::Markdown
