/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_article_layout_structure.h"
#include "iv/markdown/iv_markdown_article_text.h"

#include "styles/style_iv.h"

#include <algorithm>

namespace Iv::Markdown {
namespace {

[[nodiscard]] int NominalTextBaseline(
		const style::TextStyle &style,
		int top) {
	const auto lineHeight = TextLineHeight(style);
	const auto textTop = top
		+ (std::max(lineHeight - style.font->height, 0) / 2);
	return textTop + style.font->ascent;
}

[[nodiscard]] int LeafFirstLineBaseline(
		const Ui::Text::String &leaf,
		const QRect &textRect,
		const style::TextStyle &style) {
	const auto lines = leaf.countLinesGeometry(textRect.width(), true);
	return textRect.y() + (lines.empty()
		? NominalTextBaseline(style, 0)
		: lines.front().baseline);
}

[[nodiscard]] int MarkdownBodyBaseline(
		int top,
		const style::Markdown &markdown) {
	return NominalTextBaseline(markdown.body, top);
}

[[nodiscard]] int BlockBottom(const LaidOutBlock &block) {
	return block.outer.y() + block.outer.height();
}

[[nodiscard]] bool FirstLineComesFromChildren(const LaidOutBlock &block) {
	switch (block.kind) {
	case PreparedBlockKind::List:
	case PreparedBlockKind::ListItem:
	case PreparedBlockKind::Quote:
		return true;
	case PreparedBlockKind::Paragraph:
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
	case PreparedBlockKind::Placeholder:
	case PreparedBlockKind::Details:
		return false;
	}
	return false;
}

[[nodiscard]] int ResolveFirstDisplayedLineBaseline(
		const LaidOutBlock &block,
		const style::Markdown &markdown) {
	if (block.firstLineBaseline >= 0) {
		return block.firstLineBaseline;
	}
	if (FirstLineComesFromChildren(block)) {
		for (const auto &child : block.children) {
			if (child.outer.height() <= 0) {
				continue;
			}
			return ResolveFirstDisplayedLineBaseline(child, markdown);
		}
	}
	return MarkdownBodyBaseline(block.outer.y(), markdown);
}

[[nodiscard]] LaidOutBlock LayoutBlock(
	const PreparedBlock &prepared,
	std::vector<PreparedFormulaSlot> *formulas,
	std::vector<RenderedFormula> *renderedFormulas,
	MathRenderer *renderer,
	InlineFormulaObjectCache *inlineFormulaObjects,
	const std::shared_ptr<MediaRuntime> &mediaRuntime,
	const style::Markdown &markdown,
	int left,
	int top,
	int width,
	LayoutContext context);

[[nodiscard]] LaidOutBlock LayoutListItemBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context,
		bool tight) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::ListItem;
	block.anchorId = prepared.anchorId;
	block.listKind = prepared.listKind;
	block.listDelimiter = prepared.listDelimiter;
	block.taskState = prepared.taskState;
	block.orderedNumber = prepared.orderedNumber;

	const auto &list = markdown.list;
	const auto bodyLineHeight = TextLineHeight(markdown.body);
	const auto task = (prepared.taskState != TaskState::None);
	const auto ordered = !task && (prepared.listKind == ListKind::Ordered);
	const auto markerText = ordered ? ListMarkerText(prepared) : QString();
	auto markerTextWidth = 0;
	auto markerTextHeight = bodyLineHeight;
	if (task) {
		markerTextWidth = list.taskCheck.diameter;
		markerTextHeight = list.taskCheck.diameter;
	} else if (ordered) {
		block.marker.setMarkedText(
			markdown.body,
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

	block.markerWidth = std::max(list.markerWidth, markerTextWidth);
	const auto bodyLeft = left + block.markerWidth + list.markerSkip;
	const auto bodyWidth = std::max(
		width - block.markerWidth - list.markerSkip,
		1);

	auto childContext = context;
	childContext.tightList = tight;
	const auto childBottom = LayoutBlocks(
		prepared.children,
		formulas,
		renderedFormulas,
		renderer,
		inlineFormulaObjects,
		mediaRuntime,
		&block.children,
		markdown,
		bodyLeft,
		top,
		bodyWidth,
		childContext);
	const auto markerBaseline = [&] {
		for (const auto &child : block.children) {
			if (child.outer.height() <= 0) {
				continue;
			}
			return ResolveFirstDisplayedLineBaseline(child, markdown);
		}
		return MarkdownBodyBaseline(top, markdown);
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
			markdown.body);
		block.markerRect = QRect(
			markerLeft,
			markerBaseline - markerLeafBaseline,
			markerTextWidth,
			markerTextHeight);
	} else {
		block.markerCenter = BulletMarkerCenter(left, markerBaseline, markdown);
	}

	block.contentRect = QRect(bodyLeft, top, bodyWidth, rowHeight);
	block.outer = QRect(left, top, std::max(width, 1), rowHeight);
	block.firstLineBaseline = markerBaseline;
	return block;
}

[[nodiscard]] LaidOutBlock LayoutListBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::List;
	block.listKind = prepared.listKind;
	block.listDelimiter = prepared.listDelimiter;

