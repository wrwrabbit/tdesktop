#include "iv/markdown/iv_markdown_article.h"

#include "lang/lang_keys.h"
#include "ui/click_handler.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_scale.h"
#include "ui/widgets/checkbox.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "styles/style_iv.h"

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
constexpr auto kArticleMaxWidth = 32767;

struct LaidOutTableCell {
	Ui::Text::String leaf;
	QRect outer;
	QRect textRect;
	int textWidth = 0;
	style::align align = style::al_left;
	int segmentIndex = -1;
	int tableSegmentIndex = -1;
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
	Ui::Text::String fallbackLeaf;
	QString copyText;
	QString codeLanguage;
	std::vector<LaidOutBlock> children;
	std::vector<LaidOutTableRow> tableRows;
	std::vector<int> tableColumnWidths;
	QRect outer;
	QRect textRect;
	QRect markerRect;
	QRect contentRect;
	QRect formulaRect;
	QRect tableRect;
	QRect visibleFormulaRect;
	QRect visibleTableRect;
	QPoint markerCenter;
	QString anchorId;
	int textWidth = 0;
	int markerWidth = 0;
	int headingLevel = 0;
	ListKind listKind = ListKind::Bullet;
	ListDelimiter listDelimiter = ListDelimiter::None;
	TaskState taskState = TaskState::None;
	int formulaIndex = -1;
	int orderedNumber = 0;
	style::align formulaAlign = style::al_left;
	bool collapsed = false;
	bool overflowed = false;
	int segmentIndex = -1;
	mutable QImage colorizedFormulaImage;
	mutable QColor colorizedFormulaColor;
	mutable QSize colorizedFormulaSize;
};

enum class SelectableSegmentKind {
	TextLeaf,
	CodeBlock,
	DisplayMath,
	Table,
};

struct SelectableSegment {
	SelectableSegmentKind kind = SelectableSegmentKind::TextLeaf;
	const Ui::Text::String *leaf = nullptr;
	const LaidOutBlock *block = nullptr;
	const LaidOutTableCell *cell = nullptr;
	QRect outerRect;
	QRect textRect;
	int textWidth = 0;
	style::align align = style::al_left;
	int index = -1;
	int length = 0;
	int tableSegmentIndex = -1;

	[[nodiscard]] bool isTextLeaf() const {
		return (leaf != nullptr);
	}
};

struct PaintSelectionState {
	const std::vector<SelectableSegment> *segments = nullptr;
	MarkdownArticleSelection selection;
	const MarkdownArticleSelectionEndpoints *endpoints = nullptr;

	[[nodiscard]] bool empty() const {
		return !segments || selection.empty();
	}
};

struct LayoutContext {
	int listDepth = 0;
	int quoteDepth = 0;
	bool tightList = false;
};

struct TableCellLayoutData {
	LaidOutTableCell cell;
	int preferredWidth = 0;
	int preferredHeight = 0;
};

struct TableRowLayoutData {
	std::vector<TableCellLayoutData> cells;
	bool header = false;
};

class PreparedLinkClickHandler final : public ClickHandler {
public:
	explicit PreparedLinkClickHandler(PreparedLink link)
	: _link(std::move(link)) {
	}

	void onClick(ClickContext) const override {
	}

	[[nodiscard]] const PreparedLink &link() const {
		return _link;
	}

	QString url() const override {
		return _link.target;
	}

	QString copyToClipboardText() const override {
		if (!_link.copyText.isEmpty()) {
			return _link.copyText;
		}
		switch (_link.kind) {
		case PreparedLinkKind::Anchor:
		case PreparedLinkKind::Footnote:
		case PreparedLinkKind::FootnoteBacklink:
			return _link.target.isEmpty() ? QString() : (u"#"_q + _link.target);
		case PreparedLinkKind::LocalFile:
			return _link.fragment.isEmpty()
				? _link.target
				: (_link.target + u"#"_q + _link.fragment);
		case PreparedLinkKind::External:
			return _link.target;
		case PreparedLinkKind::RejectedRelative:
		case PreparedLinkKind::ToggleDetails:
			return QString();
		}
		return QString();
	}

	QString copyToClipboardContextItemText() const override {
		switch (_link.kind) {
		case PreparedLinkKind::RejectedRelative:
		case PreparedLinkKind::ToggleDetails:
			return QString();
		case PreparedLinkKind::External:
		case PreparedLinkKind::Anchor:
		case PreparedLinkKind::Footnote:
		case PreparedLinkKind::FootnoteBacklink:
		case PreparedLinkKind::LocalFile:
			return copyToClipboardText().isEmpty()
				? QString()
				: tr::lng_context_copy_link(tr::now);
		}
		return QString();
	}

private:
	PreparedLink _link;
};

[[nodiscard]] std::optional<PreparedLink> ExtractPreparedLink(
		const ClickHandlerPtr &link) {
	if (const auto prepared = std::dynamic_pointer_cast<PreparedLinkClickHandler>(
			link)) {
		return prepared->link();
	}
	return std::nullopt;
}

void BindLinks(
		Ui::Text::String *leaf,
		const std::vector<PreparedLink> &links) {
	for (const auto &link : links) {
		leaf->setLink(
			link.index,
			std::make_shared<PreparedLinkClickHandler>(link));
	}
}

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
	return QString();
}

[[nodiscard]] int TextLineHeight(const style::TextStyle &style) {
	return std::max(style.lineHeight, style.font->height);
}

[[nodiscard]] QPoint BulletMarkerCenter(
		int left,
		int top,
		const style::Markdown &markdown) {
	const auto &list = markdown.list;
	const auto lineHeight = TextLineHeight(markdown.body);
	return QPoint(
		left + list.markerWidth - list.bulletLeftShift - (lineHeight / 2),
		top + (lineHeight / 2));
}

[[nodiscard]] QMargins BlockquotePadding(const style::QuoteStyle &style) {
	return style.padding
		+ QMargins(0, style.header + style.verticalSkip, 0, style.verticalSkip);
}

[[nodiscard]] Ui::Text::GeometryDescriptor TextGeometry(int width) {
	auto result = Ui::Text::SimpleGeometry(std::max(width, 1), 0, 0, false);
	result.breakEverywhere = true;
	return result;
}

[[nodiscard]] int LeafTextLength(const Ui::Text::String &leaf) {
	return std::clamp(
		int(leaf.toString().size()),
		0,
		int(std::numeric_limits<uint16>::max()));
}

[[nodiscard]] int BlockSkip(
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
	case PreparedBlockKind::Details:
		return skips.paragraph;
	}
	return 0;
}

