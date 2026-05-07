#include "iv/markdown/iv_markdown_article_layout_blocks.h"

#include "iv/markdown/iv_markdown_article_text.h"

#include "lang/lang_keys.h"
#include "spellcheck/spellcheck_highlight_syntax.h"

#include <algorithm>
#include <utility>

#include "styles/style_iv.h"
#include "styles/style_widgets.h"

namespace Iv::Markdown {
namespace {

constexpr auto kIvMarkedTextOptions = TextParseOptions{
	TextParseMultiline,
	0,
	0,
	Qt::LayoutDirectionAuto,
};

constexpr auto kCodeTabColumns = 4;
constexpr auto kCodeTrailingGuard = 0x2060;
const auto kPhotoCopyLabel = u"Photo"_q;

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
		const style::Markdown &) {
	if (header) {
		return st::defaultTable.defaultLabel.style;
	}
	return st::defaultTable.defaultValue.style;
}

[[nodiscard]] TableCellLayoutData InitializeTableCellLayout(
		const PreparedTableCell &prepared,
		bool header,
		const std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown) {
	auto result = TableCellLayoutData();
	const auto &textStyle = TableCellTextStyle(header, markdown);
	result.cell.align = CellAlign(prepared.alignment);
	SetTextLeaf(
		&result.cell.leaf,
		textStyle,
		prepared.text,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		TableCellTextMinResizeWidth(textStyle, markdown));
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
		const style::Markdown &markdown,
		bool *overflowed) {
	const auto &padding = markdown.table.cellPadding;
	const auto border = st::defaultTable.border;
	const auto minimum = markdown.table.minColumnWidth;
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

} // namespace

[[nodiscard]] int SingleDigitOrderedMarkerWidth(
		const style::Markdown &markdown) {
	return std::max(
		markdown.body.font->width(u"8."_q),
		markdown.body.font->width(u"8)"_q));
}

QString CodeBlockDisplayText(const QString &text) {
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

bool IsFlowKind(PreparedBlockKind kind) {
	return (kind == PreparedBlockKind::Paragraph)
		|| (kind == PreparedBlockKind::Heading);
}

QString ListMarkerText(const PreparedBlock &block) {
	if (block.listKind == ListKind::Ordered) {
		const auto delimiter = (block.listDelimiter == ListDelimiter::Parenthesis)
			? u")"_q
			: u"."_q;
		return QString::number(block.orderedNumber) + delimiter;
	}
	return QString();
}

int TextLineHeight(const style::TextStyle &style) {
	return std::max(style.lineHeight, style.font->height);
}

QPoint BulletMarkerCenter(
		int left,
		int top,
		const style::Markdown &markdown) {
	const auto &list = markdown.list;
	const auto lineHeight = TextLineHeight(markdown.body);
	const auto markerWidth = SingleDigitOrderedMarkerWidth(markdown);
	return QPoint(
		left + list.markerWidth - list.bulletLeftShift - ((markerWidth + 1) / 2),
		top + (lineHeight / 2));
}

QMargins BlockquotePadding(const style::QuoteStyle &style) {
	return style.padding
		+ QMargins(0, style.header + style.verticalSkip, 0, style.verticalSkip);
}

Ui::Text::GeometryDescriptor TextGeometry(int width) {
	auto result = Ui::Text::SimpleGeometry(std::max(width, 1), 0, 0, false);
	result.breakEverywhere = true;
	return result;
}

int TextMinResizeWidth(int width) {
	return std::max(width, 1);
}

int TableCellTextMinResizeWidth(
		const style::TextStyle &textStyle,
		const style::Markdown &markdown) {
	const auto &padding = markdown.table.cellPadding;
	return std::max({
		markdown.table.minColumnWidth - padding.left() - padding.right(),
		textStyle.font->spacew,
		1,
	});
}

int BlockSkip(
		const PreparedBlock &block,
		const style::Markdown &markdown) {
	const auto &skips = markdown.blockSkips;
	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
		return skips.paragraph;
	case PreparedBlockKind::Heading:
		return skips.heading;
	case PreparedBlockKind::CodeBlock:
		return skips.code;
	case PreparedBlockKind::Rule:
		return skips.rule;
	case PreparedBlockKind::List:
	case PreparedBlockKind::ListItem:
		return skips.paragraph;
	case PreparedBlockKind::Quote:
		return skips.quote;
	case PreparedBlockKind::DisplayMath:
		return skips.displayMath;
	case PreparedBlockKind::Table:
		return skips.table;
	case PreparedBlockKind::Photo:
		return skips.photo;
	case PreparedBlockKind::Placeholder:
		return skips.placeholder;
	case PreparedBlockKind::Details:
		return skips.paragraph;
	}
	return 0;
}

int BlockSkip(
		const PreparedBlock &previous,
		const PreparedBlock &block,
		LayoutContext context,
		const style::Markdown &markdown) {
	if (context.tightList
		&& IsFlowKind(previous.kind)
		&& IsFlowKind(block.kind)) {
		return 0;
	}
	return BlockSkip(block, markdown);
}

const style::TextStyle &TextStyleFor(
		const PreparedBlock &block,
		const style::Markdown &markdown) {
	if (block.kind == PreparedBlockKind::CodeBlock) {
		return markdown.code;
	} else if (block.kind != PreparedBlockKind::Heading) {
		return markdown.body;
	}
	switch (std::clamp(block.headingLevel, 1, 6)) {
	case 1: return markdown.heading1;
	case 2: return markdown.heading2;
	case 3: return markdown.heading3;
	case 4: return markdown.heading4;
	case 5: return markdown.heading5;
	case 6: return markdown.heading6;
	}
	return markdown.heading6;
}

int BlockMaxRight(const std::vector<LaidOutBlock> &blocks) {
	auto result = 0;
	for (const auto &block : blocks) {
		result = std::max(result, block.outer.right() + 1);
		result = std::max(result, BlockMaxRight(block.children));
	}
	return result;
}

void RepopulateCodeBlockLeaf(
		LaidOutBlock &block,
		const style::Markdown &markdown,
		bool allowAsyncSyntaxHighlighting,
		CodeBlockSyntaxHighlightTracker *syntaxHighlightTracker) {
	auto marked = tr::marked(CodeBlockDisplayText(block.copyText));
	if (!marked.text.isEmpty()) {
		marked.entities.push_back(EntityInText(
			EntityType::Pre,
			0,
			marked.text.size(),
			block.codeLanguage));
	}
	block.syntaxHighlightProcessId = allowAsyncSyntaxHighlighting
		? (syntaxHighlightTracker
			? syntaxHighlightTracker->tryHighlightSyntax(
				marked.text,
				block.codeLanguage,
				marked)
			: Spellchecker::TryHighlightSyntax(marked))
		: 0;
	block.leaf.setMarkedText(
		markdown.code,
		std::move(marked),
		kIvMarkedTextOptions);
}

LaidOutBlock LayoutFlowBlock(
		const PreparedBlock &prepared,
		const std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = prepared.kind;
	block.anchorId = prepared.anchorId;
	block.headingLevel = prepared.headingLevel;
	block.textWidth = std::max(width, 1);

	const auto &textStyle = TextStyleFor(prepared, markdown);
	SetTextLeaf(
		&block.leaf,
		textStyle,
		prepared.text,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		block.textWidth);
	BindLinks(&block.leaf, prepared.links);

	const auto height = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(textStyle));
	block.textRect = QRect(left, top, block.textWidth, height);
	block.outer = QRect(left, top, block.textWidth, height);
	return block;
}

