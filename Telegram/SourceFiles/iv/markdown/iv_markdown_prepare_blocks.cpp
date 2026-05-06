#include "iv/markdown/iv_markdown_prepare_blocks.h"

#include "iv/markdown/iv_markdown_prepare_inline.h"
#include "iv/markdown/iv_markdown_prepare_links.h"

#include <QtCore/QString>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace Iv::Markdown {
namespace {

constexpr auto kMaxVisualListDepth = 6;
constexpr auto kMaxVisualQuoteDepth = 3;

[[nodiscard]] QString DetailsAnchorId(PrepareState *state) {
	return u"details-"_q + QString::number(++state->nextGeneratedId);
}

[[nodiscard]] int CappedListDepth(int depth) {
	return std::min(depth, kMaxVisualListDepth);
}

[[nodiscard]] int CappedQuoteDepth(int depth) {
	return std::min(depth, kMaxVisualQuoteDepth);
}

[[nodiscard]] int ScaleFormulaCap(int cap, int textSize, int baseTextSize) {
	if (cap <= 0) {
		return 0;
	}
	const auto numerator = int64(cap) * std::max(textSize, 1);
	const auto denominator = std::max(baseTextSize, 1);
	return std::max(int((numerator + denominator - 1) / denominator), 1);
}

[[nodiscard]] int FlowFormulaTextSize(
		PreparedBlockKind kind,
		int headingLevel,
		const MarkdownPrepareDimensions &dimensions) {
	if (kind != PreparedBlockKind::Heading) {
		return dimensions.bodyTextSize;
	}
	const auto index = std::clamp(headingLevel, 1, 6) - 1;
	if (index < int(dimensions.headingTextSizes.size())) {
		return dimensions.headingTextSizes[index];
	}
	return dimensions.bodyTextSize;
}

[[nodiscard]] int TableCellFormulaTextSize(
		bool header,
		const MarkdownPrepareDimensions &dimensions) {
	return header
		? dimensions.tableHeaderTextSize
		: dimensions.bodyTextSize;
}

void PrepareTableCellText(
		const MarkdownNode &cell,
		bool header,
		TextWithEntities *text,
		std::vector<PreparedLink> *links,
		PrepareState *state) {
	const auto textSize = TableCellFormulaTextSize(
		header,
		state->request->dimensions);
	PrepareInlineRichText(
		cell,
		textSize,
		ScaleFormulaCap(
			state->request->dimensions.displayMathMaxRenderWidth,
			textSize,
			state->request->dimensions.displayMathTextSize),
		ScaleFormulaCap(
			state->request->dimensions.displayMathMaxRenderHeight,
			textSize,
			state->request->dimensions.displayMathTextSize),
		nullptr,
		text,
		links,
	state);
	SortEntities(text);
}

[[nodiscard]] int EffectiveTableRowWidth(const MarkdownNode &row) {
	auto result = 0;
	auto expectedColumn = 0;
	for (const auto &cell : row.children) {
		if (cell.kind != NodeKind::TableCell) {
			return 0;
		}
		const auto column = (cell.tableColumn >= 0)
			? cell.tableColumn
			: expectedColumn;
		if (column != expectedColumn) {
			return 0;
		}
		result = std::max(result, column + 1);
		++expectedColumn;
	}
	return result;
}

[[nodiscard]] int EffectiveTableColumnCount(const MarkdownNode &node) {
	auto result = int(node.tableAlignments.size());
	for (const auto &row : node.children) {
		if (row.kind != NodeKind::TableRow) {
			return 0;
		}
		const auto width = EffectiveTableRowWidth(row);
		if (!width) {
			return 0;
		}
		result = std::max(result, width);
	}
	return result;
}

[[nodiscard]] std::vector<TableAlignment> NormalizedTableAlignments(
		const MarkdownNode &node,
		int columnCount) {
	auto result = std::vector<TableAlignment>(
		std::max(columnCount, 0),
		TableAlignment::None);
	const auto limit = std::min(columnCount, int(node.tableAlignments.size()));
	for (auto i = 0; i != limit; ++i) {
		result[i] = node.tableAlignments[i];
	}
	return result;
}

[[nodiscard]] bool ShouldFlattenTable(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	const auto &limits = PrepareLimitsForIv().tableRender;
	if (context.listDepth > 0 || context.quoteDepth > 0) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	if (node.children.empty()) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	const auto rowCount = int(node.children.size());
	if (rowCount > limits.maxRows) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	auto cellCount = 0;
	for (const auto &row : node.children) {
		if (row.kind != NodeKind::TableRow || row.children.empty()) {
			if (state) {
				state->addPrepareWarning();
			}
			return true;
		}
		const auto width = EffectiveTableRowWidth(row);
		if (!width || width > limits.maxColumns) {
			if (state) {
				state->addPrepareWarning();
			}
			return true;
		}
		cellCount += width;
		if (cellCount > limits.maxCells) {
			if (state) {
				state->addPrepareWarning();
			}
			return true;
		}
	}
	const auto columnCount = EffectiveTableColumnCount(node);
	if (!columnCount || columnCount > limits.maxColumns) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	if ((rowCount * columnCount) > limits.maxCells) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	return false;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFallbackBlocks(
	const MarkdownNode &node,
	PrepareContext context,
	PrepareState *state);

[[nodiscard]] std::vector<PreparedBlock> PrepareTableBlocks(
		const MarkdownNode &node,
		PrepareContext context,
	PrepareState *state) {
	const auto columnCount = EffectiveTableColumnCount(node);
	if (ShouldFlattenTable(node, context, state) || !columnCount) {
		return PrepareFallbackBlocks(node, context, state);
	}

	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Table;
	block.tableColumnCount = columnCount;
	block.tableAlignments = NormalizedTableAlignments(node, columnCount);
	block.tableRows.reserve(node.children.size());

	for (const auto &rowNode : node.children) {
		if (rowNode.kind != NodeKind::TableRow) {
			return PrepareFallbackBlocks(node, context, state);
		}

		auto row = PreparedTableRow();
		row.header = rowNode.tableHeader;
		row.cells.reserve(columnCount);

		auto expectedColumn = 0;
		for (const auto &cellNode : rowNode.children) {
			if (cellNode.kind != NodeKind::TableCell) {
				return PrepareFallbackBlocks(node, context, state);
			}
			const auto column = (cellNode.tableColumn >= 0)
				? cellNode.tableColumn
				: expectedColumn;
			if (column != expectedColumn || column >= columnCount) {
				return PrepareFallbackBlocks(node, context, state);
			}

			auto cell = PreparedTableCell();
			cell.column = column;
			cell.alignment = block.tableAlignments[column];
			PrepareTableCellText(
				cellNode,
				rowNode.tableHeader,
				&cell.text,
				&cell.links,
				state);
			row.cells.push_back(std::move(cell));
			++expectedColumn;
		}
		for (auto column = expectedColumn; column != columnCount; ++column) {
			auto cell = PreparedTableCell();
			cell.column = column;
			cell.alignment = block.tableAlignments[column];
			row.cells.push_back(std::move(cell));
		}
		block.tableRows.push_back(std::move(row));
	}
	return { std::move(block) };
}

void AppendPrepared(
		std::vector<PreparedBlock> &&from,
		std::vector<PreparedBlock> *to) {
	for (auto &block : from) {
		to->push_back(std::move(block));
	}
}

void AppendRichBlock(
		std::vector<PreparedBlock> *blocks,
		PreparedBlockKind kind,
		int headingLevel,
		TextWithEntities text,
		std::vector<PreparedLink> links,
		QString anchorId = QString(),
		bool collapsed = false,
		bool allowEmpty = false) {
	SortEntities(&text);
	if (text.text.isEmpty() && !allowEmpty) {
		return;
	}
	auto block = PreparedBlock();
	block.kind = kind;
	block.headingLevel = headingLevel;
	block.text = std::move(text);
	block.links = std::move(links);
	block.anchorId = std::move(anchorId);
	block.collapsed = collapsed;
	blocks->push_back(std::move(block));
}

[[nodiscard]] PreparedBlock EmptyParagraphBlock() {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Paragraph;
	return block;
}

[[nodiscard]] PreparedBlock PrepareCodeBlock(const MarkdownNode &node) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::CodeBlock;
	block.text.text = node.text;
	block.codeLanguage = FirstInfoToken(node.info);
	return block;
}

[[nodiscard]] PreparedBlock PrepareRuleBlock() {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Rule;
	return block;
}

[[nodiscard]] PreparedBlock PrepareDisplayMathBlock(
		const MarkdownNode &node,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::DisplayMath;
	block.formulaTex = !node.text.isEmpty() ? node.text : node.raw;
	block.mathKind = MathKind::Display;
	block.formulaIndex = node.formulaIndex;
	state->rememberFormula(block);
	return block;
}

void CollectFootnoteDefinitions(
		const MarkdownNode &node,
		std::vector<FootnoteDefinitionEntry> *definitions) {
	if (!definitions) {
		return;
	}
	if (node.kind == NodeKind::FootnoteDefinition && node.footnoteOrdinal > 0) {
		if (node.footnoteOrdinal > int(definitions->size())) {
			definitions->resize(node.footnoteOrdinal);
		}
		auto &entry = (*definitions)[node.footnoteOrdinal - 1];
		if (!entry.node) {
			entry.node = &node;
		}
	}
	for (const auto &child : node.children) {
		CollectFootnoteDefinitions(child, definitions);
	}
}

[[nodiscard]] std::vector<PreparedBlock> PrepareBlocks(
	const MarkdownNode &node,
	PrepareContext context,
	PrepareState *state);

[[nodiscard]] std::vector<PreparedBlock> PrepareChildren(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	auto result = std::vector<PreparedBlock>();
	for (const auto &child : node.children) {
		AppendPrepared(PrepareBlocks(child, context, state), &result);
	}
	return result;
}

void AppendFootnoteBacklink(PreparedBlock *block, const QString &target) {
	if (!block || target.isEmpty()) {
		return;
	}
	const auto index = block->links.size() + 1;
	if (index > std::numeric_limits<uint16>::max()) {
		return;
	}
	if (!block->text.text.isEmpty()) {
		block->text.append(QChar(' '));
	}
	const auto from = block->text.text.size();
	const auto label = u"[back]"_q;
	block->text.append(label);
	block->text.entities.push_back(EntityInText(
		EntityType::CustomUrl,
		from,
		label.size(),
		InternalLinkData(uint16(index))));
	block->links.push_back({
		.index = uint16(index),
		.kind = PreparedLinkKind::FootnoteBacklink,
		.target = target,
		.copyText = u"#"_q + target,
	});
	SortEntities(&block->text);
}

void AppendFootnotes(
		std::vector<PreparedBlock> *blocks,
		PrepareState *state) {
	if (!blocks || !state || state->footnoteDefinitions.empty()) {
		return;
	}
	auto list = PreparedBlock();
	list.kind = PreparedBlockKind::List;
	list.listKind = ListKind::Ordered;
	list.listDelimiter = ListDelimiter::Period;
	list.startNumber = 1;
	for (const auto &entry : state->footnoteDefinitions) {
		if (!entry.node) {
			return;
		}
		auto item = PreparedBlock();
		item.kind = PreparedBlockKind::ListItem;
		item.listKind = ListKind::Ordered;
		item.listDelimiter = ListDelimiter::Period;
		item.orderedNumber = entry.node->footnoteOrdinal;
		item.anchorId = FootnoteDefinitionAnchor(*entry.node);
		item.children = PrepareChildren(*entry.node, {}, state);
		if (item.children.empty()) {
			item.children.push_back(EmptyParagraphBlock());
		}
		const auto backlink = state->firstFootnoteReferenceAnchor(
			entry.node->footnoteLabel);
		if (!item.children.empty()
			&& item.children.back().kind == PreparedBlockKind::Paragraph) {
			AppendFootnoteBacklink(&item.children.back(), backlink);
		} else if (!backlink.isEmpty()) {
			auto paragraph = EmptyParagraphBlock();
			AppendFootnoteBacklink(&paragraph, backlink);
			item.children.push_back(std::move(paragraph));
		}
		list.children.push_back(std::move(item));
	}
	if (list.children.empty()) {
		return;
	}
	blocks->push_back(PrepareRuleBlock());
	blocks->push_back(std::move(list));
}

[[nodiscard]] std::vector<PreparedBlock> PrepareNestedDetailsBody(
		const MarkdownNode &node,
		PrepareState *state) {
	const auto fallback = [&] {
		if (state) {
			state->addPrepareWarning();
		}
		auto blocks = std::vector<PreparedBlock>();
		AppendRichBlock(
			&blocks,
			PreparedBlockKind::Paragraph,
			0,
			TextWithEntities::Simple(node.detailsBody),
			std::vector<PreparedLink>());
		return blocks;
	};
	if (node.detailsBody.isEmpty()) {
		return {};
	}
	const auto parsed = ParseMarkdownForIv(
		node.detailsBody.toUtf8(),
		ParseOptions{ state->request->document->sourceName + u"#details"_q });
	if (!parsed.ok
		|| !parsed.document.formulas.empty()
		|| parsed.document.stats.footnotesSeen) {
		return fallback();
	}
	auto nestedRequest = PrepareRequest{
		.document = std::make_shared<const PreparedDocument>(parsed.document),
		.renderer = state->request->renderer,
		.dimensions = state->request->dimensions,
		.sourcePath = state->request->sourcePath,
	};
	auto nested = PrepareSynchronously(std::move(nestedRequest));
	state->addPrepareWarnings(nested.debug.prepareWarningCount);
	state->addFormulaWarnings(nested.debug.formulaWarningCount);
	return nested.failure.failed()
		? fallback()
		: std::move(nested.blocks.blocks);
}

[[nodiscard]] std::vector<PreparedBlock> PrepareDetailsBlocks(
		const MarkdownNode &node,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Details;
	block.anchorId = DetailsAnchorId(state);
	block.collapsed = !node.detailsOpen;
	block.text.text = (node.detailsOpen ? u"v "_q : u"> "_q)
		+ node.detailsSummary;
	if (!block.text.text.isEmpty()) {
		block.text.entities.push_back(EntityInText(
			EntityType::CustomUrl,
			0,
			block.text.text.size(),
			InternalLinkData(1)));
		block.links.push_back({
			.index = 1,
			.kind = PreparedLinkKind::ToggleDetails,
			.target = block.anchorId,
		});
	}
	block.children = PrepareNestedDetailsBody(node, state);
	return { std::move(block) };
}

[[nodiscard]] std::vector<PreparedBlock> PrepareDocumentBlocks(
		const MarkdownNode &node,
		PrepareState *state) {
	auto result = PrepareChildren(node, {}, state);
	AppendFootnotes(&result, state);
	return result;
}


[[nodiscard]] std::vector<PreparedBlock> PrepareFlowBlock(
		const MarkdownNode &node,
		PreparedBlockKind kind,
		PrepareState *state) {
	auto result = std::vector<PreparedBlock>();
	auto anchorId = (kind == PreparedBlockKind::Heading)
		? node.anchorId
		: QString();
	auto text = TextWithEntities();
	auto links = std::vector<PreparedLink>();
	const auto textSize = FlowFormulaTextSize(
		kind,
		(kind == PreparedBlockKind::Heading) ? node.headingLevel : 0,
		state->request->dimensions);
	PrepareInlineRichText(
		node,
		textSize,
		ScaleFormulaCap(
			state->request->dimensions.displayMathMaxRenderWidth,
			textSize,
			state->request->dimensions.displayMathTextSize),
		ScaleFormulaCap(
			state->request->dimensions.displayMathMaxRenderHeight,
			textSize,
			state->request->dimensions.displayMathTextSize),
		&anchorId,
		&text,
		&links,
		state);
	AppendRichBlock(
		&result,
		kind,
		(kind == PreparedBlockKind::Heading) ? node.headingLevel : 0,
		std::move(text),
		std::move(links),
		std::move(anchorId));
	return result;
}

[[nodiscard]] PreparedBlock PrepareListItemBlock(
		const MarkdownNode &node,
		PrepareContext context,
		const PreparedBlock &list,
		int orderedNumber,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::ListItem;
	block.listKind = list.listKind;
	block.listDelimiter = list.listDelimiter;
	block.taskState = node.taskState;
	block.orderedNumber = orderedNumber;
	block.actualDepth = list.actualDepth;
	block.visualDepth = list.visualDepth;
	block.depthClamped = list.depthClamped;

	auto childContext = context;
	childContext.listDepth = context.listDepth + 1;
	for (const auto &child : node.children) {
		AppendPrepared(PrepareBlocks(child, childContext, state), &block.children);
	}
	if (block.children.empty()) {
		block.children.push_back(EmptyParagraphBlock());
	}
	return block;
}

[[nodiscard]] PreparedBlock PrepareListBlock(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::List;
	block.listKind = node.listKind;
	block.listDelimiter = node.listDelimiter;
	block.startNumber = (node.listKind == ListKind::Ordered && node.listStart > 0)
		? node.listStart
		: 1;
	block.actualDepth = context.listDepth;
	block.visualDepth = CappedListDepth(block.actualDepth);
	block.depthClamped = (block.actualDepth > block.visualDepth);
	block.tight = node.tight;

	auto nextNumber = block.startNumber;
	auto childContext = context;
	childContext.listDepth = context.listDepth + 1;
	for (const auto &child : node.children) {
		if (child.kind == NodeKind::ListItem) {
			auto item = PrepareListItemBlock(
				child,
				context,
				block,
				(node.listKind == ListKind::Ordered) ? nextNumber : 0,
				state);
			block.children.push_back(std::move(item));
			if (node.listKind == ListKind::Ordered) {
				++nextNumber;
			}
		} else {
			AppendPrepared(PrepareBlocks(child, childContext, state), &block.children);
		}
	}
	return block;
}

[[nodiscard]] PreparedBlock PrepareQuoteBlock(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Quote;
	block.actualDepth = context.quoteDepth;
	block.visualDepth = CappedQuoteDepth(block.actualDepth);
	block.depthClamped = (block.actualDepth > block.visualDepth);

	auto childContext = context;
	childContext.quoteDepth = context.quoteDepth + 1;
	block.children = PrepareChildren(node, childContext, state);
	return block;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFallbackBlocks(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	if (node.kind == NodeKind::HtmlBlock) {
		if (node.htmlBlockKind == HtmlBlockKind::Comment) {
			return {};
		} else if (node.htmlBlockKind == HtmlBlockKind::Details) {
			return PrepareDetailsBlocks(node, state);
		}
	}
	if (!node.children.empty()) {
		return PrepareChildren(node, context, state);
	}
	const auto text = !node.text.isEmpty() ? node.text : node.raw;
	if (text.isEmpty()) {
		return {};
	}
	auto result = std::vector<PreparedBlock>();
	AppendRichBlock(
		&result,
		PreparedBlockKind::Paragraph,
		0,
		TextWithEntities::Simple(text),
		std::vector<PreparedLink>());
	return result;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareBlocks(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	switch (node.kind) {
	case NodeKind::Document:
		return PrepareDocumentBlocks(node, state);
	case NodeKind::TableRow:
	case NodeKind::TableCell:
	case NodeKind::HtmlBlock:
	case NodeKind::Unsupported:
		return PrepareFallbackBlocks(node, context, state);
	case NodeKind::DisplayMath:
		return { PrepareDisplayMathBlock(node, state) };
	case NodeKind::Paragraph:
		return PrepareFlowBlock(node, PreparedBlockKind::Paragraph, state);
	case NodeKind::Heading:
		return PrepareFlowBlock(node, PreparedBlockKind::Heading, state);
	case NodeKind::FootnoteDefinition:
		return {};
	case NodeKind::CodeBlock:
		return { PrepareCodeBlock(node) };
	case NodeKind::ThematicBreak:
		return { PrepareRuleBlock() };
	case NodeKind::List: {
		auto block = PrepareListBlock(node, context, state);
		return { std::move(block) };
	} break;
	case NodeKind::ListItem: {
		auto list = PreparedBlock();
		list.kind = PreparedBlockKind::List;
		list.actualDepth = context.listDepth;
		list.visualDepth = CappedListDepth(list.actualDepth);
		list.depthClamped = (list.actualDepth > list.visualDepth);
		auto block = PrepareListItemBlock(node, context, list, 0, state);
		return { std::move(block) };
	} break;
	case NodeKind::Blockquote: {
		auto block = PrepareQuoteBlock(node, context, state);
		return { std::move(block) };
	} break;
	case NodeKind::Table:
		return PrepareTableBlocks(node, context, state);
	default:
		return PrepareFallbackBlocks(node, context, state);
	}
	return {};
}

} // namespace

PreparedRenderDocument PrepareRenderData(
		const PreparedDocument &document,
		PrepareState *state) {
	auto result = PreparedRenderDocument();
	state->footnoteDefinitions.clear();
	CollectFootnoteDefinitions(document.document, &state->footnoteDefinitions);
	result.blocks = PrepareBlocks(document.document, {}, state);
	return result;
}

} // namespace Iv::Markdown
