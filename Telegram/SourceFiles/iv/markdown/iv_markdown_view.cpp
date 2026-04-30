#include "iv/markdown/iv_markdown_view.h"

#include <QtCore/QDebug>

#include "base/weak_ptr.h"
#include "core/credits_amount.h"
#include "core/click_handler_types.h"
#include "ui/text/text.h"
#include "ui/widgets/scroll_area.h"
#include "ui/click_handler.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPen>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "styles/palette.h"

#include "styles/style_iv.h"
#include "styles/style_layers.h"

namespace Iv::Markdown {
namespace {

constexpr auto kIvMarkedTextOptions = TextParseOptions{
	TextParseMultiline,
	0,
	0,
	Qt::LayoutDirectionAuto,
};

enum class PreparedBlockKind {
	Paragraph,
	Heading,
	CodeBlock,
	Rule,
	List,
	ListItem,
	Quote,
	Table,
};

struct PreparedLink {
	uint16 index = 0;
	QString target;
};

struct PreparedTableCell {
	TextWithEntities text;
	std::vector<PreparedLink> links;
	int column = 0;
	TableAlignment alignment = TableAlignment::None;
};

struct PreparedTableRow {
	std::vector<PreparedTableCell> cells;
	bool header = false;
};

struct PreparedBlock {
	PreparedBlockKind kind = PreparedBlockKind::Paragraph;
	TextWithEntities text;
	std::vector<PreparedLink> links;
	std::vector<PreparedBlock> children;
	std::vector<PreparedTableRow> tableRows;
	std::vector<TableAlignment> tableAlignments;
	QString codeLanguage;
	ListKind listKind = ListKind::Bullet;
	ListDelimiter listDelimiter = ListDelimiter::None;
	TaskState taskState = TaskState::None;
	int headingLevel = 0;
	int orderedNumber = 0;
	int startNumber = 1;
	int actualDepth = 0;
	int visualDepth = 0;
	int tableColumnCount = 0;
	bool depthClamped = false;
	bool tight = false;
};

struct PreparedRenderDocument {
	std::vector<PreparedBlock> blocks;
};

struct PrepareContext {
	int listDepth = 0;
	int quoteDepth = 0;
};

struct LaidOutTableCell {
	Ui::Text::String leaf;
	QRect outer;
	QRect textRect;
	int textWidth = 0;
	style::align align = style::al_left;
};

struct LaidOutTableRow {
	std::vector<LaidOutTableCell> cells;
	QRect outer;
	bool header = false;
};

struct LaidOutBlock {
	PreparedBlockKind kind = PreparedBlockKind::Paragraph;
	Ui::Text::String leaf;
	Ui::Text::String marker;
	Ui::Text::String language;
	std::vector<LaidOutBlock> children;
	std::vector<LaidOutTableRow> tableRows;
	std::vector<int> tableColumnWidths;
	QRect outer;
	QRect textRect;
	QRect markerRect;
	QRect contentRect;
	QRect borderRect;
	QRect languageRect;
	QRect tableRect;
	QRect visibleTableRect;
	int textWidth = 0;
	int markerWidth = 0;
	int headingLevel = 0;
	ListKind listKind = ListKind::Bullet;
	ListDelimiter listDelimiter = ListDelimiter::None;
	TaskState taskState = TaskState::None;
	int orderedNumber = 0;
	bool overflowed = false;
};

struct LayoutContext {
	int listDepth = 0;
	int quoteDepth = 0;
	bool tightList = false;
};

constexpr auto kMaxVisualListDepth = 6;
constexpr auto kMaxVisualQuoteDepth = 3;
constexpr auto kCodeTabColumns = 4;
constexpr auto kCodeTrailingGuard = 0x2060;
constexpr auto kMaxRenderedTableRows = 128;
constexpr auto kMaxRenderedTableColumns = 16;
constexpr auto kMaxRenderedTableCells = 1024;

[[nodiscard]] QString InternalLinkData(uint16 index) {
	return QStringLiteral("internal:index") + QChar(index);
}

[[nodiscard]] bool IsFlowKind(PreparedBlockKind kind) {
	return (kind == PreparedBlockKind::Paragraph)
		|| (kind == PreparedBlockKind::Heading);
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

[[nodiscard]] QString ListMarkerText(const PreparedBlock &block) {
	if (block.listKind == ListKind::Ordered) {
		const auto delimiter = (block.listDelimiter == ListDelimiter::Parenthesis)
			? QStringLiteral(")")
			: QStringLiteral(".");
		return QString::number(block.orderedNumber) + delimiter;
	}
	return Ui::kQBullet;
}

[[nodiscard]] int TextLineHeight(const style::TextStyle &style) {
	return std::max(style.lineHeight, style.font->height);
}

[[nodiscard]] Ui::Text::GeometryDescriptor TextGeometry(int width) {
	auto result = Ui::Text::SimpleGeometry(std::max(width, 1), 0, 0, false);
	result.breakEverywhere = true;
	return result;
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
		std::vector<PreparedLink> *links);

void AppendInlineChildren(
		const MarkdownNode &node,
		TextWithEntities *text,
		std::vector<PreparedLink> *links) {
	for (const auto &child : node.children) {
		AppendInline(child, text, links);
	}
}

void AppendInline(
		const MarkdownNode &node,
		TextWithEntities *text,
		std::vector<PreparedLink> *links) {
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
		AppendInlineChildren(node, text, links);
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::Italic,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::Strong:
		AppendInlineChildren(node, text, links);
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::Bold,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::Strike:
		AppendInlineChildren(node, text, links);
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
			AppendInlineChildren(node, text, links);
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
		AppendInlineChildren(node, text, links);
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
			AppendInlineChildren(node, text, links);
		} else if (!node.text.isEmpty()) {
			text->append(node.text);
		}
		break;
	default:
		if (!node.children.empty()) {
			AppendInlineChildren(node, text, links);
		} else if (!node.text.isEmpty()) {
			text->append(node.text);
		}
		break;
	}
}

void PrepareTableCellText(
		const MarkdownNode &cell,
		TextWithEntities *text,
		std::vector<PreparedLink> *links) {
	if (!cell.children.empty()) {
		AppendInlineChildren(cell, text, links);
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
		PrepareContext context);

[[nodiscard]] std::vector<PreparedBlock> PrepareTableBlocks(
		const MarkdownNode &node,
		PrepareContext context) {
	const auto columnCount = EffectiveTableColumnCount(node);
	if (ShouldFlattenTable(node, context) || !columnCount) {
		return PrepareFallbackBlocks(node, context);
	}

	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Table;
	block.tableColumnCount = columnCount;
	block.tableAlignments = NormalizedTableAlignments(node, columnCount);
	block.tableRows.reserve(node.children.size());

	for (const auto &rowNode : node.children) {
		if (rowNode.kind != NodeKind::TableRow) {
			return PrepareFallbackBlocks(node, context);
		}

		auto row = PreparedTableRow();
		row.header = rowNode.tableHeader;
		row.cells.reserve(columnCount);

		auto expectedColumn = 0;
		for (const auto &cellNode : rowNode.children) {
			if (cellNode.kind != NodeKind::TableCell) {
				return PrepareFallbackBlocks(node, context);
			}
			const auto column = (cellNode.tableColumn >= 0)
				? cellNode.tableColumn
				: expectedColumn;
			if (column != expectedColumn || column >= columnCount) {
				return PrepareFallbackBlocks(node, context);
			}

			auto cell = PreparedTableCell();
			cell.column = column;
			cell.alignment = block.tableAlignments[column];
			PrepareTableCellText(cellNode, &cell.text, &cell.links);
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

[[nodiscard]] std::vector<PreparedBlock> PrepareBlocks(
	const MarkdownNode &node,
	PrepareContext context);

[[nodiscard]] std::vector<PreparedBlock> PrepareChildren(
		const MarkdownNode &node,
		PrepareContext context) {
	auto result = std::vector<PreparedBlock>();
	for (const auto &child : node.children) {
		AppendPrepared(PrepareBlocks(child, context), &result);
	}
	return result;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFlowBlock(
		const MarkdownNode &node,
		PreparedBlockKind kind) {
	auto result = std::vector<PreparedBlock>();
	auto text = TextWithEntities();
	auto links = std::vector<PreparedLink>();
	if (!node.children.empty()) {
		AppendInlineChildren(node, &text, &links);
	} else if (!node.text.isEmpty()) {
		text.append(node.text);
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
		int orderedNumber) {
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
		AppendPrepared(PrepareBlocks(child, childContext), &block.children);
	}
	if (block.children.empty()) {
		block.children.push_back(EmptyParagraphBlock());
	}
	return block;
}

[[nodiscard]] PreparedBlock PrepareListBlock(
		const MarkdownNode &node,
		PrepareContext context) {
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
			block.children.push_back(PrepareListItemBlock(
				child,
				context,
				block,
				(node.listKind == ListKind::Ordered) ? nextNumber : 0));
			if (node.listKind == ListKind::Ordered) {
				++nextNumber;
			}
		} else {
			AppendPrepared(PrepareBlocks(child, childContext), &block.children);
		}
	}
	return block;
}

[[nodiscard]] PreparedBlock PrepareQuoteBlock(
		const MarkdownNode &node,
		PrepareContext context) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Quote;
	block.actualDepth = context.quoteDepth;
	block.visualDepth = CappedQuoteDepth(block.actualDepth);
	block.depthClamped = (block.actualDepth > block.visualDepth);

	auto childContext = context;
	childContext.quoteDepth = context.quoteDepth + 1;
	block.children = PrepareChildren(node, childContext);
	return block;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFallbackBlocks(
		const MarkdownNode &node,
		PrepareContext context) {
	if (!node.children.empty()) {
		return PrepareChildren(node, context);
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
		PrepareContext context) {
	switch (node.kind) {
	case NodeKind::Document:
	case NodeKind::TableRow:
	case NodeKind::TableCell:
	case NodeKind::HtmlBlock:
	case NodeKind::Unsupported:
		return PrepareFallbackBlocks(node, context);
	case NodeKind::DisplayMath:
		return {};
	case NodeKind::Paragraph:
		return PrepareFlowBlock(node, PreparedBlockKind::Paragraph);
	case NodeKind::Heading:
		return PrepareFlowBlock(node, PreparedBlockKind::Heading);
	case NodeKind::CodeBlock:
		return { PrepareCodeBlock(node) };
	case NodeKind::ThematicBreak:
		return { PrepareRuleBlock() };
	case NodeKind::List:
		return { PrepareListBlock(node, context) };
	case NodeKind::ListItem: {
		auto list = PreparedBlock();
		list.kind = PreparedBlockKind::List;
		list.actualDepth = context.listDepth;
		list.visualDepth = CappedListDepth(list.actualDepth);
		list.depthClamped = (list.actualDepth > list.visualDepth);
		return { PrepareListItemBlock(node, context, list, 0) };
	} break;
	case NodeKind::Blockquote:
		return { PrepareQuoteBlock(node, context) };
	case NodeKind::Table:
		return PrepareTableBlocks(node, context);
	default:
		return PrepareFallbackBlocks(node, context);
	}
	return {};
}

[[nodiscard]] PreparedRenderDocument PrepareRenderData(
		const PreparedDocument &document) {
	auto result = PreparedRenderDocument();
	result.blocks = PrepareBlocks(document.document, {});
	return result;
}

[[nodiscard]] int BlockSkip(const PreparedBlock &block) {
	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
		return st::ivMarkdownParagraphSkip;
	case PreparedBlockKind::Heading:
		return st::ivMarkdownHeadingSkip;
	case PreparedBlockKind::CodeBlock:
		return st::ivMarkdownCodeSkip;
	case PreparedBlockKind::Rule:
		return st::ivMarkdownRuleSkip;
	case PreparedBlockKind::List:
	case PreparedBlockKind::ListItem:
		return st::ivMarkdownParagraphSkip;
	case PreparedBlockKind::Quote:
		return st::ivMarkdownQuoteSkip;
	case PreparedBlockKind::Table:
		return st::ivMarkdownTableSkip;
	}
	return 0;
}

[[nodiscard]] int BlockSkip(
		const PreparedBlock &previous,
		const PreparedBlock &block,
		LayoutContext context) {
	if (context.tightList
		&& IsFlowKind(previous.kind)
		&& IsFlowKind(block.kind)) {
		return 0;
	}
	return BlockSkip(block);
}

[[nodiscard]] const style::TextStyle &TextStyleFor(
		const PreparedBlock &block) {
	if (block.kind == PreparedBlockKind::CodeBlock) {
		return st::ivMarkdownCodeStyle;
	} else if (block.kind != PreparedBlockKind::Heading) {
		return st::ivMarkdownParagraphStyle;
	}
	switch (std::clamp(block.headingLevel, 1, 6)) {
	case 1: return st::ivMarkdownHeading1Style;
	case 2: return st::ivMarkdownHeading2Style;
	case 3: return st::ivMarkdownHeading3Style;
	case 4: return st::ivMarkdownHeading4Style;
	case 5: return st::ivMarkdownHeading5Style;
	case 6: return st::ivMarkdownHeading6Style;
	}
	return st::ivMarkdownHeading6Style;
}

[[nodiscard]] QString CodeBlockDisplayText(const QString &text) {
	auto result = QString();
	result.reserve(text.size());

	auto column = 0;
	for (const auto ch : text) {
		if (ch == QChar::Tabulation) {
			const auto count = kCodeTabColumns - (column % kCodeTabColumns);
			for (auto i = 0; i != count; ++i) {
				result.append(QChar::Space);
			}
			column += count;
			continue;
		}
		result.append(ch);
		if (Ui::Text::IsNewline(ch)) {
			column = 0;
		} else {
			++column;
		}
	}
	if (result.isEmpty() || Ui::Text::IsTrimmed(result.back())) {
		result.append(QChar(kCodeTrailingGuard));
	}
	return result;
}

[[nodiscard]] TextWithEntities CodeBlockText(const QString &text) {
	auto result = TextWithEntities();
	result.text = CodeBlockDisplayText(text);
	if (!result.text.isEmpty()) {
		result.entities.push_back(EntityInText(
			EntityType::Code,
			0,
			result.text.size()));
	}
	return result;
}

void BindLinks(
		Ui::Text::String *leaf,
		const std::vector<PreparedLink> &links) {
	for (const auto &link : links) {
		leaf->setLink(
			link.index,
			std::make_shared<HiddenUrlClickHandler>(link.target));
	}
}

[[nodiscard]] style::align CellAlign(TableAlignment alignment) {
	switch (alignment) {
	case TableAlignment::Center:
		return style::al_center;
	case TableAlignment::Right:
		return style::al_right;
	case TableAlignment::None:
	case TableAlignment::Left:
		return style::al_left;
	}
	return style::al_left;
}

[[nodiscard]] const style::TextStyle &TableCellTextStyle(bool header) {
	if (header) {
		return st::ivMarkdownTableHeaderStyle;
	}
	return st::ivMarkdownParagraphStyle;
}

struct TableCellLayoutData {
	LaidOutTableCell cell;
	int preferredWidth = 0;
	int preferredHeight = 0;
};

struct TableRowLayoutData {
	std::vector<TableCellLayoutData> cells;
	bool header = false;
};

[[nodiscard]] TableCellLayoutData InitializeTableCellLayout(
		const PreparedTableCell &prepared,
		bool header) {
	auto result = TableCellLayoutData();
	const auto &style = TableCellTextStyle(header);
	result.cell.align = CellAlign(prepared.alignment);
	result.cell.leaf.setMarkedText(
		style,
		prepared.text,
		kIvMarkedTextOptions);
	BindLinks(&result.cell.leaf, prepared.links);
	result.preferredWidth = result.cell.leaf.maxWidth();
	result.preferredHeight = std::max(
		result.cell.leaf.countHeight(std::max(result.preferredWidth, 1), true),
		TextLineHeight(style));
	return result;
}

[[nodiscard]] std::vector<int> ComputeTableColumnWidths(
		const std::vector<TableRowLayoutData> &rows,
		int columnCount,
		int width,
		bool *overflowed) {
	const auto &padding = st::ivMarkdownTableCellPadding;
	const auto border = st::ivMarkdownTableBorder;
	const auto minimum = st::ivMarkdownTableMinColumnWidth;
	auto result = std::vector<int>(std::max(columnCount, 0), minimum);
	auto preferred = std::vector<int>(std::max(columnCount, 0), minimum);
	for (const auto &row : rows) {
		for (auto column = 0; column != columnCount; ++column) {
			preferred[column] = std::max(
				preferred[column],
				row.cells[column].preferredWidth
					+ padding.left()
					+ padding.right());
		}
	}

	const auto availableWidth = std::max(width, 1);
	const auto minimumGridWidth = border
		+ columnCount * (minimum + border);
	*overflowed = (minimumGridWidth > availableWidth);
	if (*overflowed || !columnCount) {
		return result;
	}

	auto extra = availableWidth - minimumGridWidth;
	auto remaining = 0;
	auto deficits = std::vector<int>(columnCount, 0);
	for (auto column = 0; column != columnCount; ++column) {
		deficits[column] = std::max(preferred[column] - minimum, 0);
		remaining += deficits[column];
	}

	while (extra > 0 && remaining > 0) {
		auto active = 0;
		for (const auto deficit : deficits) {
			if (deficit > 0) {
				++active;
			}
		}
		if (!active) {
			break;
		}
		const auto step = std::max(extra / active, 1);
		for (auto column = 0; column != columnCount && extra > 0; ++column) {
			if (!deficits[column]) {
				continue;
			}
			const auto delta = std::min({ deficits[column], step, extra });
			result[column] += delta;
			deficits[column] -= delta;
			remaining -= delta;
			extra -= delta;
		}
	}

	if (extra > 0) {
		const auto base = extra / columnCount;
		const auto tail = extra % columnCount;
		for (auto column = 0; column != columnCount; ++column) {
			result[column] += base + ((column < tail) ? 1 : 0);
		}
	}
	return result;
}

[[nodiscard]] int BlockBottom(const LaidOutBlock &block) {
	return block.outer.y() + block.outer.height();
}

[[nodiscard]] LaidOutBlock LayoutFlowBlock(
		const PreparedBlock &prepared,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = prepared.kind;
	block.headingLevel = prepared.headingLevel;
	block.textWidth = std::max(width, 1);
	block.leaf.setMarkedText(
		TextStyleFor(prepared),
		prepared.text,
		kIvMarkedTextOptions);
	BindLinks(&block.leaf, prepared.links);

	const auto &style = TextStyleFor(prepared);
	const auto height = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(style));
	block.textRect = QRect(left, top, block.textWidth, height);
	block.outer = QRect(left, top, block.textWidth, height);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutCodeBlock(
		const PreparedBlock &prepared,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::CodeBlock;

	const auto &padding = st::ivMarkdownCodePadding;
	block.textWidth = std::max(width - padding.left() - padding.right(), 1);

	auto y = top + padding.top();
	if (!prepared.codeLanguage.isEmpty()) {
		block.language.setMarkedText(
			st::ivMarkdownCodeLanguageStyle,
			TextWithEntities::Simple(prepared.codeLanguage),
			kIvMarkedTextOptions);
		const auto languageHeight = std::max(
			block.language.countHeight(block.textWidth, true),
			TextLineHeight(st::ivMarkdownCodeLanguageStyle));
		block.languageRect = QRect(
			left + padding.left(),
			y,
			block.textWidth,
			languageHeight);
		y += languageHeight + st::ivMarkdownCodeLanguageSkip;
	}

	block.leaf.setMarkedText(
		st::ivMarkdownCodeStyle,
		CodeBlockText(prepared.text.text),
		kIvMarkedTextOptions);
	const auto codeHeight = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(st::ivMarkdownCodeStyle));
	block.textRect = QRect(
		left + padding.left(),
		y,
		block.textWidth,
		codeHeight);
	block.outer = QRect(
		left,
		top,
		std::max(width, 1),
		y + codeHeight + padding.bottom() - top);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutRuleBlock(
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Rule;
	block.outer = QRect(
		left,
		top,
		std::max(width, 1),
		st::ivMarkdownRuleHeight);
	block.textRect = block.outer;
	return block;
}

[[nodiscard]] LaidOutBlock LayoutTableBlock(
		const PreparedBlock &prepared,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Table;

	const auto columnCount = prepared.tableColumnCount;
	if (!columnCount || prepared.tableRows.empty()) {
		block.outer = QRect(left, top, std::max(width, 1), 0);
		return block;
	}

	auto rows = std::vector<TableRowLayoutData>();
	rows.reserve(prepared.tableRows.size());
	for (const auto &preparedRow : prepared.tableRows) {
		auto row = TableRowLayoutData();
		row.header = preparedRow.header;
		row.cells.reserve(columnCount);
		for (const auto &preparedCell : preparedRow.cells) {
			row.cells.push_back(InitializeTableCellLayout(
				preparedCell,
				preparedRow.header));
		}
		rows.push_back(std::move(row));
	}

	block.tableColumnWidths = ComputeTableColumnWidths(
		rows,
		columnCount,
		width,
		&block.overflowed);

	const auto &padding = st::ivMarkdownTableCellPadding;
	const auto border = st::ivMarkdownTableBorder;
	auto tableWidth = border;
	for (const auto columnWidth : block.tableColumnWidths) {
		tableWidth += columnWidth + border;
	}

	auto y = top + border;
	block.tableRows.reserve(rows.size());
	for (auto &rowData : rows) {
		auto rowHeight = 0;
		auto textHeights = std::vector<int>(columnCount, 0);
		for (auto column = 0; column != columnCount; ++column) {
			const auto &style = TableCellTextStyle(rowData.header);
			const auto contentWidth = std::max(
				block.tableColumnWidths[column]
					- padding.left()
					- padding.right(),
				1);
			rowData.cells[column].cell.textWidth = contentWidth;
			textHeights[column] = (contentWidth
				>= rowData.cells[column].preferredWidth)
				? rowData.cells[column].preferredHeight
				: std::max(
					rowData.cells[column].cell.leaf.countHeight(
						contentWidth,
						true),
					TextLineHeight(style));
			rowHeight = std::max(
				rowHeight,
				textHeights[column] + padding.top() + padding.bottom());
		}

		auto row = LaidOutTableRow();
		row.header = rowData.header;
		row.outer = QRect(
			left + border,
			y,
			std::max(tableWidth - (2 * border), 0),
			rowHeight);

		auto x = left + border;
		row.cells.reserve(columnCount);
		for (auto column = 0; column != columnCount; ++column) {
			auto cell = std::move(rowData.cells[column].cell);
			const auto columnWidth = block.tableColumnWidths[column];
			cell.outer = QRect(x, y, columnWidth, rowHeight);
			cell.textRect = QRect(
				x + padding.left(),
				y + padding.top(),
				cell.textWidth,
				textHeights[column]);
			row.cells.push_back(std::move(cell));
			x += columnWidth + border;
		}
		block.tableRows.push_back(std::move(row));
		y += rowHeight + border;
	}

	const auto tableHeight = std::max(y - top, border);
	block.tableRect = QRect(left, top, tableWidth, tableHeight);
	block.visibleTableRect = QRect(
		left,
		top,
		std::min(tableWidth, std::max(width, 1)),
		tableHeight);
	block.contentRect = block.visibleTableRect;
	block.outer = block.visibleTableRect;
	return block;
}

[[nodiscard]] LaidOutBlock LayoutBlock(
	const PreparedBlock &prepared,
	int left,
	int top,
	int width,
	LayoutContext context);

[[nodiscard]] int LayoutBlocks(
		const std::vector<PreparedBlock> &prepared,
		std::vector<LaidOutBlock> *blocks,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto y = top;
	auto previous = static_cast<const PreparedBlock*>(nullptr);
	for (const auto &block : prepared) {
		if (previous) {
			y += BlockSkip(*previous, block, context);
		}
		auto laidOut = LayoutBlock(
			block,
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

[[nodiscard]] LaidOutBlock LayoutListItemBlock(
		const PreparedBlock &prepared,
		int left,
		int top,
		int width,
		LayoutContext context,
		bool tight) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::ListItem;
	block.listKind = prepared.listKind;
	block.listDelimiter = prepared.listDelimiter;
	block.taskState = prepared.taskState;
	block.orderedNumber = prepared.orderedNumber;

	const auto task = (prepared.taskState != TaskState::None);
	const auto markerText = task ? QString() : ListMarkerText(prepared);
	auto markerTextWidth = 0;
	auto markerTextHeight = 0;
	if (task) {
		markerTextWidth = st::ivMarkdownTaskMarkerSize;
		markerTextHeight = st::ivMarkdownTaskMarkerSize;
	} else {
		block.marker.setMarkedText(
			st::ivMarkdownParagraphStyle,
			TextWithEntities::Simple(markerText),
			kIvMarkedTextOptions);
		markerTextWidth = std::max(block.marker.maxWidth(), 1);
		markerTextHeight = std::max(
			block.marker.countHeight(markerTextWidth, true),
			TextLineHeight(st::ivMarkdownParagraphStyle));
	}

	block.markerWidth = std::max(
		st::ivMarkdownListMarkerWidth,
		markerTextWidth);
	const auto bodyLeft = left
		+ block.markerWidth
		+ st::ivMarkdownListMarkerSkip;
	const auto bodyWidth = std::max(
		width - block.markerWidth - st::ivMarkdownListMarkerSkip,
		1);

	auto childContext = context;
	childContext.tightList = tight;
	const auto childBottom = LayoutBlocks(
		prepared.children,
		&block.children,
		bodyLeft,
		top,
		bodyWidth,
		childContext);
	const auto contentHeight = childBottom - top;
	const auto rowHeight = std::max({
		contentHeight,
		markerTextHeight,
		TextLineHeight(st::ivMarkdownParagraphStyle),
	});

	const auto markerTop = top + std::max(
		(TextLineHeight(st::ivMarkdownParagraphStyle) - markerTextHeight) / 2,
		0);
	if (task) {
		block.markerRect = QRect(
			left,
			markerTop,
			st::ivMarkdownTaskMarkerSize,
			st::ivMarkdownTaskMarkerSize);
	} else {
		const auto markerLeft = (prepared.listKind == ListKind::Ordered)
			? left + block.markerWidth - markerTextWidth
			: left;
		block.markerRect = QRect(
			markerLeft,
			top,
			markerTextWidth,
			markerTextHeight);
	}

	block.contentRect = QRect(bodyLeft, top, bodyWidth, rowHeight);
	block.outer = QRect(left, top, std::max(width, 1), rowHeight);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutListBlock(
		const PreparedBlock &prepared,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::List;
	block.listKind = prepared.listKind;
	block.listDelimiter = prepared.listDelimiter;

	const auto depthDelta = std::max(prepared.visualDepth - context.listDepth, 0);
	const auto listLeft = left + depthDelta * st::ivMarkdownListIndent;
	const auto listWidth = std::max(
		width - depthDelta * st::ivMarkdownListIndent,
		1);

	auto childContext = context;
	childContext.listDepth = prepared.visualDepth;
	childContext.tightList = false;

	auto y = top;
	auto first = true;
	for (const auto &child : prepared.children) {
		if (!first) {
			y += prepared.tight ? 0 : BlockSkip(child);
		}
		first = false;

		auto laidOut = (child.kind == PreparedBlockKind::ListItem)
			? LayoutListItemBlock(
				child,
				listLeft,
				y,
				listWidth,
				childContext,
				prepared.tight)
			: LayoutBlock(child, listLeft, y, listWidth, childContext);
		y = BlockBottom(laidOut);
		block.children.push_back(std::move(laidOut));
	}

	block.outer = QRect(
		listLeft,
		top,
		listWidth,
		std::max(y - top, 0));
	block.contentRect = block.outer;
	return block;
}

[[nodiscard]] LaidOutBlock LayoutQuoteBlock(
		const PreparedBlock &prepared,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Quote;

	const auto depthDelta = std::max(
		prepared.visualDepth - context.quoteDepth,
		0);
	const auto quoteLeft = left + depthDelta * st::ivMarkdownQuoteIndent;
	const auto quoteWidth = std::max(
		width - depthDelta * st::ivMarkdownQuoteIndent,
		1);
	const auto &padding = st::ivMarkdownQuotePadding;
	const auto contentLeft = quoteLeft
		+ st::ivMarkdownQuoteBorder
		+ padding.left();
	const auto contentTop = top + padding.top();
	const auto contentWidth = std::max(
		quoteWidth
			- st::ivMarkdownQuoteBorder
			- padding.left()
			- padding.right(),
		1);

	auto childContext = context;
	childContext.quoteDepth = prepared.visualDepth;
	childContext.tightList = false;
	const auto childBottom = LayoutBlocks(
		prepared.children,
		&block.children,
		contentLeft,
		contentTop,
		contentWidth,
		childContext);
	const auto contentHeight = std::max(
		childBottom - contentTop,
		prepared.children.empty()
			? TextLineHeight(st::ivMarkdownParagraphStyle)
			: 0);
	const auto quoteHeight = padding.top() + contentHeight + padding.bottom();

	block.outer = QRect(quoteLeft, top, quoteWidth, quoteHeight);
	block.borderRect = QRect(
		quoteLeft,
		top,
		st::ivMarkdownQuoteBorder,
		quoteHeight);
	block.contentRect = QRect(
		contentLeft,
		contentTop,
		contentWidth,
		contentHeight);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutBlock(
		const PreparedBlock &prepared,
		int left,
		int top,
		int width,
		LayoutContext context) {
	switch (prepared.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		return LayoutFlowBlock(prepared, left, top, width);
	case PreparedBlockKind::CodeBlock:
		return LayoutCodeBlock(prepared, left, top, width);
	case PreparedBlockKind::Rule:
		return LayoutRuleBlock(left, top, width);
	case PreparedBlockKind::List:
		return LayoutListBlock(prepared, left, top, width, context);
	case PreparedBlockKind::ListItem:
		return LayoutListItemBlock(prepared, left, top, width, context, false);
	case PreparedBlockKind::Quote:
		return LayoutQuoteBlock(prepared, left, top, width, context);
	case PreparedBlockKind::Table:
		return LayoutTableBlock(prepared, left, top, width);
	}
	return LayoutFlowBlock(prepared, left, top, width);
}

class DocumentLayout final {
public:
	void relayout(const PreparedRenderDocument &document, int width);

	[[nodiscard]] int height() const;
	[[nodiscard]] const std::vector<LaidOutBlock> &blocks() const;

private:
	int _width = -1;
	int _height = 0;
	std::vector<LaidOutBlock> _blocks;

};

class MarkdownDocumentWidget final
	: public Ui::RpWidget
	, public ClickHandlerHost {
public:
	MarkdownDocumentWidget(
		QWidget *parent,
		const PreparedDocument &document);

	int resizeGetHeight(int newWidth) override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void clickHandlerActiveChanged(const ClickHandlerPtr &, bool) override;
	void clickHandlerPressedChanged(const ClickHandlerPtr &, bool) override;

private:
	[[nodiscard]] ClickHandlerPtr linkAt(QPoint point) const;

	void updateHover(QPoint point);
	void applyCursor(style::cursor cursor);

	const PreparedRenderDocument _prepared;
	DocumentLayout _layout;
	style::cursor _cursor = style::cur_default;

};

void PaintTextLeaf(
		Painter &p,
		const Ui::Text::String &leaf,
		QRect rect,
		int width,
		QRect clip,
		style::align align = style::al_left) {
	leaf.draw(p, {
		.position = rect.topLeft(),
		.availableWidth = width,
		.geometry = TextGeometry(width),
		.align = align,
		.clip = clip,
		.palette = &st::ivMarkdownTextPalette,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = crl::now(),
	});
}

void PaintTaskMarker(Painter &p, const LaidOutBlock &block) {
	const auto rect = block.markerRect;
	if (rect.isEmpty()) {
		return;
	}
	const auto border = st::ivMarkdownTaskMarkerBorder;
	if (block.taskState == TaskState::Checked) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::ivMarkdownTaskMarkerFg);
		p.drawRect(rect);

		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(
			st::ivMarkdownTaskMarkerCheckFg,
			border,
			Qt::SolidLine,
			Qt::RoundCap,
			Qt::RoundJoin));
		const auto size = rect.width();
		const auto first = QPoint(
			rect.left() + size / 4,
			rect.top() + size / 2);
		const auto middle = QPoint(
			rect.left() + size / 2,
			rect.top() + (2 * size) / 3);
		const auto last = QPoint(
			rect.right() - size / 5,
			rect.top() + size / 3);
		p.drawLine(first, middle);
		p.drawLine(middle, last);
	} else {
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(st::ivMarkdownTaskMarkerFg, border));
		p.drawRect(rect.adjusted(0, 0, -border, -border));
	}
}

void PaintBlocks(
	Painter &p,
	const std::vector<LaidOutBlock> &blocks,
	QRect clip);

void PaintTableBlock(
		Painter &p,
		const LaidOutBlock &block,
		QRect clip) {
	const auto tableClip = clip.intersected(block.visibleTableRect);
	if (tableClip.isEmpty()) {
		return;
	}

	p.save();
	p.setClipRect(tableClip);

	for (const auto &row : block.tableRows) {
		if (!row.header || !row.outer.intersects(block.visibleTableRect)) {
			continue;
		}
		for (const auto &cell : row.cells) {
			if (!cell.outer.intersects(block.visibleTableRect)) {
				continue;
			}
			p.fillRect(cell.outer, st::ivMarkdownTableHeaderBg);
		}
	}

	const auto border = st::ivMarkdownTableBorder;
	if (border > 0 && !block.tableRect.isEmpty()) {
		const auto left = block.tableRect.x();
		const auto top = block.tableRect.y();
		const auto width = block.tableRect.width();
		const auto height = block.tableRect.height();
		const auto right = left + width - border;
		const auto bottom = top + height - border;

		p.fillRect(QRect(left, top, width, border), st::ivMarkdownTableBorderFg);
		p.fillRect(
			QRect(left, bottom, width, border),
			st::ivMarkdownTableBorderFg);
		p.fillRect(QRect(left, top, border, height), st::ivMarkdownTableBorderFg);
		p.fillRect(
			QRect(right, top, border, height),
			st::ivMarkdownTableBorderFg);

		auto separatorLeft = left + border;
		for (auto i = 0, count = int(block.tableColumnWidths.size()); i != count; ++i) {
			separatorLeft += block.tableColumnWidths[i];
			if (i + 1 != count) {
				p.fillRect(
					QRect(separatorLeft, top, border, height),
					st::ivMarkdownTableBorderFg);
				separatorLeft += border;
			}
		}

		for (auto i = 0, count = int(block.tableRows.size()); i != count; ++i) {
			if (i + 1 == count) {
				break;
			}
			const auto separatorTop = block.tableRows[i].outer.y()
				+ block.tableRows[i].outer.height();
			p.fillRect(
				QRect(left, separatorTop, width, border),
				st::ivMarkdownTableBorderFg);
		}
	}

	p.setPen(st::windowFg);
	for (const auto &row : block.tableRows) {
		if (!row.outer.intersects(block.visibleTableRect)) {
			continue;
		}
		for (const auto &cell : row.cells) {
			if (!cell.textRect.intersects(block.visibleTableRect)) {
				continue;
			}
			PaintTextLeaf(
				p,
				cell.leaf,
				cell.textRect,
				cell.textWidth,
				tableClip,
				cell.align);
		}
	}

	if (block.overflowed) {
		const auto indicatorWidth = std::min(
			std::max(st::ivMarkdownTableOverflowWidth, 1),
			block.visibleTableRect.width());
		p.fillRect(
			QRect(
				block.visibleTableRect.x()
					+ block.visibleTableRect.width()
					- indicatorWidth,
				block.visibleTableRect.y(),
				indicatorWidth,
				block.visibleTableRect.height()),
			st::ivMarkdownTableOverflowFg);
	}

	p.restore();
}

void PaintBlock(Painter &p, const LaidOutBlock &block, QRect clip) {
	if (!block.outer.intersects(clip)) {
		return;
	}

	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		p.setPen(st::windowFg);
		PaintTextLeaf(p, block.leaf, block.textRect, block.textWidth, clip);
		break;
	case PreparedBlockKind::CodeBlock: {
		const auto radius = st::ivMarkdownCodeRadius;
		p.setPen(Qt::NoPen);
		p.setBrush(st::ivMarkdownCodeBg);
		if (radius > 0) {
			auto hq = PainterHighQualityEnabler(p);
			p.drawRoundedRect(block.outer, radius, radius);
		} else {
			p.fillRect(block.outer, st::ivMarkdownCodeBg);
		}
		if (!block.languageRect.isEmpty()) {
			p.setPen(st::ivMarkdownCodeLanguageFg);
			PaintTextLeaf(
				p,
				block.language,
				block.languageRect,
				block.textWidth,
				clip);
		}
		p.setPen(st::windowFg);
		PaintTextLeaf(p, block.leaf, block.textRect, block.textWidth, clip);
	} break;
	case PreparedBlockKind::Rule:
		p.fillRect(block.outer, st::ivMarkdownRuleFg);
		break;
	case PreparedBlockKind::List:
		PaintBlocks(p, block.children, clip);
		break;
	case PreparedBlockKind::ListItem:
		if (block.taskState != TaskState::None) {
			PaintTaskMarker(p, block);
		} else if (!block.markerRect.isEmpty()) {
			p.setPen(st::windowFg);
			PaintTextLeaf(
				p,
				block.marker,
				block.markerRect,
				block.markerWidth,
				clip);
		}
		PaintBlocks(p, block.children, clip);
		break;
	case PreparedBlockKind::Quote:
		p.fillRect(block.borderRect, st::ivMarkdownQuoteBorderFg);
		PaintBlocks(p, block.children, clip);
		break;
	case PreparedBlockKind::Table:
		PaintTableBlock(p, block, clip);
		break;
	}
}

void PaintBlocks(
		Painter &p,
		const std::vector<LaidOutBlock> &blocks,
		QRect clip) {
	for (const auto &block : blocks) {
		if (block.outer.bottom() < clip.top()) {
			continue;
		} else if (block.outer.top() > clip.bottom()) {
			break;
		}
		PaintBlock(p, block, clip);
	}
}

[[nodiscard]] Ui::Text::StateResult TextStateAtLeaf(
		const Ui::Text::String &leaf,
		QRect rect,
		int width,
		QPoint point,
		style::align align = style::al_left) {
	if (!rect.contains(point)) {
		return {};
	}
	auto request = Ui::Text::StateRequest();
	request.align = align;
	request.flags |= Ui::Text::StateRequest::Flag::BreakEverywhere;
	return leaf.getState(
		point - rect.topLeft(),
		TextGeometry(width),
		request);
}

[[nodiscard]] ClickHandlerPtr LinkAtTextLeaf(
		const Ui::Text::String &leaf,
		QRect rect,
		int width,
		QPoint point,
		style::align align = style::al_left) {
	const auto state = TextStateAtLeaf(leaf, rect, width, point, align);
	return state.link;
}

[[nodiscard]] ClickHandlerPtr LinkAtTextBlock(
		const LaidOutBlock &block,
		QPoint point) {
	return LinkAtTextLeaf(
		block.leaf,
		block.textRect,
		block.textWidth,
		point);
}

[[nodiscard]] ClickHandlerPtr LinkAtTableCell(
		const LaidOutTableCell &cell,
		QPoint point) {
	return LinkAtTextLeaf(
		cell.leaf,
		cell.textRect,
		cell.textWidth,
		point,
		cell.align);
}

[[nodiscard]] ClickHandlerPtr LinkAtTableBlock(
		const LaidOutBlock &block,
		QPoint point) {
	const auto visibleTableRect = block.visibleTableRect;
	for (const auto &row : block.tableRows) {
		if (!row.outer.intersects(visibleTableRect)) {
			continue;
		} else if (row.outer.bottom() < point.y()) {
			continue;
		} else if (row.outer.top() > point.y()) {
			break;
		}
		for (const auto &cell : row.cells) {
			if (!cell.outer.intersects(visibleTableRect)) {
				continue;
			} else if (cell.outer.right() < point.x()) {
				continue;
			} else if (cell.outer.left() > point.x()) {
				break;
			}
			if (const auto result = LinkAtTableCell(cell, point)) {
				return result;
			}
		}
	}
	return nullptr;
}

[[nodiscard]] ClickHandlerPtr LinkAtBlocks(
	const std::vector<LaidOutBlock> &blocks,
	QPoint point);

[[nodiscard]] ClickHandlerPtr LinkAtBlock(
		const LaidOutBlock &block,
		QPoint point) {
	if (!block.outer.contains(point)) {
		return nullptr;
	}
	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		return LinkAtTextBlock(block, point);
	case PreparedBlockKind::List:
	case PreparedBlockKind::ListItem:
	case PreparedBlockKind::Quote:
		return LinkAtBlocks(block.children, point);
	case PreparedBlockKind::Table:
		return LinkAtTableBlock(block, point);
	case PreparedBlockKind::CodeBlock:
	case PreparedBlockKind::Rule:
		return nullptr;
	}
	return nullptr;
}

[[nodiscard]] ClickHandlerPtr LinkAtBlocks(
		const std::vector<LaidOutBlock> &blocks,
		QPoint point) {
	for (const auto &block : blocks) {
		if (block.outer.bottom() < point.y()) {
			continue;
		} else if (block.outer.top() > point.y()) {
			break;
		}
		if (const auto result = LinkAtBlock(block, point)) {
			return result;
		}
	}
	return nullptr;
}

void DocumentLayout::relayout(
		const PreparedRenderDocument &document,
		int width) {
	width = std::max(width, 1);
	if (_width == width) {
		return;
	}
	_width = width;
	_blocks.clear();

	const auto &page = st::ivMarkdownPagePadding;
	const auto innerWidth = std::max(width - page.left() - page.right(), 1);
	const auto y = LayoutBlocks(
		document.blocks,
		&_blocks,
		page.left(),
		page.top(),
		innerWidth,
		{});
	_height = y + page.bottom();
}

int DocumentLayout::height() const {
	return _height;
}

const std::vector<LaidOutBlock> &DocumentLayout::blocks() const {
	return _blocks;
}

MarkdownDocumentWidget::MarkdownDocumentWidget(
	QWidget *parent,
	const PreparedDocument &document)
: Ui::RpWidget(parent)
, _prepared(PrepareRenderData(document)) {
	setMouseTracking(true);
}

int MarkdownDocumentWidget::resizeGetHeight(int newWidth) {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	_layout.relayout(_prepared, newWidth);
	return std::max(_layout.height(), 1);
}

void MarkdownDocumentWidget::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	p.setTextPalette(st::ivMarkdownTextPalette);

	PaintBlocks(p, _layout.blocks(), e->rect());
}

void MarkdownDocumentWidget::mouseMoveEvent(QMouseEvent *e) {
	updateHover(e->pos());
}

void MarkdownDocumentWidget::mousePressEvent(QMouseEvent *e) {
	updateHover(e->pos());
	if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
		ClickHandler::pressed();
	}
}