LaidOutBlock LayoutCodeBlock(
		const PreparedBlock &prepared,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		bool allowAsyncSyntaxHighlighting,
		CodeBlockSyntaxHighlightTracker *syntaxHighlightTracker) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::CodeBlock;
	block.copyText = prepared.text.text;
	block.codeLanguage = prepared.codeLanguage;
	block.textWidth = std::max(width, 1);
	block.leaf = Ui::Text::String(TextMinResizeWidth(block.textWidth));
	RepopulateCodeBlockLeaf(
		block,
		markdown,
		allowAsyncSyntaxHighlighting,
		syntaxHighlightTracker);
	const auto height = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(markdown.code));
	block.textRect = QRect(left, top, block.textWidth, height);
	block.outer = block.textRect;
	return block;
}

LaidOutBlock LayoutRuleBlock(
		const style::Markdown &markdown,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Rule;
	block.outer = QRect(
		left,
		top,
		std::max(width, 1),
		markdown.rule.height);
	block.textRect = block.outer;
	return block;
}

LaidOutBlock LayoutDisplayMathBlock(
		const PreparedBlock &prepared,
		const std::vector<PreparedFormulaSlot> &formulas,
		const style::Markdown &markdown,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::DisplayMath;
	block.formulaIndex = prepared.formulaIndex;
	block.copyText = prepared.formulaTex;

	const auto &padding = markdown.displayMath.padding;
	const auto contentLeft = left + padding.left();
	const auto contentTop = top + padding.top();
	const auto contentWidth = std::max(
		width - padding.left() - padding.right(),
		1);
	const auto formula = PreparedFormulaFor(formulas, prepared.formulaIndex);

	auto formulaWidth = 0;
	auto formulaHeight = 0;
	if (formula && formula->measured.success) {
		formulaWidth = std::max(formula->measured.logicalSize.width(), 1);
		formulaHeight = std::max(formula->measured.logicalSize.height(), 1);
	} else {
		const auto &fallbackPadding = markdown.displayMath.fallbackPadding;
		const auto fallbackPaddingWidth = fallbackPadding.left()
			+ fallbackPadding.right();
		const auto fallbackText = formula
			? formula->measured.fallbackText
			: prepared.formulaTex.trimmed();
		block.textWidth = std::max(contentWidth - fallbackPaddingWidth, 1);
		block.fallbackLeaf = Ui::Text::String(
			TextMinResizeWidth(block.textWidth));
		block.fallbackLeaf.setMarkedText(
			markdown.displayMath.fallbackStyle,
			TextWithEntities::Simple(fallbackText),
			kIvMarkedTextOptions);
		block.textWidth = std::min(
			block.textWidth,
			std::max(block.fallbackLeaf.maxWidth(), 1));
		auto textHeight = std::max(
			block.fallbackLeaf.countHeight(block.textWidth, true),
			TextLineHeight(markdown.displayMath.fallbackStyle));
		formulaWidth = std::min(
			block.textWidth + fallbackPaddingWidth,
			contentWidth);
		block.textWidth = std::max(formulaWidth - fallbackPaddingWidth, 1);
		textHeight = std::max(
			block.fallbackLeaf.countHeight(block.textWidth, true),
			TextLineHeight(markdown.displayMath.fallbackStyle));
		formulaHeight = fallbackPadding.top()
			+ textHeight
			+ fallbackPadding.bottom();
		block.textRect.setSize(QSize(block.textWidth, textHeight));
	}

	const auto centered = (markdown.displayMath.align == ::style::al_center)
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
		&& formula->measured.success
		&& (block.formulaRect.width() > block.visibleFormulaRect.width());

	if (!(formula && formula->measured.success)) {
		const auto &fallbackPadding = markdown.displayMath.fallbackPadding;
		block.textRect.moveTo(
			block.formulaRect.x() + fallbackPadding.left(),
			block.formulaRect.y() + fallbackPadding.top());
	}
	return block;
}

