#include "iv/markdown/iv_markdown_view.h"
#include "iv/markdown/iv_markdown_prepare.h"

#include <QtCore/QDebug>

#include "base/weak_ptr.h"
#include "core/credits_amount.h"
#include "core/click_handler_types.h"
#include "lang/lang_keys.h"
#include "ui/click_handler.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core.h"
#include "ui/text/text.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/labels.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPen>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "styles/style_boxes.h"
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

struct SnapshotTextPalette {
	explicit SnapshotTextPalette(const MarkdownTextPaletteSnapshot &snapshot)
	: link(snapshot.link)
	, mono(snapshot.mono)
	, spoiler(snapshot.spoiler)
	, selectBackground(snapshot.selectBackground)
	, selectText(snapshot.selectText)
	, selectLink(snapshot.selectLink)
	, selectMono(snapshot.selectMono)
	, selectSpoiler(snapshot.selectSpoiler)
	, selectOverlay(snapshot.selectOverlay) {
		palette.linkFg = link.color();
		palette.monoFg = mono.color();
		palette.spoilerFg = spoiler.color();
		palette.selectBg = selectBackground.color();
		palette.selectFg = selectText.color();
		palette.selectLinkFg = selectLink.color();
		palette.selectMonoFg = selectMono.color();
		palette.selectSpoilerFg = selectSpoiler.color();
		palette.selectOverlay = selectOverlay.color();
		palette.linkAlwaysActive = snapshot.linkAlwaysActive;
	}

	style::owned_color link;
	style::owned_color mono;
	style::owned_color spoiler;
	style::owned_color selectBackground;
	style::owned_color selectText;
	style::owned_color selectLink;
	style::owned_color selectMono;
	style::owned_color selectSpoiler;
	style::owned_color selectOverlay;
	style::TextPalette palette;
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
	Ui::Text::String fallbackLeaf;
	std::vector<LaidOutBlock> children;
	std::vector<LaidOutTableRow> tableRows;
	std::vector<int> tableColumnWidths;
	QRect outer;
	QRect textRect;
	QRect markerRect;
	QRect contentRect;
	QRect borderRect;
	QRect formulaRect;
	QRect languageRect;
	QRect tableRect;
	QRect visibleFormulaRect;
	QRect visibleTableRect;
	int textWidth = 0;
	int markerWidth = 0;
	int headingLevel = 0;
	ListKind listKind = ListKind::Bullet;
	ListDelimiter listDelimiter = ListDelimiter::None;
	TaskState taskState = TaskState::None;
	int formulaIndex = -1;
	int orderedNumber = 0;
	style::align formulaAlign = style::al_left;
	bool overflowed = false;
};

struct LayoutContext {
	int listDepth = 0;
	int quoteDepth = 0;
	bool tightList = false;
};

constexpr auto kCodeTabColumns = 4;
constexpr auto kCodeTrailingGuard = 0x2060;

[[nodiscard]] bool IsFlowKind(PreparedBlockKind kind) {
	return (kind == PreparedBlockKind::Paragraph)
		|| (kind == PreparedBlockKind::Heading);
}