void MarkdownDocumentWidget::mouseReleaseEvent(QMouseEvent *e) {
	const auto activated = ClickHandler::unpressed();
	if (activated
		&& (e->button() == Qt::LeftButton
			|| e->button() == Qt::MiddleButton)) {
		ActivateClickHandler(window(), activated, e->button());
	}
	if (rect().contains(e->pos())) {
		updateHover(e->pos());
	} else {
		ClickHandler::clearActive(this);
		applyCursor(style::cur_default);
	}
}

void MarkdownDocumentWidget::leaveEventHook(QEvent *e) {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	Ui::RpWidget::leaveEventHook(e);
}

void MarkdownDocumentWidget::clickHandlerActiveChanged(
		const ClickHandlerPtr &,
		bool) {
	update();
}

void MarkdownDocumentWidget::clickHandlerPressedChanged(
		const ClickHandlerPtr &,
		bool) {
	update();
}

ClickHandlerPtr MarkdownDocumentWidget::linkAt(QPoint point) const {
	return LinkAtBlocks(_layout.blocks(), point);
}

void MarkdownDocumentWidget::updateHover(QPoint point) {
	ClickHandler::setActive(linkAt(point), this);
	applyCursor(ClickHandler::getActive()
		? style::cur_pointer
		: style::cur_default);
}

void MarkdownDocumentWidget::applyCursor(style::cursor cursor) {
	if (_cursor != cursor) {
		_cursor = cursor;
		setCursor(_cursor);
	}
}

} // namespace

std::unique_ptr<Ui::RpWidget> CreateMarkdownPreviewWidget(
	const PreparedDocument &document,
	const OpenOptions &options) {
	(void)options;

	auto root = std::make_unique<Ui::RpWidget>();
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
		root.get(),
		st::boxScroll);
	const auto body = scroll->setOwnedWidget(
		object_ptr<MarkdownDocumentWidget>(scroll, document));

	root->sizeValue() | rpl::on_next([=](QSize size) {
		scroll->setGeometry(QRect(QPoint(), size));
		if (body) {
			body->resizeToWidth(scroll->width());
		}
	}, root->lifetime());

	scroll->show();
	if (body) {
		body->show();
	}
	return root;
}

} // namespace Iv::Markdown