	const auto depthDelta = std::max(prepared.visualDepth - context.listDepth, 0);
	const auto listLeft = left + depthDelta * markdown.list.indent;
	const auto listWidth = std::max(
		width - depthDelta * markdown.list.indent,
		1);

	auto childContext = context;
	childContext.listDepth = prepared.visualDepth;
	childContext.tightList = false;

	auto y = top;
	auto first = true;
	for (const auto &child : prepared.children) {
		if (!first) {
			y += prepared.tight ? 0 : BlockSkip(child, markdown);
		}
		first = false;

		auto laidOut = (child.kind == PreparedBlockKind::ListItem)
			? LayoutListItemBlock(
				child,
				formulas,
				renderedFormulas,
				renderer,
				inlineFormulaObjects,
				mediaRuntime,
				markdown,
				listLeft,
				y,
				listWidth,
				childContext,
				prepared.tight)
			: LayoutBlock(
				child,
				formulas,
				renderedFormulas,
				renderer,
				inlineFormulaObjects,
				mediaRuntime,
				markdown,
				listLeft,
				y,
				listWidth,
				childContext);
		y = BlockBottom(laidOut);
		block.children.push_back(std::move(laidOut));
	}

	block.outer = QRect(
		listLeft,
		top,
		listWidth,
		std::max(y - top, 0));
	block.contentRect = block.outer;
	block.firstLineBaseline = ResolveFirstDisplayedLineBaseline(block, markdown);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutQuoteBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Quote;

	const auto depthDelta = std::max(
		prepared.visualDepth - context.quoteDepth,
		0);
	const auto quoteLeft = left + depthDelta * markdown.quoteIndent;
	const auto quoteWidth = std::max(
		width - depthDelta * markdown.quoteIndent,
		1);
	const auto &quoteStyle = markdown.body.blockquote;
	const auto padding = BlockquotePadding(quoteStyle);
	const auto contentLeft = quoteLeft + padding.left();
	const auto contentTop = top + padding.top();
	const auto contentWidth = std::max(
		quoteWidth - padding.left() - padding.right(),
		1);

	auto childContext = context;
	childContext.quoteDepth = prepared.visualDepth;
	childContext.tightList = false;
	const auto childBottom = LayoutBlocks(
		prepared.children,
		formulas,
		renderedFormulas,
		renderer,
		inlineFormulaObjects,
		mediaRuntime,
		&block.children,
		markdown,
		contentLeft,
		contentTop,
		contentWidth,
		childContext);
	const auto contentHeight = std::max(
		childBottom - contentTop,
		prepared.children.empty()
			? TextLineHeight(markdown.body)
			: 0);
	const auto quoteHeight = padding.top() + contentHeight + padding.bottom();

	block.outer = QRect(quoteLeft, top, quoteWidth, quoteHeight);
	block.contentRect = QRect(
		contentLeft,
		contentTop,
		contentWidth,
		contentHeight);
	block.firstLineBaseline = ResolveFirstDisplayedLineBaseline(block, markdown);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutDetailsBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Details;
	block.anchorId = prepared.anchorId;
	block.collapsed = prepared.collapsed;
	const auto &details = markdown.details;
	const auto headerWidth = std::max(width, 1);
	const auto iconWidth = details.icon.width();
	const auto iconHeight = details.icon.height();
	const auto iconSkip = iconWidth ? details.iconSkip : 0;
	const auto textLeft = left
		+ details.headerPadding.left()
		+ iconWidth
		+ iconSkip;
	block.textWidth = std::max(
		headerWidth
			- details.headerPadding.left()
			- details.headerPadding.right()
			- iconWidth
			- iconSkip,
		1);

