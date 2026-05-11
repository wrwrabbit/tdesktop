/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_article_layout_blocks.h"
#include "iv/markdown/iv_markdown_media_block.h"
#include "iv/markdown/iv_markdown_article_text.h"
#include "lang/lang_keys.h"
#include "spellcheck/spellcheck_highlight_syntax.h"

#include "styles/style_iv.h"
#include "styles/style_widgets.h"

#include <algorithm>
#include <cmath>
#include <utility>

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
const auto kUsernamePrefix = u"@"_q;

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
		const PreparedTableCell &prepared,
		const style::Markdown &) {
	if (prepared.header) {
		return st::defaultTable.defaultLabel.style;
	}
	return st::defaultTable.defaultValue.style;
}

[[nodiscard]] const style::TextStyle &TableCellTextStyle(
		const LaidOutTableCell &cell,
		const style::Markdown &) {
	if (cell.header) {
		return st::defaultTable.defaultLabel.style;
	}
	return st::defaultTable.defaultValue.style;
}

[[nodiscard]] int TableBorder(
		bool bordered,
		const style::Markdown &markdown) {
	return bordered ? markdown.table.border : 0;
}

[[nodiscard]] TableCellLayoutData InitializeTableCellLayout(
		const PreparedTableCell &prepared,
		const std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown) {
	auto result = TableCellLayoutData();
	const auto &textStyle = TableCellTextStyle(prepared, markdown);
	result.cell.header = prepared.header;
	result.cell.verticalAlignment = prepared.verticalAlignment;
	result.cell.align = CellAlign(prepared.alignment);
	result.cell.column = std::max(prepared.column, 0);
	result.cell.colspan = std::max(prepared.colspan, 1);
	result.cell.rowspan = std::max(prepared.rowspan, 1);
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

[[nodiscard]] int TableSpanWidth(
		const std::vector<int> &columnWidths,
		int column,
		int colspan,
		int border) {
	const auto from = std::clamp(column, 0, int(columnWidths.size()));
	const auto to = std::clamp(column + colspan, 0, int(columnWidths.size()));
	if (from >= to) {
		return 0;
	}
	auto result = 0;
	for (auto current = from; current != to; ++current) {
		result += columnWidths[current];
	}
	return result + std::max(to - from - 1, 0) * border;
}

[[nodiscard]] int TableSpanHeight(
		const std::vector<int> &rowHeights,
		int row,
		int rowspan,
		int border) {
	const auto from = std::clamp(row, 0, int(rowHeights.size()));
	const auto to = std::clamp(row + rowspan, 0, int(rowHeights.size()));
	if (from >= to) {
		return 0;
	}
	auto result = 0;
	for (auto current = from; current != to; ++current) {
		result += rowHeights[current];
	}
	return result + std::max(to - from - 1, 0) * border;
}

void DistributeSizeDeficits(
		std::vector<int> *sizes,
		std::vector<int> deficits,
		int *extra) {
	auto remaining = 0;
	for (const auto deficit : deficits) {
		remaining += std::max(deficit, 0);
	}
	while (*extra > 0 && remaining > 0) {
		auto active = 0;
		for (const auto deficit : deficits) {
			if (deficit > 0) {
				++active;
			}
		}
		if (!active) {
			break;
		}
		const auto step = std::max(*extra / active, 1);
		for (auto i = 0, count = int(deficits.size()); i != count && *extra > 0; ++i) {
			if (deficits[i] <= 0) {
				continue;
			}
			const auto delta = std::min({ deficits[i], step, *extra });
			(*sizes)[i] += delta;
			deficits[i] -= delta;
			remaining -= delta;
			*extra -= delta;
		}
	}
}

void DistributeSpanDelta(
		std::vector<int> *sizes,
		int from,
		int to,
		int delta) {
	from = std::clamp(from, 0, int(sizes->size()));
	to = std::clamp(to, 0, int(sizes->size()));
	while (delta > 0 && from < to) {
		const auto active = to - from;
		const auto step = std::max(delta / active, 1);
		for (auto i = from; i != to && delta > 0; ++i) {
			const auto current = std::min(step, delta);
			(*sizes)[i] += current;
			delta -= current;
		}
	}
}

struct TableSpannedCellLayout {
	int row = 0;
	const TableCellLayoutData *cell = nullptr;
};

[[nodiscard]] std::vector<int> ComputeTableColumnWidths(
		const std::vector<TableRowLayoutData> &rows,
		int columnCount,
		int width,
		const style::Markdown &markdown,
		bool bordered,
		bool *overflowed) {
	const auto &padding = markdown.table.cellPadding;
	const auto border = TableBorder(bordered, markdown);
	const auto minimum = markdown.table.minColumnWidth;
	auto result = std::vector<int>(std::max(columnCount, 0), minimum);
	auto singleColumnDeficits = std::vector<int>(std::max(columnCount, 0), 0);
	auto spannedCells = std::vector<TableSpannedCellLayout>();
	for (auto row = 0, rowCount = int(rows.size()); row != rowCount; ++row) {
		for (const auto &cell : rows[row].cells) {
			const auto from = std::clamp(cell.cell.column, 0, columnCount);
			const auto to = std::clamp(
				cell.cell.column + cell.cell.colspan,
				0,
				columnCount);
			if (from >= to) {
				continue;
			}
			const auto preferredWidth = cell.preferredWidth
				+ padding.left()
				+ padding.right();
			if ((to - from) == 1) {
				singleColumnDeficits[from] = std::max(
					singleColumnDeficits[from],
					preferredWidth - minimum);
			} else {
				spannedCells.push_back({ row, &cell });
			}
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
	DistributeSizeDeficits(&result, std::move(singleColumnDeficits), &extra);
	std::sort(
		spannedCells.begin(),
		spannedCells.end(),
		[](const TableSpannedCellLayout &a, const TableSpannedCellLayout &b) {
			const auto aSpan = a.cell ? a.cell->cell.colspan : 0;
			const auto bSpan = b.cell ? b.cell->cell.colspan : 0;
			return (aSpan < bSpan)
				|| ((aSpan == bSpan) && (a.row < b.row))
				|| ((aSpan == bSpan)
					&& (a.row == b.row)
					&& a.cell
					&& b.cell
					&& (a.cell->cell.column < b.cell->cell.column));
		});
	for (const auto &spanned : spannedCells) {
		if (!spanned.cell || extra <= 0) {
			break;
		}
		const auto from = std::clamp(spanned.cell->cell.column, 0, columnCount);
		const auto to = std::clamp(
			spanned.cell->cell.column + spanned.cell->cell.colspan,
			0,
			columnCount);
		if (from >= to) {
			continue;
		}
		const auto preferredWidth = spanned.cell->preferredWidth
			+ padding.left()
			+ padding.right();
		const auto currentWidth = TableSpanWidth(
			result,
			from,
			to - from,
			border);
		const auto delta = std::min(
			std::max(preferredWidth - currentWidth, 0),
			extra);
		DistributeSpanDelta(&result, from, to, delta);
		extra -= delta;
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

void SetPlainTextLeaf(
		Ui::Text::String *leaf,
		const style::TextStyle &textStyle,
		const QString &text,
		int width) {
	*leaf = Ui::Text::String(TextMinResizeWidth(width));
	leaf->setMarkedText(
		textStyle,
		TextWithEntities::Simple(text),
		kIvMarkedTextOptions);
}

[[nodiscard]] int LeafHeight(
		const Ui::Text::String &leaf,
		const style::TextStyle &textStyle,
		int width) {
	return std::max(
		leaf.countHeight(width, true),
		TextLineHeight(textStyle));
}

[[nodiscard]] int MediaHeightForWidth(
		int width,
		int aspectWidth,
		int aspectHeight) {
	aspectWidth = std::max(aspectWidth, 1);
	aspectHeight = std::max(aspectHeight, 1);
	return std::max(
		int((int64(width) * aspectHeight + aspectWidth - 1) / aspectWidth),
		1);
}

void LayoutMediaCaption(
		LaidOutBlock *block,
		const TextWithEntities &text,
		const std::vector<PreparedLink> &links,
		const std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::TextStyle &textStyle,
		int left,
		int top,
		int width) {
	block->textWidth = std::max(width, 1);
	SetTextLeaf(
		&block->leaf,
		textStyle,
		text,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		block->textWidth);
	BindLinks(&block->leaf, links);
	block->textRect = QRect(
		left,
		top,
		block->textWidth,
		std::max(
			block->leaf.countHeight(block->textWidth, true),
			TextLineHeight(textStyle)));
}

void LayoutMediaCaption(
		LaidOutBlock *block,
		const PreparedBlock &prepared,
		const std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		int skip,
		int *bottom) {
	if (prepared.text.text.isEmpty()) {
		return;
	}
	LayoutMediaCaption(
		block,
		prepared.text,
		prepared.links,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown.body,
		left,
		top + skip,
		width);
	*bottom = block->textRect.y() + block->textRect.height();
}

[[nodiscard]] QString AudioTitleText(const PreparedAudioBlockData &audio) {
	if (!audio.title.isEmpty()) {
		return audio.title;
	}
	if (!audio.fileName.isEmpty()) {
		return audio.fileName;
	}
	return tr::lng_in_dlg_audio_file(tr::now);
}

[[nodiscard]] QString AudioSubtitleText(const PreparedAudioBlockData &audio) {
	if (!audio.performer.isEmpty()) {
		return audio.performer;
	}
	if (!audio.fileName.isEmpty() && audio.fileName != AudioTitleText(audio)) {
		return audio.fileName;
	}
	return QString();
}

[[nodiscard]] QString AudioCopyText(const PreparedAudioBlockData &audio) {
	const auto title = AudioTitleText(audio);
	const auto subtitle = AudioSubtitleText(audio);
	return subtitle.isEmpty() ? title : (title + u"\n"_q + subtitle);
}

[[nodiscard]] QString ChannelSubtitleText(
		const PreparedChannelBlockData &channel) {
	return channel.username.isEmpty()
		? QString()
		: (kUsernamePrefix + channel.username);
}

[[nodiscard]] QString ChannelCopyText(const PreparedChannelBlockData &channel) {
	const auto subtitle = ChannelSubtitleText(channel);
	return subtitle.isEmpty()
		? channel.title
		: (channel.title + u"\n"_q + subtitle);
}

[[nodiscard]] QString GroupedMediaCopyText(
		const PreparedGroupedMediaBlockData &grouped) {
	auto photos = 0;
	auto videos = 0;
	for (const auto &item : grouped.items) {
		if (item.media.kind == PreparedMediaItemKind::Photo) {
			++photos;
		} else {
			++videos;
		}
	}
	if (photos && !videos) {
		return tr::lng_media_selected_photo(tr::now, lt_count, photos);
	} else if (videos && !photos) {
		return tr::lng_media_selected_video(tr::now, lt_count, videos);
	}
	return QString();
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
		const style::TextStyle &style,
		bool breakEverywhere = true) {
	const auto lines = leaf.countLinesGeometry(textRect.width(), breakEverywhere);
	return textRect.y() + (lines.empty()
		? NominalTextBaseline(style, 0)
		: lines.front().baseline);
}

QPoint BulletMarkerCenter(
		int left,
		int baseline,
		const style::Markdown &markdown) {
	const auto &list = markdown.list;
	const auto lineHeight = TextLineHeight(markdown.body);
	const auto markerWidth = SingleDigitOrderedMarkerWidth(markdown);
	const auto nominalBaseline = NominalTextBaseline(markdown.body, 0);
	return QPoint(
		left + list.markerWidth - list.bulletLeftShift - ((markerWidth + 1) / 2),
		baseline + (lineHeight / 2) - nominalBaseline);
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
	case PreparedBlockKind::Video:
		return skips.video;
	case PreparedBlockKind::Audio:
		return skips.audio;
	case PreparedBlockKind::Map:
		return skips.map;
	case PreparedBlockKind::Channel:
		return skips.channel;
	case PreparedBlockKind::Placeholder:
		return skips.placeholder;
	case PreparedBlockKind::Details:
		return skips.paragraph;
	case PreparedBlockKind::GroupedMedia:
		return skips.groupedMedia;
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
		int width,
		LayoutContext context) {
	if (prepared.kind == PreparedBlockKind::GroupedMedia) {
		return LayoutGroupedMediaBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width,
			context);
	}
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
	block.firstLineBaseline = LeafFirstLineBaseline(
		block.leaf,
		block.textRect,
		textStyle);
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
	block.firstLineBaseline = LeafFirstLineBaseline(
		block.leaf,
		block.textRect,
		markdown.code);
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
		block.firstLineBaseline = LeafFirstLineBaseline(
			block.fallbackLeaf,
			block.textRect,
			markdown.displayMath.fallbackStyle);
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
	block.anchorId = prepared.anchorId;
	block.tableBordered = prepared.tableBordered;
	block.tableStriped = prepared.tableStriped;

	auto tableTop = top;
	if (!prepared.text.text.isEmpty()) {
		LayoutMediaCaption(
			&block,
			prepared.text,
			prepared.links,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown.body,
			left,
			top,
			width);
		block.firstLineBaseline = LeafFirstLineBaseline(
			block.leaf,
			block.textRect,
			markdown.body);
		tableTop = block.textRect.y()
			+ block.textRect.height()
			+ markdown.table.captionSkip;
	}

	const auto columnCount = prepared.tableColumnCount;
	if (!columnCount || prepared.tableRows.empty()) {
		if (!block.textRect.isEmpty()) {
			block.contentRect = block.textRect;
			block.outer = block.textRect;
		} else {
			block.outer = QRect(left, top, std::max(width, 1), 0);
		}
		return block;
	}

	auto rows = std::vector<TableRowLayoutData>();
	rows.reserve(prepared.tableRows.size());
	for (const auto &preparedRow : prepared.tableRows) {
		auto row = TableRowLayoutData();
		row.header = preparedRow.header;
		row.cells.reserve(preparedRow.cells.size());
		for (const auto &preparedCell : preparedRow.cells) {
			row.cells.push_back(InitializeTableCellLayout(
				preparedCell,
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
		block.tableBordered,
		&block.overflowed);

	const auto &padding = markdown.table.cellPadding;
	const auto border = TableBorder(block.tableBordered, markdown);
	auto tableWidth = border;
	for (const auto columnWidth : block.tableColumnWidths) {
		tableWidth += columnWidth + border;
	}
	auto columnLefts = std::vector<int>(columnCount, left + border);
	auto x = left + border;
	for (auto column = 0; column != columnCount; ++column) {
		columnLefts[column] = x;
		x += block.tableColumnWidths[column] + border;
	}

	auto rowHeights = std::vector<int>(rows.size(), 0);
	auto rowSpans = std::vector<TableSpannedCellLayout>();
	for (auto rowIndex = 0, rowCount = int(rows.size()); rowIndex != rowCount; ++rowIndex) {
		for (auto &cellData : rows[rowIndex].cells) {
			const auto spanWidth = TableSpanWidth(
				block.tableColumnWidths,
				cellData.cell.column,
				cellData.cell.colspan,
				border);
			cellData.cell.textWidth = std::max(
				spanWidth - padding.left() - padding.right(),
				1);
			const auto &textStyle = TableCellTextStyle(cellData.cell, markdown);
			cellData.textHeight = (cellData.cell.textWidth >= cellData.preferredWidth)
				? cellData.preferredHeight
				: std::max(
					cellData.cell.leaf.countHeight(
						cellData.cell.textWidth,
						true),
					TextLineHeight(textStyle));
			const auto outerHeight = cellData.textHeight
				+ padding.top()
				+ padding.bottom();
			if (cellData.cell.rowspan == 1) {
				rowHeights[rowIndex] = std::max(rowHeights[rowIndex], outerHeight);
			} else {
				rowSpans.push_back({ rowIndex, &cellData });
			}
		}
	}
	std::sort(
		rowSpans.begin(),
		rowSpans.end(),
		[](const TableSpannedCellLayout &a, const TableSpannedCellLayout &b) {
			const auto aSpan = a.cell ? a.cell->cell.rowspan : 0;
			const auto bSpan = b.cell ? b.cell->cell.rowspan : 0;
			return (aSpan < bSpan)
				|| ((aSpan == bSpan) && (a.row < b.row))
				|| ((aSpan == bSpan)
					&& (a.row == b.row)
					&& a.cell
					&& b.cell
					&& (a.cell->cell.column < b.cell->cell.column));
		});
	for (const auto &spanned : rowSpans) {
		if (!spanned.cell) {
			continue;
		}
		const auto outerHeight = spanned.cell->textHeight
			+ padding.top()
			+ padding.bottom();
		const auto currentHeight = TableSpanHeight(
			rowHeights,
			spanned.row,
			spanned.cell->cell.rowspan,
			border);
		DistributeSpanDelta(
			&rowHeights,
			spanned.row,
			spanned.row + spanned.cell->cell.rowspan,
			std::max(outerHeight - currentHeight, 0));
	}

	auto y = tableTop + border;
	block.tableRows.reserve(rows.size());
	for (auto rowIndex = 0, rowCount = int(rows.size()); rowIndex != rowCount; ++rowIndex) {
		auto &rowData = rows[rowIndex];
		const auto rowHeight = rowHeights[rowIndex];
		auto row = LaidOutTableRow();
		row.header = rowData.header;
		row.outer = QRect(
			left + border,
			y,
			std::max(tableWidth - (2 * border), 0),
			rowHeight);

		row.cells.reserve(rowData.cells.size());
		for (auto &cellData : rowData.cells) {
			auto cell = std::move(cellData.cell);
			const auto column = std::clamp(cell.column, 0, columnCount - 1);
			const auto spanWidth = TableSpanWidth(
				block.tableColumnWidths,
				cell.column,
				cell.colspan,
				border);
			const auto spanHeight = TableSpanHeight(
				rowHeights,
				rowIndex,
				cell.rowspan,
				border);
			const auto cellTop = y;
			const auto contentHeight = std::max(
				spanHeight - padding.top() - padding.bottom(),
				0);
			auto textTop = cellTop + padding.top();
			switch (cell.verticalAlignment) {
			case PreparedTableCellVerticalAlignment::Middle:
				textTop += std::max((contentHeight - cellData.textHeight) / 2, 0);
				break;
			case PreparedTableCellVerticalAlignment::Bottom:
				textTop = cellTop
					+ spanHeight
					- padding.bottom()
					- cellData.textHeight;
				break;
			case PreparedTableCellVerticalAlignment::Top:
				break;
			}
			cell.outer = QRect(
				columnLefts[column],
				cellTop,
				spanWidth,
				spanHeight);
			cell.textRect = QRect(
				columnLefts[column] + padding.left(),
				textTop,
				cell.textWidth,
				cellData.textHeight);
			row.cells.push_back(std::move(cell));
		}
		block.tableRows.push_back(std::move(row));
		y += rowHeight + border;
	}

	const auto tableHeight = std::max(y - tableTop, border);
	block.tableRect = QRect(left, tableTop, tableWidth, tableHeight);
	block.visibleTableRect = QRect(
		left,
		tableTop,
		std::min(tableWidth, std::max(width, 1)),
		tableHeight);
	block.contentRect = block.textRect.isEmpty()
		? block.visibleTableRect
		: block.visibleTableRect.isEmpty()
		? block.textRect
		: block.textRect.united(block.visibleTableRect);
	block.outer = block.contentRect;
	if (!block.textRect.isEmpty()) {
		return block;
	}
	for (const auto &row : block.tableRows) {
		for (const auto &cell : row.cells) {
			if (cell.leaf.isEmpty()) {
				continue;
			}
			block.firstLineBaseline = LeafFirstLineBaseline(
				cell.leaf,
				cell.textRect,
				TableCellTextStyle(cell, markdown));
			return block;
		}
	}
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
	block.firstLineBaseline = LeafFirstLineBaseline(
		block.labelLeaf,
		block.labelRect,
		style.labelStyle);

	auto bottom = top + mediaHeight;
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		contentLeft,
		bottom,
		contentWidth,
		style.captionSkip,
		&bottom);

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
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Photo;
	block.anchorId = prepared.anchorId;
	if (context.mediaBlockFactory) {
		block.mediaBlock = context.mediaBlockFactory(prepared);
	}
	if (block.mediaBlock) {
		block.copyText = block.mediaBlock->selectionData().copyText;
	}
	if (block.copyText.isEmpty()) {
		block.copyText = kPhotoCopyLabel;
	}

	const auto &style = markdown.photo;
	const auto blockWidth = std::max(width, 1);
	const auto mediaLeft = left + style.padding.left();
	const auto mediaTop = top + style.padding.top();
	const auto mediaWidth = std::max(
		blockWidth - style.padding.left() - style.padding.right(),
		1);
	const auto mediaHeight = block.mediaBlock
		? block.mediaBlock->resizeGetHeight(mediaWidth)
		: MediaHeightForWidth(
			mediaWidth,
			prepared.photo.width,
			prepared.photo.height);
	block.mediaRect = QRect(mediaLeft, mediaTop, mediaWidth, mediaHeight);
	block.visibleMediaRect = block.mediaRect;
	if (block.mediaBlock) {
		block.mediaBlock->setGeometry(block.mediaRect);
		block.mediaRect = block.mediaBlock->geometry();
		block.firstLineBaseline = block.mediaBlock->firstLineBaseline();
		block.visibleMediaRect = block.mediaRect;
	}

	auto bottom = block.mediaRect.y() + block.mediaRect.height()
		+ style.padding.bottom();
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		block.mediaRect.x(),
		bottom,
		std::max(block.mediaRect.width(), 1),
		style.captionSkip,
		&bottom);

	block.contentRect = QRect(
		block.mediaRect.x(),
		block.mediaRect.y(),
		block.mediaRect.width(),
		std::max(bottom - block.mediaRect.y(), block.mediaRect.height()));
	block.outer = QRect(
		left,
		top,
		blockWidth,
		std::max(bottom - top, block.mediaRect.bottom() - top + 1));
	return block;
}

LaidOutBlock LayoutVideoBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Video;
	block.anchorId = prepared.anchorId;
	if (context.mediaBlockFactory) {
		block.mediaBlock = context.mediaBlockFactory(prepared);
	}
	if (block.mediaBlock) {
		block.copyText = block.mediaBlock->selectionData().copyText;
	}
	if (block.copyText.isEmpty()) {
		block.copyText = tr::lng_in_dlg_video(tr::now);
	}

	const auto &style = markdown.photo;
	const auto blockWidth = std::max(width, 1);
	const auto mediaLeft = left + style.padding.left();
	const auto mediaTop = top + style.padding.top();
	const auto mediaWidth = std::max(
		blockWidth - style.padding.left() - style.padding.right(),
		1);
	const auto mediaHeight = block.mediaBlock
		? block.mediaBlock->resizeGetHeight(mediaWidth)
		: MediaHeightForWidth(
			mediaWidth,
			prepared.video.media.width,
			prepared.video.media.height);
	block.mediaRect = QRect(mediaLeft, mediaTop, mediaWidth, mediaHeight);
	block.visibleMediaRect = block.mediaRect;
	if (block.mediaBlock) {
		block.mediaBlock->setGeometry(block.mediaRect);
		block.mediaRect = block.mediaBlock->geometry();
		block.firstLineBaseline = block.mediaBlock->firstLineBaseline();
		block.visibleMediaRect = block.mediaRect;
	}

	auto bottom = block.mediaRect.y() + block.mediaRect.height()
		+ style.padding.bottom();
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		block.mediaRect.x(),
		bottom,
		std::max(block.mediaRect.width(), 1),
		style.captionSkip,
		&bottom);

	block.contentRect = QRect(
		block.mediaRect.x(),
		block.mediaRect.y(),
		block.mediaRect.width(),
		std::max(bottom - block.mediaRect.y(), block.mediaRect.height()));
	block.outer = QRect(
		left,
		top,
		blockWidth,
		std::max(bottom - top, block.mediaRect.bottom() - top + 1));
	return block;
}

LaidOutBlock LayoutAudioBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Audio;
	block.anchorId = prepared.anchorId;
	block.labelText = AudioTitleText(prepared.audio);
	if (context.mediaBlockFactory) {
		block.mediaBlock = context.mediaBlockFactory(prepared);
	}
	if (block.mediaBlock) {
		block.copyText = block.mediaBlock->selectionData().copyText;
	} else {
		block.copyText = AudioCopyText(prepared.audio);
	}

	const auto &card = markdown.audio;
	const auto blockWidth = std::max(width, 1);
	if (block.mediaBlock) {
		const auto cardHeight = block.mediaBlock->resizeGetHeight(blockWidth);
		block.mediaRect = QRect(left, top, blockWidth, cardHeight);
		block.visibleMediaRect = block.mediaRect;
		block.mediaBlock->setGeometry(block.mediaRect);
		block.firstLineBaseline = block.mediaBlock->firstLineBaseline();

		auto bottom = top + cardHeight;
		LayoutMediaCaption(
			&block,
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left + card.padding.left(),
			bottom,
			std::max(blockWidth - card.padding.left() - card.padding.right(), 1),
			markdown.audio.captionSkip,
			&bottom);
		block.contentRect = QRect(
			left,
			top,
			blockWidth,
			std::max(bottom - top, cardHeight));
		block.outer = block.contentRect;
		return block;
	}

	const auto &padding = card.padding;
	const auto &titleStyle = card.titleStyle;
	const auto &subtitleStyle = card.subtitleStyle;
	const auto subtitleText = AudioSubtitleText(prepared.audio);
	const auto contentLeft = left + padding.left();
	const auto contentWidth = std::max(
		blockWidth - padding.left() - padding.right(),
		1);

	block.labelWidth = contentWidth;
	SetPlainTextLeaf(
		&block.labelLeaf,
		titleStyle,
		block.labelText,
		block.labelWidth);
	const auto titleHeight = LeafHeight(
		block.labelLeaf,
		titleStyle,
		block.labelWidth);

	auto subtitleHeight = 0;
	if (!subtitleText.isEmpty()) {
		block.subtitleWidth = contentWidth;
		SetPlainTextLeaf(
			&block.subtitleLeaf,
			subtitleStyle,
			subtitleText,
			block.subtitleWidth);
		subtitleHeight = LeafHeight(
			block.subtitleLeaf,
			subtitleStyle,
			block.subtitleWidth);
	}
	const auto textSkip = subtitleHeight ? card.textSkip : 0;
	const auto textHeight = titleHeight + textSkip + subtitleHeight;
	const auto cardHeight = padding.top() + textHeight + padding.bottom();

	block.mediaRect = QRect(left, top, blockWidth, cardHeight);
	block.visibleMediaRect = block.mediaRect;
	block.labelRect = QRect(
		contentLeft,
		top + padding.top(),
		block.labelWidth,
		titleHeight);
	if (subtitleHeight) {
		block.subtitleRect = QRect(
			contentLeft,
			block.labelRect.y() + block.labelRect.height() + textSkip,
			block.subtitleWidth,
			subtitleHeight);
	}
	block.firstLineBaseline = LeafFirstLineBaseline(
		block.labelLeaf,
		block.labelRect,
		titleStyle);

	if (mediaRuntime) {
		block.documentRuntime = mediaRuntime->resolveDocument(
			prepared.audio.documentId);
	}
	if (block.documentRuntime) {
		block.activation.kind = MediaActivationKind::Document;
		block.activation.document = block.documentRuntime;
	}

	auto bottom = top + cardHeight;
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		contentLeft,
		bottom,
		contentWidth,
		card.captionSkip,
		&bottom);
	block.contentRect = QRect(left, top, blockWidth, std::max(bottom - top, cardHeight));
	block.outer = block.contentRect;
	return block;
}

LaidOutBlock LayoutMapBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Map;
	block.anchorId = prepared.anchorId;
	if (context.mediaBlockFactory) {
		block.mediaBlock = context.mediaBlockFactory(prepared);
	}
	if (block.mediaBlock) {
		block.copyText = block.mediaBlock->selectionData().copyText;
	}
	if (block.copyText.isEmpty()) {
		block.copyText = tr::lng_maps_point(tr::now);
	}

	const auto &style = markdown.photo;
	const auto blockWidth = std::max(width, 1);
	const auto mediaLeft = left + style.padding.left();
	const auto mediaTop = top + style.padding.top();
	const auto mediaWidth = std::max(
		blockWidth - style.padding.left() - style.padding.right(),
		1);
	const auto mediaHeight = block.mediaBlock
		? block.mediaBlock->resizeGetHeight(mediaWidth)
		: MediaHeightForWidth(
			mediaWidth,
			prepared.map.width,
			prepared.map.height);
	block.mediaRect = QRect(mediaLeft, mediaTop, mediaWidth, mediaHeight);
	block.visibleMediaRect = block.mediaRect;
	if (block.mediaBlock) {
		block.mediaBlock->setGeometry(block.mediaRect);
		block.mediaRect = block.mediaBlock->geometry();
		block.firstLineBaseline = block.mediaBlock->firstLineBaseline();
		block.visibleMediaRect = block.mediaRect;
	}

	auto bottom = block.mediaRect.y() + block.mediaRect.height()
		+ style.padding.bottom();
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		block.mediaRect.x(),
		bottom,
		std::max(block.mediaRect.width(), 1),
		style.captionSkip,
		&bottom);

	block.contentRect = QRect(
		block.mediaRect.x(),
		block.mediaRect.y(),
		block.mediaRect.width(),
		std::max(bottom - block.mediaRect.y(), block.mediaRect.height()));
	block.outer = QRect(
		left,
		top,
		blockWidth,
		std::max(bottom - top, block.mediaRect.bottom() - top + 1));
	return block;
}