[[nodiscard]] int BlockSkip(
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

[[nodiscard]] const style::TextStyle &TextStyleFor(
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

[[nodiscard]] TextWithEntities CodeBlockText(
		const QString &text,
		const QString &language) {
	auto result = tr::marked(CodeBlockDisplayText(text));
	if (!result.text.isEmpty()) {
		result.entities.push_back(EntityInText(
			EntityType::Pre,
			0,
			result.text.size(),
			language));
	}
	return result;
}

[[nodiscard]] TextForMimeData CopyTextForDisplayMath(const LaidOutBlock &block) {
	return TextForMimeData::Simple(u"$$"_q + block.copyText + u"$$"_q);
}

[[nodiscard]] TextForMimeData CopyTextForCodeBlock(
		const LaidOutBlock &block,
		TextSelection selection = AllTextSelection) {
	if (selection == AllTextSelection) {
		auto rich = tr::marked(block.copyText);
		if (!rich.text.isEmpty()) {
			rich.entities.push_back(EntityInText(
				EntityType::Pre,
				0,
				rich.text.size(),
				block.codeLanguage));
		}
		return TextForMimeData::Rich(std::move(rich));
	}
	auto from = 0;
	auto to = 0;
	auto displayPosition = 0;
	auto column = 0;
	auto found = false;
	const auto &text = block.copyText;
	for (auto i = 0, count = int(text.size()); i != count; ++i) {
		const auto ch = text[i];
		const auto width = (ch == QChar::Tabulation)
			? (kCodeTabColumns - (column % kCodeTabColumns))
			: 1;
		const auto nextDisplayPosition = displayPosition + width;
		if (selection.to <= displayPosition) {
			break;
		}
		if (selection.from < nextDisplayPosition
			&& selection.to > displayPosition) {
			if (!found) {
				from = i;
				found = true;
			}
			to = i + 1;
		}
		displayPosition = nextDisplayPosition;
		if (Ui::Text::IsNewline(ch)) {
			column = 0;
		} else {
			column += width;
		}
	}
	if (!found || to <= from) {
		return TextForMimeData();
	}
	auto rich = tr::marked(text.mid(from, to - from));
	if (!rich.text.isEmpty()) {
		rich.entities.push_back(EntityInText(
			EntityType::Pre,
			0,
			rich.text.size(),
			block.codeLanguage));
	}
	return TextForMimeData::Rich(std::move(rich));
}

[[nodiscard]] TextForMimeData CopyTextForTable(const LaidOutBlock &block) {
	auto result = TextForMimeData();
	auto firstRow = true;
	for (const auto &row : block.tableRows) {
		if (!firstRow) {
			result.append(u"\n"_q);
		}
		firstRow = false;
		auto firstCell = true;
		for (const auto &cell : row.cells) {
			if (!firstCell) {
				result.append(u"\t"_q);
			}
			firstCell = false;
			result.append(cell.leaf.toTextForMimeData());
		}
	}
	return result;
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

[[nodiscard]] PreparedFormulaSlot *PreparedFormulaFor(
		std::vector<PreparedFormulaSlot> *formulas,
		int formulaIndex) {
	if (!formulas || formulaIndex < 0 || formulaIndex >= int(formulas->size())) {
		return nullptr;
	} else if (!(*formulas)[formulaIndex].present) {
		return nullptr;
	}
	return &(*formulas)[formulaIndex];
}

[[nodiscard]] RenderedFormula MeasuredFallback(const PreparedFormulaSlot &slot) {
	auto result = RenderedFormula();
	result.logicalSize = slot.measured.logicalSize;
	result.logicalDepth = slot.measured.logicalDepth;
	result.fallbackText = slot.measured.fallbackText;
	result.error = slot.measured.error;
	result.success = false;
	result.overflow = slot.measured.overflow;
	result.tooLarge = slot.measured.tooLarge;
	return result;
}

[[nodiscard]] int RenderFormulaDevicePixelRatio(const RenderedFormula &formula) {
	const auto ratio = formula.image.devicePixelRatio();
	return (ratio > 0.) ? int(std::round(ratio)) : 0;
}

[[nodiscard]] RenderedFormula *FormulaRasterSlot(
		std::vector<RenderedFormula> *rendered,
		int formulaIndex) {
	if (!rendered || formulaIndex < 0) {
		return nullptr;
	}
	if (formulaIndex >= int(rendered->size())) {
		rendered->resize(formulaIndex + 1);
	}
	return &(*rendered)[formulaIndex];
}

[[nodiscard]] RenderedFormula EnsureFormulaRendered(
		const PreparedFormulaSlot *slot,
		RenderedFormula *rendered,
		MathRenderer *renderer,
		int devicePixelRatio) {
	if (!slot) {
		return RenderedFormula();
	}
	if (!slot->measured.success) {
		return MeasuredFallback(*slot);
	}
	if (rendered
		&& rendered->success
		&& (RenderFormulaDevicePixelRatio(*rendered) == devicePixelRatio)) {
		return *rendered;
	}
	auto ownedRenderer = std::shared_ptr<MathRenderer>();
	if (!renderer) {
		ownedRenderer = std::make_shared<MathRenderer>();
		renderer = ownedRenderer.get();
	}
	auto local = renderer->renderFormula({
		.trimmedTex = slot->trimmedTex,
		.kind = slot->kind,
		.textSize = slot->textSize,
		.renderWidthCap = slot->renderWidthCap,
		.renderHeightCap = slot->renderHeightCap,
		.devicePixelRatio = devicePixelRatio,
	});
	if (local.logicalSize.isEmpty()) {
		local.logicalSize = slot->measured.logicalSize;
		local.logicalDepth = slot->measured.logicalDepth;
		local.fallbackText = slot->measured.fallbackText;
		local.error = slot->measured.error;
		local.overflow = slot->measured.overflow;
		local.tooLarge = slot->measured.tooLarge;
	}
	if (rendered) {
		*rendered = std::move(local);
		return rendered->success ? *rendered : MeasuredFallback(*slot);
	}
	return local.success ? local : MeasuredFallback(*slot);
}

[[nodiscard]] QString InlineFormulaFallbackText(
		const PreparedInlineObject &prepared,
		const PreparedFormulaSlot *formula) {
	if (!prepared.copySource.isEmpty()) {
		return prepared.copySource;
	} else if (formula && !formula->measured.fallbackText.isEmpty()) {
		return formula->measured.fallbackText;
	} else if (formula && !formula->trimmedTex.isEmpty()) {
		return formula->trimmedTex;
	}
	return u"[math]"_q;
}

struct InlineFormulaMetrics {
	int ascent = 0;
	int descent = 0;
};

[[nodiscard]] InlineFormulaMetrics InlineFormulaMetricsFromMeasured(
		const MeasuredFormula &formula) {
	const auto height = std::max(formula.logicalSize.height(), 0);
	const auto descent = std::clamp(formula.logicalDepth, 0, height);
	return {
		.ascent = height - descent,
		.descent = descent,
	};
}

[[nodiscard]] std::vector<Ui::Text::InlineObjectPlacement> InlineFormulaPlacements(
		const std::vector<PreparedInlineObject> &preparedInlineObjects,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		const style::TextStyle &textStyle) {
	auto result = std::vector<Ui::Text::InlineObjectPlacement>();
	result.reserve(preparedInlineObjects.size());
	for (const auto &prepared : preparedInlineObjects) {
		const auto formula = PreparedFormulaFor(formulas, prepared.formulaIndex);
		auto placement = Ui::Text::InlineObjectPlacement();
		placement.position = prepared.position;
		placement.object.copySource = prepared.copySource;
		placement.object.fallbackText = InlineFormulaFallbackText(
			prepared,
			formula);
		if (formula && formula->measured.success) {
			const auto metrics = InlineFormulaMetricsFromMeasured(
				formula->measured);
			placement.object.imageProvider = [
				formulas,
				renderedFormulas,
				renderer,
				formulaIndex = prepared.formulaIndex
			](int devicePixelRatio) {
				const auto rendered = EnsureFormulaRendered(
					PreparedFormulaFor(formulas, formulaIndex),
					FormulaRasterSlot(renderedFormulas, formulaIndex),
					renderer,
					devicePixelRatio);
				return rendered.success ? rendered.image : QImage();
			};
			placement.object.colorizeToTextColor = true;
			placement.object.width = std::max(
				formula->measured.logicalSize.width(),
				1);
			placement.object.ascent = metrics.ascent;
			placement.object.descent = metrics.descent;
			placement.object.align
				= Ui::Text::InlineObjectVerticalAlign::AscentDescent;
		} else {
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
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer) {
	auto context = Ui::Text::MarkedContext();
	auto placements = InlineFormulaPlacements(
		inlineObjects,
		formulas,
		renderedFormulas,
		renderer,
		textStyle);
	context.inlineObjects = Ui::Text::InlineObjectPlacements(
		placements.data(),
		placements.size());
	leaf->setMarkedText(textStyle, text, kIvMarkedTextOptions, context);
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
		const style::Markdown &markdown) {
	if (header) {
		return markdown.table.headerStyle;
	}
	return markdown.body;
}

[[nodiscard]] TableCellLayoutData InitializeTableCellLayout(
		const PreparedTableCell &prepared,
		bool header,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		const style::Markdown &markdown) {
	auto result = TableCellLayoutData();
	const auto &textStyle = TableCellTextStyle(header, markdown);
	result.cell.align = CellAlign(prepared.alignment);
	SetTextLeaf(
		&result.cell.leaf,
		textStyle,
		prepared.text,
		prepared.inlineObjects,
		formulas,
		renderedFormulas,
		renderer);
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
	const auto border = markdown.table.border;
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

[[nodiscard]] int BlockBottom(const LaidOutBlock &block) {
	return block.outer.y() + block.outer.height();
}

[[nodiscard]] int BlockMaxRight(const std::vector<LaidOutBlock> &blocks) {
	auto result = 0;
	for (const auto &block : blocks) {
		result = std::max(result, block.outer.right() + 1);
		result = std::max(result, BlockMaxRight(block.children));
	}
	return result;
}

[[nodiscard]] LaidOutBlock LayoutFlowBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
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
		prepared.inlineObjects,
		formulas,
		renderedFormulas,
		renderer);
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
		const style::Markdown &markdown,
		int left,
		int top,
		int width) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::CodeBlock;
	block.copyText = prepared.text.text;
	block.codeLanguage = prepared.codeLanguage;
	block.leaf.setMarkedText(
		markdown.code,
		CodeBlockText(prepared.text.text, prepared.codeLanguage),
		kIvMarkedTextOptions);
	block.textWidth = std::max(width, 1);
	const auto height = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(markdown.code));
	block.textRect = QRect(left, top, block.textWidth, height);
	block.outer = block.textRect;
	return block;
}

[[nodiscard]] LaidOutBlock LayoutRuleBlock(
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

[[nodiscard]] LaidOutBlock LayoutDisplayMathBlock(
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
		block.fallbackLeaf.setMarkedText(
			markdown.displayMath.fallbackStyle,
			TextWithEntities::Simple(fallbackText),
			kIvMarkedTextOptions);
		block.textWidth = std::max(contentWidth - fallbackPaddingWidth, 1);
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

[[nodiscard]] LaidOutBlock LayoutTableBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
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
				renderedFormulas,
				renderer,
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
	const auto border = markdown.table.border;
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

[[nodiscard]] LaidOutBlock LayoutBlock(
	const PreparedBlock &prepared,
	std::vector<PreparedFormulaSlot> *formulas,
	std::vector<RenderedFormula> *renderedFormulas,
	MathRenderer *renderer,
	const style::Markdown &markdown,
	int left,
	int top,
	int width,
	LayoutContext context);

[[nodiscard]] int LayoutBlocks(
		const std::vector<PreparedBlock> &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
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

[[nodiscard]] LaidOutBlock LayoutListItemBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
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
			kIvMarkedTextOptions);
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
		&block.children,
		markdown,
		bodyLeft,
		top,
		bodyWidth,
		childContext);
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
		block.markerRect = QRect(
			markerLeft,
			top,
			markerTextWidth,
			markerTextHeight);
	} else {
		block.markerCenter = BulletMarkerCenter(left, top, markdown);
	}

	block.contentRect = QRect(bodyLeft, top, bodyWidth, rowHeight);
	block.outer = QRect(left, top, std::max(width, 1), rowHeight);
	return block;
}

[[nodiscard]] LaidOutBlock LayoutListBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
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
	return block;
}

