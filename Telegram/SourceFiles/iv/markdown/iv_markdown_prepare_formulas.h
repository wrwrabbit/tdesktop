#pragma once

#include "iv/markdown/iv_markdown_prepare_state.h"

namespace Iv::Markdown {

[[nodiscard]] int CountPreparedBlocks(const std::vector<PreparedBlock> &blocks);
[[nodiscard]] int FormulaSlotCount(const PreparedDocument &document);
void MeasurePreparedFormulas(PrepareState *state);

} // namespace Iv::Markdown