[[nodiscard]] QString ListMarkerText(const PreparedBlock &block) {
	if (block.listKind == ListKind::Ordered) {
		const auto delimiter = (block.listDelimiter == ListDelimiter::Parenthesis)
			? u")"_q
			: u"."_q;
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

[[nodiscard]] int BlockSkip(
		const PreparedBlock &block,
		const MarkdownStyleSnapshot &style) {
	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
		return style.paragraphSkip;
	case PreparedBlockKind::Heading:
		return style.headingSkip;
	case PreparedBlockKind::CodeBlock:
		return style.codeSkip;
	case PreparedBlockKind::Rule:
		return style.ruleSkip;
	case PreparedBlockKind::List:
	case PreparedBlockKind::ListItem:
		return style.paragraphSkip;
	case PreparedBlockKind::Quote:
		return style.quoteSkip;
	case PreparedBlockKind::DisplayMath:
		return style.displayMathSkip;
	case PreparedBlockKind::Table:
		return style.tableSkip;
	}
	return 0;
}

[[nodiscard]] int BlockSkip(
		const PreparedBlock &previous,
		const PreparedBlock &block,
		LayoutContext context,
		const MarkdownStyleSnapshot &style) {
	if (context.tightList
		&& IsFlowKind(previous.kind)
		&& IsFlowKind(block.kind)) {
		return 0;
	}
	return BlockSkip(block, style);
}

[[nodiscard]] const style::TextStyle &TextStyleFor(
		const PreparedBlock &block,
		const MarkdownStyleSnapshot &style) {
	if (block.kind == PreparedBlockKind::CodeBlock) {
		return style.codeStyle;
	} else if (block.kind != PreparedBlockKind::Heading) {
		return style.paragraphStyle;
	}
	switch (std::clamp(block.headingLevel, 1, 6)) {
	case 1: return style.heading1Style;
	case 2: return style.heading2Style;
	case 3: return style.heading3Style;
	case 4: return style.heading4Style;
	case 5: return style.heading5Style;
	case 6: return style.heading6Style;
	}
	return style.heading6Style;
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

[[nodiscard]] const style::TextStyle &TableCellTextStyle(
		bool header,
		const MarkdownStyleSnapshot &style) {
	if (header) {
		return style.tableHeaderStyle;
	}
	return style.paragraphStyle;
}

void SetTextLeaf(
		Ui::Text::String *leaf,
		const style::TextStyle &textStyle,
		const TextWithEntities &text,
		const std::vector<PreparedInlineObject> &inlineObjects,
		const std::vector<PreparedFormulaSlot> &formulas);

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
		bool header,
		const std::vector<PreparedFormulaSlot> &formulas,
		const MarkdownStyleSnapshot &style) {
	auto result = TableCellLayoutData();
	const auto &textStyle = TableCellTextStyle(header, style);
	result.cell.align = CellAlign(prepared.alignment);
	SetTextLeaf(
		&result.cell.leaf,
		textStyle,
		prepared.text,
		prepared.inlineObjects,
		formulas);
	BindLinks(&result.cell.leaf, prepared.links);
	result.preferredWidth = result.cell.leaf.maxWidth();
	result.preferredHeight = std::max(
		result.cell.leaf.countHeight(std::max(result.preferredWidth, 1), true),
		TextLineHeight(textStyle));
	return result;
}

[[nodiscard]] std::vector<int> ComputeTableColumnWidths(
		const std::vector<TableRowLayoutData> &rows,
		int columnCount,
		int width,
		const MarkdownStyleSnapshot &style,
		bool *overflowed) {
	const auto &padding = style.tableCellPadding;
	const auto border = style.tableBorder;
	const auto minimum = style.tableMinColumnWidth;
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

[[nodiscard]] const RenderedFormula *RenderedFormulaFor(
		const std::vector<PreparedFormulaSlot> &formulas,
		int formulaIndex) {
	if (formulaIndex < 0 || formulaIndex >= int(formulas.size())) {
		return nullptr;
	} else if (!formulas[formulaIndex].present) {
		return nullptr;
	}
	return &formulas[formulaIndex].rendered;
}

[[nodiscard]] const PreparedFormulaSlot *PreparedFormulaFor(
		const std::vector<PreparedFormulaSlot> &formulas,
		int formulaIndex) {
	if (formulaIndex < 0 || formulaIndex >= int(formulas.size())) {
		return nullptr;
	} else if (!formulas[formulaIndex].present) {
		return nullptr;
	}
	return &formulas[formulaIndex];
}

[[nodiscard]] QString InlineFormulaFallbackText(
		const PreparedInlineObject &prepared,
		const PreparedFormulaSlot *formula) {
	if (!prepared.copySource.isEmpty()) {
		return prepared.copySource;
	} else if (formula && !formula->rendered.fallbackText.isEmpty()) {
		return formula->rendered.fallbackText;
	} else if (formula && !formula->trimmedTex.isEmpty()) {
		return formula->trimmedTex;
	}
	return u"[math]"_q;
}

[[nodiscard]] std::vector<Ui::Text::InlineObjectPlacement> InlineFormulaPlacements(
		const std::vector<PreparedInlineObject> &preparedInlineObjects,
		const std::vector<PreparedFormulaSlot> &formulas,
		const style::TextStyle &textStyle) {
	auto result = std::vector<Ui::Text::InlineObjectPlacement>();
	result.reserve(preparedInlineObjects.size());
	for (const auto &prepared : preparedInlineObjects) {
		const auto formula = PreparedFormulaFor(formulas, prepared.formulaIndex);
		auto placement = Ui::Text::InlineObjectPlacement();
		placement.position = prepared.position;
		placement.object.align = Ui::Text::InlineObjectVerticalAlign::CenterInText;
		placement.object.copySource = prepared.copySource;
		if (formula && formula->rendered.success) {
			placement.object.image = formula->rendered.image;
			placement.object.width = std::max(
				formula->rendered.logicalSize.width(),
				1);
			placement.object.fallbackText = InlineFormulaFallbackText(
				prepared,
				formula);
		} else {
			placement.object.fallbackText = InlineFormulaFallbackText(
				prepared,
				formula);
			placement.object.width = std::max(
				textStyle.font->width(placement.object.fallbackText),
				1);
		}
		result.push_back(std::move(placement));
	}
	return result;
}

void SetTextLeaf(
		Ui::Text::String *leaf,
		const style::TextStyle &textStyle,
		const TextWithEntities &text,
	const std::vector<PreparedInlineObject> &inlineObjects,
	const std::vector<PreparedFormulaSlot> &formulas) {
	auto context = Ui::Text::MarkedContext();
	auto placements = InlineFormulaPlacements(inlineObjects, formulas, textStyle);
	context.inlineObjects = Ui::Text::InlineObjectPlacements(
		placements.data(),
		placements.size());
	leaf->setMarkedText(textStyle, text, kIvMarkedTextOptions, context);
}

[[nodiscard]] LaidOutBlock LayoutFlowBlock(
		const PreparedBlock &prepared,
		const std::vector<PreparedFormulaSlot> &formulas,
		const MarkdownStyleSnapshot &style,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = prepared.kind;
	block.headingLevel = prepared.headingLevel;
	block.textWidth = std::max(width, 1);

	const auto &textStyle = TextStyleFor(prepared, style);
	SetTextLeaf(
		&block.leaf,
		textStyle,
		prepared.text,
		prepared.inlineObjects,
		formulas);
	BindLinks(&block.leaf, prepared.links);

	const auto height = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(textStyle));
	block.textRect = QRect(left, top, block.textWidth, height);
	block.outer = QRect(left, top, block.textWidth, height);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutCodeBlock(
		const PreparedBlock &prepared,
		const MarkdownStyleSnapshot &style,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::CodeBlock;

	const auto &padding = style.codePadding;
	block.textWidth = std::max(width - padding.left() - padding.right(), 1);

	auto y = top + padding.top();
	if (!prepared.codeLanguage.isEmpty()) {
		block.language.setMarkedText(
			style.codeLanguageStyle,
			TextWithEntities::Simple(prepared.codeLanguage),
			kIvMarkedTextOptions);
		const auto languageHeight = std::max(
			block.language.countHeight(block.textWidth, true),
			TextLineHeight(style.codeLanguageStyle));
		block.languageRect = QRect(
			left + padding.left(),
			y,
			block.textWidth,
			languageHeight);
		y += languageHeight + style.codeLanguageSkip;
	}

	block.leaf.setMarkedText(
		style.codeStyle,
		CodeBlockText(prepared.text.text),
		kIvMarkedTextOptions);
	const auto codeHeight = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(style.codeStyle));
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
		const MarkdownStyleSnapshot &style,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Rule;
	block.outer = QRect(
		left,
		top,
		std::max(width, 1),
		style.ruleHeight);
	block.textRect = block.outer;
	return block;
}

[[nodiscard]] LaidOutBlock LayoutDisplayMathBlock(
		const PreparedBlock &prepared,
		const std::vector<PreparedFormulaSlot> &formulas,
		const MarkdownStyleSnapshot &style,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::DisplayMath;
	block.formulaIndex = prepared.formulaIndex;

	const auto &padding = style.displayMathPadding;
	const auto contentLeft = left + padding.left();
	const auto contentTop = top + padding.top();
	const auto contentWidth = std::max(
		width - padding.left() - padding.right(),
		1);
	const auto formula = RenderedFormulaFor(formulas, prepared.formulaIndex);

	auto formulaWidth = 0;
	auto formulaHeight = 0;
	if (formula && formula->success) {
		formulaWidth = std::max(formula->logicalSize.width(), 1);
		formulaHeight = std::max(formula->logicalSize.height(), 1);
	} else {
		const auto &fallbackPadding = style.displayMathFallbackPadding;
		const auto fallbackPaddingWidth = fallbackPadding.left()
			+ fallbackPadding.right();
		const auto fallbackText = formula
			? formula->fallbackText
			: prepared.formulaTex.trimmed();
		block.fallbackLeaf.setMarkedText(
			style.displayMathFallbackStyle,
			TextWithEntities::Simple(fallbackText),
			kIvMarkedTextOptions);
		block.textWidth = std::max(contentWidth - fallbackPaddingWidth, 1);
		block.textWidth = std::min(
			block.textWidth,
			std::max(block.fallbackLeaf.maxWidth(), 1));
		auto textHeight = std::max(
			block.fallbackLeaf.countHeight(block.textWidth, true),
			TextLineHeight(style.displayMathFallbackStyle));
		formulaWidth = std::min(
			block.textWidth + fallbackPaddingWidth,
			contentWidth);
		block.textWidth = std::max(formulaWidth - fallbackPaddingWidth, 1);
		textHeight = std::max(
			block.fallbackLeaf.countHeight(block.textWidth, true),
			TextLineHeight(style.displayMathFallbackStyle));
		formulaHeight = fallbackPadding.top()
			+ textHeight
			+ fallbackPadding.bottom();
		block.textRect.setSize(QSize(block.textWidth, textHeight));
	}

	const auto centered = (style.displayMathAlign == ::style::al_center)
		&& (formulaWidth <= contentWidth);
	block.formulaAlign = centered ? ::style::al_center : ::style::al_left;

	block.contentRect = QRect(
		contentLeft,
		contentTop,
		contentWidth,
		formulaHeight);
	block.formulaRect = QRect(
		centered
			? (contentLeft + ((contentWidth - formulaWidth) / 2))
			: contentLeft,
		contentTop,
		formulaWidth,
		formulaHeight);
	block.visibleFormulaRect = block.formulaRect.intersected(block.contentRect);
	block.outer = QRect(
		left,
		top,
		std::max(width, 1),
		padding.top() + formulaHeight + padding.bottom());
	block.overflowed = formula
		&& formula->success
		&& (block.formulaRect.width() > block.visibleFormulaRect.width());

	if (!(formula && formula->success)) {
		const auto &fallbackPadding = style.displayMathFallbackPadding;
		block.textRect.moveTo(
			block.formulaRect.x() + fallbackPadding.left(),
			block.formulaRect.y() + fallbackPadding.top());
	}
	return block;
}

[[nodiscard]] LaidOutBlock LayoutTableBlock(
		const PreparedBlock &prepared,
		const std::vector<PreparedFormulaSlot> &formulas,
		const MarkdownStyleSnapshot &style,
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
				preparedRow.header,
				formulas,
				style));
		}
		rows.push_back(std::move(row));
	}

	block.tableColumnWidths = ComputeTableColumnWidths(
		rows,
		columnCount,
		width,
		style,
		&block.overflowed);

	const auto &padding = style.tableCellPadding;
	const auto border = style.tableBorder;
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
			const auto &textStyle = TableCellTextStyle(rowData.header, style);
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
					TextLineHeight(textStyle));
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
	const MarkdownStyleSnapshot &style,
	const std::vector<PreparedFormulaSlot> &formulas,
	int left,
	int top,
	int width,
	LayoutContext context);

