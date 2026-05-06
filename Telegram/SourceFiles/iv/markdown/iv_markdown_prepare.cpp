#include "iv/markdown/iv_markdown_prepare.h"

#include "iv/markdown/iv_markdown_prepare_blocks.h"
#include "iv/markdown/iv_markdown_prepare_formulas.h"
#include "iv/markdown/iv_markdown_prepare_native_blocks.h"
#include "iv/markdown/iv_markdown_prepare_native_richtext.h"
#include "iv/markdown/iv_markdown_prepare_state.h"

#include <QtCore/QElapsedTimer>

#include <utility>

namespace Iv::Markdown {

const MarkdownPrepareLimits &PrepareLimitsForIv() {
	static const auto result = MarkdownPrepareLimits{
		.tableRender = {
			.maxRows = 128,
			.maxColumns = 16,
			.maxCells = 1024,
		},
		.maxPreparedBlocks = 4096,
	};
	return result;
}

const MarkdownPrepareTableRenderLimits &PrepareTableRenderLimitsForIv() {
	return PrepareLimitsForIv().tableRender;
}

MarkdownArticleContent PrepareSynchronously(PrepareRequest request) {
	auto state = PrepareState();
	auto timer = QElapsedTimer();
	timer.start();
	state.request = &request;
	const auto finish = [&] {
		state.result.debug.prepareMs = int(timer.elapsed());
		return std::move(state.result);
	};

	if (!request.document) {
		state.setTerminalFailure(
			PrepareTerminalFailure::InvalidRequest,
			u"missing-document"_q);
		return finish();
	}
	if (const auto invalidStyle = InvalidStyleReason(request.dimensions)
		; !invalidStyle.isEmpty()) {
		state.setTerminalFailure(
			PrepareTerminalFailure::InvalidStyle,
			invalidStyle);
		return finish();
	}

	state.sourceUtf8 = request.document->sourceText.toUtf8();
	state.result.formulas.resize(FormulaSlotCount(*request.document));
	state.result.debug.sourceWarningCount = int(request.document->warnings.size());

	state.result.blocks = PrepareRenderData(*request.document, &state);
	if (CountPreparedBlocks(state.result.blocks.blocks)
		> PrepareLimitsForIv().maxPreparedBlocks) {
		state.setTerminalFailure(
			PrepareTerminalFailure::DocumentTooLarge,
			u"prepared-block-limit"_q);
		ClearPreparedOutput(&state.result);
		return finish();
	}
	MeasurePreparedFormulas(&state);
	if (state.result.failure.failed()) {
		ClearPreparedOutput(&state.result);
	}
	return finish();
}

NativeInstantViewPrepareResult TryPrepareNativeInstantView(
		NativeInstantViewPrepareRequest request) {
	auto state = NativeIvPrepareState();
	auto timer = QElapsedTimer();
	timer.start();
	state.result.mediaRuntime = std::move(request.mediaRuntime);
	const auto finish = [&](NativeInstantViewPrepareResultKind kind, QString reason) {
		state.result.debug.prepareMs = int(timer.elapsed());
		return NativeInstantViewPrepareResult{
			.kind = kind,
			.content = std::move(state.result),
			.debugReason = std::move(reason),
		};
	};

	if (!request.source) {
		state.setFailure(
			PrepareTerminalFailure::InvalidRequest,
			u"missing-native-iv-source"_q);
		ClearPreparedOutput(&state.result);
		return finish(
			NativeInstantViewPrepareResultKind::Failure,
			state.result.failure.debugReason);
	}

	for (const auto &photo : request.source->page.data().vphotos().v) {
		RememberNativeIvPhoto(&state, photo);
	}
	if (request.source->webpagePhoto) {
		RememberNativeIvPhoto(&state, *request.source->webpagePhoto);
	}

	if (!PrepareNativeIvBlocks(
			request.source->page.data().vblocks().v,
			&state.result.blocks.blocks,
			&state)) {
		if (state.result.failure.failed()) {
			ClearPreparedOutput(&state.result);
			return finish(
				NativeInstantViewPrepareResultKind::Failure,
				state.result.failure.debugReason);
		}
		(void)PrepareNativeIvPlainPlaceholderBlock(
			u"Unsupported Content"_q,
			&state.result.blocks.blocks);
	}
	if (CountPreparedBlocks(state.result.blocks.blocks)
		> PrepareLimitsForIv().maxPreparedBlocks) {
		state.setFailure(
			PrepareTerminalFailure::DocumentTooLarge,
			u"prepared-block-limit"_q);
		ClearPreparedOutput(&state.result);
		return finish(
			NativeInstantViewPrepareResultKind::Failure,
			state.result.failure.debugReason);
	}
	return finish(
		NativeInstantViewPrepareResultKind::Supported,
		QString());
}

} // namespace Iv::Markdown