LaidOutBlock LayoutTableBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
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
				inlineFormulaObjects,
				mediaRuntime,
				markdown));
		}
		rows.push_back(std::move(row));
	}

	block.tableColumnWidths = ComputeTableColumnWidths(
		rows,
		columnCount,
		width,
		markdown,
		&block.overflowed);

	const auto &padding = markdown.table.cellPadding;
	const auto border = st::defaultTable.border;
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
			const auto &textStyle = TableCellTextStyle(rowData.header, markdown);
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

LaidOutBlock LayoutPlaceholderBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Placeholder;
	block.anchorId = prepared.anchorId;
	block.copyText = prepared.placeholder.copyText;
	block.labelText = prepared.placeholder.label;

	const auto &style = markdown.placeholder;
	const auto blockWidth = std::max(width, 1);
	const auto contentLeft = left + style.padding.left();
	const auto contentWidth = std::max(
		blockWidth - style.padding.left() - style.padding.right(),
		1);
	block.labelWidth = contentWidth;
	block.labelLeaf = Ui::Text::String(TextMinResizeWidth(contentWidth));
	block.labelLeaf.setMarkedText(
		style.labelStyle,
		TextWithEntities::Simple(block.labelText),
		kIvMarkedTextOptions);
	const auto labelHeight = std::max(
		block.labelLeaf.countHeight(contentWidth, true),
		TextLineHeight(style.labelStyle));
	const auto mediaHeight = std::max(
		style.minHeight,
		labelHeight + style.padding.top() + style.padding.bottom());
	block.mediaRect = QRect(left, top, blockWidth, mediaHeight);
	block.visibleMediaRect = block.mediaRect;
	block.labelRect = QRect(
		contentLeft,
		top + std::max((mediaHeight - labelHeight) / 2, 0),
		contentWidth,
		labelHeight);

	auto bottom = top + mediaHeight;
	if (!prepared.text.text.isEmpty()) {
		block.textWidth = contentWidth;
		SetTextLeaf(
			&block.leaf,
			markdown.body,
			prepared.text,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			block.textWidth);
		BindLinks(&block.leaf, prepared.links);
		const auto captionTop = bottom + style.captionSkip;
		const auto captionHeight = std::max(
			block.leaf.countHeight(block.textWidth, true),
			TextLineHeight(markdown.body));
		block.textRect = QRect(
			contentLeft,
			captionTop,
			block.textWidth,
			captionHeight);
		bottom = captionTop + captionHeight;
	}

	block.contentRect = QRect(
		left,
		top,
		blockWidth,
		std::max(bottom - top, mediaHeight));
	block.outer = block.contentRect;
	return block;
}