LaidOutBlock LayoutChannelBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Channel;
	block.anchorId = prepared.anchorId;
	block.labelText = prepared.channel.title;
	if (context.mediaBlockFactory) {
		block.mediaBlock = context.mediaBlockFactory(prepared);
	}
	if (block.mediaBlock) {
		block.copyText = block.mediaBlock->selectionData().copyText;
	} else {
		block.copyText = ChannelCopyText(prepared.channel);
	}

	const auto &card = markdown.channel;
	const auto blockWidth = std::max(width, 1);
	if (block.mediaBlock) {
		const auto cardHeight = block.mediaBlock->resizeGetHeight(blockWidth);
		block.mediaRect = QRect(left, top, blockWidth, cardHeight);
		block.visibleMediaRect = block.mediaRect;
		block.mediaBlock->setGeometry(block.mediaRect);
		block.firstLineBaseline = block.mediaBlock->firstLineBaseline();

		auto bottom = top + cardHeight;
		LayoutMediaCaption(
			&block,
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left + card.padding.left(),
			bottom,
			std::max(blockWidth - card.padding.left() - card.padding.right(), 1),
			markdown.audio.captionSkip,
			&bottom);
		block.contentRect = QRect(
			left,
			top,
			blockWidth,
			std::max(bottom - top, cardHeight));
		block.outer = block.contentRect;
		return block;
	}

	const auto &padding = card.padding;
	const auto &button = card.button;
	const auto &buttonPadding = button.padding;
	const auto &titleStyle = card.titleStyle;
	const auto &subtitleStyle = card.subtitleStyle;
	const auto &actionStyle = button.textStyle;
	const auto subtitleText = ChannelSubtitleText(prepared.channel);
	const auto contentLeft = left + padding.left();
	const auto contentWidth = std::max(
		blockWidth - padding.left() - padding.right(),
		1);

	if (mediaRuntime) {
		block.channelRuntime = mediaRuntime->resolveChannel(
			prepared.channel.channelId,
			prepared.channel.username);
	}
	const auto joinVisible = block.channelRuntime
		&& block.channelRuntime->joinVisible();
	if (block.channelRuntime) {
		block.activation.kind = MediaActivationKind::OpenChannel;
		block.activation.channel = block.channelRuntime;
	}
	if (joinVisible) {
		block.actionActivation.kind = MediaActivationKind::JoinChannel;
		block.actionActivation.channel = block.channelRuntime;
	}

	auto actionTextHeight = 0;
	auto actionOuterWidth = 0;
	auto actionOuterHeight = 0;
	if (joinVisible) {
		SetPlainTextLeaf(
			&block.actionLeaf,
			actionStyle,
			tr::lng_iv_join_channel(tr::now),
			contentWidth);
		block.actionWidth = std::max(block.actionLeaf.maxWidth(), 1);
		actionTextHeight = LeafHeight(
			block.actionLeaf,
			actionStyle,
			block.actionWidth);
		actionOuterWidth = block.actionWidth
			+ buttonPadding.left()
			+ buttonPadding.right();
		actionOuterHeight = actionTextHeight
			+ buttonPadding.top()
			+ buttonPadding.bottom();
	}

	block.labelWidth = std::max(
		contentWidth
			- (joinVisible ? (actionOuterWidth + card.buttonSkip) : 0),
		1);
	SetPlainTextLeaf(
		&block.labelLeaf,
		titleStyle,
		block.labelText,
		block.labelWidth);
	const auto titleHeight = LeafHeight(
		block.labelLeaf,
		titleStyle,
		block.labelWidth);

	auto subtitleHeight = 0;
	if (!subtitleText.isEmpty()) {
		block.subtitleWidth = block.labelWidth;
		SetPlainTextLeaf(
			&block.subtitleLeaf,
			subtitleStyle,
			subtitleText,
			block.subtitleWidth);
		subtitleHeight = LeafHeight(
			block.subtitleLeaf,
			subtitleStyle,
			block.subtitleWidth);
	}
	const auto textSkip = subtitleHeight ? card.textSkip : 0;
	const auto textHeight = titleHeight + textSkip + subtitleHeight;
	const auto cardContentHeight = std::max(textHeight, actionOuterHeight);
	const auto cardHeight = padding.top() + cardContentHeight + padding.bottom();

	block.mediaRect = QRect(left, top, blockWidth, cardHeight);
	block.visibleMediaRect = block.mediaRect;

	const auto textTop = top + padding.top()
		+ std::max((cardContentHeight - textHeight) / 2, 0);
	block.labelRect = QRect(
		contentLeft,
		textTop,
		block.labelWidth,
		titleHeight);
	if (subtitleHeight) {
		block.subtitleRect = QRect(
			contentLeft,
			block.labelRect.y() + block.labelRect.height() + textSkip,
			block.subtitleWidth,
			subtitleHeight);
	}
	if (joinVisible) {
		block.actionRect = QRect(
			left + blockWidth - padding.right() - actionOuterWidth,
			top + padding.top()
				+ std::max((cardContentHeight - actionOuterHeight) / 2, 0),
			actionOuterWidth,
			actionOuterHeight);
	}
	block.firstLineBaseline = LeafFirstLineBaseline(
		block.labelLeaf,
		block.labelRect,
		titleStyle);
	block.contentRect = block.mediaRect;
	block.outer = block.mediaRect;
	return block;
}