[[nodiscard]] int LayoutBlocks(
		const std::vector<PreparedBlock> &prepared,
		std::vector<LaidOutBlock> *blocks,
		const MarkdownStyleSnapshot &style,
		const std::vector<PreparedFormulaSlot> &formulas,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto y = top;
	auto previous = static_cast<const PreparedBlock*>(nullptr);
	for (const auto &block : prepared) {
		if (previous) {
			y += BlockSkip(*previous, block, context, style);
		}
		auto laidOut = LayoutBlock(
			block,
			style,
			formulas,
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
		const MarkdownStyleSnapshot &style,
		const std::vector<PreparedFormulaSlot> &formulas,
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
		markerTextWidth = style.taskMarkerSize;
		markerTextHeight = style.taskMarkerSize;
	} else {
		block.marker.setMarkedText(
			style.paragraphStyle,
			TextWithEntities::Simple(markerText),
			kIvMarkedTextOptions);
		markerTextWidth = std::max(block.marker.maxWidth(), 1);
		markerTextHeight = std::max(
			block.marker.countHeight(markerTextWidth, true),
			TextLineHeight(style.paragraphStyle));
	}

	block.markerWidth = std::max(style.listMarkerWidth, markerTextWidth);
	const auto bodyLeft = left + block.markerWidth + style.listMarkerSkip;
	const auto bodyWidth = std::max(
		width - block.markerWidth - style.listMarkerSkip,
		1);

	auto childContext = context;
	childContext.tightList = tight;
	const auto childBottom = LayoutBlocks(
		prepared.children,
		&block.children,
		style,
		formulas,
		bodyLeft,
		top,
		bodyWidth,
		childContext);
	const auto contentHeight = childBottom - top;
	const auto rowHeight = std::max({
		contentHeight,
		markerTextHeight,
		TextLineHeight(style.paragraphStyle),
	});

	const auto markerTop = top + std::max(
		(TextLineHeight(style.paragraphStyle) - markerTextHeight) / 2,
		0);
	if (task) {
		block.markerRect = QRect(
			left,
			markerTop,
			style.taskMarkerSize,
			style.taskMarkerSize);
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
		const MarkdownStyleSnapshot &style,
		const std::vector<PreparedFormulaSlot> &formulas,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::List;
	block.listKind = prepared.listKind;
	block.listDelimiter = prepared.listDelimiter;

	const auto depthDelta = std::max(prepared.visualDepth - context.listDepth, 0);
	const auto listLeft = left + depthDelta * style.listIndent;
	const auto listWidth = std::max(
		width - depthDelta * style.listIndent,
		1);

	auto childContext = context;
	childContext.listDepth = prepared.visualDepth;
	childContext.tightList = false;

	auto y = top;
	auto first = true;
	for (const auto &child : prepared.children) {
		if (!first) {
			y += prepared.tight ? 0 : BlockSkip(child, style);
		}
		first = false;

		auto laidOut = (child.kind == PreparedBlockKind::ListItem)
			? LayoutListItemBlock(
				child,
				style,
				formulas,
				listLeft,
				y,
				listWidth,
				childContext,
				prepared.tight)
			: LayoutBlock(
				child,
				style,
				formulas,
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
	return block;
}

[[nodiscard]] LaidOutBlock LayoutQuoteBlock(
		const PreparedBlock &prepared,
		const MarkdownStyleSnapshot &style,
		const std::vector<PreparedFormulaSlot> &formulas,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Quote;

	const auto depthDelta = std::max(
		prepared.visualDepth - context.quoteDepth,
		0);
	const auto quoteLeft = left + depthDelta * style.quoteIndent;
	const auto quoteWidth = std::max(
		width - depthDelta * style.quoteIndent,
		1);
	const auto &padding = style.quotePadding;
	const auto contentLeft = quoteLeft + style.quoteBorder + padding.left();
	const auto contentTop = top + padding.top();
	const auto contentWidth = std::max(
		quoteWidth - style.quoteBorder - padding.left() - padding.right(),
		1);

	auto childContext = context;
	childContext.quoteDepth = prepared.visualDepth;
	childContext.tightList = false;
	const auto childBottom = LayoutBlocks(
		prepared.children,
		&block.children,
		style,
		formulas,
		contentLeft,
		contentTop,
		contentWidth,
		childContext);
	const auto contentHeight = std::max(
		childBottom - contentTop,
		prepared.children.empty()
			? TextLineHeight(style.paragraphStyle)
			: 0);
	const auto quoteHeight = padding.top() + contentHeight + padding.bottom();

	block.outer = QRect(quoteLeft, top, quoteWidth, quoteHeight);
	block.borderRect = QRect(
		quoteLeft,
		top,
		style.quoteBorder,
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
		const MarkdownStyleSnapshot &style,
		const std::vector<PreparedFormulaSlot> &formulas,
		int left,
		int top,
		int width,
		LayoutContext context) {
	switch (prepared.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		return LayoutFlowBlock(prepared, formulas, style, left, top, width);
	case PreparedBlockKind::CodeBlock:
		return LayoutCodeBlock(prepared, style, left, top, width);
	case PreparedBlockKind::Rule:
		return LayoutRuleBlock(style, left, top, width);
	case PreparedBlockKind::List:
		return LayoutListBlock(
			prepared,
			style,
			formulas,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::ListItem:
		return LayoutListItemBlock(
			prepared,
			style,
			formulas,
			left,
			top,
			width,
			context,
			false);
	case PreparedBlockKind::Quote:
		return LayoutQuoteBlock(
			prepared,
			style,
			formulas,
			left,
			top,
			width,
			context);
	case PreparedBlockKind::DisplayMath:
		return LayoutDisplayMathBlock(
			prepared,
			formulas,
			style,
			left,
			top,
			width);
	case PreparedBlockKind::Table:
		return LayoutTableBlock(prepared, formulas, style, left, top, width);
	}
	return LayoutFlowBlock(prepared, formulas, style, left, top, width);
}

class DocumentLayout final {
public:
	void invalidate();
	void relayout(const PreparedResult &prepared, int width);

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
	explicit MarkdownDocumentWidget(QWidget *parent);

	void setPreparedResult(PreparedResult prepared);
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

	void relayoutCurrentWidth();
	void forceRelayoutCurrentWidth();
	void updateHover(QPoint point);
	void applyCursor(style::cursor cursor);

	PreparedResult _prepared;
	DocumentLayout _layout;
	std::optional<SnapshotTextPalette> _textPalette;
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
		.palette = &p.textPalette(),
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = crl::now(),
	});
}

void PaintTaskMarker(
		Painter &p,
		const LaidOutBlock &block,
		const MarkdownStyleSnapshot &style) {
	const auto rect = block.markerRect;
	if (rect.isEmpty()) {
		return;
	}
	const auto border = style.taskMarkerBorder;
	if (block.taskState == TaskState::Checked) {
		p.setPen(Qt::NoPen);
		p.setBrush(style.taskMarkerColor);
		p.drawRect(rect);

		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(
			style.taskMarkerCheckColor,
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
		p.setPen(QPen(style.taskMarkerColor, border));
		p.drawRect(rect.adjusted(0, 0, -border, -border));
	}
}

void PaintBlocks(
	Painter &p,
	const std::vector<LaidOutBlock> &blocks,
	const PreparedResult &prepared,
	QRect clip);

void PaintTableBlock(
		Painter &p,
		const LaidOutBlock &block,
		const MarkdownStyleSnapshot &style,
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
			p.fillRect(cell.outer, style.tableHeaderBackgroundColor);
		}
	}

	const auto border = style.tableBorder;
	if (border > 0 && !block.tableRect.isEmpty()) {
		const auto left = block.tableRect.x();
		const auto top = block.tableRect.y();
		const auto width = block.tableRect.width();
		const auto height = block.tableRect.height();
		const auto right = left + width - border;
		const auto bottom = top + height - border;

		p.fillRect(QRect(left, top, width, border), style.tableBorderColor);
		p.fillRect(
			QRect(left, bottom, width, border),
			style.tableBorderColor);
		p.fillRect(QRect(left, top, border, height), style.tableBorderColor);
		p.fillRect(
			QRect(right, top, border, height),
			style.tableBorderColor);

		auto separatorLeft = left + border;
		for (auto i = 0, count = int(block.tableColumnWidths.size()); i != count; ++i) {
			separatorLeft += block.tableColumnWidths[i];
			if (i + 1 != count) {
				p.fillRect(
					QRect(separatorLeft, top, border, height),
					style.tableBorderColor);
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
				style.tableBorderColor);
		}
	}

	p.setPen(style.defaultTextColor);
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
			std::max(style.tableOverflowWidth, 1),
			block.visibleTableRect.width());
		p.fillRect(
			QRect(
				block.visibleTableRect.x()
					+ block.visibleTableRect.width()
					- indicatorWidth,
				block.visibleTableRect.y(),
				indicatorWidth,
				block.visibleTableRect.height()),
			style.tableOverflowColor);
	}

	p.restore();
}

void PaintDisplayMathBlock(
		Painter &p,
		const LaidOutBlock &block,
		const PreparedResult &prepared,
		QRect clip) {
	const auto formulaClip = clip.intersected(block.visibleFormulaRect);
	if (formulaClip.isEmpty()) {
		return;
	}

	p.save();
	p.setClipRect(formulaClip);

	const auto &style = prepared.style;
	const auto formula = RenderedFormulaFor(prepared.formulas, block.formulaIndex);
	if (formula && formula->success) {
		p.drawImage(block.formulaRect.topLeft(), formula->image);
	} else {
		const auto radius = style.displayMathFallbackRadius;
		p.setPen(Qt::NoPen);
		p.setBrush(style.displayMathFallbackBackgroundColor);
		if (radius > 0) {
			auto hq = PainterHighQualityEnabler(p);
			p.drawRoundedRect(block.formulaRect, radius, radius);
		} else {
			p.fillRect(block.formulaRect, style.displayMathFallbackBackgroundColor);
		}
		p.setPen(style.defaultTextColor);
		PaintTextLeaf(
			p,
			block.fallbackLeaf,
			block.textRect,
			block.textWidth,
			formulaClip);
	}

	if (block.overflowed) {
		const auto indicatorWidth = std::min(
			std::max(style.displayMathOverflowWidth, 1),
			block.visibleFormulaRect.width());
		p.fillRect(
			QRect(
				block.visibleFormulaRect.x()
					+ block.visibleFormulaRect.width()
					- indicatorWidth,
				block.visibleFormulaRect.y(),
				indicatorWidth,
				block.visibleFormulaRect.height()),
			style.displayMathOverflowColor);
	}

	p.restore();
}

void PaintBlock(
		Painter &p,
		const LaidOutBlock &block,
		const PreparedResult &prepared,
		QRect clip) {
	if (!block.outer.intersects(clip)) {
		return;
	}

	const auto &style = prepared.style;
	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		p.setPen(style.defaultTextColor);
		PaintTextLeaf(p, block.leaf, block.textRect, block.textWidth, clip);
		break;
	case PreparedBlockKind::CodeBlock: {
		const auto radius = style.codeRadius;
		p.setPen(Qt::NoPen);
		p.setBrush(style.codeBackgroundColor);
		if (radius > 0) {
			auto hq = PainterHighQualityEnabler(p);
			p.drawRoundedRect(block.outer, radius, radius);
		} else {
			p.fillRect(block.outer, style.codeBackgroundColor);
		}
		if (!block.languageRect.isEmpty()) {
			p.setPen(style.codeLanguageColor);
			PaintTextLeaf(
				p,
				block.language,
				block.languageRect,
				block.textWidth,
				clip);
		}
		p.setPen(style.defaultTextColor);
		PaintTextLeaf(p, block.leaf, block.textRect, block.textWidth, clip);
	} break;
	case PreparedBlockKind::Rule:
		p.fillRect(block.outer, style.ruleColor);
		break;
	case PreparedBlockKind::List:
		PaintBlocks(p, block.children, prepared, clip);
		break;
	case PreparedBlockKind::ListItem:
		if (block.taskState != TaskState::None) {
			PaintTaskMarker(p, block, style);
		} else if (!block.markerRect.isEmpty()) {
			p.setPen(style.defaultTextColor);
			PaintTextLeaf(
				p,
				block.marker,
				block.markerRect,
				block.markerWidth,
				clip);
		}
		PaintBlocks(p, block.children, prepared, clip);
		break;
	case PreparedBlockKind::Quote:
		p.fillRect(block.borderRect, style.quoteBorderColor);
		PaintBlocks(p, block.children, prepared, clip);
		break;
	case PreparedBlockKind::DisplayMath:
		PaintDisplayMathBlock(p, block, prepared, clip);
		break;
	case PreparedBlockKind::Table:
		PaintTableBlock(p, block, style, clip);
		break;
	}
}

