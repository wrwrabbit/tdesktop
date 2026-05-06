#pragma once

#include "iv/markdown/iv_markdown_article.h"
#include "spellcheck/spellcheck_highlight_syntax.h"

#include <memory>
#include <optional>
#include <vector>

namespace style {
struct Markdown;
struct QuoteStyle;
struct TextStyle;
} // namespace style

namespace Iv::Markdown {

class InlineFormulaObjectCache;
class CodeBlockSyntaxHighlightTracker {
public:
	virtual ~CodeBlockSyntaxHighlightTracker() = default;

	[[nodiscard]] virtual Spellchecker::HighlightProcessId tryHighlightSyntax(
		const QString &displayText,
		const QString &language,
		TextWithEntities &marked) = 0;
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
	Ui::Text::String labelLeaf;
	Ui::Text::String marker;
	Ui::Text::String fallbackLeaf;
	QString copyText;
	QString labelText;
	QString codeLanguage;
	Spellchecker::HighlightProcessId syntaxHighlightProcessId = 0;
	std::vector<LaidOutBlock> children;
	std::vector<LaidOutTableRow> tableRows;
	std::vector<int> tableColumnWidths;
	QRect outer;
	QRect headerRect;
	QRect bodyRect;
	QRect iconRect;
	QRect textRect;
	QRect labelRect;
	QRect markerRect;
	QRect contentRect;
	QRect formulaRect;
	QRect tableRect;
	QRect mediaRect;
	QRect visibleFormulaRect;
	QRect visibleTableRect;
	QRect visibleMediaRect;
	QPoint markerCenter;
	QString anchorId;
	int textWidth = 0;
	int labelWidth = 0;
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
	int secondarySegmentIndex = -1;
	std::shared_ptr<PhotoRuntime> photoRuntime;
	std::shared_ptr<Ui::DynamicImage> thumbnailImage;
	std::shared_ptr<Ui::DynamicImage> fullImage;
	MediaActivation activation;
	mutable bool thumbnailSubscribed = false;
	mutable bool fullSubscribed = false;
	mutable QImage colorizedFormulaImage;
	mutable QColor colorizedFormulaColor;
	mutable QSize colorizedFormulaSize;
};

struct LayoutContext {
	int listDepth = 0;
	int quoteDepth = 0;
	bool tightList = false;
	bool allowAsyncSyntaxHighlighting = true;
	CodeBlockSyntaxHighlightTracker *syntaxHighlightTracker = nullptr;
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

[[nodiscard]] bool IsFlowKind(PreparedBlockKind kind);
[[nodiscard]] QString ListMarkerText(const PreparedBlock &block);
[[nodiscard]] int TextLineHeight(const style::TextStyle &style);
[[nodiscard]] QPoint BulletMarkerCenter(
	int left,
	int top,
	const style::Markdown &markdown);
[[nodiscard]] QMargins BlockquotePadding(const style::QuoteStyle &style);
[[nodiscard]] Ui::Text::GeometryDescriptor TextGeometry(int width);
[[nodiscard]] int TextMinResizeWidth(int width);
[[nodiscard]] int TableCellTextMinResizeWidth(
	const style::TextStyle &textStyle,
	const style::Markdown &markdown);
[[nodiscard]] int LeafTextLength(const Ui::Text::String &leaf);
[[nodiscard]] QString CodeBlockDisplayText(const QString &text);
[[nodiscard]] int BlockSkip(
	const PreparedBlock &block,
	const style::Markdown &markdown);
[[nodiscard]] int BlockSkip(
	const PreparedBlock &previous,
	const PreparedBlock &block,
	LayoutContext context,
	const style::Markdown &markdown);
[[nodiscard]] const style::TextStyle &TextStyleFor(
	const PreparedBlock &block,
	const style::Markdown &markdown);
[[nodiscard]] int BlockMaxRight(const std::vector<LaidOutBlock> &blocks);
void RepopulateCodeBlockLeaf(
	LaidOutBlock &block,
	const style::Markdown &markdown,
	bool allowAsyncSyntaxHighlighting,
	CodeBlockSyntaxHighlightTracker *syntaxHighlightTracker = nullptr);

[[nodiscard]] LaidOutBlock LayoutFlowBlock(
	const PreparedBlock &prepared,
	const std::vector<PreparedFormulaSlot> *formulas,
	InlineFormulaObjectCache *inlineFormulaObjects,
	const std::shared_ptr<MediaRuntime> &mediaRuntime,
	const style::Markdown &markdown,
	int left,
	int top,
	int width);
[[nodiscard]] LaidOutBlock LayoutCodeBlock(
	const PreparedBlock &prepared,
	const style::Markdown &markdown,
	int left,
	int top,
	int width,
	bool allowAsyncSyntaxHighlighting,
	CodeBlockSyntaxHighlightTracker *syntaxHighlightTracker = nullptr);
[[nodiscard]] LaidOutBlock LayoutRuleBlock(
	const style::Markdown &markdown,
	int left,
	int top,
	int width);
[[nodiscard]] LaidOutBlock LayoutDisplayMathBlock(
	const PreparedBlock &prepared,
	const std::vector<PreparedFormulaSlot> &formulas,
	const style::Markdown &markdown,
	int left,
	int top,
	int width);
[[nodiscard]] LaidOutBlock LayoutTableBlock(
	const PreparedBlock &prepared,
	std::vector<PreparedFormulaSlot> *formulas,
	InlineFormulaObjectCache *inlineFormulaObjects,
	const std::shared_ptr<MediaRuntime> &mediaRuntime,
	const style::Markdown &markdown,
	int left,
	int top,
	int width);
[[nodiscard]] LaidOutBlock LayoutPlaceholderBlock(
	const PreparedBlock &prepared,
	std::vector<PreparedFormulaSlot> *formulas,
	InlineFormulaObjectCache *inlineFormulaObjects,
	const std::shared_ptr<MediaRuntime> &mediaRuntime,
	const style::Markdown &markdown,
	int left,
	int top,
	int width);
[[nodiscard]] LaidOutBlock LayoutPhotoBlock(
	const PreparedBlock &prepared,
	std::vector<PreparedFormulaSlot> *formulas,
	InlineFormulaObjectCache *inlineFormulaObjects,
	const std::shared_ptr<MediaRuntime> &mediaRuntime,
	const style::Markdown &markdown,
	int left,
	int top,
	int width);

} // namespace Iv::Markdown