[[nodiscard]] LaidOutBlock LayoutQuoteBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
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
	return block;
}

[[nodiscard]] LaidOutBlock LayoutDetailsBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		const style::Markdown &markdown,
		int left,
		int top,
		int width,
		LayoutContext context) {
	auto block = LaidOutBlock();
	block.kind = PreparedBlockKind::Details;
	block.anchorId = prepared.anchorId;
	block.collapsed = prepared.collapsed;
	block.textWidth = std::max(width, 1);

	SetTextLeaf(
		&block.leaf,
		markdown.body,
		prepared.text,
		prepared.inlineObjects,
		formulas,
		renderedFormulas,
		renderer);
	BindLinks(&block.leaf, prepared.links);

	const auto summaryHeight = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(markdown.body));
	block.textRect = QRect(left, top, block.textWidth, summaryHeight);

	auto bottom = top + summaryHeight;
	if (!prepared.collapsed && !prepared.children.empty()) {
		const auto childLeft = left + markdown.list.continuationIndent;
		const auto childWidth = std::max(
			width - markdown.list.continuationIndent,
			1);
		const auto childTop = bottom + markdown.list.markerSkip;
		bottom = LayoutBlocks(
			prepared.children,
			formulas,
			renderedFormulas,
			renderer,
			&block.children,
			markdown,
			childLeft,
			childTop,
			childWidth,
			context);
	}
	block.outer = QRect(
		left,
		top,
		std::max(width, 1),
		std::max(bottom - top, summaryHeight));
	block.contentRect = block.textRect;
	return block;
}

[[nodiscard]] LaidOutBlock LayoutBlock(
		const PreparedBlock &prepared,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
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
			renderedFormulas,
			renderer,
			markdown,
			left,
			top,
			width);
	case PreparedBlockKind::CodeBlock:
		return LayoutCodeBlock(prepared, markdown, left, top, width);
	case PreparedBlockKind::Rule:
		return LayoutRuleBlock(markdown, left, top, width);
	case PreparedBlockKind::List:
		return LayoutListBlock(
			prepared,
			formulas,
			renderedFormulas,
			renderer,
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
			renderedFormulas,
			renderer,
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
			markdown,
			left,
			top,
			width,
			context);
	}
	return LayoutFlowBlock(
		prepared,
		formulas,
		renderedFormulas,
		renderer,
		markdown,
		left,
		top,
		width);
}

[[nodiscard]] int AddSelectableSegment(
		std::vector<SelectableSegment> *segments,
		SelectableSegment segment) {
	segment.index = int(segments->size());
	segment.length = std::max(segment.length, 0);
	segments->push_back(std::move(segment));
	return segment.index;
}