void PaintBlocks(
		Painter &p,
		const std::vector<LaidOutBlock> &blocks,
		const PreparedResult &prepared,
		QRect clip) {
	for (const auto &block : blocks) {
		if (block.outer.bottom() < clip.top()) {
			continue;
		} else if (block.outer.top() > clip.bottom()) {
			break;
		}
		PaintBlock(p, block, prepared, clip);
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
	case PreparedBlockKind::DisplayMath:
		return nullptr;
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
		const PreparedResult &prepared,
		int width) {
	width = std::max(width, 1);
	if (_width == width) {
		return;
	}
	_width = width;
	_blocks.clear();

	const auto &page = prepared.style.pagePadding;
	const auto innerWidth = std::max(width - page.left() - page.right(), 1);
	const auto y = LayoutBlocks(
		prepared.blocks.blocks,
		&_blocks,
		prepared.style,
		prepared.formulas,
		page.left(),
		page.top(),
		innerWidth,
		{});
	_height = y + page.bottom();
}

void DocumentLayout::invalidate() {
	_width = -1;
	_height = 0;
	_blocks.clear();
}

int DocumentLayout::height() const {
	return _height;
}

const std::vector<LaidOutBlock> &DocumentLayout::blocks() const {
	return _blocks;
}

MarkdownDocumentWidget::MarkdownDocumentWidget(
	QWidget *parent)
: Ui::RpWidget(parent) {
	setMouseTracking(true);
}

void MarkdownDocumentWidget::setPreparedResult(PreparedResult prepared) {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	_layout.invalidate();
	_prepared = std::move(prepared);
	_textPalette.emplace(_prepared.style.textPalette);
	forceRelayoutCurrentWidth();
}

int MarkdownDocumentWidget::resizeGetHeight(int newWidth) {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	_layout.relayout(_prepared, newWidth);
	return std::max(_layout.height(), 1);
}

void MarkdownDocumentWidget::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_textPalette) {
		p.setTextPalette(_textPalette->palette);
	}

	PaintBlocks(p, _layout.blocks(), _prepared, e->rect());
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

