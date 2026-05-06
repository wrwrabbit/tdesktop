#pragma once

#include "iv/markdown/iv_markdown_prepare_native_richtext.h"

#include <QtCore/QVector>

namespace Iv::Markdown {

[[nodiscard]] bool PrepareNativeIvBlocks(
	const QVector<MTPPageBlock> &blocks,
	std::vector<PreparedBlock> *result,
	NativeIvPrepareState *state);

} // namespace Iv::Markdown