void CollectSelectableSegments(
		std::vector<LaidOutBlock> *blocks,
		std::vector<SelectableSegment> *segments) {
	if (!blocks) {
		return;
	}
	for (auto &block : *blocks) {
		block.segmentIndex = -1;
		switch (block.kind) {
		case PreparedBlockKind::Paragraph:
		case PreparedBlockKind::Heading:
		case PreparedBlockKind::Details: {
			auto segment = SelectableSegment();
			segment.kind = SelectableSegmentKind::TextLeaf;
			segment.leaf = &block.leaf;
			segment.block = &block;
			segment.outerRect = block.textRect;
			segment.textRect = block.textRect;
			segment.textWidth = block.textWidth;
			segment.length = LeafTextLength(block.leaf);
			block.segmentIndex = AddSelectableSegment(
				segments,
				std::move(segment));
		} break;
		case PreparedBlockKind::CodeBlock: {
			auto segment = SelectableSegment();
			segment.kind = SelectableSegmentKind::CodeBlock;
			segment.leaf = &block.leaf;
			segment.block = &block;
			segment.outerRect = block.outer;
			segment.textRect = block.textRect;
			segment.textWidth = block.textWidth;
			segment.length = LeafTextLength(block.leaf);
			block.segmentIndex = AddSelectableSegment(
				segments,
				std::move(segment));
		} break;
		case PreparedBlockKind::DisplayMath: {
			auto segment = SelectableSegment();
			segment.kind = SelectableSegmentKind::DisplayMath;
			segment.block = &block;
			segment.outerRect = block.visibleFormulaRect;
			segment.length = 1;
			block.segmentIndex = AddSelectableSegment(
				segments,
				std::move(segment));
		} break;
		case PreparedBlockKind::Table: {
			auto segment = SelectableSegment();
			segment.kind = SelectableSegmentKind::Table;
			segment.block = &block;
			segment.outerRect = block.visibleTableRect;
			segment.length = 1;
			block.segmentIndex = AddSelectableSegment(
				segments,
				std::move(segment));
			for (auto &row : block.tableRows) {
				for (auto &cell : row.cells) {
					auto cellSegment = SelectableSegment();
					cellSegment.kind = SelectableSegmentKind::TextLeaf;
					cellSegment.leaf = &cell.leaf;
					cellSegment.block = &block;
					cellSegment.cell = &cell;
					cellSegment.outerRect = cell.outer;
					cellSegment.textRect = cell.textRect;
					cellSegment.textWidth = cell.textWidth;
					cellSegment.align = cell.align;
					cellSegment.length = LeafTextLength(cell.leaf);
					cellSegment.tableSegmentIndex = block.segmentIndex;
					cell.tableSegmentIndex = block.segmentIndex;
					cell.segmentIndex = AddSelectableSegment(
						segments,
						std::move(cellSegment));
				}
			}
		} break;
		case PreparedBlockKind::List:
		case PreparedBlockKind::ListItem:
		case PreparedBlockKind::Quote:
		case PreparedBlockKind::Rule:
			break;
		}
		CollectSelectableSegments(&block.children, segments);
	}
}

void CollectAnchors(
		const std::vector<LaidOutBlock> &blocks,
		std::vector<std::pair<QString, int>> *anchors) {
	if (!anchors) {
		return;
	}
	for (const auto &block : blocks) {
		if (!block.anchorId.isEmpty()) {
			anchors->push_back({ block.anchorId, block.outer.top() });
		}
		CollectAnchors(block.children, anchors);
	}
}

[[nodiscard]] int CompareSelectionPositions(
		MarkdownArticleSelectionPosition a,
		MarkdownArticleSelectionPosition b) {
	if (a.segment != b.segment) {
		return (a.segment < b.segment) ? -1 : 1;
	}
	if (a.offset != b.offset) {
		return (a.offset < b.offset) ? -1 : 1;
	}
	return 0;
}

[[nodiscard]] MarkdownArticleSelection NormalizeSelection(
		MarkdownArticleSelection selection) {
	if (selection.empty()) {
		return {};
	}
	if (CompareSelectionPositions(selection.from, selection.to) > 0) {
		std::swap(selection.from, selection.to);
	}
	return selection;
}

[[nodiscard]] const SelectableSegment *FindSegment(
		const std::vector<SelectableSegment> *segments,
		int index) {
	if (!segments || index < 0 || index >= int(segments->size())) {
		return nullptr;
	}
	return &(*segments)[index];
}

[[nodiscard]] int SegmentLength(const SelectableSegment &segment) {
	return std::max(segment.length, 0);
}

[[nodiscard]] int LastTableCellSegmentIndex(
		const std::vector<SelectableSegment> *segments,
		int tableSegmentIndex) {
	auto result = tableSegmentIndex;
	if (!segments) {
		return result;
	}
	for (const auto &segment : *segments) {
		if (segment.tableSegmentIndex == tableSegmentIndex) {
			result = std::max(result, segment.index);
		}
	}
	return result;
}

[[nodiscard]] std::optional<int> SingleTableCellSelection(
		const PaintSelectionState &selectionState,
		int tableSegmentIndex) {
	if (selectionState.empty()
		|| !selectionState.endpoints
		|| tableSegmentIndex < 0) {
		return std::nullopt;
	}
	const auto normalized = NormalizeSelection(selectionState.selection);
	if (normalized.empty()) {
		return std::nullopt;
	}
	const auto lastCellSegment = LastTableCellSegmentIndex(
		selectionState.segments,
		tableSegmentIndex);
	const auto spansWholeTable = (normalized.from.segment < tableSegmentIndex)
		&& (normalized.to.segment > lastCellSegment);
	auto tableHit = false;
	auto cellSegment = -1;
	auto multipleCells = false;
	const auto consider = [&](MarkdownArticleSelectionEndpoint endpoint) {
		if (!endpoint.valid() || !endpoint.direct) {
			return;
		}
		const auto segment = FindSegment(selectionState.segments, endpoint.segment);
		if (!segment) {
			return;
		}
		if (segment->index == tableSegmentIndex) {
			tableHit = true;
			return;
		}
		if (segment->tableSegmentIndex != tableSegmentIndex) {
			return;
		}
		if (cellSegment < 0) {
			cellSegment = segment->index;
		} else if (cellSegment != segment->index) {
			multipleCells = true;
		}
	};
	consider(selectionState.endpoints->from);
	consider(selectionState.endpoints->to);
	if (tableHit || multipleCells || cellSegment < 0 || spansWholeTable) {
		return std::nullopt;
	}
	return cellSegment;
}

[[nodiscard]] std::optional<TextSelection> BaseTextSelectionForSegment(
		const SelectableSegment &segment,
		MarkdownArticleSelection selection) {
	if (selection.empty() || !segment.isTextLeaf()) {
		return std::nullopt;
	}
	selection = NormalizeSelection(selection);
	if (selection.empty()
		|| selection.from.segment > segment.index
		|| selection.to.segment < segment.index) {
		return std::nullopt;
	}
	auto from = 0;
	auto to = SegmentLength(segment);
	if (selection.from.segment == segment.index) {
		from = selection.from.offset;
	}
	if (selection.to.segment == segment.index) {
		to = selection.to.offset;
	}
	from = std::clamp(from, 0, SegmentLength(segment));
	to = std::clamp(to, 0, SegmentLength(segment));
	if (from >= to) {
		return std::nullopt;
	}
	return TextSelection(uint16(from), uint16(to));
}

[[nodiscard]] bool RangeSelectsWholeSegment(
		const SelectableSegment &segment,
		MarkdownArticleSelection selection) {
	selection = NormalizeSelection(selection);
	if (selection.empty()
		|| selection.from.segment > segment.index
		|| selection.to.segment < segment.index) {
		return false;
	}
	auto from = 0;
	auto to = SegmentLength(segment);
	if (selection.from.segment == segment.index) {
		from = selection.from.offset;
	}
	if (selection.to.segment == segment.index) {
		to = selection.to.offset;
	}
	from = std::clamp(from, 0, SegmentLength(segment));
	to = std::clamp(to, 0, SegmentLength(segment));
	return (from < to);
}