LaidOutBlock LayoutGroupedMediaBlock(
		const PreparedBlock &prepared,
		const std::vector<PreparedFormulaSlot> *formulas,
		InlineFormulaObjectCache *inlineFormulaObjects,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::GroupedMedia;
	block.anchorId = prepared.anchorId;
	if (context.mediaBlockFactory) {
		block.mediaBlock = context.mediaBlockFactory(prepared);
	}
	if (block.mediaBlock) {
		block.copyText = block.mediaBlock->selectionData().copyText;
	}
	if (block.copyText.isEmpty()) {
		block.copyText = GroupedMediaCopyText(prepared.groupedMedia);
	}

	const auto &style = markdown.groupedMedia;
	const auto blockWidth = std::max(width, 1);
	const auto mediaLeft = left + style.padding.left();
	const auto mediaTop = top + style.padding.top();
	const auto mediaWidth = std::max(
		blockWidth - style.padding.left() - style.padding.right(),
		1);
	const auto mediaHeight = block.mediaBlock
		? block.mediaBlock->resizeGetHeight(mediaWidth)
		: std::max(st::defaultMarkdown.placeholder.minHeight, 1);
	block.mediaRect = QRect(mediaLeft, mediaTop, mediaWidth, mediaHeight);
	if (block.mediaBlock) {
		block.mediaBlock->setGeometry(block.mediaRect);
		block.mediaRect = block.mediaBlock->geometry();
		block.firstLineBaseline = block.mediaBlock->firstLineBaseline();
	}
	block.visibleMediaRect = block.mediaRect;

	auto bottom = block.mediaRect.y() + block.mediaRect.height()
		+ style.padding.bottom();
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		mediaLeft,
		bottom,
		std::max(block.mediaRect.width(), 1),
		style.captionSkip,
		&bottom);

	block.contentRect = QRect(
		mediaLeft,
		mediaTop,
		block.mediaRect.width(),
		std::max(bottom - mediaTop, block.mediaRect.height()));
	block.outer = QRect(
		left,
		top,
		blockWidth,
		std::max(bottom - top, block.mediaRect.height()));
	return block;
}

} // namespace Iv::Markdown
