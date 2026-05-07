/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "iv/markdown/iv_markdown_document.h"
#include "iv/markdown/iv_markdown_math_renderer.h"

#include "ui/text/text_entity.h"

#include <array>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace Iv {
struct Source;
} // namespace Iv

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
	Photo,
	Placeholder,
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
	EntityType entityType = EntityType::Invalid;
	EntityLinkShown shown = EntityLinkShown::Full;
};

enum class InlineTextObjectKind {
	Formula,
	IvImage,
};

struct InlineTextObjectFormulaData {
	QString copySource;
	QString trimmedTex;
};

struct InlineTextObjectIvImageData {
	uint64 documentId = 0;
	int width = 0;
	int height = 0;
	QString replacementText;
};

struct InlineTextObjectEntity {
	InlineTextObjectKind kind = InlineTextObjectKind::Formula;
	std::variant<
		InlineTextObjectFormulaData,
		InlineTextObjectIvImageData> data = InlineTextObjectFormulaData();
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

struct PreparedPhotoBlockData {
	uint64 photoId = 0;
	int width = 0;
	int height = 0;
	QString urlOverride;
	bool viewerOpen = false;
};

struct PreparedPlaceholderBlockData {
	QString label;
	QString copyText;
};

struct PreparedBlock {
	PreparedBlockKind kind = PreparedBlockKind::Paragraph;
	TextWithEntities text;
	std::vector<PreparedLink> links;
	std::vector<PreparedBlock> children;
	std::vector<PreparedTableRow> tableRows;
	std::vector<TableAlignment> tableAlignments;
	QString codeLanguage;
	QString formulaTex;
	QString anchorId;
	PreparedPhotoBlockData photo;
	PreparedPlaceholderBlockData placeholder;
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

struct PreparedFootnote {
	QString label;
	QString displayText;
	TextWithEntities text;
	std::vector<PreparedLink> links;
	std::vector<PreparedBlock> blocks;
};

struct MarkdownPrepareDimensions {
	int bodyTextSize = 0;
	std::array<int, 6> headingTextSizes = { 0, 0, 0, 0, 0, 0 };
	int tableHeaderTextSize = 0;
	int tableBodyTextSize = 0;
	int displayMathTextSize = 0;
	int displayMathMaxRenderWidth = 0;
	int displayMathMaxRenderHeight = 0;
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
	int formulaMeasureMs = 0;
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
	std::shared_ptr<const MeasuredFormula> measuredData;
	MeasuredFormula measured;
	bool present = false;
};

struct PrepareRequest {
	std::shared_ptr<const PreparedDocument> document;
	std::shared_ptr<MathRenderer> renderer;
	MarkdownPrepareDimensions dimensions;
	QString sourcePath;
};

struct NativeInstantViewPrepareRequest {
	const Iv::Source *source = nullptr;
	std::shared_ptr<MediaRuntime> mediaRuntime;
};

struct MarkdownArticleContent {
	PreparedRenderDocument blocks;
	std::vector<PreparedFootnote> footnotes;
	std::vector<PreparedFormulaSlot> formulas;
	std::shared_ptr<MediaRuntime> mediaRuntime;
	PrepareFailureStatus failure;
	PrepareDebugStats debug;
};

enum class NativeInstantViewPrepareResultKind {
	Supported,
	Unsupported,
	Failure,
};

struct NativeInstantViewPrepareResult {
	NativeInstantViewPrepareResultKind kind
		= NativeInstantViewPrepareResultKind::Unsupported;
	MarkdownArticleContent content;
	QString debugReason;

	[[nodiscard]] bool supported() const {
		return (kind == NativeInstantViewPrepareResultKind::Supported);
	}

	[[nodiscard]] bool unsupported() const {
		return (kind == NativeInstantViewPrepareResultKind::Unsupported);
	}

	[[nodiscard]] bool failed() const {
		return (kind == NativeInstantViewPrepareResultKind::Failure);
	}
};

[[nodiscard]] const MarkdownPrepareTableRenderLimits &PrepareTableRenderLimitsForIv();
[[nodiscard]] const MarkdownPrepareLimits &PrepareLimitsForIv();
[[nodiscard]] MarkdownPrepareDimensions CaptureMarkdownPrepareDimensions();
[[nodiscard]] QString SerializeInlineTextObjectEntity(
	const InlineTextObjectEntity &object);
[[nodiscard]] MarkdownArticleContent PrepareSynchronously(PrepareRequest request);
[[nodiscard]] NativeInstantViewPrepareResult TryPrepareNativeInstantView(
	NativeInstantViewPrepareRequest request);

} // namespace Iv::Markdown
