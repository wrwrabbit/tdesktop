#pragma once

#include "iv/markdown/iv_markdown_prepare_state.h"

namespace Iv::Markdown {

struct PreparedIvRichText {
	TextWithEntities text;
	std::vector<PreparedLink> links;
};

void RememberNativeIvPhoto(
	NativeIvPrepareState *state,
	const MTPPhoto &photo);
[[nodiscard]] bool PrepareNativeIvPlainPlaceholderBlock(
	QString label,
	std::vector<PreparedBlock> *result);
[[nodiscard]] bool PrepareNativeIvPlaceholderBlock(
	QString label,
	const MTPPageCaption &caption,
	std::vector<PreparedBlock> *result,
	NativeIvPrepareState *state);
[[nodiscard]] bool PrepareNativeIvPhotoBlock(
	const MTPDpageBlockPhoto &data,
	std::vector<PreparedBlock> *result,
	NativeIvPrepareState *state);
[[nodiscard]] bool PrepareNativeIvRichText(
	const MTPRichText &text,
	PreparedIvRichText *result,
	QString *blockAnchorId,
	NativeIvPrepareState *state);
[[nodiscard]] bool AppendPreparedIvRichBlock(
	std::vector<PreparedBlock> *result,
	PreparedBlockKind kind,
	int headingLevel,
	PreparedIvRichText prepared,
	QString anchorId = QString(),
	bool allowEmpty = false);

} // namespace Iv::Markdown
