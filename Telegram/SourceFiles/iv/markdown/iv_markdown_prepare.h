#pragma once

#include "iv/markdown/iv_markdown_document.h"
#include "iv/markdown/iv_markdown_math_renderer.h"

#include "base/basic_types.h"
#include "styles/style_basic.h"
#include "styles/style_iv.h"
#include "ui/text/text_entity.h"

#include <QtGui/QColor>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace Iv::Markdown {

enum class PreparedBlockKind {
	Paragraph,
	Heading,
	CodeBlock,
	Rule,
	List,
	ListItem,
	Quote,
	DisplayMath,
	Table,
	Details,
};

enum class PreparedLinkKind {
	External,
	Anchor,
	Footnote,
	FootnoteBacklink,
	LocalFile,
	RejectedRelative,
	ToggleDetails,
};

struct PreparedLink {
	uint16 index = 0;
	PreparedLinkKind kind = PreparedLinkKind::External;
	QString target;
	QString fragment;
	QString copyText;
};

struct PreparedInlineObject {
	int position = 0;
	int formulaIndex = -1;
	int sourceLength = 0;
	QString copySource;
};

struct PreparedTableCell {
	TextWithEntities text;
	std::vector<PreparedLink> links;
	std::vector<PreparedInlineObject> inlineObjects;
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
	std::vector<PreparedInlineObject> inlineObjects;
	std::vector<PreparedBlock> children;
	std::vector<PreparedTableRow> tableRows;
	std::vector<TableAlignment> tableAlignments;
	QString codeLanguage;
	QString formulaTex;
	QString anchorId;
	ListKind listKind = ListKind::Bullet;
	ListDelimiter listDelimiter = ListDelimiter::None;
	MathKind mathKind = MathKind::Display;
	TaskState taskState = TaskState::None;
	int headingLevel = 0;
	int formulaIndex = -1;
	int orderedNumber = 0;
	int startNumber = 1;
	int actualDepth = 0;
	int visualDepth = 0;
	int tableColumnCount = 0;
	bool collapsed = false;
	bool depthClamped = false;
	bool tight = false;
};

struct PreparedRenderDocument {
	std::vector<PreparedBlock> blocks;
};

using PrepareGeneration = uint64;

struct MarkdownTextPaletteSnapshot {
	QColor link;
	QColor mono;
	QColor mark;
	QColor spoiler;
	QColor selectBackground;
	QColor selectText;
	QColor selectLink;
	QColor selectMono;
	QColor selectSpoiler;
	QColor selectOverlay;
	bool linkAlwaysActive = false;
};

struct MarkdownQuotePaintColorsSnapshot {
	std::array<QColor, 3> outlines = {
		QColor(0, 0, 0, 0),
		QColor(0, 0, 0, 0),
		QColor(0, 0, 0, 0),
	};
	QColor background = QColor(0, 0, 0, 0);
	QColor header = QColor(0, 0, 0, 0);
	QColor icon = QColor(0, 0, 0, 0);
};

struct MarkdownStyleSnapshot {
	MarkdownTextPaletteSnapshot textPalette;
	style::Markdown markdown;
	QColor defaultTextColor = Qt::black;
	QColor bulletColor = Qt::black;
	QColor taskMarkerColor = Qt::black;
	QColor taskMarkerCheckColor = Qt::black;
	QColor ruleColor = Qt::black;
	QColor displayMathForegroundColor = Qt::black;
	QColor displayMathFallbackBackgroundColor = Qt::black;
	QColor displayMathOverflowColor = Qt::black;
	QColor tableBorderColor = Qt::black;
	QColor tableHeaderBackgroundColor = Qt::black;
	QColor tableOverflowColor = Qt::black;
	MarkdownQuotePaintColorsSnapshot blockquotePaint;
	MarkdownQuotePaintColorsSnapshot prePaint;
	int displayMathMaxRenderWidth = 0;
	int displayMathMaxRenderHeight = 0;
	int paletteVersion = 0;
	int devicePixelRatio = 1;
};

struct MarkdownPrepareTableRenderLimits {
	int maxRows = 0;
	int maxColumns = 0;
	int maxCells = 0;
};

struct MarkdownPrepareLimits {
	MarkdownPrepareTableRenderLimits tableRender;
	int maxPreparedBlocks = 0;
};

enum class PrepareTerminalFailure {
	None,
	InvalidRequest,
	InvalidStyle,
	DocumentTooLarge,
	InternalError,
};

struct PrepareFailureStatus {
	PrepareTerminalFailure terminal = PrepareTerminalFailure::None;
	QString debugReason;

	[[nodiscard]] bool failed() const {
		return (terminal != PrepareTerminalFailure::None);
	}
};

struct PrepareDebugStats {
	int prepareMs = 0;
	int formulaRenderMs = 0;
	int sourceWarningCount = 0;
	int prepareWarningCount = 0;
	int formulaWarningCount = 0;
};

struct PreparedFormulaSlot {
	QString trimmedTex;
	MathKind kind = MathKind::Display;
	int textSize = 0;
	int renderWidthCap = 0;
	int renderHeightCap = 0;
	RenderedFormula rendered;
	bool present = false;
};

struct PrepareRequest {
	std::shared_ptr<const PreparedDocument> document;
	std::shared_ptr<MathRenderer> renderer;
	MarkdownStyleSnapshot style;
	PrepareGeneration generation = 0;
	QString sourcePath;
	std::shared_ptr<std::atomic_bool> cancelled;
};

struct PreparedResult {
	PreparedRenderDocument blocks;
	MarkdownStyleSnapshot style;
	std::vector<PreparedFormulaSlot> formulas;
	PrepareFailureStatus failure;
	PrepareDebugStats debug;
	PrepareGeneration generation = 0;
	bool cancelled = false;
};

[[nodiscard]] const MarkdownPrepareTableRenderLimits &PrepareTableRenderLimitsForIv();
[[nodiscard]] const MarkdownPrepareLimits &PrepareLimitsForIv();
[[nodiscard]] MarkdownStyleSnapshot CaptureMarkdownStyleSnapshot();
[[nodiscard]] PreparedResult PrepareSynchronously(PrepareRequest request);
void PrepareAsync(PrepareRequest request, Fn<void(PreparedResult)> done);

} // namespace Iv::Markdown