void MarkdownDocumentWidget::relayoutCurrentWidth() {
	_layout.relayout(_prepared, width());
}

void MarkdownDocumentWidget::forceRelayoutCurrentWidth() {
	resizeToWidth(width());
	update();
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

constexpr auto kDeferredPreparationSourceBytes = 128 * 1024;
constexpr auto kDeferredPreparationFormulaCount = 4;
constexpr auto kDeferredPreparationConvertedNodes = 1200;

class MarkdownPreviewRoot final : public Ui::RpWidget {
public:
	MarkdownPreviewRoot(
		const PreparedDocument &document,
		const OpenOptions &options,
		QWidget *parent = nullptr);
	~MarkdownPreviewRoot();

private:
	[[nodiscard]] bool shouldDeferPreparation() const;

	void startPreparation(
		bool deferred,
		std::optional<MarkdownStyleSnapshot> style = std::nullopt);
	void applyPreparedResult(PreparedResult prepared);
	void updateChildrenGeometry(QSize size);
	void updateLoadingGeometry();
	void cancelInFlightRequest();

	const std::shared_ptr<const PreparedDocument> _document;
	Ui::ScrollArea *_scroll = nullptr;
	MarkdownDocumentWidget *_body = nullptr;
	Ui::FlatLabel *_loading = nullptr;
	PrepareGeneration _generation = 0;
	int _requestedDevicePixelRatio = 0;
	std::shared_ptr<std::atomic_bool> _cancelled;

};

MarkdownPreviewRoot::MarkdownPreviewRoot(
	const PreparedDocument &document,
	const OpenOptions &options,
	QWidget *parent)
: Ui::RpWidget(parent)
, _document(std::make_shared<PreparedDocument>(document))
, _cancelled(std::make_shared<std::atomic_bool>(false)) {
	(void)options;

	_scroll = Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll);
	_body = _scroll->setOwnedWidget(object_ptr<MarkdownDocumentWidget>(_scroll));
	_loading = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_contacts_loading(tr::now),
		st::membersAbout);

	_scroll->hide();
	if (_body) {
		_body->hide();
	}
	_loading->hide();

	const auto initialStyle = CaptureMarkdownStyleSnapshot();
	_requestedDevicePixelRatio = initialStyle.devicePixelRatio;

	sizeValue() | rpl::on_next([=](QSize size) {
		updateChildrenGeometry(size);
	}, lifetime());

	style::PaletteChanged() | rpl::on_next([=] {
		startPreparation(shouldDeferPreparation());
	}, lifetime());

	screenValue() | rpl::on_next([=](not_null<QScreen*>) {
		const auto style = CaptureMarkdownStyleSnapshot();
		if (style.devicePixelRatio == _requestedDevicePixelRatio) {
			return;
		}
		startPreparation(shouldDeferPreparation(), std::move(style));
	}, lifetime());

	startPreparation(shouldDeferPreparation(), std::move(initialStyle));
}