LaidOutBlock LayoutPhotoBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Photo;
	block.anchorId = prepared.anchorId;
	block.copyText = kPhotoCopyLabel;

	const auto &style = markdown.photo;
	const auto blockWidth = std::max(width, 1);
	const auto mediaLeft = left + style.padding.left();
	const auto mediaTop = top + style.padding.top();
	const auto mediaWidth = std::max(
		blockWidth - style.padding.left() - style.padding.right(),
		1);
	const auto aspectWidth = std::max(prepared.photo.width, 1);
	const auto aspectHeight = std::max(prepared.photo.height, 1);
	const auto mediaHeight = std::max(
		int((int64(mediaWidth) * aspectHeight + aspectWidth - 1) / aspectWidth),
		1);
	block.mediaRect = QRect(mediaLeft, mediaTop, mediaWidth, mediaHeight);
	block.visibleMediaRect = block.mediaRect;

	if (mediaRuntime) {
		block.photoRuntime = mediaRuntime->resolvePhoto(prepared.photo.photoId);
	}
	if (block.photoRuntime) {
		const auto size = QSize(mediaWidth, mediaHeight);
		block.thumbnailImage = block.photoRuntime->thumbnail(size);
		block.fullImage = block.photoRuntime->full(size);
	}
	if (!prepared.photo.urlOverride.isEmpty()) {
		block.activation.kind = MediaActivationKind::ExternalUrl;
		block.activation.url = prepared.photo.urlOverride;
	} else if (prepared.photo.viewerOpen && block.photoRuntime) {
		block.activation.kind = MediaActivationKind::Photo;
		block.activation.photo = block.photoRuntime;
	}

	auto bottom = mediaTop + mediaHeight + style.padding.bottom();
	if (!prepared.text.text.isEmpty()) {
		block.textWidth = mediaWidth;
		SetTextLeaf(
			&block.leaf,
			markdown.body,
			prepared.text,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			block.textWidth);
		BindLinks(&block.leaf, prepared.links);
		const auto captionTop = bottom + style.captionSkip;
		const auto captionHeight = std::max(
			block.leaf.countHeight(block.textWidth, true),
			TextLineHeight(markdown.body));
		block.textRect = QRect(
			mediaLeft,
			captionTop,
			block.textWidth,
			captionHeight);
		bottom = captionTop + captionHeight;
	}

	block.contentRect = QRect(
		mediaLeft,
		mediaTop,
		mediaWidth,
		std::max(bottom - mediaTop, mediaHeight));
	block.outer = QRect(left, top, blockWidth, std::max(bottom - top, mediaHeight));
	return block;
}

} // namespace Iv::Markdown
