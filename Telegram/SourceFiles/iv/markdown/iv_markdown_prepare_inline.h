#pragma once

#include "iv/markdown/iv_markdown_prepare_state.h"

namespace Iv::Markdown {

void PrepareInlineRichText(
	const MarkdownNode &node,
	int textSize,
	int renderWidthCap,
	int renderHeightCap,
	QString *blockAnchorId,
	TextWithEntities *text,
	std::vector<PreparedLink> *links,
	PrepareState *state);

} // namespace Iv::Markdown
