/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_article_layout_structure.h"
#include "iv/markdown/iv_markdown_article_text.h"

#include "lang/lang_keys.h"
#include "styles/style_iv.h"

#include <algorithm>

namespace Iv::Markdown {
namespace {

[[nodiscard]] int LeafFirstLineBaseline(
		const Ui::Text::String &leaf,
		const QRect &textRect,
		const style::TextStyle &style) {
	const auto lines = leaf.countLinesGeometry(textRect.width(), true);
	return textRect.y() + (lines.empty()
		? TextLineBaseline(style)
		: lines.front().baseline);
}

[[nodiscard]] int MarkdownBodyBaseline(
		int top,
		const style::Markdown &st) {
	return TextLineBaseline(st.body, top);
}

void SetEditPlaceholderLeaf(
		LaidOutBlock *block,
		const QString &text,
		const style::TextStyle &textStyle,
		int width) {
	if (text.isEmpty()) {
		return;
	}
	block->placeholderText = text;
	block->placeholderLeaf = Ui::Text::String(TextMinResizeWidth(width));
	block->placeholderLeaf.setMarkedText(
		textStyle,
		TextWithEntities::Simple(block->placeholderText),
		TextParseOptions{
			TextParseMultiline,
			0,
			0,
			Qt::LayoutDirectionAuto,
		});
}

[[nodiscard]] int BlockBottom(const LaidOutBlock &block) {
	return block.outer.y() + block.outer.height();
}

[[nodiscard]] bool UsesMediaBand(PreparedBlockKind kind) {
	switch (kind) {
	case PreparedBlockKind::Photo:
	case PreparedBlockKind::Video:
	case PreparedBlockKind::Audio:
	case PreparedBlockKind::Map:
	case PreparedBlockKind::Channel:
	case PreparedBlockKind::GroupedMedia:
	case PreparedBlockKind::RelatedArticle:
	case PreparedBlockKind::Rule:
	case PreparedBlockKind::Details:
		return true;
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Thinking:
	case PreparedBlockKind::Heading:
	case PreparedBlockKind::CodeBlock:
	case PreparedBlockKind::List:
	case PreparedBlockKind::ListItem:
	case PreparedBlockKind::Quote:
	case PreparedBlockKind::DisplayMath:
	case PreparedBlockKind::Table:
	case PreparedBlockKind::Placeholder:
	case PreparedBlockKind::EmbedPost:
		return false;
	}
	return false;
}

[[nodiscard]] QRect PaddedBand(
		int left,
		int width,
		QMargins padding) {
	return QRect(
		left + padding.left(),
		0,
		std::max(width - padding.left() - padding.right(), 1),
		0);
}

[[nodiscard]] int PaddedWidth(
		int width,
		QMargins padding) {
	return std::max(width - padding.left() - padding.right(), 1);
}

[[nodiscard]] QRect BlockBand(
		PreparedBlockKind kind,
		const style::Markdown &st,
		int left,
		int width,
		LayoutContext context) {
	if (!context.useArticleBands) {
		return QRect(left, 0, std::max(width, 1), 0);
	}
	return UsesMediaBand(kind)
		? PaddedBand(left, width, st.mediaPadding)
		: PaddedBand(left, width, st.textPadding);
}

[[nodiscard]] int BlockBandWidth(
		PreparedBlockKind kind,
		const style::Markdown &st,
		int width,
		LayoutContext context) {
	if (!context.useArticleBands) {
		return std::max(width, 1);
	}
	return UsesMediaBand(kind)
		? PaddedWidth(width, st.mediaPadding)
		: PaddedWidth(width, st.textPadding);
}

[[nodiscard]] bool IsRelatedArticlesHeader(
		const PreparedBlock &block,
		const PreparedBlock *next) {
	return next
		&& (block.kind == PreparedBlockKind::Heading)
		&& (next->kind == PreparedBlockKind::RelatedArticle);
}

[[nodiscard]] const PreparedBlock *NextVisibleBlock(
		const std::vector<PreparedBlock> &blocks,
		int index) {
	for (auto i = index + 1, count = int(blocks.size()); i != count; ++i) {
		if (!IsAnchorOnlyBlock(blocks[i])) {
			return &blocks[i];
		}
	}
	return nullptr;
}

void PrepareNestedContext(
		LayoutContext *context,
		int left,
		int width) {
	context->useArticleBands = false;
	context->articleLeft = left;
	context->articleWidth = std::max(width, 1);
}

[[nodiscard]] bool FirstLineComesFromChildren(const LaidOutBlock &block) {
	switch (block.kind) {
	case PreparedBlockKind::List:
	case PreparedBlockKind::ListItem:
	case PreparedBlockKind::Quote:
	case PreparedBlockKind::EmbedPost:
		return true;
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Thinking:
	case PreparedBlockKind::Heading:
	case PreparedBlockKind::CodeBlock:
	case PreparedBlockKind::Rule:
	case PreparedBlockKind::DisplayMath:
	case PreparedBlockKind::Table:
	case PreparedBlockKind::Photo:
	case PreparedBlockKind::Video:
	case PreparedBlockKind::Audio:
	case PreparedBlockKind::Map:
	case PreparedBlockKind::Channel:
	case PreparedBlockKind::GroupedMedia:
	case PreparedBlockKind::RelatedArticle:
	case PreparedBlockKind::Placeholder:
	case PreparedBlockKind::Details:
		return false;
	}
	return false;
}

[[nodiscard]] int ResolveFirstDisplayedLineBaseline(
		const LaidOutBlock &block,
		const style::Markdown &st) {
	if (block.firstLineBaseline >= 0) {
		return block.firstLineBaseline;
	}
	if (FirstLineComesFromChildren(block)) {
		for (const auto &child : block.children) {
			if (child.outer.height() <= 0) {
				continue;
			}
			return ResolveFirstDisplayedLineBaseline(child, st);
		}
	}
	return MarkdownBodyBaseline(block.outer.y(), st);
}

void RefreshLogicalGeometry(LaidOutBlock *block) {
	block->logicalGeometry = {
		.outer = block->outer,
		.headerRect = block->headerRect,
		.bodyRect = block->bodyRect,
		.iconRect = block->iconRect,
		.textRect = block->textRect,
		.labelRect = block->labelRect,
		.subtitleRect = block->subtitleRect,
		.actionRect = block->actionRect,
		.markerRect = block->markerRect,
		.contentRect = block->contentRect,
		.formulaRect = block->formulaRect,
		.tableRect = block->tableRect,
		.mediaRect = block->mediaRect,
		.thumbnailRect = block->thumbnailRect,
		.markerCenter = block->markerCenter,
	};
}

struct WidthAnalysisNode {
	std::vector<WidthAnalysisNode> children;
	int contentMinimumWidth = 1;
	int outerMinimumWidth = 1;
	int scrollOwnerMinimumWidth = 1;
	bool ownerEligible = false;
	bool chosenScrollOwner = false;
	bool subtreeHasScrollOwner = false;
};

[[nodiscard]] int MaxChildOuterMinimumWidth(
		const std::vector<WidthAnalysisNode> &children) {
	auto result = 1;
	for (const auto &child : children) {
		result = std::max(result, child.outerMinimumWidth);
	}
	return result;
}

void FinalizeOwnerSelection(
		WidthAnalysisNode *analysis,
		int availableWidth) {
	analysis->subtreeHasScrollOwner = false;
	for (const auto &child : analysis->children) {
		analysis->subtreeHasScrollOwner |= child.subtreeHasScrollOwner;
	}
	analysis->chosenScrollOwner = !analysis->subtreeHasScrollOwner
		&& analysis->ownerEligible
		&& (analysis->scrollOwnerMinimumWidth > std::max(availableWidth, 1));
	analysis->subtreeHasScrollOwner |= analysis->chosenScrollOwner;
}

[[nodiscard]] int OrderedMarkerMinimumWidth(
		const PreparedBlock &prepared,
		const style::Markdown &st) {
	return std::max(st.body.font->width(ListMarkerText(prepared)), 1);
}

[[nodiscard]] int ListItemMarkerMinimumWidth(
		const PreparedBlock &prepared,
		const style::Markdown &st) {
	if (prepared.taskState != TaskState::None) {
		return std::max(st.list.taskCheck.diameter, 1);
	} else if (prepared.listKind == ListKind::Ordered) {
		return OrderedMarkerMinimumWidth(prepared, st);
	}
	return std::max(st.list.markerWidth, 1);
}

[[nodiscard]] QMargins DetailsHeaderPadding(
		LayoutContext context,
		const style::Markdown &st) {
	const auto &details = st.details;
	return context.useArticleBands
		? QMargins(
			st.textPadding.left(),
			details.headerPadding.top(),
			st.textPadding.right(),
			details.headerPadding.bottom())
		: details.headerPadding;
}

[[nodiscard]] QString DetailsStateText(bool open) {
	return open
		? tr::lng_iv_details_state_expanded(tr::now)
		: tr::lng_iv_details_state_collapsed(tr::now);
}

[[nodiscard]] int DetailsStateReserveWidth(const style::Markdown &st) {
	return std::max({
		st.details.summaryStyle.font->width(DetailsStateText(false)),
		st.details.summaryStyle.font->width(DetailsStateText(true)),
		1,
	});
}

[[nodiscard]] QMargins DetailsBodyPadding(
		LayoutContext context,
		const style::Markdown &st) {
	const auto &details = st.details;
	return context.useArticleBands
		? QMargins(
			st.textPadding.left(),
			details.bodyPadding.top(),
			st.textPadding.right(),
			details.bodyPadding.bottom())
		: details.bodyPadding;
}

[[nodiscard]] const WidthAnalysisNode *NextActiveScrollOwner(
		const WidthAnalysisNode &analysis,
		const WidthAnalysisNode *activeScrollOwner) {
	return activeScrollOwner
		? activeScrollOwner
		: analysis.chosenScrollOwner
		? &analysis
		: nullptr;
}

[[nodiscard]] bool IsActiveScrollOwner(
		const WidthAnalysisNode &analysis,
		const WidthAnalysisNode *activeScrollOwner) {
	return (activeScrollOwner == &analysis);
}

[[nodiscard]] int LogicalOuterWidth(
		const WidthAnalysisNode &analysis,
		const WidthAnalysisNode *activeScrollOwner,
		int logicalWidth) {
	const auto result = std::max(logicalWidth, 1);
	return IsActiveScrollOwner(analysis, activeScrollOwner)
		? std::max(result, analysis.scrollOwnerMinimumWidth)
		: result;
}

[[nodiscard]] int ScrollbarReserveHeight(
		bool scrollOwner,
		int horizontalScrollMax,
		const style::Markdown &st) {
	return (scrollOwner && (horizontalScrollMax > 0))
		? (st.table.scrollbarSkip + st.table.scrollbarHeight)
		: 0;
}

[[nodiscard]] WidthAnalysisNode AnalyzeBlock(
	const PreparedBlock &prepared,
	const std::vector<PreparedFormulaSlot> &formulas,
	const style::Markdown &st,
	int width,
	LayoutContext context);

[[nodiscard]] std::vector<WidthAnalysisNode> AnalyzeBlocks(
		const std::vector<PreparedBlock> &prepared,
		const std::vector<PreparedFormulaSlot> &formulas,
		const style::Markdown &st,
		int width,
		LayoutContext context) {
	auto result = std::vector<WidthAnalysisNode>();
	result.reserve(prepared.size());
	for (auto i = 0, count = int(prepared.size()); i != count; ++i) {
		const auto &block = prepared[i];
		const auto next = NextVisibleBlock(prepared, i);
		auto blockWidth = BlockBandWidth(block.kind, st, width, context);
		if (IsRelatedArticlesHeader(block, next)) {
			blockWidth = std::max(
				width
					- st.relatedArticle.headerPadding.left()
					- st.relatedArticle.headerPadding.right(),
				1);
		}
		result.push_back(AnalyzeBlock(
			block,
			formulas,
			st,
			blockWidth,
			context));
	}
	return result;
}

[[nodiscard]] WidthAnalysisNode AnalyzeBlock(
		const PreparedBlock &prepared,
		const std::vector<PreparedFormulaSlot> &formulas,
		const style::Markdown &st,
		int width,
		LayoutContext context) {
	auto analysis = WidthAnalysisNode();
	const auto availableWidth = std::max(width, 1);
	switch (prepared.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Thinking:
	case PreparedBlockKind::Heading:
		analysis.contentMinimumWidth = FlowBlockMinimumWidth(prepared, st);
		analysis.outerMinimumWidth = analysis.contentMinimumWidth;
		analysis.scrollOwnerMinimumWidth = analysis.outerMinimumWidth;
		analysis.ownerEligible = !IsAnchorOnlyBlock(prepared);
		break;
	case PreparedBlockKind::CodeBlock:
		analysis.contentMinimumWidth = CodeBlockMinimumWidth(st);
		analysis.outerMinimumWidth = analysis.contentMinimumWidth;
		analysis.scrollOwnerMinimumWidth = analysis.outerMinimumWidth;
		analysis.ownerEligible = true;
		break;
	case PreparedBlockKind::DisplayMath:
		analysis.contentMinimumWidth = DisplayMathMinimumWidth(
			prepared,
			formulas,
			st);
		analysis.outerMinimumWidth = analysis.contentMinimumWidth;
		analysis.scrollOwnerMinimumWidth = analysis.outerMinimumWidth;
		analysis.ownerEligible = true;
		break;
	case PreparedBlockKind::Table:
		analysis.contentMinimumWidth = TableBlockMinimumWidth(prepared, st);
		analysis.outerMinimumWidth = analysis.contentMinimumWidth;
		analysis.scrollOwnerMinimumWidth = analysis.outerMinimumWidth;
		analysis.ownerEligible = true;
		break;
	case PreparedBlockKind::List: {
		const auto depthDelta = std::max(
			prepared.visualDepth - context.listDepth,
			0);
		const auto overhead = depthDelta * st.list.indent;
		const auto childWidth = std::max(availableWidth - overhead, 1);
		auto childContext = context;
		childContext.listDepth = prepared.visualDepth;
		childContext.tightList = false;
		PrepareNestedContext(&childContext, 0, childWidth);
		analysis.children = AnalyzeBlocks(
			prepared.children,
			formulas,
			st,
			childWidth,
			childContext);
		analysis.contentMinimumWidth = MaxChildOuterMinimumWidth(analysis.children);
		analysis.outerMinimumWidth = overhead + analysis.contentMinimumWidth;
		analysis.scrollOwnerMinimumWidth = analysis.outerMinimumWidth;
		analysis.ownerEligible = !prepared.children.empty();
	} break;
	case PreparedBlockKind::ListItem: {
		const auto markerWidth = std::max(
			st.list.markerWidth,
			ListItemMarkerMinimumWidth(prepared, st));
		const auto overhead = markerWidth + st.list.markerSkip;
		const auto childWidth = std::max(availableWidth - overhead, 1);
		auto childContext = context;
		childContext.tightList = false;
		PrepareNestedContext(&childContext, 0, childWidth);
		analysis.children = AnalyzeBlocks(
			prepared.children,
			formulas,
			st,
			childWidth,
			childContext);
		analysis.contentMinimumWidth = MaxChildOuterMinimumWidth(analysis.children);
		analysis.outerMinimumWidth = overhead + analysis.contentMinimumWidth;
		analysis.scrollOwnerMinimumWidth = analysis.outerMinimumWidth;
		analysis.ownerEligible = !prepared.children.empty();
	} break;
	case PreparedBlockKind::Quote: {
		const auto depthDelta = std::max(
			prepared.visualDepth - context.quoteDepth,
			0);
		const auto overhead = depthDelta * st.quoteIndent;
		const auto padding = prepared.pullquote
			? st.pullquote.padding
			: BlockquotePadding(st.body.blockquote);
		auto childWidth = std::max(
			availableWidth - overhead - HorizontalMarginsWidth(padding),
			1);
		if (prepared.pullquote) {
			childWidth = std::min(childWidth, std::max(st.pullquote.maxWidth, 1));
		}
		auto childContext = context;
		childContext.quoteDepth = prepared.visualDepth;
		childContext.tightList = false;
		PrepareNestedContext(&childContext, 0, childWidth);
		analysis.children = AnalyzeBlocks(
			prepared.children,
			formulas,
			st,
			childWidth,
			childContext);
		analysis.contentMinimumWidth = MaxChildOuterMinimumWidth(analysis.children);
		const auto visibleContentWidth = prepared.pullquote
			? std::min(
				analysis.contentMinimumWidth,
				std::max(st.pullquote.maxWidth, 1))
			: analysis.contentMinimumWidth;
		analysis.outerMinimumWidth = overhead
			+ HorizontalMarginsWidth(padding)
			+ visibleContentWidth;
		analysis.scrollOwnerMinimumWidth = analysis.outerMinimumWidth;
		analysis.ownerEligible = !prepared.children.empty();
	} break;
	case PreparedBlockKind::Details: {
		const auto headerPadding = DetailsHeaderPadding(context, st);
		const auto bodyPadding = DetailsBodyPadding(context, st);
		const auto iconWidth = st.details.icon.empty()
			? 0
			: TextLineHeight(st.details.summaryStyle);
		const auto iconSkip = iconWidth ? st.details.iconSkip : 0;
		const auto actionWidth = context.editMode
			? DetailsStateReserveWidth(st)
			: 0;
		const auto actionSkip = actionWidth ? st.details.stateSkip : 0;
		const auto headerMinimumWidth = HorizontalMarginsWidth(headerPadding)
			+ iconWidth
			+ iconSkip
			+ actionSkip
			+ actionWidth
			+ ReadableTextMinWidth(st.details.summaryStyle);
		auto bodyMinimumWidth = 1;
		if (!prepared.collapsed) {
			const auto childWidth = std::max(
				availableWidth - HorizontalMarginsWidth(bodyPadding),
				1);
			auto childContext = context;
			PrepareNestedContext(&childContext, 0, childWidth);
			analysis.children = AnalyzeBlocks(
				prepared.children,
				formulas,
				st,
				childWidth,
				childContext);
			analysis.contentMinimumWidth = MaxChildOuterMinimumWidth(
				analysis.children);
			bodyMinimumWidth = ContainerMinimumWidth(
				analysis.contentMinimumWidth,
				bodyPadding);
		}
		analysis.outerMinimumWidth = std::max(
			headerMinimumWidth,
			bodyMinimumWidth);
		analysis.scrollOwnerMinimumWidth = bodyMinimumWidth;
		analysis.ownerEligible = !prepared.collapsed
			&& !prepared.children.empty();
	} break;
	case PreparedBlockKind::EmbedPost: {
		const auto &style = st.embedPost;
		const auto contentOverhead = style.accentWidth
			+ style.accentSkip
			+ style.padding.left()
			+ style.padding.right();
		const auto avatarWidth = prepared.embedPost.authorPhotoId
			? std::max(style.avatarSize, 1)
			: 0;
		const auto headerGap = avatarWidth ? style.headerGap : 0;
		auto headerTextWidth = 1;
		if (!prepared.embedPost.author.isEmpty()) {
			headerTextWidth = std::max(
				headerTextWidth,
				ReadableTextMinWidth(style.authorStyle));
		}
		if (!prepared.embedPost.dateText.isEmpty()) {
			headerTextWidth = std::max(
				headerTextWidth,
				ReadableTextMinWidth(style.dateStyle));
		}
		const auto childWidth = std::max(availableWidth - contentOverhead, 1);
		auto childContext = context;
		PrepareNestedContext(&childContext, 0, childWidth);
		analysis.children = AnalyzeBlocks(
			prepared.children,
			formulas,
			st,
			childWidth,
			childContext);
		analysis.contentMinimumWidth = MaxChildOuterMinimumWidth(analysis.children);
		analysis.scrollOwnerMinimumWidth = contentOverhead
			+ analysis.contentMinimumWidth;
		analysis.outerMinimumWidth = std::max(
			contentOverhead + avatarWidth + headerGap + headerTextWidth,
			analysis.scrollOwnerMinimumWidth);
		analysis.ownerEligible = !prepared.children.empty();
	} break;
	case PreparedBlockKind::Rule:
	case PreparedBlockKind::Photo:
	case PreparedBlockKind::Video:
	case PreparedBlockKind::Audio:
	case PreparedBlockKind::Map:
	case PreparedBlockKind::Channel:
	case PreparedBlockKind::GroupedMedia:
	case PreparedBlockKind::RelatedArticle:
	case PreparedBlockKind::Placeholder:
		analysis.contentMinimumWidth = 1;
		analysis.outerMinimumWidth = 1;
		analysis.scrollOwnerMinimumWidth = analysis.outerMinimumWidth;
		analysis.ownerEligible = false;
		break;
	}
	FinalizeOwnerSelection(&analysis, availableWidth);
	return analysis;
}

[[nodiscard]] LaidOutBlock LayoutBlock(
	const PreparedBlock &prepared,
	std::vector<PreparedFormulaSlot> *formulas,
	std::vector<RenderedFormula> *renderedFormulas,
	MathRenderer *renderer,
	InlineFormulaObjectCache *inlineFormulaObjects,
	const std::shared_ptr<MediaRuntime> &mediaRuntime,
	const WidthAnalysisNode &analysis,
	const WidthAnalysisNode *activeScrollOwner,
	const style::Markdown &st,
	int left,
	int top,
	int width,
	int logicalWidth,
	LayoutContext context);

[[nodiscard]] int LayoutBlocks(
		const std::vector<PreparedBlock> &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		std::vector<LaidOutBlock> *blocks,
		const std::vector<WidthAnalysisNode> &analysis,
		const WidthAnalysisNode *activeScrollOwner,
		const style::Markdown &st,
		int left,
		int top,
		int width,
		int logicalWidth,
		LayoutContext context);

[[nodiscard]] LaidOutBlock LayoutListItemBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const WidthAnalysisNode &analysis,
		const WidthAnalysisNode *activeScrollOwner,
		const style::Markdown &st,
		int left,
		int top,
		int width,
		int logicalWidth,
		LayoutContext context,
		bool tight) {
	auto block = LaidOutBlock();
	ApplyPreparedEditSources(&block, prepared);
	block.kind = PreparedBlockKind::ListItem;
	block.anchorId = prepared.anchorId;
	block.anchorIds = prepared.anchorIds;
	block.supplementary = prepared.supplementary;
	block.listKind = prepared.listKind;
	block.listDelimiter = prepared.listDelimiter;
	block.taskState = prepared.taskState;
	block.orderedNumber = prepared.orderedNumber;

	const auto &list = st.list;
	const auto bodyLineHeight = TextLineHeight(st.body);
	const auto task = (prepared.taskState != TaskState::None);
	const auto ordered = !task && (prepared.listKind == ListKind::Ordered);
	const auto markerText = ordered ? ListMarkerText(prepared) : QString();
	auto markerTextWidth = 0;
	auto markerTextHeight = bodyLineHeight;
	if (task
		&& prepared.editListItem
		&& context.taskMarkerRippleRuntimeFactory) {
		block.taskMarkerRippleRuntime
			= context.taskMarkerRippleRuntimeFactory(*prepared.editListItem);
	}
	if (task) {
		markerTextWidth = list.taskCheck.diameter;
		markerTextHeight = list.taskCheck.diameter;
	} else if (ordered) {
		block.marker.setMarkedText(
			st.body,
			TextWithEntities::Simple(markerText),
			TextParseOptions{
				TextParseMultiline,
				0,
				0,
				Qt::LayoutDirectionAuto,
			});
		markerTextWidth = std::max(block.marker.maxWidth(), 1);
		markerTextHeight = std::max(
			block.marker.countHeight(markerTextWidth, true),
			bodyLineHeight);
	}

	const auto currentScrollOwner = NextActiveScrollOwner(
		analysis,
		activeScrollOwner);
	const auto scrollOwner = IsActiveScrollOwner(analysis, currentScrollOwner);
	const auto outerLogicalWidth = LogicalOuterWidth(
		analysis,
		currentScrollOwner,
		logicalWidth);
	block.markerWidth = std::max(list.markerWidth, markerTextWidth);
	const auto bodyLeft = left + block.markerWidth + list.markerSkip;
	const auto bodyWidth = std::max(
		width - block.markerWidth - list.markerSkip,
		1);
	const auto bodyLogicalWidth = std::max(
		outerLogicalWidth - block.markerWidth - list.markerSkip,
		1);
	const auto childActiveScrollOwner = currentScrollOwner;

	auto childContext = context;
	childContext.tightList = tight;
	PrepareNestedContext(&childContext, bodyLeft, bodyWidth);
	const auto childBottom = LayoutBlocks(
		prepared.children,
		formulas,
		renderedFormulas,
		renderer,
		inlineFormulaObjects,
		mediaRuntime,
		&block.children,
		analysis.children,
		childActiveScrollOwner,
		st,
		bodyLeft,
		top,
		bodyWidth,
		bodyLogicalWidth,
		childContext);
	const auto markerBaseline = [&] {
		for (const auto &child : block.children) {
			if (child.outer.height() <= 0) {
				continue;
			}
			return ResolveFirstDisplayedLineBaseline(child, st);
		}
		return MarkdownBodyBaseline(top, st);
	}();
	const auto contentHeight = childBottom - top;
	const auto rowHeight = std::max({
		contentHeight,
		markerTextHeight,
		bodyLineHeight,
	});

	const auto markerTop = top + std::max(
		(bodyLineHeight - markerTextHeight) / 2,
		0);
	if (task) {
		block.markerRect = QRect(
			left,
			markerTop,
			list.taskCheck.diameter,
			list.taskCheck.diameter);
	} else if (ordered) {
		const auto markerLeft = left + block.markerWidth - markerTextWidth;
		const auto markerLeafBaseline = LeafFirstLineBaseline(
			block.marker,
			QRect(0, 0, markerTextWidth, markerTextHeight),
			st.body);
		block.markerRect = QRect(
			markerLeft,
			markerBaseline - markerLeafBaseline,
			markerTextWidth,
			markerTextHeight);
	} else {
		block.markerCenter = BulletMarkerCenter(left, markerBaseline, st);
	}

	block.contentRect = QRect(bodyLeft, top, bodyWidth, rowHeight);
	block.horizontalScrollMax = scrollOwner
		? std::max(bodyLogicalWidth - bodyWidth, 0)
		: 0;
	if (scrollOwner) {
		block.scrollViewportRect = block.contentRect;
		block.scrollLogicalContentRect = QRect(
			bodyLeft,
			top,
			bodyLogicalWidth,
			rowHeight);
		if (block.horizontalScrollMax > 0) {
			block.scrollScrollbarTrackRect = QRect(
				bodyLeft,
				top + rowHeight + st.table.scrollbarSkip,
				bodyWidth,
				st.table.scrollbarHeight);
		}
	}
	const auto scrollbarHeight = ScrollbarReserveHeight(
		scrollOwner,
		block.horizontalScrollMax,
		st);
	block.outer = QRect(
		left,
		top,
		std::max(width, 1),
		rowHeight + scrollbarHeight);
	block.firstLineBaseline = markerBaseline;
	RefreshLogicalGeometry(&block);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutListBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const WidthAnalysisNode &analysis,
		const WidthAnalysisNode *activeScrollOwner,
		const style::Markdown &st,
		int left,
		int top,
		int width,
		int logicalWidth,
		LayoutContext context) {
	auto block = LaidOutBlock();
	ApplyPreparedEditSources(&block, prepared);
	block.kind = PreparedBlockKind::List;
	block.anchorId = prepared.anchorId;
	block.anchorIds = prepared.anchorIds;
	block.listKind = prepared.listKind;
	block.listDelimiter = prepared.listDelimiter;

	const auto depthDelta = std::max(prepared.visualDepth - context.listDepth, 0);
	const auto listLeft = left + depthDelta * st.list.indent;
	const auto listWidth = std::max(
		width - depthDelta * st.list.indent,
		1);
	const auto currentScrollOwner = NextActiveScrollOwner(
		analysis,
		activeScrollOwner);
	const auto scrollOwner = IsActiveScrollOwner(analysis, currentScrollOwner);
	const auto outerLogicalWidth = LogicalOuterWidth(
		analysis,
		currentScrollOwner,
		logicalWidth);
	const auto listLogicalWidth = std::max(
		outerLogicalWidth - depthDelta * st.list.indent,
		1);
	const auto childActiveScrollOwner = currentScrollOwner;

	auto childContext = context;
	childContext.listDepth = prepared.visualDepth;
	childContext.tightList = false;
	PrepareNestedContext(&childContext, listLeft, listWidth);

	auto y = top;
	auto previous = static_cast<const PreparedBlock*>(nullptr);
	for (auto i = 0, count = int(prepared.children.size()); i != count; ++i) {
		const auto &child = prepared.children[i];
		const auto anchorOnly = IsAnchorOnlyBlock(child);
		if (previous && !anchorOnly) {
			y += prepared.tight ? 0 : BlockSkip(child, st);
		}

		auto laidOut = (child.kind == PreparedBlockKind::ListItem)
			? LayoutListItemBlock(
				child,
				formulas,
				renderedFormulas,
				renderer,
				inlineFormulaObjects,
				mediaRuntime,
				analysis.children[i],
				childActiveScrollOwner,
				st,
				listLeft,
				y,
				listWidth,
				listLogicalWidth,
				childContext,
				prepared.tight)
			: LayoutBlock(
				child,
				formulas,
				renderedFormulas,
				renderer,
				inlineFormulaObjects,
				mediaRuntime,
				analysis.children[i],
				childActiveScrollOwner,
				st,
				listLeft,
				y,
				listWidth,
				listLogicalWidth,
				childContext);
		y = BlockBottom(laidOut);
		block.children.push_back(std::move(laidOut));
		if (!anchorOnly) {
			previous = &child;
		}
	}

	block.outer = QRect(
		listLeft,
		top,
		listWidth,
		std::max(y - top, 0));
	block.contentRect = block.outer;
	block.horizontalScrollMax = scrollOwner
		? std::max(listLogicalWidth - listWidth, 0)
		: 0;
	if (scrollOwner) {
		block.scrollViewportRect = block.contentRect;
		block.scrollLogicalContentRect = QRect(
			listLeft,
			top,
			listLogicalWidth,
			block.contentRect.height());
		if (block.horizontalScrollMax > 0) {
			block.scrollScrollbarTrackRect = QRect(
				listLeft,
				block.contentRect.y()
					+ block.contentRect.height()
					+ st.table.scrollbarSkip,
				listWidth,
				st.table.scrollbarHeight);
			block.outer.setHeight(
				block.outer.height()
					+ ScrollbarReserveHeight(
						scrollOwner,
						block.horizontalScrollMax,
						st));
		}
	}
	block.firstLineBaseline = ResolveFirstDisplayedLineBaseline(block, st);
	RefreshLogicalGeometry(&block);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutQuoteBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const WidthAnalysisNode &analysis,
		const WidthAnalysisNode *activeScrollOwner,
		const style::Markdown &st,
		int left,
		int top,
		int width,
		int logicalWidth,
		LayoutContext context) {
	auto block = LaidOutBlock();
	ApplyPreparedEditSources(&block, prepared);
	block.kind = PreparedBlockKind::Quote;
	block.anchorId = prepared.anchorId;
	block.anchorIds = prepared.anchorIds;
	block.pullquote = prepared.pullquote;

	const auto depthDelta = std::max(
		prepared.visualDepth - context.quoteDepth,
		0);
	const auto quoteLeft = left + depthDelta * st.quoteIndent;
	const auto quoteWidth = std::max(
		width - depthDelta * st.quoteIndent,
		1);
	const auto scrollOwner = IsActiveScrollOwner(analysis, activeScrollOwner);
	const auto outerLogicalWidth = LogicalOuterWidth(
		analysis,
		activeScrollOwner,
		logicalWidth);
	const auto quoteLogicalWidth = std::max(
		outerLogicalWidth - depthDelta * st.quoteIndent,
		1);
	const auto pullquote = prepared.pullquote;
	const auto padding = pullquote
		? st.pullquote.padding
		: BlockquotePadding(st.body.blockquote);
	const auto availableWidth = std::max(
		quoteWidth - padding.left() - padding.right(),
		1);
	const auto contentWidth = pullquote
		? std::min(availableWidth, std::max(st.pullquote.maxWidth, 1))
		: availableWidth;
	const auto logicalAvailableWidth = std::max(
		quoteLogicalWidth - padding.left() - padding.right(),
		1);
	const auto contentLogicalWidth = pullquote
		? std::min(logicalAvailableWidth, std::max(st.pullquote.maxWidth, 1))
		: logicalAvailableWidth;
	const auto contentLeft = pullquote
		? (quoteLeft + padding.left() + ((availableWidth - contentWidth) / 2))
		: (quoteLeft + padding.left());
	const auto contentTop = top + padding.top();
	const auto childActiveScrollOwner = NextActiveScrollOwner(
		analysis,
		activeScrollOwner);

	auto childContext = context;
	childContext.quoteDepth = prepared.visualDepth;
	childContext.tightList = false;
	PrepareNestedContext(&childContext, contentLeft, contentWidth);
	const auto childBottom = LayoutBlocks(
		prepared.children,
		formulas,
		renderedFormulas,
		renderer,
		inlineFormulaObjects,
		mediaRuntime,
		&block.children,
		analysis.children,
		childActiveScrollOwner,
		st,
		contentLeft,
		contentTop,
		contentWidth,
		contentLogicalWidth,
		childContext);
	const auto contentHeight = std::max(
		childBottom - contentTop,
		prepared.children.empty()
			? TextLineHeight(st.body)
			: 0);
	block.horizontalScrollMax = scrollOwner
		? std::max(contentLogicalWidth - contentWidth, 0)
		: 0;
	if (scrollOwner) {
		block.scrollViewportRect = QRect(
			contentLeft,
			contentTop,
			contentWidth,
			contentHeight);
		block.scrollLogicalContentRect = QRect(
			contentLeft,
			contentTop,
			contentLogicalWidth,
			contentHeight);
		if (block.horizontalScrollMax > 0) {
			block.scrollScrollbarTrackRect = QRect(
				contentLeft,
				contentTop + contentHeight + st.table.scrollbarSkip,
				contentWidth,
				st.table.scrollbarHeight);
		}
	}
	const auto quoteHeight = padding.top()
		+ contentHeight
		+ padding.bottom()
		+ ScrollbarReserveHeight(
			scrollOwner,
			block.horizontalScrollMax,
			st);

	block.outer = QRect(quoteLeft, top, quoteWidth, quoteHeight);
	block.contentRect = QRect(
		contentLeft,
		contentTop,
		contentWidth,
		contentHeight);
	block.firstLineBaseline = ResolveFirstDisplayedLineBaseline(block, st);
	RefreshLogicalGeometry(&block);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutDetailsBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const WidthAnalysisNode &analysis,
		const WidthAnalysisNode *activeScrollOwner,
		const style::Markdown &st,
		int left,
		int top,
		int width,
		int logicalWidth,
		LayoutContext context) {
	auto block = LaidOutBlock();
	ApplyPreparedEditSources(&block, prepared);
	block.kind = PreparedBlockKind::Details;
	block.anchorId = prepared.anchorId;
	block.anchorIds = prepared.anchorIds;
	block.collapsed = prepared.collapsed;
	block.detailsOpen = prepared.detailsOpen;
	block.supplementary = prepared.supplementary;
	const auto &details = st.details;
	const auto headerPadding = DetailsHeaderPadding(context, st);
	const auto bodyPadding = DetailsBodyPadding(context, st);
	const auto headerWidth = std::max(width, 1);
	const auto scrollOwner = IsActiveScrollOwner(analysis, activeScrollOwner);
	const auto outerLogicalWidth = LogicalOuterWidth(
		analysis,
		activeScrollOwner,
		logicalWidth);
	const auto iconSize = details.icon.empty()
		? 0
		: TextLineHeight(details.summaryStyle);
	const auto iconWidth = iconSize;
	const auto iconHeight = iconSize;
	const auto iconSkip = iconWidth ? details.iconSkip : 0;
	const auto actionWidth = context.editMode
		? DetailsStateReserveWidth(st)
		: 0;
	const auto actionSkip = actionWidth ? details.stateSkip : 0;
	const auto actionZoneWidth = actionSkip + actionWidth;
	const auto textLeft = left
		+ headerPadding.left()
		+ iconWidth
		+ iconSkip;
	block.textWidth = std::max(
		headerWidth
			- headerPadding.left()
			- headerPadding.right()
			- iconWidth
			- iconSkip
			- actionZoneWidth,
		1);

	SetTextLeaf(
		&block.leaf,
		details.summaryStyle,
		st,
		prepared.text,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		block.textWidth);
	BindLinks(&block.leaf, prepared.links);
	const auto usePlaceholder = prepared.text.text.isEmpty()
		&& !prepared.editPlaceholderText.isEmpty();
	if (usePlaceholder) {
		SetEditPlaceholderLeaf(
			&block,
			prepared.editPlaceholderText,
			details.summaryStyle,
			block.textWidth);
	}
	const auto &displayLeaf = usePlaceholder
		? block.placeholderLeaf
		: block.leaf;

	const auto summaryHeight = ResolveTextLeafHeight(
		std::max(
			displayLeaf.countHeight(block.textWidth, true),
			TextLineHeight(details.summaryStyle)),
		context);
	auto actionHeight = 0;
	if (actionWidth > 0) {
		block.actionWidth = actionWidth;
		SetTextLeaf(
			&block.actionLeaf,
			details.summaryStyle,
			st,
			TextWithEntities::Simple(
				DetailsStateText(prepared.detailsOpen)),
			nullptr,
			nullptr,
			mediaRuntime,
			block.actionWidth);
		actionHeight = std::max(
			block.actionLeaf.countHeight(block.actionWidth, true),
			TextLineHeight(details.summaryStyle));
	}
	const auto headerContentHeight = std::max(summaryHeight, iconHeight);
	const auto headerHeight = headerPadding.top()
		+ headerContentHeight
		+ headerPadding.bottom();
	block.headerRect = QRect(left, top, headerWidth, headerHeight);
	if (iconWidth > 0 && iconHeight > 0) {
		block.iconRect = QRect(
			left + headerPadding.left(),
			top + (headerHeight - iconHeight) / 2,
			iconWidth,
			iconHeight);
	}
	block.textRect = QRect(
		textLeft,
		top + headerPadding.top()
			+ std::max((headerContentHeight - summaryHeight) / 2, 0),
		block.textWidth,
		summaryHeight);
	if (actionZoneWidth > 0 && actionHeight > 0) {
		block.actionRect = QRect(
			left + headerWidth - headerPadding.right() - actionZoneWidth,
			top + headerPadding.top()
				+ std::max((headerContentHeight - actionHeight) / 2, 0),
			actionZoneWidth,
			actionHeight);
	}
	block.firstLineBaseline = LeafFirstLineBaseline(
		displayLeaf,
		block.textRect,
		details.summaryStyle);
	const auto childActiveScrollOwner = NextActiveScrollOwner(
		analysis,
		activeScrollOwner);

	auto bottom = top + headerHeight;
	if (!prepared.collapsed) {
		const auto childLeft = left + bodyPadding.left();
		const auto childTop = bottom + bodyPadding.top();
		const auto childWidth = std::max(
			headerWidth
				- bodyPadding.left()
				- bodyPadding.right(),
			1);
		const auto childLogicalWidth = std::max(
			outerLogicalWidth
				- bodyPadding.left()
				- bodyPadding.right(),
			1);
		auto childContext = context;
		PrepareNestedContext(&childContext, childLeft, childWidth);
		const auto childBottom = LayoutBlocks(
			prepared.children,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			&block.children,
			analysis.children,
			childActiveScrollOwner,
			st,
			childLeft,
			childTop,
			childWidth,
			childLogicalWidth,
			childContext);
		const auto contentHeight = std::max(childBottom - childTop, 0);
		const auto bodyHeight = bodyPadding.top()
			+ contentHeight
			+ bodyPadding.bottom();
		block.bodyRect = QRect(left, bottom, headerWidth, bodyHeight);
		block.contentRect = QRect(
			childLeft,
			childTop,
			childWidth,
			contentHeight);
		block.horizontalScrollMax = scrollOwner
			? std::max(childLogicalWidth - childWidth, 0)
			: 0;
		if (scrollOwner) {
			block.scrollViewportRect = block.contentRect;
			block.scrollLogicalContentRect = QRect(
				childLeft,
				childTop,
				childLogicalWidth,
				contentHeight);
			if (block.horizontalScrollMax > 0) {
				block.scrollScrollbarTrackRect = QRect(
					childLeft,
					childTop + contentHeight + st.table.scrollbarSkip,
					childWidth,
					st.table.scrollbarHeight);
			}
		}
		bottom += bodyHeight
			+ ScrollbarReserveHeight(
				scrollOwner,
				block.horizontalScrollMax,
				st);
	}
	block.outer = QRect(
		left,
		top,
		headerWidth,
		std::max(bottom - top, headerHeight));
	RefreshLogicalGeometry(&block);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutEmbedPostBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const WidthAnalysisNode &analysis,
		const WidthAnalysisNode *activeScrollOwner,
		const style::Markdown &st,
		int left,
		int top,
		int width,
		int logicalWidth,
		LayoutContext context) {
	auto block = LaidOutBlock();
	ApplyPreparedEditSources(&block, prepared);
	block.kind = PreparedBlockKind::EmbedPost;
	block.anchorId = prepared.anchorId;
	block.anchorIds = prepared.anchorIds;
	block.supplementary = prepared.supplementary;
	block.thumbnailPhotoId = prepared.embedPost.authorPhotoId;
	if (prepared.embedPost.authorPhotoId && mediaRuntime) {
		block.photoRuntime = mediaRuntime->resolvePhoto(
			prepared.embedPost.authorPhotoId);
	}

	const auto parseOptions = TextParseOptions{
		TextParseMultiline,
		0,
		0,
		Qt::LayoutDirectionAuto,
	};
	const auto &style = st.embedPost;
	const auto blockWidth = std::max(width, 1);
	const auto scrollOwner = IsActiveScrollOwner(analysis, activeScrollOwner);
	const auto outerLogicalWidth = LogicalOuterWidth(
		analysis,
		activeScrollOwner,
		logicalWidth);
	const auto contentLeft = left
		+ style.accentWidth
		+ style.accentSkip
		+ style.padding.left();
	const auto contentTop = top + style.padding.top();
	const auto contentWidth = std::max(
		blockWidth
			- style.accentWidth
			- style.accentSkip
		- style.padding.left()
		- style.padding.right(),
		1);
	const auto contentLogicalWidth = std::max(
		outerLogicalWidth
			- style.accentWidth
			- style.accentSkip
			- style.padding.left()
			- style.padding.right(),
		1);
	const auto hasAvatar = (block.photoRuntime != nullptr);
	const auto avatarSize = hasAvatar ? std::max(style.avatarSize, 1) : 0;
	const auto headerGap = hasAvatar ? style.headerGap : 0;
	const auto textLeft = contentLeft + avatarSize + headerGap;
	const auto textWidth = std::max(contentWidth - avatarSize - headerGap, 1);
	const auto childActiveScrollOwner = NextActiveScrollOwner(
		analysis,
		activeScrollOwner);

	auto authorHeight = 0;
	if (!prepared.embedPost.author.isEmpty()) {
		block.labelWidth = textWidth;
		block.labelLeaf = Ui::Text::String(TextMinResizeWidth(textWidth));
		block.labelLeaf.setMarkedText(
			style.authorStyle,
			TextWithEntities::Simple(prepared.embedPost.author),
			parseOptions);
		authorHeight = ResolveTextLeafHeight(
			std::max(
				block.labelLeaf.countHeight(textWidth, true),
				TextLineHeight(style.authorStyle)),
			context);
	}

	auto dateHeight = 0;
	if (!prepared.embedPost.dateText.isEmpty()) {
		block.subtitleWidth = textWidth;
		block.subtitleLeaf = Ui::Text::String(TextMinResizeWidth(textWidth));
		block.subtitleLeaf.setMarkedText(
			style.dateStyle,
			TextWithEntities::Simple(prepared.embedPost.dateText),
			parseOptions);
		dateHeight = ResolveTextLeafHeight(
			std::max(
				block.subtitleLeaf.countHeight(textWidth, true),
				TextLineHeight(style.dateStyle)),
			context);
	}

	const auto textHeight = authorHeight + dateHeight;
	const auto headerHeight = std::max(textHeight, avatarSize);
	if (headerHeight > 0) {
		block.headerRect = QRect(contentLeft, contentTop, contentWidth, headerHeight);
	}
	if (avatarSize > 0) {
		block.thumbnailRect = QRect(
			contentLeft,
			contentTop + std::max((headerHeight - avatarSize) / 2, 0),
			avatarSize,
			avatarSize);
	}

	auto textTop = contentTop + std::max((headerHeight - textHeight) / 2, 0);
	if (authorHeight > 0) {
		block.labelRect = QRect(textLeft, textTop, textWidth, authorHeight);
		textTop += authorHeight;
	}
	if (dateHeight > 0) {
		block.subtitleRect = QRect(textLeft, textTop, textWidth, dateHeight);
	}

	auto wrapperBottom = contentTop + headerHeight;
	if (!prepared.children.empty()) {
		const auto bodyTop = wrapperBottom + ((headerHeight > 0) ? style.bodySkip : 0);
		auto childContext = context;
		PrepareNestedContext(&childContext, contentLeft, contentWidth);
		const auto childBottom = LayoutBlocks(
			prepared.children,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			&block.children,
			analysis.children,
			childActiveScrollOwner,
			st,
			contentLeft,
			bodyTop,
			contentWidth,
			contentLogicalWidth,
			childContext);
		const auto bodyHeight = std::max(childBottom - bodyTop, 0);
		block.bodyRect = QRect(contentLeft, bodyTop, contentWidth, bodyHeight);
		wrapperBottom = std::max(wrapperBottom, childBottom);
		block.horizontalScrollMax = scrollOwner
			? std::max(contentLogicalWidth - contentWidth, 0)
			: 0;
		if (scrollOwner) {
			block.scrollViewportRect = block.bodyRect;
			block.scrollLogicalContentRect = QRect(
				contentLeft,
				bodyTop,
				contentLogicalWidth,
				bodyHeight);
			if (block.horizontalScrollMax > 0) {
				block.scrollScrollbarTrackRect = QRect(
					contentLeft,
					bodyTop + bodyHeight + st.table.scrollbarSkip,
					contentWidth,
					st.table.scrollbarHeight);
				wrapperBottom = std::max(
					wrapperBottom,
					block.scrollScrollbarTrackRect.y()
						+ block.scrollScrollbarTrackRect.height());
			}
		}
	}

	block.contentRect = QRect(
		contentLeft,
		contentTop,
		contentWidth,
		std::max(wrapperBottom - contentTop, 0));
	block.mediaRect = QRect(
		left,
		top,
		blockWidth,
		style.padding.top()
			+ block.contentRect.height()
			+ style.padding.bottom());

	auto bottom = block.mediaRect.y() + block.mediaRect.height();
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		st,
		left,
		bottom,
		blockWidth,
		style.captionSkip,
		&bottom,
		context);
	block.outer = QRect(
		left,
		top,
		blockWidth,
		std::max(bottom - top, block.mediaRect.height()));

