#include "iv/markdown/iv_markdown_view.h"
#include "iv/markdown/iv_markdown_controller.h"
#include "iv/markdown/iv_markdown_prepare.h"
#include "iv/iv_delegate.h"

#include <QtCore/QUrl>

#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>

#include "base/weak_ptr.h"
#include "core/credits_amount.h"
#include "core/click_handler_types.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "ui/click_handler.h"
#include "ui/integration.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core.h"
#include "ui/text/text.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QPen>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "styles/style_boxes.h"
#include "styles/style_iv.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

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
	Ui::Text::String language;
	Ui::Text::String fallbackLeaf;
	QString copyText;
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

struct DocumentHitTestResult {
	int segmentIndex = -1;
	Ui::Text::StateResult state;
	int forcedOffset = -1;
	bool direct = false;

	[[nodiscard]] bool valid() const {
		return (segmentIndex >= 0);
	}
};

struct DocumentSelectionPosition {
	int segment = -1;
	int offset = 0;

	[[nodiscard]] bool valid() const {
		return (segment >= 0);
	}
};

inline bool operator==(
		DocumentSelectionPosition a,
		DocumentSelectionPosition b) {
	return (a.segment == b.segment) && (a.offset == b.offset);
}

inline bool operator!=(
		DocumentSelectionPosition a,
		DocumentSelectionPosition b) {
	return !(a == b);
}

struct DocumentSelection {
	DocumentSelectionPosition from;
	DocumentSelectionPosition to;

	[[nodiscard]] bool empty() const {
		return !from.valid()
			|| !to.valid()
			|| (from == to);
	}
};

struct SelectionEndpoint {
	int segment = -1;
	bool direct = false;

	[[nodiscard]] bool valid() const {
		return (segment >= 0);
	}
};

struct SelectionEndpoints {
	SelectionEndpoint from;
	SelectionEndpoint to;
};

inline bool operator==(
		DocumentSelection a,
		DocumentSelection b) {
	return (a.from == b.from) && (a.to == b.to);
}

inline bool operator!=(
		DocumentSelection a,
		DocumentSelection b) {
	return !(a == b);
}

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

