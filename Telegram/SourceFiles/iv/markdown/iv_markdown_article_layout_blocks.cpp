/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_article_layout_blocks.h"
#include "iv/markdown/iv_markdown_article_text.h"
#include "lang/lang_keys.h"
#include "spellcheck/spellcheck_highlight_syntax.h"
#include "ui/grouped_layout.h"

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

template <typename Runtime>
void ResolveRuntimeImages(
		const std::shared_ptr<Runtime> &runtime,
		QSize size,
		std::shared_ptr<Ui::DynamicImage> *thumbnail,
		std::shared_ptr<Ui::DynamicImage> *full) {
	if (!runtime) {
		return;
	}
	if (thumbnail) {
		*thumbnail = runtime->thumbnail(size);
	}
	if (full) {
		*full = runtime->full(size);
	}
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

[[nodiscard]] int GroupedMediaMinWidth(int width, int spacing) {
	return std::max((width - 2 * spacing) / 3, 1);
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
	block->textWidth = std::max(width, 1);
	SetTextLeaf(
		&block->leaf,
		markdown.body,
		prepared.text,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		block->textWidth);
	BindLinks(&block->leaf, prepared.links);
	const auto captionTop = top + skip;
	const auto captionHeight = std::max(
		block->leaf.countHeight(block->textWidth, true),
		TextLineHeight(markdown.body));
	block->textRect = QRect(left, captionTop, block->textWidth, captionHeight);
	*bottom = captionTop + captionHeight;
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

[[nodiscard]] QString GroupedMediaItemCopyText(PreparedMediaItemKind kind) {
	return (kind == PreparedMediaItemKind::Photo)
		? kPhotoCopyLabel
		: tr::lng_in_dlg_video(tr::now);
}

[[nodiscard]] int GroupedMediaLayoutWidth(
		const std::vector<Ui::GroupMediaLayout> &layout) {
	auto result = 0;
	for (const auto &part : layout) {
		result = std::max(
			result,
			part.geometry.x() + part.geometry.width());
	}
	return result;
}

[[nodiscard]] int GroupedMediaLayoutHeight(
		const std::vector<Ui::GroupMediaLayout> &layout) {
	auto result = 0;
	for (const auto &part : layout) {
		result = std::max(
			result,
			part.geometry.y() + part.geometry.height());
	}
	return result;
}

void ResolveGroupedMediaItemLayout(
		LaidOutGroupedMediaItem *item,
		const PreparedGroupedMediaItemData &prepared,
		const std::shared_ptr<MediaRuntime> &mediaRuntime,
		QRect rect) {
	if (!item) {
		return;
	}
	item->kind = prepared.media.kind;
	item->copyText = GroupedMediaItemCopyText(prepared.media.kind);
	item->rect = rect;
	if (prepared.media.kind == PreparedMediaItemKind::Photo) {
		if (mediaRuntime) {
			item->photoRuntime = mediaRuntime->resolvePhoto(prepared.media.id);
		}
		ResolveRuntimeImages(
			item->photoRuntime,
			rect.size(),
			&item->thumbnailImage,
			&item->fullImage);
		if (item->photoRuntime) {
			item->activation.kind = MediaActivationKind::Photo;
			item->activation.photo = item->photoRuntime;
		}
		return;
	}
	if (mediaRuntime) {
		item->documentRuntime = mediaRuntime->resolveDocument(prepared.media.id);
	}
	ResolveRuntimeImages(
		item->documentRuntime,
		rect.size(),
		&item->thumbnailImage,
		&item->fullImage);
	if (item->documentRuntime) {
		item->activation.kind = MediaActivationKind::Document;
		item->activation.document = item->documentRuntime;
	}
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
		int width) {
	if (prepared.kind == PreparedBlockKind::GroupedMedia) {
		return LayoutGroupedMediaBlock(
			prepared,
			formulas,
			inlineFormulaObjects,
			mediaRuntime,
			markdown,
			left,
			top,
			width);
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
	for (const auto &row : block.tableRows) {
		const auto &textStyle = TableCellTextStyle(row.header, markdown);
		for (const auto &cell : row.cells) {
			if (cell.leaf.isEmpty()) {
				continue;
			}
			block.firstLineBaseline = LeafFirstLineBaseline(
				cell.leaf,
				cell.textRect,
				textStyle);
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
	const auto mediaHeight = MediaHeightForWidth(
		mediaWidth,
		prepared.photo.width,
		prepared.photo.height);
	block.mediaRect = QRect(mediaLeft, mediaTop, mediaWidth, mediaHeight);
	block.visibleMediaRect = block.mediaRect;

	if (mediaRuntime) {
		block.photoRuntime = mediaRuntime->resolvePhoto(prepared.photo.photoId);
	}
	ResolveRuntimeImages(
		block.photoRuntime,
		QSize(mediaWidth, mediaHeight),
		&block.thumbnailImage,
		&block.fullImage);
	if (!prepared.photo.urlOverride.isEmpty()) {
		block.activation.kind = MediaActivationKind::ExternalUrl;
		block.activation.url = prepared.photo.urlOverride;
	} else if (prepared.photo.viewerOpen && block.photoRuntime) {
		block.activation.kind = MediaActivationKind::Photo;
		block.activation.photo = block.photoRuntime;
	}

	auto bottom = mediaTop + mediaHeight + style.padding.bottom();
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		mediaLeft,
		bottom,
		mediaWidth,
		style.captionSkip,
		&bottom);

	block.contentRect = QRect(
		mediaLeft,
		mediaTop,
		mediaWidth,
		std::max(bottom - mediaTop, mediaHeight));
	block.outer = QRect(left, top, blockWidth, std::max(bottom - top, mediaHeight));
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
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Video;
	block.anchorId = prepared.anchorId;
	block.copyText = tr::lng_in_dlg_video(tr::now);

	const auto &style = markdown.photo;
	const auto blockWidth = std::max(width, 1);
	const auto mediaLeft = left + style.padding.left();
	const auto mediaTop = top + style.padding.top();
	const auto mediaWidth = std::max(
		blockWidth - style.padding.left() - style.padding.right(),
		1);
	const auto mediaHeight = MediaHeightForWidth(
		mediaWidth,
		prepared.video.media.width,
		prepared.video.media.height);
	block.mediaRect = QRect(mediaLeft, mediaTop, mediaWidth, mediaHeight);
	block.visibleMediaRect = block.mediaRect;

	if (mediaRuntime) {
		block.documentRuntime = mediaRuntime->resolveDocument(
			prepared.video.media.id);
	}
	ResolveRuntimeImages(
		block.documentRuntime,
		QSize(mediaWidth, mediaHeight),
		&block.thumbnailImage,
		&block.fullImage);
	if (block.documentRuntime) {
		block.activation.kind = MediaActivationKind::Document;
		block.activation.document = block.documentRuntime;
	}

	auto bottom = mediaTop + mediaHeight + style.padding.bottom();
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		mediaLeft,
		bottom,
		mediaWidth,
		style.captionSkip,
		&bottom);

	block.contentRect = QRect(
		mediaLeft,
		mediaTop,
		mediaWidth,
		std::max(bottom - mediaTop, mediaHeight));
	block.outer = QRect(left, top, blockWidth, std::max(bottom - top, mediaHeight));
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
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Audio;
	block.anchorId = prepared.anchorId;
	block.labelText = AudioTitleText(prepared.audio);
	block.copyText = AudioCopyText(prepared.audio);

	const auto &card = markdown.audio;
	const auto &padding = card.padding;
	const auto &titleStyle = card.titleStyle;
	const auto &subtitleStyle = card.subtitleStyle;
	const auto subtitleText = AudioSubtitleText(prepared.audio);
	const auto blockWidth = std::max(width, 1);
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
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Map;
	block.anchorId = prepared.anchorId;
	block.copyText = tr::lng_maps_point(tr::now);

	const auto &style = markdown.photo;
	const auto blockWidth = std::max(width, 1);
	const auto mediaLeft = left + style.padding.left();
	const auto mediaTop = top + style.padding.top();
	const auto mediaWidth = std::max(
		blockWidth - style.padding.left() - style.padding.right(),
		1);
	const auto mediaHeight = MediaHeightForWidth(
		mediaWidth,
		prepared.map.width,
		prepared.map.height);
	block.mediaRect = QRect(mediaLeft, mediaTop, mediaWidth, mediaHeight);
	block.visibleMediaRect = block.mediaRect;

	if (mediaRuntime) {
		block.mapRuntime = mediaRuntime->resolveMap(
			prepared.map.latitude,
			prepared.map.longitude,
			prepared.map.accessHash,
			QSize(mediaWidth, mediaHeight),
			prepared.map.zoom);
	}
	ResolveRuntimeImages(
		block.mapRuntime,
		QSize(mediaWidth, mediaHeight),
		&block.thumbnailImage,
		&block.fullImage);
	if (!prepared.map.url.isEmpty()) {
		block.activation.kind = MediaActivationKind::ExternalUrl;
		block.activation.url = prepared.map.url;
	}

	auto bottom = mediaTop + mediaHeight + style.padding.bottom();
	LayoutMediaCaption(
		&block,
		prepared,
		formulas,
		inlineFormulaObjects,
		mediaRuntime,
		markdown,
		mediaLeft,
		bottom,
		mediaWidth,
		style.captionSkip,
		&bottom);

	block.contentRect = QRect(
		mediaLeft,
		mediaTop,
		mediaWidth,
		std::max(bottom - mediaTop, mediaHeight));
	block.outer = QRect(left, top, blockWidth, std::max(bottom - top, mediaHeight));
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
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Channel;
	block.anchorId = prepared.anchorId;
	block.labelText = prepared.channel.title;
	block.copyText = ChannelCopyText(prepared.channel);

	const auto &card = markdown.channel;
	const auto &padding = card.padding;
	const auto &button = card.button;
	const auto &buttonPadding = button.padding;
	const auto &titleStyle = card.titleStyle;
	const auto &subtitleStyle = card.subtitleStyle;
	const auto &actionStyle = button.textStyle;
	const auto subtitleText = ChannelSubtitleText(prepared.channel);
	const auto blockWidth = std::max(width, 1);
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
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::GroupedMedia;
	block.anchorId = prepared.anchorId;
	block.copyText = GroupedMediaCopyText(prepared.groupedMedia);

	const auto &style = markdown.groupedMedia;
	const auto blockWidth = std::max(width, 1);
	const auto mediaLeft = left + style.padding.left();
	const auto mediaTop = top + style.padding.top();
	const auto mediaWidth = std::max(
		blockWidth - style.padding.left() - style.padding.right(),
		1);

	auto sizes = std::vector<QSize>();
	sizes.reserve(prepared.groupedMedia.items.size());
	for (const auto &item : prepared.groupedMedia.items) {
		sizes.push_back(QSize(
			std::max(item.media.width, 1),
			std::max(item.media.height, 1)));
	}
	auto layout = Ui::LayoutMediaGroup(
		sizes,
		mediaWidth,
		GroupedMediaMinWidth(mediaWidth, style.itemSkip),
		style.itemSkip);
	block.groupedMediaItems.reserve(layout.size());
	for (auto i = 0, count = std::min(
			int(layout.size()),
			int(prepared.groupedMedia.items.size())); i != count; ++i) {
		auto item = LaidOutGroupedMediaItem();
		ResolveGroupedMediaItemLayout(
			&item,
			prepared.groupedMedia.items[i],
			mediaRuntime,
			layout[i].geometry.translated(mediaLeft, mediaTop));
		block.groupedMediaItems.push_back(std::move(item));
	}
	const auto mediaHeight = GroupedMediaLayoutHeight(layout);
	const auto laidOutWidth = GroupedMediaLayoutWidth(layout);
	block.mediaRect = QRect(
		mediaLeft,
		mediaTop,
		std::max(laidOutWidth, 1),
		std::max(mediaHeight, 1));
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
		block.mediaRect.width(),
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