	if (!block.labelRect.isEmpty() && !block.labelLeaf.isEmpty()) {
		block.firstLineBaseline = LeafFirstLineBaseline(
			block.labelLeaf,
			block.labelRect,
			style.authorStyle);
	} else if (!block.subtitleRect.isEmpty() && !block.subtitleLeaf.isEmpty()) {
		block.firstLineBaseline = LeafFirstLineBaseline(
			block.subtitleLeaf,
			block.subtitleRect,
			style.dateStyle);
	} else {
		for (const auto &child : block.children) {
			if (child.outer.height() <= 0) {
				continue;
			}
			block.firstLineBaseline = ResolveFirstDisplayedLineBaseline(
				child,
				st);
			break;
		}
		if (block.firstLineBaseline < 0) {
			block.firstLineBaseline = MarkdownBodyBaseline(top, st);
		}
	}
	RefreshLogicalGeometry(&block);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const WidthAnalysisNode &analysis,
		const WidthAnalysisNode *activeScrollOwner,
		const style::Markdown &st,
		int left,
		int top,
		int width,
		int logicalWidth,
		LayoutContext context) {
	const auto currentScrollOwner = NextActiveScrollOwner(
		analysis,
		activeScrollOwner);
	const auto scrollOwner = IsActiveScrollOwner(analysis, currentScrollOwner);
	const auto effectiveLogicalWidth = LogicalOuterWidth(
		analysis,
		currentScrollOwner,
		logicalWidth);
	switch (prepared.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Thinking:
	case PreparedBlockKind::Heading:
		return LayoutFlowBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			effectiveLogicalWidth,
			scrollOwner,
			context);
	case PreparedBlockKind::CodeBlock:
		return LayoutCodeBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			effectiveLogicalWidth,
			scrollOwner,
			context.allowAsyncSyntaxHighlighting,
			context.syntaxHighlightTracker,
			context);
	case PreparedBlockKind::Rule:
		return LayoutRuleBlock(prepared, st, left, top, width);
	case PreparedBlockKind::List:
		return LayoutListBlock(
			prepared,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			analysis,
			currentScrollOwner,
			st,
			left,
			top,
			width,
			effectiveLogicalWidth,
			context);
	case PreparedBlockKind::ListItem:
		return LayoutListItemBlock(
			prepared,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			analysis,
			currentScrollOwner,
			st,
			left,
			top,
			width,
			effectiveLogicalWidth,
			context,
			false);
	case PreparedBlockKind::Quote:
		return LayoutQuoteBlock(
			prepared,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			analysis,
			currentScrollOwner,
			st,
			left,
			top,
			width,
			effectiveLogicalWidth,
			context);
	case PreparedBlockKind::DisplayMath:
		return LayoutDisplayMathBlock(
			prepared,
			*formulas,
			st,
			left,
			top,
			width,
			effectiveLogicalWidth,
			scrollOwner);
	case PreparedBlockKind::Table:
		return LayoutTableBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			effectiveLogicalWidth,
			scrollOwner,
			context);
	case PreparedBlockKind::Photo:
		return LayoutPhotoBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::Video:
		return LayoutVideoBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::Audio:
		return LayoutAudioBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::Map:
		return LayoutMapBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::Channel:
		return LayoutChannelBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::GroupedMedia:
		return LayoutGroupedMediaBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::RelatedArticle:
		return LayoutRelatedArticleBlock(
			prepared,
			st,
			left,
			top,
			width,
			mediaRuntime);
	case PreparedBlockKind::Placeholder:
		return LayoutPlaceholderBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			st,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::Details:
		return LayoutDetailsBlock(
			prepared,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			analysis,
			currentScrollOwner,
			st,
			left,
			top,
			width,
			effectiveLogicalWidth,
			context);
	case PreparedBlockKind::EmbedPost:
		return LayoutEmbedPostBlock(
			prepared,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			analysis,
			currentScrollOwner,
			st,
			left,
			top,
			width,
			effectiveLogicalWidth,
			context);
	}
	Unexpected("Unknown markdown article block kind.");
}