[[nodiscard]] int LeafTextLength(const Ui::Text::String &leaf) {
	return std::clamp(
		int(leaf.toString().size()),
		0,
		int(std::numeric_limits<uint16>::max()));
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
	case PreparedBlockKind::Details:
		return style.paragraphSkip;
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

[[nodiscard]] TextForMimeData CopyTextForDisplayMath(const LaidOutBlock &block) {
	return TextForMimeData::Simple(u"$$"_q + block.copyText + u"$$"_q);
}

[[nodiscard]] TextForMimeData CopyTextForCodeBlock(
		const LaidOutBlock &block,
		TextSelection selection = AllTextSelection) {
	if (selection == AllTextSelection) {
		auto rich = TextWithEntities::Simple(block.copyText);
		if (!rich.text.isEmpty()) {
			rich.entities.push_back(EntityInText(
				EntityType::Code,
				0,
				rich.text.size()));
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
	auto rich = TextWithEntities::Simple(text.mid(from, to - from));
	if (!rich.text.isEmpty()) {
		rich.entities.push_back(EntityInText(
			EntityType::Code,
			0,
			rich.text.size()));
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

[[nodiscard]] QString CopyableLinkText(const PreparedLink &link) {
	if (!link.copyText.isEmpty()) {
		return link.copyText;
	}
	switch (link.kind) {
	case PreparedLinkKind::Anchor:
	case PreparedLinkKind::Footnote:
	case PreparedLinkKind::FootnoteBacklink:
		return link.target.isEmpty() ? QString() : (u"#"_q + link.target);
	case PreparedLinkKind::LocalFile:
		return link.fragment.isEmpty()
			? link.target
			: (link.target + u"#"_q + link.fragment);
	case PreparedLinkKind::External:
		return link.target;
	case PreparedLinkKind::RejectedRelative:
	case PreparedLinkKind::ToggleDetails:
		return QString();
	}
	return QString();
}

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
		return CopyableLinkText(_link);
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

void BindLinks(
		Ui::Text::String *leaf,
		const std::vector<PreparedLink> &links) {
	for (const auto &link : links) {
		leaf->setLink(
			link.index,
			std::make_shared<PreparedLinkClickHandler>(link));
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
		const MarkdownStyleSnapshot &style,
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
		style,
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

struct InlineFormulaMetrics {
	int ascent = 0;
	int descent = 0;
};

[[nodiscard]] InlineFormulaMetrics InlineFormulaMetricsFromRendered(
		const RenderedFormula &formula) {
	const auto height = std::max(formula.logicalSize.height(), 0);
	const auto descent = std::clamp(formula.logicalDepth, 0, height);
	return {
		.ascent = height - descent,
		.descent = descent,
	};
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
		placement.object.copySource = prepared.copySource;
		if (formula && formula->rendered.success) {
			const auto metrics = InlineFormulaMetricsFromRendered(
				formula->rendered);
			placement.object.image = formula->rendered.image;
			placement.object.width = std::max(
				formula->rendered.logicalSize.width(),
				1);
			placement.object.ascent = metrics.ascent;
			placement.object.descent = metrics.descent;
			placement.object.align
				= Ui::Text::InlineObjectVerticalAlign::AscentDescent;
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

[[nodiscard]] Ui::Text::InlineHtmlMetrics InlineHtmlMetricsFor(
		const MarkdownStyleSnapshot &style) {
	return {
		.subscriptScale = style.subscriptScale,
		.superscriptScale = style.superscriptScale,
		.subscriptBaselineOffset = style.subscriptBaselineOffset,
		.superscriptBaselineOffset = style.superscriptBaselineOffset,
		.markBackgroundColor = style.markBackgroundColor,
	};
}

void SetTextLeaf(
		Ui::Text::String *leaf,
		const style::TextStyle &textStyle,
		const TextWithEntities &text,
		const std::vector<PreparedInlineObject> &inlineObjects,
		const MarkdownStyleSnapshot &style,
		const std::vector<PreparedFormulaSlot> &formulas) {
	auto context = Ui::Text::MarkedContext();
	auto placements = InlineFormulaPlacements(inlineObjects, formulas, textStyle);
	context.inlineObjects = Ui::Text::InlineObjectPlacements(
		placements.data(),
		placements.size());
	context.other = InlineHtmlMetricsFor(style);
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
	block.anchorId = prepared.anchorId;
	block.headingLevel = prepared.headingLevel;
	block.textWidth = std::max(width, 1);

	const auto &textStyle = TextStyleFor(prepared, style);
	SetTextLeaf(
		&block.leaf,
		textStyle,
		prepared.text,
		prepared.inlineObjects,
		style,
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
	block.copyText = prepared.text.text;

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
	block.copyText = prepared.formulaTex;

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
	block.anchorId = prepared.anchorId;
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

[[nodiscard]] LaidOutBlock LayoutDetailsBlock(
		const PreparedBlock &prepared,
		const MarkdownStyleSnapshot &style,
		const std::vector<PreparedFormulaSlot> &formulas,
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
		style.paragraphStyle,
		prepared.text,
		prepared.inlineObjects,
		style,
		formulas);
	BindLinks(&block.leaf, prepared.links);

	const auto summaryHeight = std::max(
		block.leaf.countHeight(block.textWidth, true),
		TextLineHeight(style.paragraphStyle));
	block.textRect = QRect(left, top, block.textWidth, summaryHeight);

	auto bottom = top + summaryHeight;
	if (!prepared.collapsed && !prepared.children.empty()) {
		const auto childLeft = left + style.listContinuationIndent;
		const auto childWidth = std::max(
			width - style.listContinuationIndent,
			1);
		const auto childTop = bottom + style.listMarkerSkip;
		bottom = LayoutBlocks(
			prepared.children,
			&block.children,
			style,
			formulas,
			childLeft,
			childTop,
			childWidth,
			context);
	}
	block.outer = QRect(left, top, std::max(width, 1), std::max(bottom - top, summaryHeight));
	block.contentRect = block.textRect;
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
	case PreparedBlockKind::Details:
		return LayoutDetailsBlock(
			prepared,
			style,
			formulas,
			left,
			top,
			width,
			context);
	}
	return LayoutFlowBlock(prepared, formulas, style, left, top, width);
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

class DocumentLayout final {
public:
	void invalidate();
	void relayout(const PreparedResult &prepared, int width);

	[[nodiscard]] int height() const;
	[[nodiscard]] int anchorTop(const QString &anchorId) const;
	[[nodiscard]] const std::vector<LaidOutBlock> &blocks() const;
	[[nodiscard]] const std::vector<SelectableSegment> &segments() const;
	[[nodiscard]] const SelectableSegment *segment(int index) const;
	[[nodiscard]] DocumentHitTestResult hitTest(
		QPoint point,
		Ui::Text::StateRequest::Flags flags) const;

private:
	int _width = -1;
	int _height = 0;
	std::vector<LaidOutBlock> _blocks;
	std::vector<std::pair<QString, int>> _anchors;
	std::vector<SelectableSegment> _segments;

};

class MarkdownDocumentWidget final
	: public Ui::RpWidget
	, public ClickHandlerHost {
public:
	explicit MarkdownDocumentWidget(QWidget *parent);

	void setLinkActivationCallback(
		std::function<void(const PreparedLink &, Qt::MouseButton)> callback);
	void setPreparedResult(PreparedResult prepared);
	void setZoom(int value);
	[[nodiscard]] int anchorTop(const QString &anchorId) const;
	[[nodiscard]] bool toggleDetails(const QString &anchorId);
	[[nodiscard]] int lastRelayoutMs() const;
	int resizeGetHeight(int newWidth) override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void clickHandlerActiveChanged(const ClickHandlerPtr &, bool) override;
	void clickHandlerPressedChanged(const ClickHandlerPtr &, bool) override;

private:
	enum DragAction {
		NoDrag = 0x00,
		PrepareDrag = 0x01,
		Dragging = 0x02,
		Selecting = 0x04,
	};

	[[nodiscard]] ClickHandlerPtr linkAt(QPoint point) const;
	[[nodiscard]] DocumentHitTestResult hitTest(
		QPoint point,
		Ui::Text::StateRequest::Flags flags) const;
	[[nodiscard]] DocumentSelection selectionForCopy() const;
	[[nodiscard]] SelectionEndpoints selectionEndpointsForCopy() const;
	[[nodiscard]] bool selectionContains(
		DocumentSelection selection,
		const DocumentHitTestResult &result) const;
	[[nodiscard]] TextForMimeData textForSegment(
		const SelectableSegment &segment,
		TextSelection selection = AllTextSelection) const;
	[[nodiscard]] TextForMimeData textForContext(
		const DocumentHitTestResult &result) const;
	[[nodiscard]] int selectionOffsetFromHit(
		const DocumentHitTestResult &result) const;
	[[nodiscard]] DocumentSelection selectionFromHit(
		const DocumentHitTestResult &result) const;
	[[nodiscard]] TextForMimeData getSelectedText() const;
	void copySelectedText();

	void relayoutCurrentWidth();
	void forceRelayoutCurrentWidth();
	void updateHover(const DocumentHitTestResult &state);
	void resetSelection();
	void clearSelection();
	void dragActionStart(QPoint point, Qt::MouseButton button);
	DocumentHitTestResult dragActionUpdate(QPoint point);
	DocumentHitTestResult dragActionFinish(
		QPoint point,
		Qt::MouseButton button);
	void applyCursor(style::cursor cursor);
	[[nodiscard]] double zoomScale() const;

	PreparedResult _prepared;
	DocumentLayout _layout;
	std::optional<SnapshotTextPalette> _textPalette;
	std::function<void(const PreparedLink &, Qt::MouseButton)> _activateLink;
	DocumentSelection _selection;
	DocumentSelection _savedSelection;
	SelectionEndpoints _selectionEndpoints;
	SelectionEndpoints _savedSelectionEndpoints;
	TextSelectType _selectionType = TextSelectType::Letters;
	style::cursor _cursor = style::cur_default;
	DragAction _dragAction = NoDrag;
	QPoint _dragStartPosition;
	int _dragSegment = -1;
	int _dragSymbol = 0;
	TextSelection _dragExpandedSelection;
	int _lastRelayoutMs = 0;
	int _zoom = 100;
	base::unique_qptr<Ui::PopupMenu> _contextMenu;

};

struct PaintSelectionState {
	const std::vector<SelectableSegment> *segments = nullptr;
	DocumentSelection selection;
	const SelectionEndpoints *endpoints = nullptr;

	[[nodiscard]] bool empty() const {
		return !segments || selection.empty();
	}
};

[[nodiscard]] int CompareSelectionPositions(
		DocumentSelectionPosition a,
		DocumentSelectionPosition b) {
	if (a.segment != b.segment) {
		return (a.segment < b.segment) ? -1 : 1;
	}
	if (a.offset != b.offset) {
		return (a.offset < b.offset) ? -1 : 1;
	}
	return 0;
}

[[nodiscard]] DocumentSelection NormalizeSelection(
		DocumentSelection selection) {
	if (selection.empty()) {
		return {};
	}
	if (CompareSelectionPositions(selection.from, selection.to) > 0) {
		std::swap(selection.from, selection.to);
	}
	return selection;
}

[[nodiscard]] SelectionEndpoint MakeSelectionEndpoint(
		const DocumentHitTestResult &result) {
	return {
		.segment = result.segmentIndex,
		.direct = result.direct,
	};
}

[[nodiscard]] const SelectableSegment *FindSegment(
		const std::vector<SelectableSegment> *segments,
		int index) {
	if (!segments || index < 0 || index >= int(segments->size())) {
		return nullptr;
	}
	return &(*segments)[index];
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

[[nodiscard]] int SegmentLength(const SelectableSegment &segment) {
	return std::max(segment.length, 0);
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
	const auto consider = [&](SelectionEndpoint endpoint) {
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
		DocumentSelection selection) {
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
		DocumentSelection selection) {
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
		&& TableSegmentSelected(
			selectionState,
			segment.tableSegmentIndex)) {
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
		QRect rect,
		int width,
		QRect clip,
		style::align align = style::al_left,
		std::optional<TextSelection> selection = std::nullopt) {
	leaf.draw(p, {
		.position = rect.topLeft(),
		.availableWidth = width,
		.geometry = TextGeometry(width),
		.align = align,
		.clip = clip,
		.palette = &p.textPalette(),
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = crl::now(),
		.selection = selection.value_or(TextSelection()),
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
	const PaintSelectionState &selectionState,
	QRect clip);

void PaintTableBlock(
		Painter &p,
		const LaidOutBlock &block,
		const MarkdownStyleSnapshot &style,
		const PaintSelectionState &selectionState,
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
		const PaintSelectionState &selectionState,
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

	if (block.segmentIndex >= 0
		&& WholeSegmentSelected(selectionState, block.segmentIndex)) {
		p.fillRect(block.visibleFormulaRect, p.textPalette().selectOverlay);
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
		const PaintSelectionState &selectionState,
		QRect clip) {
	if (!block.outer.intersects(clip)) {
		return;
	}

	const auto &style = prepared.style;
	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		p.setPen(style.defaultTextColor);
		PaintTextLeaf(
			p,
			block.leaf,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.segmentIndex));
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
		PaintTextLeaf(
			p,
			block.leaf,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.segmentIndex));
	} break;
	case PreparedBlockKind::Rule:
		p.fillRect(block.outer, style.ruleColor);
		break;
	case PreparedBlockKind::List:
		PaintBlocks(p, block.children, prepared, selectionState, clip);
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
		PaintBlocks(p, block.children, prepared, selectionState, clip);
		break;
	case PreparedBlockKind::Quote:
		p.fillRect(block.borderRect, style.quoteBorderColor);
		PaintBlocks(p, block.children, prepared, selectionState, clip);
		break;
	case PreparedBlockKind::DisplayMath:
		PaintDisplayMathBlock(p, block, prepared, selectionState, clip);
		break;
	case PreparedBlockKind::Table:
		PaintTableBlock(p, block, style, selectionState, clip);
		break;
	case PreparedBlockKind::Details:
		PaintTextLeaf(
			p,
			block.leaf,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.segmentIndex));
		PaintBlocks(p, block.children, prepared, selectionState, clip);
		break;
	}
}

void PaintBlocks(
		Painter &p,
		const std::vector<LaidOutBlock> &blocks,
		const PreparedResult &prepared,
		const PaintSelectionState &selectionState,
		QRect clip) {
	for (const auto &block : blocks) {
		if (block.outer.bottom() < clip.top()) {
			continue;
		} else if (block.outer.top() > clip.bottom()) {
			break;
		}
		PaintBlock(p, block, prepared, selectionState, clip);
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
	return leaf.getState(
		point - rect.topLeft(),
		TextGeometry(width),
		request);
}

[[nodiscard]] DocumentHitTestResult HitSegmentBoundary(
		const SelectableSegment &segment,
		int offset) {
	auto result = DocumentHitTestResult();
	result.segmentIndex = segment.index;
	result.forcedOffset = std::clamp(offset, 0, SegmentLength(segment));
	result.state.uponSymbol = true;
	result.state.afterSymbol = (result.forcedOffset > 0);
	return result;
}

[[nodiscard]] DocumentHitTestResult HitTextSegment(
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
	auto result = DocumentHitTestResult();
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
	result.direct = true;
	return result;
}

[[nodiscard]] DocumentHitTestResult HitBlockSegment(
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

[[nodiscard]] DocumentHitTestResult HitSegmentFallback(
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

void DocumentLayout::relayout(
		const PreparedResult &prepared,
		int width) {
	width = std::max(width, 1);
	if (_width == width) {
		return;
	}
	_width = width;
	_blocks.clear();
	_anchors.clear();
	_segments.clear();

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
	CollectAnchors(_blocks, &_anchors);
	CollectSelectableSegments(&_blocks, &_segments);
}

void DocumentLayout::invalidate() {
	_width = -1;
	_height = 0;
	_blocks.clear();
	_anchors.clear();
	_segments.clear();
}

int DocumentLayout::height() const {
	return _height;
}

int DocumentLayout::anchorTop(const QString &anchorId) const {
	for (const auto &entry : _anchors) {
		if (entry.first == anchorId) {
			return entry.second;
		}
	}
	return -1;
}

const std::vector<LaidOutBlock> &DocumentLayout::blocks() const {
	return _blocks;
}

const std::vector<SelectableSegment> &DocumentLayout::segments() const {
	return _segments;
}

const SelectableSegment *DocumentLayout::segment(int index) const {
	return FindSegment(&_segments, index);
}

DocumentHitTestResult DocumentLayout::hitTest(
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

MarkdownDocumentWidget::MarkdownDocumentWidget(
	QWidget *parent)
: Ui::RpWidget(parent) {
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
}

void MarkdownDocumentWidget::setLinkActivationCallback(
		std::function<void(const PreparedLink &, Qt::MouseButton)> callback) {
	_activateLink = std::move(callback);
}

void MarkdownDocumentWidget::setPreparedResult(PreparedResult prepared) {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	_layout.invalidate();
	_lastRelayoutMs = 0;
	_prepared = std::move(prepared);
	_textPalette.emplace(_prepared.style.textPalette);
	resetSelection();
	forceRelayoutCurrentWidth();
}

void MarkdownDocumentWidget::setZoom(int value) {
	value = (value > 0) ? value : 100;
	if (_zoom == value) {
		return;
	}
	_zoom = value;
	clearSelection();
	forceRelayoutCurrentWidth();
}

int MarkdownDocumentWidget::anchorTop(const QString &anchorId) const {
	const auto top = _layout.anchorTop(anchorId);
	if (top < 0) {
		return -1;
	}
	return int(std::floor(top * zoomScale()));
}

bool MarkdownDocumentWidget::toggleDetails(const QString &anchorId) {
	if (!ToggleDetailsBlock(&_prepared.blocks.blocks, anchorId)) {
		return false;
	}
	clearSelection();
	_layout.invalidate();
	forceRelayoutCurrentWidth();
	return true;
}

int MarkdownDocumentWidget::lastRelayoutMs() const {
	return _lastRelayoutMs;
}

int MarkdownDocumentWidget::resizeGetHeight(int newWidth) {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	clearSelection();
	const auto scale = zoomScale();
	const auto layoutWidth = std::max(int(std::floor(newWidth / scale)), 1);
	auto timer = QElapsedTimer();
	timer.start();
	_layout.relayout(_prepared, layoutWidth);
	_lastRelayoutMs = int(timer.elapsed());
	return std::max(int(std::ceil(_layout.height() * scale)), 1);
}

void MarkdownDocumentWidget::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_textPalette) {
		p.setTextPalette(_textPalette->palette);
	}
	const auto selectionState = PaintSelectionState{
		.segments = &_layout.segments(),
		.selection = _selection,
		.endpoints = &_selectionEndpoints,
	};

	const auto scale = zoomScale();
	if (scale == 1.) {
		PaintBlocks(
			p,
			_layout.blocks(),
			_prepared,
			selectionState,
			e->rect());
		return;
	}
	const auto clip = QRect(
		int(std::floor(e->rect().x() / scale)),
		int(std::floor(e->rect().y() / scale)),
		int(std::ceil(e->rect().width() / scale)) + 1,
		int(std::ceil(e->rect().height() / scale)) + 1);
	p.save();
	p.scale(scale, scale);
	PaintBlocks(p, _layout.blocks(), _prepared, selectionState, clip);
	p.restore();
}

void MarkdownDocumentWidget::keyPressEvent(QKeyEvent *e) {
	if (e == QKeySequence::Copy && !selectionForCopy().empty()) {
		copySelectedText();
		return;
	}
	Ui::RpWidget::keyPressEvent(e);
}

void MarkdownDocumentWidget::contextMenuEvent(QContextMenuEvent *e) {
	const auto globalPoint = (e->reason() == QContextMenuEvent::Mouse)
		? e->globalPos()
		: QCursor::pos();
	const auto localPoint = (e->reason() == QContextMenuEvent::Mouse)
		? e->pos()
		: mapFromGlobal(globalPoint);
	const auto state = hitTest(
		localPoint,
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol);
	const auto selection = selectionForCopy();
	const auto uponSelection = !selection.empty()
		&& ((e->reason() != QContextMenuEvent::Mouse)
			|| selectionContains(selection, state));
	const auto contextText = uponSelection ? TextForMimeData() : textForContext(state);
	const auto link = state.direct
		? std::dynamic_pointer_cast<PreparedLinkClickHandler>(state.state.link)
		: nullptr;

	_contextMenu = base::make_unique_q<Ui::PopupMenu>(this);
	if (uponSelection) {
		_contextMenu->addAction(
			Ui::Integration::Instance().phraseContextCopySelected(),
			[=] { copySelectedText(); },
			&st::menuIconCopy);
	} else if (!contextText.empty()) {
		_contextMenu->addAction(
			tr::lng_context_copy_text(tr::now),
			[text = contextText] {
				TextUtilities::SetClipboardText(text);
			},
			&st::menuIconCopy);
	}

	if (link) {
		if (const auto label = link->copyToClipboardContextItemText();
			!label.isEmpty()) {
			_contextMenu->addAction(
				label,
				[text = link->copyToClipboardText()] {
					QGuiApplication::clipboard()->setText(text);
				},
				&st::menuIconCopy);
		}
		switch (link->link().kind) {
		case PreparedLinkKind::RejectedRelative:
		case PreparedLinkKind::ToggleDetails:
			break;
		case PreparedLinkKind::External:
		case PreparedLinkKind::Anchor:
		case PreparedLinkKind::Footnote:
		case PreparedLinkKind::FootnoteBacklink:
		case PreparedLinkKind::LocalFile:
			_contextMenu->addAction(
				tr::lng_open_link(tr::now),
				[=, prepared = link->link()] {
					if (_activateLink) {
						_activateLink(prepared, Qt::LeftButton);
					}
				},
				&st::menuIconAddress);
			break;
		}
	}

	if (_contextMenu->empty()) {
		_contextMenu = nullptr;
		return;
	}
	_contextMenu->popup(globalPoint);
	e->accept();
}

void MarkdownDocumentWidget::mouseMoveEvent(QMouseEvent *e) {
	dragActionUpdate(e->pos());
}

void MarkdownDocumentWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		dragActionStart(e->pos(), e->button());
		return;
	}
	updateHover(hitTest(
		e->pos(),
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol));
	if (e->button() == Qt::MiddleButton) {
		ClickHandler::pressed();
	}
}

void MarkdownDocumentWidget::mouseReleaseEvent(QMouseEvent *e) {
	dragActionFinish(e->pos(), e->button());
	if (!rect().contains(e->pos())) {
		ClickHandler::clearActive(this);
		applyCursor(style::cur_default);
	}
}

void MarkdownDocumentWidget::mouseDoubleClickEvent(QMouseEvent *e) {
	dragActionStart(e->pos(), e->button());
	if (_dragAction != Selecting || _selectionType != TextSelectType::Letters) {
		return;
	}
	const auto state = hitTest(
		e->pos(),
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol);
	const auto segment = _layout.segment(state.segmentIndex);
	if (!segment
		|| !segment->isTextLeaf()
		|| !state.direct
		|| !state.state.uponSymbol) {
		return;
	}
	_dragSegment = state.segmentIndex;
	_dragSymbol = std::clamp(
		int(state.state.symbol),
		0,
		SegmentLength(*segment));
	_selectionType = TextSelectType::Words;
	_selection = selectionFromHit(state);
	_savedSelection = {};
	_selectionEndpoints = {
		.from = MakeSelectionEndpoint(state),
		.to = MakeSelectionEndpoint(state),
	};
	_savedSelectionEndpoints = {};
	if (_selection.from.segment == _dragSegment
		&& _selection.to.segment == _dragSegment) {
		_dragExpandedSelection = TextSelection(
			uint16(_selection.from.offset),
			uint16(_selection.to.offset));
	}
	setFocus();
	updateHover(state);
	update();
}

void MarkdownDocumentWidget::focusOutEvent(QFocusEvent *e) {
	if (!_selection.empty()) {
		_savedSelection = _selection;
		_savedSelectionEndpoints = _selectionEndpoints;
		_selection = {};
		_selectionEndpoints = {};
		update();
	}
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	Ui::RpWidget::focusOutEvent(e);
}

void MarkdownDocumentWidget::focusInEvent(QFocusEvent *e) {
	if (!_savedSelection.empty()) {
		_selection = _savedSelection;
		_selectionEndpoints = _savedSelectionEndpoints;
		_savedSelection = {};
		_savedSelectionEndpoints = {};
		update();
	}
	Ui::RpWidget::focusInEvent(e);
}

void MarkdownDocumentWidget::leaveEventHook(QEvent *e) {
	ClickHandler::clearActive(this);
	applyCursor((_dragAction == Selecting)
		? style::cur_text
		: style::cur_default);
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
	return hitTest(
		point,
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol).state.link;
}

DocumentHitTestResult MarkdownDocumentWidget::hitTest(
		QPoint point,
		Ui::Text::StateRequest::Flags flags) const {
	const auto scale = zoomScale();
	if (scale != 1.) {
		point = QPoint(
			int(std::floor(point.x() / scale)),
			int(std::floor(point.y() / scale)));
	}
	return _layout.hitTest(point, flags);
}

DocumentSelection MarkdownDocumentWidget::selectionForCopy() const {
	return !_selection.empty()
		? _selection
		: _contextMenu
		? _savedSelection
		: DocumentSelection();
}

SelectionEndpoints MarkdownDocumentWidget::selectionEndpointsForCopy() const {
	return !_selection.empty()
		? _selectionEndpoints
		: _contextMenu
		? _savedSelectionEndpoints
		: SelectionEndpoints();
}

bool MarkdownDocumentWidget::selectionContains(
		DocumentSelection selection,
		const DocumentHitTestResult &result) const {
	const auto segment = _layout.segment(result.segmentIndex);
	if (!segment || selection.empty() || !result.valid()) {
		return false;
	}
	const auto endpoints = selectionEndpointsForCopy();
	const auto selectionState = PaintSelectionState{
		.segments = &_layout.segments(),
		.selection = selection,
		.endpoints = &endpoints,
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
	const auto offset = selectionOffsetFromHit(result);
	return (offset >= textSelection->from) && (offset < textSelection->to);
}

TextForMimeData MarkdownDocumentWidget::textForSegment(
		const SelectableSegment &segment,
		TextSelection selection) const {
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

TextForMimeData MarkdownDocumentWidget::textForContext(
		const DocumentHitTestResult &result) const {
	if (!result.valid() || !result.direct) {
		return TextForMimeData();
	}
	const auto segment = _layout.segment(result.segmentIndex);
	if (!segment) {
		return TextForMimeData();
	}
	return textForSegment(*segment);
}

int MarkdownDocumentWidget::selectionOffsetFromHit(
		const DocumentHitTestResult &result) const {
	const auto segment = _layout.segment(result.segmentIndex);
	if (!segment) {
		return 0;
	}
	if (result.forcedOffset >= 0) {
		return std::clamp(result.forcedOffset, 0, SegmentLength(*segment));
	}
	auto offset = int(result.state.symbol);
	if (_selectionType == TextSelectType::Letters
		&& result.state.afterSymbol) {
		++offset;
	}
	return std::clamp(offset, 0, SegmentLength(*segment));
}

DocumentSelection MarkdownDocumentWidget::selectionFromHit(
		const DocumentHitTestResult &result) const {
	if (_dragSegment < 0 || !result.valid()) {
		return {};
	}
	auto first = _dragSymbol;
	auto second = selectionOffsetFromHit(result);
	if (_selectionType != TextSelectType::Letters
		&& !_dragExpandedSelection.empty()
		&& result.segmentIndex != _dragSegment) {
		first = (CompareSelectionPositions(
			DocumentSelectionPosition{ result.segmentIndex, second },
			DocumentSelectionPosition{ _dragSegment, _dragSymbol }) < 0)
			? _dragExpandedSelection.to
			: _dragExpandedSelection.from;
	}
	if (result.segmentIndex == _dragSegment) {
		if (const auto segment = _layout.segment(_dragSegment);
			segment && segment->isTextLeaf()) {
			const auto adjusted = segment->leaf->adjustSelection(
				TextSelection(
					uint16(std::min(first, second)),
					uint16(std::max(first, second))),
				_selectionType);
			return {
				{ _dragSegment, adjusted.from },
				{ _dragSegment, adjusted.to },
			};
		}
	}
	return NormalizeSelection({
		{ _dragSegment, first },
		{ result.segmentIndex, second },
	});
}

TextForMimeData MarkdownDocumentWidget::getSelectedText() const {
	const auto selection = selectionForCopy();
	if (selection.empty()) {
		return TextForMimeData();
	}
	const auto endpoints = selectionEndpointsForCopy();
	const auto selectionState = PaintSelectionState{
		.segments = &_layout.segments(),
		.selection = selection,
		.endpoints = &endpoints,
	};
	auto pieces = std::vector<TextForMimeData>();
	for (const auto &segment : _layout.segments()) {
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

void MarkdownDocumentWidget::copySelectedText() {
	if (const auto text = getSelectedText(); !text.empty()) {
		TextUtilities::SetClipboardText(text);
	}
}

void MarkdownDocumentWidget::relayoutCurrentWidth() {
	clearSelection();
	const auto scale = zoomScale();
	const auto layoutWidth = std::max(int(std::floor(width() / scale)), 1);
	auto timer = QElapsedTimer();
	timer.start();
	_layout.relayout(_prepared, layoutWidth);
	_lastRelayoutMs = int(timer.elapsed());
}

void MarkdownDocumentWidget::forceRelayoutCurrentWidth() {
	resizeToWidth(width());
	update();
}

void MarkdownDocumentWidget::updateHover(const DocumentHitTestResult &state) {
	const auto changed = ClickHandler::setActive(state.state.link, this);
	auto cursor = style::cur_default;
	if (_dragAction == NoDrag) {
		if (state.state.link) {
			cursor = style::cur_pointer;
		} else if (state.direct) {
			cursor = style::cur_text;
		}
	} else {
		if (_dragAction == Selecting) {
			const auto selection = selectionFromHit(state);
			const auto endpoints = SelectionEndpoints{
				.from = _selectionEndpoints.from.valid()
					? _selectionEndpoints.from
					: SelectionEndpoint{ _dragSegment, false },
				.to = MakeSelectionEndpoint(state),
			};
			const auto endpointsChanged
				= (_selectionEndpoints.from.segment != endpoints.from.segment)
				|| (_selectionEndpoints.from.direct != endpoints.from.direct)
				|| (_selectionEndpoints.to.segment != endpoints.to.segment)
				|| (_selectionEndpoints.to.direct != endpoints.to.direct);
			if (_selection != selection || endpointsChanged) {
				_selection = selection;
				_selectionEndpoints = endpoints;
				_savedSelection = {};
				_savedSelectionEndpoints = {};
				setFocus();
				update();
			} else {
				_selectionEndpoints = endpoints;
			}
			cursor = style::cur_text;
		} else if (ClickHandler::getPressed()) {
			cursor = style::cur_pointer;
		}
	}
	if (changed || cursor != _cursor) {
		applyCursor(cursor);
	}
}

void MarkdownDocumentWidget::resetSelection() {
	_selection = {};
	_savedSelection = {};
	_selectionEndpoints = {};
	_savedSelectionEndpoints = {};
	_selectionType = TextSelectType::Letters;
	_dragAction = NoDrag;
	_dragStartPosition = QPoint();
	_dragSegment = -1;
	_dragSymbol = 0;
	_dragExpandedSelection = {};
}

void MarkdownDocumentWidget::clearSelection() {
	const auto hadSelection = !_selection.empty()
		|| !_savedSelection.empty()
		|| (_dragAction != NoDrag);
	resetSelection();
	if (hadSelection) {
		update();
	}
}

void MarkdownDocumentWidget::dragActionStart(
		QPoint point,
		Qt::MouseButton button) {
	const auto state = hitTest(
		point,
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol);
	updateHover(state);
	if (button != Qt::LeftButton) {
		return;
	}
	ClickHandler::pressed();
	_dragAction = NoDrag;
	_dragExpandedSelection = {};
	_dragSegment = -1;
	_dragSymbol = 0;
	if (ClickHandler::getPressed()) {
		_dragStartPosition = point;
		_dragAction = PrepareDrag;
		return;
	}
	if (!state.valid()) {
		clearSelection();
		return;
	}
	_dragSegment = state.segmentIndex;
	_dragSymbol = selectionOffsetFromHit(state);
	_selection = {
		{ _dragSegment, _dragSymbol },
		{ _dragSegment, _dragSymbol },
	};
	_savedSelection = {};
	_selectionEndpoints = {
		.from = MakeSelectionEndpoint(state),
		.to = MakeSelectionEndpoint(state),
	};
	_savedSelectionEndpoints = {};
	_dragAction = Selecting;
	update();
}

DocumentHitTestResult MarkdownDocumentWidget::dragActionUpdate(QPoint point) {
	const auto state = hitTest(
		point,
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol);
	if (_dragAction == PrepareDrag
		&& (point - _dragStartPosition).manhattanLength()
			>= QApplication::startDragDistance()) {
		_dragAction = Dragging;
	}
	updateHover(state);
	return state;
}

DocumentHitTestResult MarkdownDocumentWidget::dragActionFinish(
		QPoint point,
		Qt::MouseButton button) {
	const auto state = dragActionUpdate(point);
	auto activated = ClickHandler::unpressed();
	if (_dragAction == Dragging
		|| (_dragAction == Selecting && !_selection.empty())) {
		activated = nullptr;
	} else if (_dragAction == PrepareDrag && button != Qt::RightButton) {
		clearSelection();
	}
	_dragAction = NoDrag;
	_selectionType = TextSelectType::Letters;
	_dragExpandedSelection = {};
	updateHover(state);
	if (activated
		&& (button == Qt::LeftButton || button == Qt::MiddleButton)) {
		if (const auto prepared = std::dynamic_pointer_cast<
				PreparedLinkClickHandler>(activated)) {
			if (_activateLink) {
				_activateLink(prepared->link(), button);
			}
		} else {
			ActivateClickHandler(window(), activated, button);
		}
	}
	if (QGuiApplication::clipboard()->supportsSelection()
		&& !_selection.empty()) {
		if (const auto text = getSelectedText(); !text.empty()) {
			TextUtilities::SetClipboardText(text, QClipboard::Selection);
		}
	}
	return state;
}

void MarkdownDocumentWidget::applyCursor(style::cursor cursor) {
	if (_cursor != cursor) {
		_cursor = cursor;
		setCursor(_cursor);
	}
}

double MarkdownDocumentWidget::zoomScale() const {
	return std::max(_zoom, 1) / 100.;
}

constexpr auto kDeferredPreparationSourceBytes = 128 * 1024;
constexpr auto kDeferredPreparationFormulaCount = 4;
constexpr auto kDeferredPreparationConvertedNodes = 1200;

[[nodiscard]] QString PrepareTerminalFailureName(
		PrepareTerminalFailure failure) {
	switch (failure) {
	case PrepareTerminalFailure::None:
		return u"none"_q;
	case PrepareTerminalFailure::InvalidRequest:
		return u"invalid-request"_q;
	case PrepareTerminalFailure::InvalidStyle:
		return u"invalid-style"_q;
	case PrepareTerminalFailure::DocumentTooLarge:
		return u"document-too-large"_q;
	case PrepareTerminalFailure::InternalError:
		return u"internal-error"_q;
	}
	return u"unknown"_q;
}

[[nodiscard]] QString PrepareFailureReasonText(
		const PrepareFailureStatus &failure) {
	return !failure.debugReason.isEmpty()
		? failure.debugReason
		: PrepareTerminalFailureName(failure.terminal);
}

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
		std::optional<MarkdownStyleSnapshot> style = std::nullopt,
		bool clearRendererCache = false);
	void activateLink(const PreparedLink &link, Qt::MouseButton button);
	void applyPreparedResult(PreparedResult prepared);
	[[nodiscard]] bool scrollToAnchor(const QString &anchorId);
	void updateChildrenGeometry(QSize size);
	void updateLoadingGeometry();
	void updateFailureGeometry();
	void logPreparationSummary(
		const PrepareFailureStatus &failure,
		const PrepareDebugStats &debug,
		int prepareMs,
		int layoutMs) const;
	void cancelInFlightRequest();

	const OpenOptions _options;
	const std::shared_ptr<const PreparedDocument> _document;
	Ui::ScrollArea *_scroll = nullptr;
	MarkdownDocumentWidget *_body = nullptr;
	Ui::FlatLabel *_loading = nullptr;
	Ui::FlatLabel *_failure = nullptr;
	Ui::LinkButton *_failureOpen = nullptr;
	std::shared_ptr<MathRenderer> _renderer;
	PrepareGeneration _generation = 0;
	int _requestedDevicePixelRatio = 0;
	QString _pendingFragment;
	std::shared_ptr<std::atomic_bool> _cancelled;
	QElapsedTimer _prepareTimer;
	PrepareGeneration _prepareTimerGeneration = 0;
	bool _prepareTimerActive = false;

};

MarkdownPreviewRoot::MarkdownPreviewRoot(
	const PreparedDocument &document,
	const OpenOptions &options,
	QWidget *parent)
: Ui::RpWidget(parent)
, _options(options)
, _document(std::make_shared<PreparedDocument>(document))
, _renderer(std::make_shared<MathRenderer>())
, _pendingFragment(options.initialFragment)
, _cancelled(std::make_shared<std::atomic_bool>(false)) {
	_scroll = Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll);
	_body = _scroll->setOwnedWidget(object_ptr<MarkdownDocumentWidget>(_scroll));
	_loading = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_contacts_loading(tr::now),
		st::membersAbout);
	_failure = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_markdown_preview_cant(tr::now),
		st::ivMarkdownFailureLabel);
	_failureOpen = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_markdown_preview_open_file(tr::now));

	_scroll->hide();
	if (_body) {
		_body->hide();
		_body->setLinkActivationCallback([=](
				const PreparedLink &link,
				Qt::MouseButton button) {
			activateLink(link, button);
		});
		if (_options.delegate) {
			_body->setZoom(_options.delegate->ivZoom());
		}
	}
	_loading->hide();
	_failure->hide();
	_failureOpen->hide();
	_failureOpen->setClickedCallback([=] {
		if (!_options.sourcePath.isEmpty()) {
			File::Launch(_options.sourcePath);
		}
	});

	const auto initialStyle = CaptureMarkdownStyleSnapshot();
	_requestedDevicePixelRatio = initialStyle.devicePixelRatio;

	sizeValue() | rpl::on_next([=](QSize size) {
		updateChildrenGeometry(size);
	}, lifetime());

	style::PaletteChanged() | rpl::on_next([=] {
		startPreparation(shouldDeferPreparation(), std::nullopt, true);
	}, lifetime());

	screenValue() | rpl::on_next([=](not_null<QScreen*>) {
		const auto style = CaptureMarkdownStyleSnapshot();
		if (style.devicePixelRatio == _requestedDevicePixelRatio) {
			return;
		}
		startPreparation(shouldDeferPreparation(), std::move(style), true);
	}, lifetime());

	if (_options.delegate) {
		_options.delegate->ivZoomValue(
		) | rpl::on_next([=](int value) {
			if (_body) {
				_body->setZoom(value);
			}
		}, lifetime());
	}

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
		std::optional<MarkdownStyleSnapshot> style,
		bool clearRendererCache) {
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
	if (_renderer) {
		if (clearRendererCache) {
			_renderer->clearCache();
		}
		_renderer->resetDebugCounters();
	}
	_prepareTimer.start();
	_prepareTimerGeneration = generation;
	_prepareTimerActive = true;
	auto request = PrepareRequest{
		.document = _document,
		.renderer = _renderer,
		.style = std::move(*style),
		.generation = generation,
		.sourcePath = _options.sourcePath,
		.cancelled = cancelled,
	};

	if (showLoading) {
		_scroll->hide();
		if (_body) {
			_body->hide();
		}
		_failure->hide();
		_failureOpen->hide();
		_loading->show();
		_loading->raise();
		updateLoadingGeometry();
	} else {
		_loading->hide();
		_failure->hide();
		_failureOpen->hide();
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

void MarkdownPreviewRoot::activateLink(
		const PreparedLink &link,
		Qt::MouseButton button) {
	if (button != Qt::LeftButton && button != Qt::MiddleButton) {
		return;
	}
	switch (link.kind) {
	case PreparedLinkKind::External:
		HiddenUrlClickHandler::Open(link.target);
		break;
	case PreparedLinkKind::Anchor:
	case PreparedLinkKind::Footnote:
	case PreparedLinkKind::FootnoteBacklink:
		if (!scrollToAnchor(link.target)) {
			DEBUG_LOG(("Native Markdown IV: unresolved anchor: %1").arg(
				link.target));
		}
		break;
	case PreparedLinkKind::LocalFile: {
		auto path = link.target;
		if (!link.fragment.isEmpty()) {
			path += u"#"_q + link.fragment;
		}
		if (!TryOpenLocalFile(path)) {
			DEBUG_LOG(("Native Markdown IV: failed local markdown link: %1").arg(
				path));
		}
	} break;
	case PreparedLinkKind::RejectedRelative:
		DEBUG_LOG(("Native Markdown IV: rejected relative markdown link: %1").arg(
			link.target));
		break;
	case PreparedLinkKind::ToggleDetails:
		if (_body && !_body->toggleDetails(link.target)) {
			DEBUG_LOG(("Native Markdown IV: failed details toggle: %1").arg(
				link.target));
		}
		break;
	}
}

void MarkdownPreviewRoot::applyPreparedResult(PreparedResult prepared) {
	const auto prepareMs = (_prepareTimerActive
			&& (prepared.generation == _prepareTimerGeneration))
		? int(_prepareTimer.elapsed())
		: prepared.debug.prepareMs;
	_prepareTimerActive = false;

	const auto failure = prepared.failure;
	const auto debug = prepared.debug;
	if (failure.failed()) {
		_scroll->hide();
		if (_body) {
			_body->hide();
		}
		_loading->hide();
		_failure->show();
		if (_options.sourcePath.isEmpty()) {
			_failureOpen->hide();
		} else {
			_failureOpen->show();
		}
		_failure->raise();
		_failureOpen->raise();
		updateFailureGeometry();
		logPreparationSummary(failure, debug, prepareMs, 0);
		return;
	}

	if (!_body) {
		logPreparationSummary(failure, debug, prepareMs, 0);
		return;
	}

	updateChildrenGeometry(size());
	_body->setPreparedResult(std::move(prepared));
	if (_options.delegate) {
		_body->setZoom(_options.delegate->ivZoom());
	}
	_scroll->show();
	_body->show();
	_loading->hide();
	_failure->hide();
	_failureOpen->hide();
	if (!_pendingFragment.isEmpty()) {
		const auto scrolled = scrollToAnchor(_pendingFragment);
		static_cast<void>(scrolled);
		_pendingFragment.clear();
	}
	logPreparationSummary(
		failure,
		debug,
		prepareMs,
		_body->lastRelayoutMs());
}

bool MarkdownPreviewRoot::scrollToAnchor(const QString &anchorId) {
	if (!_body || !_scroll || anchorId.isEmpty()) {
		return false;
	}
	const auto top = _body->anchorTop(anchorId);
	if (top < 0) {
		return false;
	}
	_scroll->scrollToY(top, top + 1);
	return true;
}

void MarkdownPreviewRoot::updateChildrenGeometry(QSize size) {
	_scroll->setGeometry(QRect(QPoint(), size));
	if (_body) {
		_body->resizeToWidth(_scroll->width());
	}
	updateLoadingGeometry();
	updateFailureGeometry();
}

void MarkdownPreviewRoot::updateLoadingGeometry() {
	const auto availableWidth = std::max(width(), 1);
	_loading->resizeToWidth(availableWidth);
	_loading->moveToLeft(
		0,
		std::max((height() - _loading->height()) / 2, 0),
		availableWidth);
}

void MarkdownPreviewRoot::updateFailureGeometry() {
	const auto availableWidth = std::max(width(), 1);
	const auto failureWidth = std::min(availableWidth, st::ivMarkdownFailureWidth);
	_failure->resizeToWidth(failureWidth);
	_failureOpen->resizeToNaturalWidth(failureWidth);
	const auto openVisible = !_failureOpen->isHidden();
	const auto totalHeight = _failure->height()
		+ (openVisible ? (st::ivMarkdownFailureSkip + _failureOpen->height()) : 0);
	const auto top = std::max((height() - totalHeight) / 2, 0);
	_failure->moveToLeft(
		(availableWidth - failureWidth) / 2,
		top,
		availableWidth);
	if (openVisible) {
		_failureOpen->moveToLeft(
			(availableWidth - _failureOpen->width()) / 2,
			top + _failure->height() + st::ivMarkdownFailureSkip,
			availableWidth);
	}
}

void MarkdownPreviewRoot::logPreparationSummary(
		const PrepareFailureStatus &failure,
		const PrepareDebugStats &debug,
		int prepareMs,
		int layoutMs) const {
#ifndef NDEBUG
	const auto counters = _renderer ? _renderer->debugCounters() : FormulaDebugCounters();
	const auto reason = PrepareFailureReasonText(failure);
	DEBUG_LOG((
		failure.failed()
			? "Native Markdown IV: unexpected preview prepare failure (%1 ms prepare, %2 ms layout, %3 ms formulas, cache hits=%4 misses=%5 bytes=%6, terminal=%7): %8"
			: "Native Markdown IV: preview prepare success (%1 ms prepare, %2 ms layout, %3 ms formulas, cache hits=%4 misses=%5 bytes=%6, terminal=%7): %8"
		).arg(prepareMs
		).arg(layoutMs
		).arg(debug.formulaRenderMs
		).arg(counters.hits
		).arg(counters.misses
		).arg(qlonglong(counters.cacheBytes)
		).arg(reason
		).arg(_options.sourcePath));
#else
	Q_UNUSED(failure);
	Q_UNUSED(debug);
	Q_UNUSED(prepareMs);
	Q_UNUSED(layoutMs);
#endif
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