[[nodiscard]] bool TableSegmentSelected(
		const PaintSelectionState &selectionState,
		int tableSegmentIndex) {
	if (selectionState.empty() || tableSegmentIndex < 0) {
		return false;
	}
	if (SingleTableCellSelection(selectionState, tableSegmentIndex)) {
		return false;
	}
	const auto normalized = NormalizeSelection(selectionState.selection);
	if (normalized.empty()) {
		return false;
	}
	auto selectedCells = 0;
	auto selectedCellIndex = -1;
	for (const auto &segment : *selectionState.segments) {
		if (segment.tableSegmentIndex != tableSegmentIndex
			|| segment.index == tableSegmentIndex) {
			continue;
		}
		const auto textSelection = BaseTextSelectionForSegment(
			segment,
			normalized);
		if (!textSelection || textSelection->empty()) {
			continue;
		}
		if (++selectedCells == 1) {
			selectedCellIndex = segment.index;
		} else {
			return true;
		}
	}
	const auto table = FindSegment(selectionState.segments, tableSegmentIndex);
	if (!table || !RangeSelectsWholeSegment(*table, normalized)) {
		return false;
	}
	if (selectedCells != 1) {
		return true;
	}
	if (normalized.from.segment == tableSegmentIndex
		|| normalized.to.segment == tableSegmentIndex) {
		return true;
	}
	const auto lower = std::min(tableSegmentIndex, selectedCellIndex);
	const auto upper = std::max(tableSegmentIndex, selectedCellIndex);
	return (normalized.from.segment < lower)
		&& (normalized.to.segment > upper);
}

[[nodiscard]] std::optional<TextSelection> TextSelectionForSegment(
		const SelectableSegment &segment,
		const PaintSelectionState &selectionState) {
	if (selectionState.empty()) {
		return std::nullopt;
	}
	if (segment.tableSegmentIndex >= 0) {
		if (const auto singleCell = SingleTableCellSelection(
				selectionState,
				segment.tableSegmentIndex);
			singleCell && *singleCell != segment.index) {
			return std::nullopt;
		}
	}
	if (segment.tableSegmentIndex >= 0
		&& TableSegmentSelected(selectionState, segment.tableSegmentIndex)) {
		return std::nullopt;
	}
	return BaseTextSelectionForSegment(segment, selectionState.selection);
}

[[nodiscard]] std::optional<TextSelection> TextSelectionForSegmentIndex(
		const PaintSelectionState &selectionState,
		int index) {
	const auto segment = FindSegment(selectionState.segments, index);
	return segment
		? TextSelectionForSegment(*segment, selectionState)
		: std::nullopt;
}

[[nodiscard]] bool WholeSegmentSelected(
		const SelectableSegment &segment,
		const PaintSelectionState &selectionState) {
	if (selectionState.empty() || segment.isTextLeaf()) {
		return false;
	}
	if (segment.kind == SelectableSegmentKind::Table) {
		return TableSegmentSelected(selectionState, segment.index);
	}
	return RangeSelectsWholeSegment(segment, selectionState.selection);
}

[[nodiscard]] bool WholeSegmentSelected(
		const PaintSelectionState &selectionState,
		int index) {
	const auto segment = FindSegment(selectionState.segments, index);
	return segment
		? WholeSegmentSelected(*segment, selectionState)
		: false;
}

void PaintTextLeaf(
		Painter &p,
		const Ui::Text::String &leaf,
		const MarkdownArticlePaintCaches &caches,
		QRect rect,
		int width,
		QRect clip,
		style::align align = style::al_left,
		std::optional<TextSelection> selection = std::nullopt) {
	const auto availableWidth = std::max(width, 1);
	leaf.draw(p, {
		.position = rect.topLeft(),
		.availableWidth = availableWidth,
		.geometry = TextGeometry(availableWidth),
		.align = align,
		.clip = clip,
		.palette = &p.textPalette(),
		.pre = caches.pre,
		.blockquote = caches.blockquote,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = crl::now(),
		.selection = selection.value_or(TextSelection()),
	});
}

void PaintTaskMarker(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &markdown,
		int outerWidth) {
	const auto rect = block.markerRect;
	if (rect.isEmpty()) {
		return;
	}
	auto view = Ui::CheckView(
		markdown.list.taskCheck,
		block.taskState == TaskState::Checked);
	view.finishAnimating();
	view.paint(p, rect.left(), rect.top(), outerWidth);
}

void PaintBulletMarker(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &markdown) {
	const auto radius = markdown.list.bulletRadius;
	if (radius <= 0) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(markdown.list.bulletFg->c);
	p.drawEllipse(QPointF(block.markerCenter), radius, radius);
}

void PaintBlocks(
		Painter &p,
		const std::vector<LaidOutBlock> &blocks,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip);

[[nodiscard]] const QImage &ColorizedDisplayFormulaImage(
		const LaidOutBlock &block,
		const RenderedFormula &formula,
		QColor color) {
	const auto size = formula.image.size();
	if (block.colorizedFormulaImage.isNull()
		|| (block.colorizedFormulaSize != size)
		|| (block.colorizedFormulaColor != color)) {
		block.colorizedFormulaImage = style::colorizeImage(
			formula.image,
			color);
		block.colorizedFormulaColor = color;
		block.colorizedFormulaSize = size;
	}
	return block.colorizedFormulaImage;
}

void PaintTableBlock(
		Painter &p,
		const LaidOutBlock &block,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	const auto &markdown = st::defaultMarkdown;
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
			p.fillRect(cell.outer, markdown.table.headerBg->c);
		}
	}

	const auto border = markdown.table.border;
	if (border > 0 && !block.tableRect.isEmpty()) {
		const auto left = block.tableRect.x();
		const auto top = block.tableRect.y();
		const auto width = block.tableRect.width();
		const auto height = block.tableRect.height();
		const auto right = left + width - border;
		const auto bottom = top + height - border;

		p.fillRect(QRect(left, top, width, border), markdown.table.borderFg->c);
		p.fillRect(
			QRect(left, bottom, width, border),
			markdown.table.borderFg->c);
		p.fillRect(QRect(left, top, border, height), markdown.table.borderFg->c);
		p.fillRect(
			QRect(right, top, border, height),
			markdown.table.borderFg->c);

		auto separatorLeft = left + border;
		for (auto i = 0, count = int(block.tableColumnWidths.size()); i != count; ++i) {
			separatorLeft += block.tableColumnWidths[i];
			if (i + 1 != count) {
				p.fillRect(
					QRect(separatorLeft, top, border, height),
					markdown.table.borderFg->c);
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
				markdown.table.borderFg->c);
		}
	}

	p.setPen(markdown.textColor->c);
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
				caches,
				cell.textRect,
				cell.textWidth,
				tableClip,
				cell.align,
				TextSelectionForSegmentIndex(
					selectionState,
					cell.segmentIndex));
		}
	}

	if (block.segmentIndex >= 0
		&& WholeSegmentSelected(selectionState, block.segmentIndex)) {
		p.fillRect(block.visibleTableRect, p.textPalette().selectOverlay);
	}

	if (block.overflowed) {
		const auto indicatorWidth = std::min(
			std::max(markdown.table.overflowWidth, 1),
			block.visibleTableRect.width());
		p.fillRect(
			QRect(
				block.visibleTableRect.x()
					+ block.visibleTableRect.width()
					- indicatorWidth,
				block.visibleTableRect.y(),
				indicatorWidth,
				block.visibleTableRect.height()),
			markdown.table.overflowFg->c);
	}

	p.restore();
}

void PaintDisplayMathBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	const auto formulaClip = clip.intersected(block.visibleFormulaRect);
	if (formulaClip.isEmpty()) {
		return;
	}

	p.save();
	p.setClipRect(formulaClip);

	const auto &markdown = st::defaultMarkdown;
	const auto formula = PreparedFormulaFor(formulas, block.formulaIndex);
	p.setPen(markdown.textColor->c);
	const auto rendered = EnsureFormulaRendered(
		formula,
		FormulaRasterSlot(renderedFormulas, block.formulaIndex),
		renderer,
		devicePixelRatio);
	if (rendered.success) {
		p.drawImage(
			block.formulaRect.topLeft(),
			ColorizedDisplayFormulaImage(
				block,
				rendered,
				p.pen().color()));
	}
	if (!rendered.success) {
		const auto radius = markdown.displayMath.fallbackRadius;
		p.setPen(Qt::NoPen);
		p.setBrush(markdown.displayMath.fallbackBg->c);
		if (radius > 0) {
			auto hq = PainterHighQualityEnabler(p);
			p.drawRoundedRect(block.formulaRect, radius, radius);
		} else {
			p.fillRect(block.formulaRect, markdown.displayMath.fallbackBg->c);
		}
		p.setPen(markdown.textColor->c);
		PaintTextLeaf(
			p,
			block.fallbackLeaf,
			caches,
			block.textRect,
			block.textWidth,
			formulaClip);
	}

	if (block.segmentIndex >= 0
		&& WholeSegmentSelected(selectionState, block.segmentIndex)) {
		p.fillRect(block.visibleFormulaRect, p.textPalette().selectOverlay);
	}

	if (block.overflowed) {
		const auto indicatorWidth = std::min(
			std::max(markdown.displayMath.overflowWidth, 1),
			block.visibleFormulaRect.width());
		p.fillRect(
			QRect(
				block.visibleFormulaRect.x()
					+ block.visibleFormulaRect.width()
					- indicatorWidth,
				block.visibleFormulaRect.y(),
				indicatorWidth,
				block.visibleFormulaRect.height()),
			markdown.displayMath.overflowFg->c);
	}

	p.restore();
}

void PaintQuoteBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	const auto quoteClip = clip.intersected(block.outer);
	if (quoteClip.isEmpty()) {
		return;
	}

	if (caches.blockquote) {
		const auto &quoteStyle = st::defaultMarkdown.body.blockquote;
		Ui::Text::ValidateQuotePaintCache(*caches.blockquote, quoteStyle);

		p.save();
		p.setClipRect(quoteClip);
		Ui::Text::FillQuotePaint(
			p,
			block.outer,
			*caches.blockquote,
			quoteStyle);
		p.restore();
	}

	PaintBlocks(
		p,
		block.children,
		formulas,
		renderedFormulas,
		renderer,
		devicePixelRatio,
		outerWidth,
		caches,
		selectionState,
		clip.intersected(block.contentRect));
}

void PaintBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	if (!block.outer.intersects(clip)) {
		return;
	}

	const auto &markdown = st::defaultMarkdown;
	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		p.setPen(markdown.textColor->c);
		PaintTextLeaf(
			p,
			block.leaf,
			caches,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.segmentIndex));
		break;
	case PreparedBlockKind::CodeBlock:
		p.setPen(markdown.textColor->c);
		PaintTextLeaf(
			p,
			block.leaf,
			caches,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.segmentIndex));
		break;
	case PreparedBlockKind::Rule:
		p.fillRect(block.outer, markdown.rule.fg->c);
		break;
	case PreparedBlockKind::List:
		PaintBlocks(
			p,
			block.children,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::ListItem:
		if (block.taskState != TaskState::None) {
			PaintTaskMarker(p, block, markdown, outerWidth);
		} else if (block.listKind == ListKind::Ordered
			&& !block.markerRect.isEmpty()) {
			p.setPen(markdown.textColor->c);
			PaintTextLeaf(
				p,
				block.marker,
				caches,
				block.markerRect,
				block.markerWidth,
				clip);
		} else if (block.listKind == ListKind::Bullet) {
			PaintBulletMarker(p, block, markdown);
		}
		PaintBlocks(
			p,
			block.children,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::Quote:
		PaintQuoteBlock(
			p,
			block,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::DisplayMath:
		PaintDisplayMathBlock(
			p,
			block,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::Table:
		PaintTableBlock(
			p,
			block,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::Details:
		p.setPen(markdown.textColor->c);
		PaintTextLeaf(
			p,
			block.leaf,
			caches,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.segmentIndex));
		PaintBlocks(
			p,
			block.children,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			caches,
			selectionState,
			clip);
		break;
	}
}

void PaintBlocks(
		Painter &p,
		const std::vector<LaidOutBlock> &blocks,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	for (const auto &block : blocks) {
		if (block.outer.bottom() < clip.top()) {
			continue;
		} else if (block.outer.top() > clip.bottom()) {
			break;
		}
		PaintBlock(
			p,
			block,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			caches,
			selectionState,
			clip);
	}
}

[[nodiscard]] Ui::Text::StateResult TextStateAtLeaf(
		const Ui::Text::String &leaf,
		QRect rect,
		int width,
		QPoint point,
		Ui::Text::StateRequest::Flags flags,
		style::align align = style::al_left,
		bool clampToRect = false) {
	if (rect.isEmpty()) {
		return {};
	}
	if (!rect.contains(point)) {
		if (!clampToRect) {
			return {};
		}
		point.setX(std::clamp(point.x(), rect.left(), rect.right()));
		point.setY(std::clamp(point.y(), rect.top(), rect.bottom()));
	}
	auto request = Ui::Text::StateRequest();
	request.align = align;
	request.flags = flags | Ui::Text::StateRequest::Flag::BreakEverywhere;
	const auto availableWidth = std::max(width, 1);
	return leaf.getState(
		point - rect.topLeft(),
		TextGeometry(availableWidth),
		request);
}

[[nodiscard]] MarkdownArticleHitTestResult HitSegmentBoundary(
		const SelectableSegment &segment,
		int offset) {
	auto result = MarkdownArticleHitTestResult();
	result.segmentIndex = segment.index;
	result.forcedOffset = std::clamp(offset, 0, SegmentLength(segment));
	result.state.uponSymbol = true;
	result.state.afterSymbol = (result.forcedOffset > 0);
	return result;
}

[[nodiscard]] MarkdownArticleHitTestResult HitTextSegment(
		const SelectableSegment &segment,
		QPoint point,
		Ui::Text::StateRequest::Flags flags) {
	if (!segment.isTextLeaf() || !segment.outerRect.contains(point)) {
		return {};
	}
	const auto insideText = segment.textRect.contains(point);
	if (!insideText
		&& !(flags & Ui::Text::StateRequest::Flag::LookupSymbol)) {
		return {};
	}
	auto result = MarkdownArticleHitTestResult();
	result.segmentIndex = segment.index;
	result.state = TextStateAtLeaf(
		*segment.leaf,
		segment.textRect,
		segment.textWidth,
		point,
		flags,
		segment.align,
		!insideText);
	if (!insideText) {
		result.state.link = nullptr;
	}
	result.preparedLink = ExtractPreparedLink(result.state.link);
	result.direct = true;
	return result;
}

[[nodiscard]] MarkdownArticleHitTestResult HitBlockSegment(
		const SelectableSegment &segment,
		QPoint point,
		Ui::Text::StateRequest::Flags flags) {
	if (segment.isTextLeaf()
		|| !(flags & Ui::Text::StateRequest::Flag::LookupSymbol)
		|| !segment.outerRect.contains(point)) {
		return {};
	}
	const auto after = (point.y() > segment.outerRect.center().y())
		|| ((point.y() == segment.outerRect.center().y())
			&& (point.x() >= segment.outerRect.center().x()));
	auto result = HitSegmentBoundary(
		segment,
		after ? SegmentLength(segment) : 0);
	result.direct = true;
	return result;
}

[[nodiscard]] MarkdownArticleHitTestResult HitSegmentFallback(
		const std::vector<SelectableSegment> &segments,
		QPoint point) {
	if (segments.empty()) {
		return {};
	}
	for (const auto &segment : segments) {
		const auto &rect = segment.outerRect;
		if (point.y() < rect.top()) {
			return HitSegmentBoundary(segment, 0);
		}
		if (point.y() <= rect.bottom()) {
			if (point.x() < rect.left()) {
				return HitSegmentBoundary(segment, 0);
			} else if (point.x() > rect.right()) {
				return HitSegmentBoundary(
					segment,
					SegmentLength(segment));
			}
		}
	}
	return HitSegmentBoundary(
		segments.back(),
		SegmentLength(segments.back()));
}

[[nodiscard]] bool ToggleDetailsBlock(
		std::vector<PreparedBlock> *blocks,
		const QString &anchorId) {
	if (!blocks) {
		return false;
	}
	for (auto &block : *blocks) {
		if (block.kind == PreparedBlockKind::Details
			&& block.anchorId == anchorId) {
			block.collapsed = !block.collapsed;
			if (block.text.text.startsWith(u"> "_q)
				|| block.text.text.startsWith(u"v "_q)) {
				block.text.text.replace(
					0,
					2,
					block.collapsed ? u"> "_q : u"v "_q);
			}
			return true;
		}
		if (ToggleDetailsBlock(&block.children, anchorId)) {
			return true;
		}
	}
	return false;
}

void ClearColorizedFormulaImages(std::vector<LaidOutBlock> *blocks) {
	if (!blocks) {
		return;
	}
	for (auto &block : *blocks) {
		block.colorizedFormulaImage = QImage();
		block.colorizedFormulaColor = QColor();
		block.colorizedFormulaSize = QSize();
		ClearColorizedFormulaImages(&block.children);
	}
}

} // namespace

class MarkdownArticle::Impl {
public:
	explicit Impl(std::shared_ptr<MathRenderer> renderer)
	: _renderer(std::move(renderer)) {
	}

	void setRenderer(std::shared_ptr<MathRenderer> renderer) {
		_renderer = std::move(renderer);
		invalidateRasterCache();
		invalidateLayout();
	}

	void setContent(MarkdownArticleContent content) {
		_content = std::move(content);
		resetFormulaRasterCache();
		invalidateLayout();
	}

	[[nodiscard]] int maxWidth() {
		if (_content.blocks.blocks.empty()) {
			return st::defaultMarkdown.pagePadding.left()
				+ st::defaultMarkdown.pagePadding.right();
		}
		const auto &markdown = st::defaultMarkdown;
		const auto &page = markdown.pagePadding;
		auto blocks = std::vector<LaidOutBlock>();
		const auto innerWidth = std::max(
			kArticleMaxWidth - page.left() - page.right(),
			1);
		const auto layoutBottom = LayoutBlocks(
			_content.blocks.blocks,
			&_content.formulas,
			&_formulaRenders,
			_renderer.get(),
			&blocks,
			markdown,
			page.left(),
			page.top(),
			innerWidth,
			{});
		(void)layoutBottom;
		return BlockMaxRight(blocks) + page.right();
	}

	[[nodiscard]] int resizeGetHeight(int width) {
		relayout(width);
		return std::max(_height, 1);
	}

	void paint(
			Painter &p,
			QRect clip,
			MarkdownArticlePaintCaches caches,
			MarkdownArticleSelection selection,
			const MarkdownArticleSelectionEndpoints *endpoints) {
		const auto selectionState = PaintSelectionState{
			.segments = &_segments,
			.selection = selection,
			.endpoints = endpoints,
		};
		PaintBlocks(
			p,
			_blocks,
			&_content.formulas,
			&_formulaRenders,
			_renderer.get(),
			currentDevicePixelRatio(),
			std::max(_width, 1),
			caches,
			selectionState,
			clip);
	}

	[[nodiscard]] MarkdownArticleHitTestResult hitTest(
			QPoint point,
			Ui::Text::StateRequest::Flags flags) const {
		for (const auto &segment : _segments) {
			if (const auto result = HitTextSegment(segment, point, flags);
				result.valid()) {
				return result;
			}
		}
		for (const auto &segment : _segments) {
			if (const auto result = HitBlockSegment(segment, point, flags);
				result.valid()) {
				return result;
			}
		}
		if (flags & Ui::Text::StateRequest::Flag::LookupSymbol) {
			return HitSegmentFallback(_segments, point);
		}
		return {};
	}

	[[nodiscard]] int anchorTop(const QString &anchorId) const {
		for (const auto &entry : _anchors) {
			if (entry.first == anchorId) {
				return entry.second;
			}
		}
		return -1;
	}

	[[nodiscard]] bool toggleDetails(const QString &anchorId) {
		if (!ToggleDetailsBlock(&_content.blocks.blocks, anchorId)) {
			return false;
		}
		invalidateLayout();
		return true;
	}

	[[nodiscard]] bool segmentIsText(int index) const {
		const auto segment = FindSegment(&_segments, index);
		return segment && segment->isTextLeaf();
	}

	[[nodiscard]] int segmentLength(int index) const {
		const auto segment = FindSegment(&_segments, index);
		return segment ? SegmentLength(*segment) : 0;
	}

	[[nodiscard]] int selectionOffsetFromHit(
			const MarkdownArticleHitTestResult &result,
			TextSelectType selectionType) const {
		const auto segment = FindSegment(&_segments, result.segmentIndex);
		if (!segment) {
			return 0;
		}
		if (result.forcedOffset >= 0) {
			return std::clamp(result.forcedOffset, 0, SegmentLength(*segment));
		}
		auto offset = int(result.state.symbol);
		if (selectionType == TextSelectType::Letters
			&& result.state.afterSymbol) {
			++offset;
		}
		return std::clamp(offset, 0, SegmentLength(*segment));
	}

	[[nodiscard]] TextSelection adjustSelection(
			int segmentIndex,
			TextSelection selection,
			TextSelectType selectionType) const {
		const auto segment = FindSegment(&_segments, segmentIndex);
		if (!segment || !segment->isTextLeaf()) {
			return selection;
		}
		return segment->leaf->adjustSelection(selection, selectionType);
	}

	[[nodiscard]] bool selectionContains(
			MarkdownArticleSelection selection,
			const MarkdownArticleSelectionEndpoints *endpoints,
			const MarkdownArticleHitTestResult &result) const {
		const auto segment = FindSegment(&_segments, result.segmentIndex);
		if (!segment || selection.empty() || !result.valid()) {
			return false;
		}
		const auto selectionState = PaintSelectionState{
			.segments = &_segments,
			.selection = selection,
			.endpoints = endpoints,
		};
		if (segment->tableSegmentIndex >= 0
			&& TableSegmentSelected(selectionState, segment->tableSegmentIndex)) {
			return true;
		}
		if (!segment->isTextLeaf()) {
			return WholeSegmentSelected(*segment, selectionState);
		}
		const auto textSelection = TextSelectionForSegment(*segment, selectionState);
		if (!textSelection || textSelection->empty()) {
			return false;
		}
		const auto offset = selectionOffsetFromHit(result, TextSelectType::Letters);
		return (offset >= textSelection->from) && (offset < textSelection->to);
	}

	[[nodiscard]] TextForMimeData textForContext(
			const MarkdownArticleHitTestResult &result) const {
		if (!result.valid() || !result.direct) {
			return TextForMimeData();
		}
		const auto segment = FindSegment(&_segments, result.segmentIndex);
		if (!segment) {
			return TextForMimeData();
		}
		return textForSegment(*segment);
	}

	[[nodiscard]] TextForMimeData textForSelection(
			MarkdownArticleSelection selection,
			const MarkdownArticleSelectionEndpoints *endpoints) const {
		if (selection.empty()) {
			return TextForMimeData();
		}
		const auto selectionState = PaintSelectionState{
			.segments = &_segments,
			.selection = selection,
			.endpoints = endpoints,
		};
		auto pieces = std::vector<TextForMimeData>();
		for (const auto &segment : _segments) {
			if (segment.isTextLeaf()) {
				if (const auto textSelection = TextSelectionForSegment(
						segment,
						selectionState);
					textSelection && !textSelection->empty()) {
					if (auto text = textForSegment(segment, *textSelection);
						!text.empty()) {
						pieces.push_back(std::move(text));
					}
				}
				continue;
			}
			if (!WholeSegmentSelected(segment, selectionState)) {
				continue;
			}
			if (auto text = textForSegment(segment); !text.empty()) {
				pieces.push_back(std::move(text));
			}
		}
		if (pieces.empty()) {
			return TextForMimeData();
		} else if (pieces.size() == 1) {
			return std::move(pieces.front());
		}
		auto result = TextForMimeData();
		for (auto i = 0, count = int(pieces.size()); i != count; ++i) {
			if (i) {
				result.append(u"\n"_q);
			}
			result.append(std::move(pieces[i]));
		}
		return result;
	}

	void invalidatePaletteCache() {
		ClearColorizedFormulaImages(&_blocks);
	}

	void invalidateRasterCache() {
		resetFormulaRasterCache();
		ClearColorizedFormulaImages(&_blocks);
	}

private:
	[[nodiscard]] int currentDevicePixelRatio() const {
		return std::max(style::DevicePixelRatio(), 1);
	}

	void invalidateLayout() {
		_width = -1;
		_height = 0;
		_blocks.clear();
		_anchors.clear();
		_segments.clear();
	}

	void resetFormulaRasterCache() {
		_formulaRenders.clear();
		_formulaRenders.resize(_content.formulas.size());
	}

	void relayout(int width) {
		width = std::max(width, 1);
		if (_width == width) {
			return;
		}
		_width = width;
		_blocks.clear();
		_anchors.clear();
		_segments.clear();

		const auto &markdown = st::defaultMarkdown;
		const auto &page = markdown.pagePadding;
		const auto innerWidth = std::max(width - page.left() - page.right(), 1);
		const auto y = LayoutBlocks(
			_content.blocks.blocks,
			&_content.formulas,
			&_formulaRenders,
			_renderer.get(),
			&_blocks,
			markdown,
			page.left(),
			page.top(),
			innerWidth,
			{});
		_height = y + page.bottom();
		CollectAnchors(_blocks, &_anchors);
		CollectSelectableSegments(&_blocks, &_segments);
	}

	[[nodiscard]] TextForMimeData textForSegment(
			const SelectableSegment &segment,
			TextSelection selection = AllTextSelection) const {
		switch (segment.kind) {
		case SelectableSegmentKind::TextLeaf:
			return segment.leaf
				? segment.leaf->toTextForMimeData(selection)
				: TextForMimeData();
		case SelectableSegmentKind::CodeBlock:
			return segment.block
				? CopyTextForCodeBlock(*segment.block, selection)
				: TextForMimeData();
		case SelectableSegmentKind::DisplayMath:
			return segment.block
				? CopyTextForDisplayMath(*segment.block)
				: TextForMimeData();
		case SelectableSegmentKind::Table:
			return segment.block
				? CopyTextForTable(*segment.block)
				: TextForMimeData();
		}
		return TextForMimeData();
	}

	mutable MarkdownArticleContent _content;
	std::vector<RenderedFormula> _formulaRenders;
	std::shared_ptr<MathRenderer> _renderer;
	int _width = -1;
	int _height = 0;
	std::vector<LaidOutBlock> _blocks;
	std::vector<std::pair<QString, int>> _anchors;
	std::vector<SelectableSegment> _segments;
};

MarkdownArticle::MarkdownArticle(std::shared_ptr<MathRenderer> renderer)
: _impl(std::make_unique<Impl>(std::move(renderer))) {
}

MarkdownArticle::~MarkdownArticle() = default;
MarkdownArticle::MarkdownArticle(MarkdownArticle &&) noexcept = default;
MarkdownArticle &MarkdownArticle::operator=(MarkdownArticle &&) noexcept = default;

void MarkdownArticle::setRenderer(std::shared_ptr<MathRenderer> renderer) {
	_impl->setRenderer(std::move(renderer));
}

void MarkdownArticle::setContent(MarkdownArticleContent content) {
	_impl->setContent(std::move(content));
}

int MarkdownArticle::maxWidth() const {
	return const_cast<Impl*>(_impl.get())->maxWidth();
}

int MarkdownArticle::resizeGetHeight(int width) {
	return _impl->resizeGetHeight(width);
}

void MarkdownArticle::paint(
		Painter &p,
		QRect clip,
		MarkdownArticlePaintCaches caches,
		MarkdownArticleSelection selection,
		const MarkdownArticleSelectionEndpoints *endpoints) const {
	_impl->paint(
		p,
		clip,
		caches,
		selection,
		endpoints);
}

MarkdownArticleHitTestResult MarkdownArticle::hitTest(
		QPoint point,
		Ui::Text::StateRequest::Flags flags) const {
	return _impl->hitTest(point, flags);
}

int MarkdownArticle::anchorTop(const QString &anchorId) const {
	return _impl->anchorTop(anchorId);
}

bool MarkdownArticle::toggleDetails(const QString &anchorId) {
	return _impl->toggleDetails(anchorId);
}

bool MarkdownArticle::segmentIsText(int index) const {
	return _impl->segmentIsText(index);
}

int MarkdownArticle::segmentLength(int index) const {
	return _impl->segmentLength(index);
}

int MarkdownArticle::selectionOffsetFromHit(
		const MarkdownArticleHitTestResult &result,
		TextSelectType selectionType) const {
	return _impl->selectionOffsetFromHit(result, selectionType);
}

TextSelection MarkdownArticle::adjustSelection(
		int segmentIndex,
		TextSelection selection,
		TextSelectType selectionType) const {
	return _impl->adjustSelection(segmentIndex, selection, selectionType);
}

bool MarkdownArticle::selectionContains(
		MarkdownArticleSelection selection,
		const MarkdownArticleSelectionEndpoints *endpoints,
		const MarkdownArticleHitTestResult &result) const {
	return _impl->selectionContains(selection, endpoints, result);
}

TextForMimeData MarkdownArticle::textForContext(
		const MarkdownArticleHitTestResult &result) const {
	return _impl->textForContext(result);
}

TextForMimeData MarkdownArticle::textForSelection(
		MarkdownArticleSelection selection,
		const MarkdownArticleSelectionEndpoints *endpoints) const {
	return _impl->textForSelection(selection, endpoints);
}

void MarkdownArticle::invalidatePaletteCache() {
	_impl->invalidatePaletteCache();
}

void MarkdownArticle::invalidateRasterCache() {
	_impl->invalidateRasterCache();
}

} // namespace Iv::Markdown