MarkdownPreviewRoot::~MarkdownPreviewRoot() {
	cancelInFlightRequest();
}

bool MarkdownPreviewRoot::shouldDeferPreparation() const {
	return (_document->sourceText.size() >= kDeferredPreparationSourceBytes)
		|| (int(_document->formulas.size()) >= kDeferredPreparationFormulaCount)
		|| (_document->stats.convertedNodeCount
			>= kDeferredPreparationConvertedNodes);
}

void MarkdownPreviewRoot::startPreparation(
		bool deferred,
		std::optional<MarkdownStyleSnapshot> style) {
	cancelInFlightRequest();

	_cancelled = std::make_shared<std::atomic_bool>(false);
	const auto cancelled = _cancelled;
	const auto generation = ++_generation;
	const auto showLoading = deferred
		|| !_loading->isHidden()
		|| !_scroll->isHidden();
	if (!style) {
		style = CaptureMarkdownStyleSnapshot();
	}
	_requestedDevicePixelRatio = style->devicePixelRatio;
	auto request = PrepareRequest{
		.document = _document,
		.style = std::move(*style),
		.generation = generation,
		.cancelled = cancelled,
	};

	if (showLoading) {
		_scroll->hide();
		if (_body) {
			_body->hide();
		}
		_loading->show();
		_loading->raise();
		updateLoadingGeometry();
	} else {
		_loading->hide();
	}

	const auto weak = base::make_weak(this);
	auto done = [=](PreparedResult result) mutable {
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (cancelled->load(std::memory_order_relaxed)) {
			return;
		} else if (result.cancelled) {
			return;
		} else if (result.generation != strong->_generation) {
			return;
		}
		strong->applyPreparedResult(std::move(result));
	};

	if (deferred) {
		PrepareAsync(std::move(request), std::move(done));
	} else {
		done(PrepareSynchronously(std::move(request)));
	}
}

