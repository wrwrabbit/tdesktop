#pragma once

#include "iv/markdown/iv_markdown_common.h"

#include <QtCore/QStringList>

#include <vector>

namespace Iv::Markdown {

enum class NodeKind {
	Document,
	Paragraph,
	Heading,
	Text,
	Emphasis,
	Strong,
	Strike,
	InlineCode,
	CodeBlock,
	Link,
	List,
	ListItem,
	Blockquote,
	ThematicBreak,
	Table,
	TableRow,
	TableCell,
	HtmlInline,
	HtmlBlock,
	FootnoteReference,
	FootnoteDefinition,
	DisplayMath,
	InlineMath,
	SoftBreak,
	LineBreak,
	Unsupported,
};

enum class MathKind {
	Inline,
	Display,
};

enum class ListKind {
	Bullet,
	Ordered,
};

enum class ListDelimiter {
	None,
	Period,
	Parenthesis,
};

enum class TaskState {
	None,
	Unchecked,
	Checked,
};

enum class TableAlignment {
	None,
	Left,
	Center,
	Right,
};

enum class HtmlBlockKind {
	None,
	Comment,
	Details,
	Unsupported,
};

struct SourceRange {
	bool available = false;
	int startLine = 0;
	int startColumn = 0;
	int endLine = 0;
	int endColumn = 0;
	int startOffset = 0;
	int endOffset = 0;
};

struct MarkdownNode {
	NodeKind kind = NodeKind::Unsupported;
	SourceRange range;
	QString text;
	QString url;
	QString title;
	QString info;
	QString raw;
	QString anchorId;
	QString footnoteLabel;
	QString detailsSummary;
	QString detailsBody;
	QString unsupportedKind;
	std::vector<MarkdownNode> children;
	std::vector<TableAlignment> tableAlignments;
	int headingLevel = 0;
	int listStart = 0;
	int tableColumn = -1;
	int formulaIndex = -1;
	int footnoteOrdinal = 0;
	ListKind listKind = ListKind::Bullet;
	ListDelimiter listDelimiter = ListDelimiter::None;
	TaskState taskState = TaskState::None;
	HtmlBlockKind htmlBlockKind = HtmlBlockKind::None;
	bool tight = false;
	bool autolink = false;
	bool tableHeader = false;
	bool detailsOpen = false;
};

struct MathFormula {
	int index = 0;
	MathKind kind = MathKind::Inline;
	QString tex;
	SourceRange range;
	QString parentNodeKind;
};

struct ParseStats {
	int cmarkNodeCount = 0;
	int convertedNodeCount = 0;
	int maxDepth = 0;
	int inlineFormulaCount = 0;
	int displayFormulaCount = 0;
	bool tablesSeen = false;
	bool taskListsSeen = false;
	bool strikethroughSeen = false;
	bool autolinksSeen = false;
	bool footnotesSeen = false;
};

struct PreparedDocument {
	QString sourceName;
	QString title;
	QString sourceText;
	MarkdownNode document;
	std::vector<MathFormula> formulas;
	ParseStats stats;
	QStringList warnings;
	bool empty = true;
};

struct ParseResult {
	PreparedDocument document;
	QString error;
	bool ok = true;
};

[[nodiscard]] PreparedDocument EmptyDocument(QString sourceName = QString());
[[nodiscard]] QString DumpForDebug(const PreparedDocument &document);

} // namespace Iv::Markdown