	SetTextLeaf(
		&block.leaf,
		details.summaryStyle,
		prepared.text,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		block.textWidth);
	BindLinks(&block.leaf, prepared.links);

	const auto summaryHeight = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(details.summaryStyle));
	const auto headerContentHeight = std::max(summaryHeight, iconHeight);
	const auto headerHeight = details.headerPadding.top()
		+ headerContentHeight
		+ details.headerPadding.bottom();
	block.headerRect = QRect(left, top, headerWidth, headerHeight);
	if (iconWidth > 0 && iconHeight > 0) {
		block.iconRect = QRect(
			left + details.headerPadding.left(),
			top + (headerHeight - iconHeight) / 2,
			iconWidth,
			iconHeight);
	}
	block.textRect = QRect(
		textLeft,
		top + details.headerPadding.top()
			+ std::max((headerContentHeight - summaryHeight) / 2, 0),
		block.textWidth,
		summaryHeight);
	block.firstLineBaseline = LeafFirstLineBaseline(
		block.leaf,
		block.textRect,
		details.summaryStyle);

	auto bottom = top + headerHeight;
	if (!prepared.collapsed) {
		const auto childLeft = left + details.bodyPadding.left();
		const auto childTop = bottom + details.bodyPadding.top();
		const auto childWidth = std::max(
			headerWidth
				- details.bodyPadding.left()
				- details.bodyPadding.right(),
			1);
		const auto childBottom = LayoutBlocks(
			prepared.children,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			&block.children,
			markdown,
			childLeft,
			childTop,
			childWidth,
			context);
		const auto contentHeight = std::max(childBottom - childTop, 0);
		const auto bodyHeight = details.bodyPadding.top()
			+ contentHeight
			+ details.bodyPadding.bottom();
		block.bodyRect = QRect(left, bottom, headerWidth, bodyHeight);
		block.contentRect = QRect(
			childLeft,
			childTop,
			childWidth,
			contentHeight);
		bottom += bodyHeight;
	}
	block.outer = QRect(
		left,
		top,
		headerWidth,
		std::max(bottom - top, headerHeight));
	return block;
}

[[nodiscard]] LaidOutBlock LayoutBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	switch (prepared.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		return LayoutFlowBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width);
	case PreparedBlockKind::CodeBlock:
		return LayoutCodeBlock(
			prepared,
			markdown,
			left,
			top,
			width,
			context.allowAsyncSyntaxHighlighting,
			context.syntaxHighlightTracker);
	case PreparedBlockKind::Rule:
		return LayoutRuleBlock(markdown, left, top, width);
	case PreparedBlockKind::List:
		return LayoutListBlock(
			prepared,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::ListItem:
		return LayoutListItemBlock(
			prepared,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width,
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
			markdown,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::DisplayMath:
		return LayoutDisplayMathBlock(prepared, *formulas, markdown, left, top, width);
	case PreparedBlockKind::Table:
		return LayoutTableBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width);
	case PreparedBlockKind::Photo:
		return LayoutPhotoBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width);
	case PreparedBlockKind::Video:
		return LayoutVideoBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width);
	case PreparedBlockKind::Audio:
		return LayoutAudioBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width);
	case PreparedBlockKind::Map:
		return LayoutMapBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width);
	case PreparedBlockKind::Channel:
		return LayoutChannelBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width);
	case PreparedBlockKind::Placeholder:
		return LayoutPlaceholderBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width);
	case PreparedBlockKind::Details:
		return LayoutDetailsBlock(
			prepared,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width,
			context);
	}
	return LayoutFlowBlock(
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		left,
		top,
		width);
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
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto y = top;
	auto previous = static_cast<const PreparedBlock*>(nullptr);
	for (const auto &block : prepared) {
		if (previous) {
			y += BlockSkip(*previous, block, context, markdown);
		}
		auto laidOut = LayoutBlock(
			block,
			formulas,
			renderedFormulas,
			renderer,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			y,
			std::max(width, 1),
			context);
		y = BlockBottom(laidOut);
		blocks->push_back(std::move(laidOut));
		previous = &block;
	}
	return y;
}

} // namespace Iv::Markdown