void MarkdownPreviewRoot::applyPreparedResult(PreparedResult prepared) {
	if (!_body) {
		return;
	}
	_body->setPreparedResult(std::move(prepared));
	_body->resizeToWidth(_scroll->width());
	_scroll->show();
	_body->show();
	_loading->hide();
}

void MarkdownPreviewRoot::updateChildrenGeometry(QSize size) {
	_scroll->setGeometry(QRect(QPoint(), size));
	if (_body) {
		_body->resizeToWidth(_scroll->width());
	}
	updateLoadingGeometry();
}

void MarkdownPreviewRoot::updateLoadingGeometry() {
	const auto availableWidth = std::max(width(), 1);
	_loading->resizeToWidth(availableWidth);
	_loading->moveToLeft(
		0,
		std::max((height() - _loading->height()) / 2, 0),
		availableWidth);
}

void MarkdownPreviewRoot::cancelInFlightRequest() {
	if (_cancelled) {
		_cancelled->store(true, std::memory_order_relaxed);
	}
}

} // namespace

std::unique_ptr<Ui::RpWidget> CreateMarkdownPreviewWidget(
	const PreparedDocument &document,
	const OpenOptions &options) {
	return std::make_unique<MarkdownPreviewRoot>(document, options);
}

} // namespace Iv::Markdown