int LayoutBlocks(
		const std::vector<PreparedBlock> &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		std::vector<LaidOutBlock> *blocks,
		const std::vector<WidthAnalysisNode> &analysis,
		const WidthAnalysisNode *activeScrollOwner,
		const style::Markdown &st,
		int left,
		int top,
		int width,
		int logicalWidth,
		LayoutContext context) {
	auto y = top;
	auto previous = static_cast<const PreparedBlock*>(nullptr);
	for (auto i = 0, count = int(prepared.size()); i != count; ++i) {
		const auto &block = prepared[i];
		const auto anchorOnly = IsAnchorOnlyBlock(block);
		const auto next = NextVisibleBlock(prepared, i);
		if (previous && !anchorOnly) {
			y += BlockSkip(*previous, block, context, st);
		}
		const auto band = BlockBand(
			block.kind,
			st,
			left,
			std::max(width, 1),
			context);
		const auto logicalBandWidth = BlockBandWidth(
			block.kind,
			st,
			logicalWidth,
			context);
		auto laidOut = IsRelatedArticlesHeader(block, next)
			? LayoutFlowBlock(
				block,
				formulas,
				inlineFormulaObjects,
				mediaRuntime,
				st,
				left + st.relatedArticle.headerPadding.left(),
				y + st.relatedArticle.headerPadding.top(),
				std::max(
					width
						- st.relatedArticle.headerPadding.left()
						- st.relatedArticle.headerPadding.right(),
					1),
				std::max(
					logicalWidth
						- st.relatedArticle.headerPadding.left()
						- st.relatedArticle.headerPadding.right(),
					1),
				false,
				context)
			: LayoutBlock(
				block,
				formulas,
				renderedFormulas,
				renderer,
				inlineFormulaObjects,
				mediaRuntime,
				analysis[i],
				activeScrollOwner,
				st,
				band.x(),
				y,
				band.width(),
				logicalBandWidth,
				context);
		if (IsRelatedArticlesHeader(block, next)) {
			laidOut.headerRect = QRect(
				left,
				y,
				std::max(width, 1),
				laidOut.outer.height()
					+ st.relatedArticle.headerPadding.top()
					+ st.relatedArticle.headerPadding.bottom());
			laidOut.outer = laidOut.headerRect;
			laidOut.contentRect = laidOut.headerRect;
			RefreshLogicalGeometry(&laidOut);
		}
		y = BlockBottom(laidOut);
		blocks->push_back(std::move(laidOut));
		if (!anchorOnly) {
			previous = &block;
		}
	}
	return y;
}

} // namespace

int LayoutBlocks(
		const std::vector<PreparedBlock> &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		std::vector<LaidOutBlock> *blocks,
		const style::Markdown &st,
		int left,
		int top,
		int width,
		LayoutContext context) {
	const auto analysis = AnalyzeBlocks(
		prepared,
		*formulas,
		st,
		width,
		context);
	return LayoutBlocks(
		prepared,
		formulas,
		renderedFormulas,
		renderer,
		inlineFormulaObjects,
		mediaRuntime,
		blocks,
		analysis,
		nullptr,
		st,
		left,
		top,
		width,
		width,
		context);
}

} // namespace Iv::Markdown
