#include "iv/markdown/iv_markdown_prepare.h"

#include "base/call_delayed.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "ui/style/style_core.h"

#include "styles/palette.h"
#include "styles/style_iv.h"

namespace Iv::Markdown {
namespace {

constexpr auto kMaxVisualListDepth = 6;
constexpr auto kMaxVisualQuoteDepth = 3;
constexpr auto kMaxRenderedTableRows = 128;
constexpr auto kMaxRenderedTableColumns = 16;
constexpr auto kMaxRenderedTableCells = 1024;

struct PrepareContext {
	int listDepth = 0;
	int quoteDepth = 0;
};

struct PrepareState {
	const PrepareRequest *request = nullptr;
	PreparedResult result;

	[[nodiscard]] bool cancelled() {
		if (!request->cancelled) {
			return false;
		} else if (!request->cancelled->load(std::memory_order_relaxed)) {
			return false;
		}
		result.cancelled = true;
		return true;
	}

	void rememberFormula(const PreparedBlock &block) {
		if (block.formulaIndex < 0) {
			return;
		}
		const auto index = block.formulaIndex;
		if (index >= int(result.formulas.size())) {
			result.formulas.resize(index + 1);
		}
		auto &slot = result.formulas[index];
		slot.trimmedTex = block.formulaTex.trimmed();
		slot.kind = block.mathKind;
		slot.present = true;
	}
};

void ClearPreparedOutput(PreparedResult *result) {
	result->blocks.blocks.clear();
	result->formulas.clear();
}

[[nodiscard]] QString InternalLinkData(uint16 index) {
	return u"internal:index"_q + QChar(index);
}

[[nodiscard]] int CappedListDepth(int depth) {
	return std::min(depth, kMaxVisualListDepth);
}

[[nodiscard]] int CappedQuoteDepth(int depth) {
	return std::min(depth, kMaxVisualQuoteDepth);
}

[[nodiscard]] QString FirstInfoToken(const QString &info) {
	const auto trimmed = info.trimmed();
	for (auto i = 0; i != trimmed.size(); ++i) {
		if (trimmed[i].isSpace()) {
			return trimmed.left(i);
		}
	}
	return trimmed;
}

void SortEntities(TextWithEntities *text) {
	auto &entities = text->entities;
	std::sort(
		entities.begin(),
		entities.end(),
		[](const EntityInText &left, const EntityInText &right) {
			if (left.offset() != right.offset()) {
				return left.offset() < right.offset();
			} else if (left.length() != right.length()) {
				return left.length() > right.length();
			}
			return int(left.type()) < int(right.type());
		});
}

void AppendInline(
	const MarkdownNode &node,
	TextWithEntities *text,
	std::vector<PreparedLink> *links,
	PrepareState *state);

void AppendInlineChildren(
		const MarkdownNode &node,
		TextWithEntities *text,
		std::vector<PreparedLink> *links,
		PrepareState *state) {
	for (const auto &child : node.children) {
		if (state->cancelled()) {
			return;
		}
		AppendInline(child, text, links, state);
	}
}

void AppendInline(
		const MarkdownNode &node,
		TextWithEntities *text,
		std::vector<PreparedLink> *links,
		PrepareState *state) {
	if (state->cancelled()) {
		return;
	}

	const auto from = text->text.size();
	switch (node.kind) {
	case NodeKind::Text:
	case NodeKind::InlineMath:
		text->append(node.text);
		break;
	case NodeKind::SoftBreak:
		text->append(QChar(' '));
		break;
	case NodeKind::LineBreak:
		text->append(QChar('\n'));
		break;
	case NodeKind::Emphasis:
		AppendInlineChildren(node, text, links, state);
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::Italic,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::Strong:
		AppendInlineChildren(node, text, links, state);
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::Bold,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::Strike:
		AppendInlineChildren(node, text, links, state);
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::StrikeOut,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::InlineCode:
		if (!node.text.isEmpty()) {
			text->append(node.text);
		} else {
			AppendInlineChildren(node, text, links, state);
		}
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::Code,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::Link: {
		AppendInlineChildren(node, text, links, state);
		if (text->text.size() == from && !node.url.isEmpty()) {
			text->append(node.url);
		}
		const auto length = text->text.size() - from;
		if (length <= 0 || node.url.isEmpty()) {
			break;
		}
		const auto index = links->size() + 1;
		if (index > std::numeric_limits<uint16>::max()) {
			break;
		}
		text->entities.push_back(
			EntityInText(
				EntityType::CustomUrl,
				from,
				length,
				InternalLinkData(uint16(index))));
		links->push_back({
			.index = uint16(index),
			.target = node.url,
		});
	} break;
	case NodeKind::HtmlInline:
	case NodeKind::Unsupported:
		if (!node.children.empty()) {
			AppendInlineChildren(node, text, links, state);
		} else if (!node.text.isEmpty()) {
			text->append(node.text);
		}
		break;
	default:
		if (!node.children.empty()) {
			AppendInlineChildren(node, text, links, state);
		} else if (!node.text.isEmpty()) {
			text->append(node.text);
		}
		break;
	}
}

void PrepareTableCellText(
		const MarkdownNode &cell,
		TextWithEntities *text,
		std::vector<PreparedLink> *links,
		PrepareState *state) {
	if (!cell.children.empty()) {
		AppendInlineChildren(cell, text, links, state);
	} else if (!cell.text.isEmpty()) {
		text->append(cell.text);
	} else if (!cell.raw.isEmpty()) {
		text->append(cell.raw);
	}
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
		PrepareContext context) {
	if (context.listDepth > 0 || context.quoteDepth > 0) {
		return true;
	}
	if (node.children.empty()) {
		return true;
	}
	const auto rowCount = int(node.children.size());
	if (rowCount > kMaxRenderedTableRows) {
		return true;
	}
	auto cellCount = 0;
	for (const auto &row : node.children) {
		if (row.kind != NodeKind::TableRow || row.children.empty()) {
			return true;
		}
		const auto width = EffectiveTableRowWidth(row);
		if (!width || width > kMaxRenderedTableColumns) {
			return true;
		}
		cellCount += width;
		if (cellCount > kMaxRenderedTableCells) {
			return true;
		}
	}
	const auto columnCount = EffectiveTableColumnCount(node);
	if (!columnCount || columnCount > kMaxRenderedTableColumns) {
		return true;
	}
	return (rowCount * columnCount) > kMaxRenderedTableCells;
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
	if (ShouldFlattenTable(node, context) || !columnCount) {
		return PrepareFallbackBlocks(node, context, state);
	}

	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Table;
	block.tableColumnCount = columnCount;
	block.tableAlignments = NormalizedTableAlignments(node, columnCount);
	block.tableRows.reserve(node.children.size());

	for (const auto &rowNode : node.children) {
		if (state->cancelled()) {
			return {};
		} else if (rowNode.kind != NodeKind::TableRow) {
			return PrepareFallbackBlocks(node, context, state);
		}

		auto row = PreparedTableRow();
		row.header = rowNode.tableHeader;
		row.cells.reserve(columnCount);

		auto expectedColumn = 0;
		for (const auto &cellNode : rowNode.children) {
			if (state->cancelled()) {
				return {};
			} else if (cellNode.kind != NodeKind::TableCell) {
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
			PrepareTableCellText(cellNode, &cell.text, &cell.links, state);
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
		if (state->cancelled()) {
			return {};
		}
		AppendPrepared(PrepareBlocks(child, context, state), &result);
	}
	return result;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFlowBlock(
		const MarkdownNode &node,
		PreparedBlockKind kind,
		PrepareState *state) {
	auto result = std::vector<PreparedBlock>();
	auto text = TextWithEntities();
	auto links = std::vector<PreparedLink>();
	if (!node.children.empty()) {
		AppendInlineChildren(node, &text, &links, state);
	} else if (!node.text.isEmpty()) {
		text.append(node.text);
	}
	if (state->cancelled()) {
		return {};
	}
	AppendRichBlock(
		&result,
		kind,
		(kind == PreparedBlockKind::Heading) ? node.headingLevel : 0,
		std::move(text),
		std::move(links));
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
		if (state->cancelled()) {
			return {};
		}
		AppendPrepared(PrepareBlocks(child, childContext, state), &block.children);
		if (state->result.cancelled) {
			return {};
		}
	}
	if (block.children.empty() && !state->result.cancelled) {
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
		if (state->cancelled()) {
			return {};
		}
		if (child.kind == NodeKind::ListItem) {
			auto item = PrepareListItemBlock(
				child,
				context,
				block,
				(node.listKind == ListKind::Ordered) ? nextNumber : 0,
				state);
			if (state->result.cancelled) {
				return {};
			}
			block.children.push_back(std::move(item));
			if (node.listKind == ListKind::Ordered) {
				++nextNumber;
			}
		} else {
			AppendPrepared(PrepareBlocks(child, childContext, state), &block.children);
			if (state->result.cancelled) {
				return {};
			}
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
	if (state->result.cancelled) {
		return {};
	}
	return block;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFallbackBlocks(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	if (state->cancelled()) {
		return {};
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
	if (state->cancelled()) {
		return {};
	}

	switch (node.kind) {
	case NodeKind::Document:
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
	case NodeKind::CodeBlock:
		return { PrepareCodeBlock(node) };
	case NodeKind::ThematicBreak:
		return { PrepareRuleBlock() };
	case NodeKind::List: {
		auto block = PrepareListBlock(node, context, state);
		if (state->result.cancelled) {
			return {};
		}
		return { std::move(block) };
	} break;
	case NodeKind::ListItem: {
		auto list = PreparedBlock();
		list.kind = PreparedBlockKind::List;
		list.actualDepth = context.listDepth;
		list.visualDepth = CappedListDepth(list.actualDepth);
		list.depthClamped = (list.actualDepth > list.visualDepth);
		auto block = PrepareListItemBlock(node, context, list, 0, state);
		if (state->result.cancelled) {
			return {};
		}
		return { std::move(block) };
	} break;
	case NodeKind::Blockquote: {
		auto block = PrepareQuoteBlock(node, context, state);
		if (state->result.cancelled) {
			return {};
		}
		return { std::move(block) };
	} break;
	case NodeKind::Table:
		return PrepareTableBlocks(node, context, state);
	default:
		return PrepareFallbackBlocks(node, context, state);
	}
	return {};
}

[[nodiscard]] PreparedRenderDocument PrepareRenderData(
		const PreparedDocument &document,
		PrepareState *state) {
	auto result = PreparedRenderDocument();
	result.blocks = PrepareBlocks(document.document, {}, state);
	return result;
}

[[nodiscard]] int FormulaSlotCount(const PreparedDocument &document) {
	auto result = 0;
	for (const auto &formula : document.formulas) {
		result = std::max(result, formula.index + 1);
	}
	return result;
}

[[nodiscard]] QColor Resolve(style::color color) {
	return color->c;
}

[[nodiscard]] bool RenderPreparedFormulas(PrepareState *state) {
	const auto &style = state->result.style;
	auto renderer = MathRenderer();
	for (auto &slot : state->result.formulas) {
		if (!slot.present) {
			continue;
		}
		if (state->cancelled()) {
			return false;
		}
		slot.rendered = renderer.renderFormula({
			.trimmedTex = slot.trimmedTex,
			.kind = slot.kind,
			.textSize = style.displayMathTextSize,
			.renderWidthCap = style.displayMathMaxRenderWidth,
			.renderHeightCap = style.displayMathMaxRenderHeight,
			.foreground = style.displayMathForegroundColor,
			.devicePixelRatio = style.devicePixelRatio,
		}, style.paletteVersion);
		if (state->cancelled()) {
			return false;
		}
	}
	return true;
}

} // namespace

MarkdownStyleSnapshot CaptureMarkdownStyleSnapshot() {
	auto result = MarkdownStyleSnapshot();
	result.textPalette = {
		.link = Resolve(st::ivMarkdownTextPalette.linkFg),
		.mono = Resolve(st::ivMarkdownTextPalette.monoFg),
		.spoiler = Resolve(st::ivMarkdownTextPalette.spoilerFg),
		.selectBackground = Resolve(st::ivMarkdownTextPalette.selectBg),
		.selectText = Resolve(st::ivMarkdownTextPalette.selectFg),
		.selectLink = Resolve(st::ivMarkdownTextPalette.selectLinkFg),
		.selectMono = Resolve(st::ivMarkdownTextPalette.selectMonoFg),
		.selectSpoiler = Resolve(st::ivMarkdownTextPalette.selectSpoilerFg),
		.selectOverlay = Resolve(st::ivMarkdownTextPalette.selectOverlay),
		.linkAlwaysActive = st::ivMarkdownTextPalette.linkAlwaysActive,
	};
	result.paragraphStyle = st::ivMarkdownParagraphStyle;
	result.heading1Style = st::ivMarkdownHeading1Style;
	result.heading2Style = st::ivMarkdownHeading2Style;
	result.heading3Style = st::ivMarkdownHeading3Style;
	result.heading4Style = st::ivMarkdownHeading4Style;
	result.heading5Style = st::ivMarkdownHeading5Style;
	result.heading6Style = st::ivMarkdownHeading6Style;
	result.codeStyle = st::ivMarkdownCodeStyle;
	result.codeLanguageStyle = st::ivMarkdownCodeLanguageStyle;
	result.displayMathFallbackStyle = st::ivMarkdownDisplayMathFallbackStyle;
	result.tableHeaderStyle = st::ivMarkdownTableHeaderStyle;
	result.pagePadding = st::ivMarkdownPagePadding;
	result.quotePadding = st::ivMarkdownQuotePadding;
	result.codePadding = st::ivMarkdownCodePadding;
	result.displayMathPadding = st::ivMarkdownDisplayMathPadding;
	result.displayMathFallbackPadding = st::ivMarkdownDisplayMathFallbackPadding;
	result.tableCellPadding = st::ivMarkdownTableCellPadding;
	result.displayMathAlign = st::ivMarkdownDisplayMathAlign;
	result.defaultTextColor = Resolve(st::windowFg);
	result.codeLanguageColor = Resolve(st::ivMarkdownCodeLanguageFg);
	result.taskMarkerColor = Resolve(st::ivMarkdownTaskMarkerFg);
	result.taskMarkerCheckColor = Resolve(st::ivMarkdownTaskMarkerCheckFg);
	result.quoteBorderColor = Resolve(st::ivMarkdownQuoteBorderFg);
	result.codeBackgroundColor = Resolve(st::ivMarkdownCodeBg);
	result.ruleColor = Resolve(st::ivMarkdownRuleFg);
	result.displayMathForegroundColor = Resolve(st::windowFg);
	result.displayMathFallbackBackgroundColor = Resolve(
		st::ivMarkdownDisplayMathFallbackBg);
	result.displayMathOverflowColor = Resolve(st::ivMarkdownDisplayMathOverflowFg);
	result.tableBorderColor = Resolve(st::ivMarkdownTableBorderFg);
	result.tableHeaderBackgroundColor = Resolve(st::ivMarkdownTableHeaderBg);
	result.tableOverflowColor = Resolve(st::ivMarkdownTableOverflowFg);
	result.paragraphSkip = st::ivMarkdownParagraphSkip;
	result.headingSkip = st::ivMarkdownHeadingSkip;
	result.codeSkip = st::ivMarkdownCodeSkip;
	result.ruleSkip = st::ivMarkdownRuleSkip;
	result.displayMathSkip = st::ivMarkdownDisplayMathSkip;
	result.tableSkip = st::ivMarkdownTableSkip;
	result.quoteSkip = st::ivMarkdownQuoteSkip;
	result.listIndent = st::ivMarkdownListIndent;
	result.listContinuationIndent = st::ivMarkdownListContinuationIndent;
	result.listMarkerWidth = st::ivMarkdownListMarkerWidth;
	result.listMarkerSkip = st::ivMarkdownListMarkerSkip;
	result.taskMarkerSize = st::ivMarkdownTaskMarkerSize;
	result.taskMarkerBorder = st::ivMarkdownTaskMarkerBorder;
	result.quoteIndent = st::ivMarkdownQuoteIndent;
	result.quoteBorder = st::ivMarkdownQuoteBorder;
	result.codeRadius = st::ivMarkdownCodeRadius;
	result.codeLanguageSkip = st::ivMarkdownCodeLanguageSkip;
	result.ruleHeight = st::ivMarkdownRuleHeight;
	result.displayMathTextSize = st::ivMarkdownDisplayMathTextSize;
	result.displayMathMaxRenderWidth = st::ivMarkdownDisplayMathMaxRenderWidth;
	result.displayMathMaxRenderHeight = st::ivMarkdownDisplayMathMaxRenderHeight;
	result.displayMathFallbackRadius = st::ivMarkdownDisplayMathFallbackRadius;
	result.displayMathOverflowWidth = st::ivMarkdownDisplayMathOverflowWidth;
	result.tableBorder = st::ivMarkdownTableBorder;
	result.tableMinColumnWidth = st::ivMarkdownTableMinColumnWidth;
	result.tableOverflowWidth = st::ivMarkdownTableOverflowWidth;
	result.paletteVersion = style::PaletteVersion();
	result.devicePixelRatio = style::DevicePixelRatio();
	return result;
}

PreparedResult PrepareSynchronously(PrepareRequest request) {
	auto state = PrepareState();
	state.request = &request;
	state.result.style = request.style;
	state.result.generation = request.generation;

	if (!request.document) {
		return state.result;
	}

	state.result.formulas.resize(FormulaSlotCount(*request.document));
	if (state.cancelled()) {
		return std::move(state.result);
	}

	state.result.blocks = PrepareRenderData(*request.document, &state);
	if (state.result.cancelled) {
		ClearPreparedOutput(&state.result);
		return std::move(state.result);
	}
	if (!RenderPreparedFormulas(&state)) {
		ClearPreparedOutput(&state.result);
	}
	return std::move(state.result);
}

void PrepareAsync(PrepareRequest request, Fn<void(PreparedResult)> done) {
	if (!done) {
		return;
	}
	base::call_delayed(0, [
		request = std::move(request),
		done = std::move(done)
	]() mutable {
		done(PrepareSynchronously(std::move(request)));
	});
}

} // namespace Iv::Markdown
