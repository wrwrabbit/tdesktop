#pragma once

#include "iv/markdown/iv_markdown_prepare_state.h"

namespace Iv::Markdown {

[[nodiscard]] PreparedRenderDocument PrepareRenderData(
	const PreparedDocument &document,
	PrepareState *state);

} // namespace Iv::Markdown
