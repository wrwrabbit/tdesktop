#include "iv/markdown/iv_markdown_document.h"

#include <utility>

namespace Iv::Markdown {
namespace {

[[nodiscard]] QString FromLatin1(const char *value) {
	return QString::fromLatin1(value);
}

[[nodiscard]] QString BoolString(bool value) {
	return FromLatin1(value ? "true" : "false");
}

[[nodiscard]] QString NodeKindName(NodeKind kind) {
	switch (kind) {
	case NodeKind::Document: return FromLatin1("Document");
	case NodeKind::Paragraph: return FromLatin1("Paragraph");
	case NodeKind::Heading: return FromLatin1("Heading");
	case NodeKind::Text: return FromLatin1("Text");
	case NodeKind::Emphasis: return FromLatin1("Emphasis");
	case NodeKind::Strong: return FromLatin1("Strong");
	case NodeKind::Strike: return FromLatin1("Strike");
	case NodeKind::InlineCode: return FromLatin1("InlineCode");
	case NodeKind::CodeBlock: return FromLatin1("CodeBlock");
	case NodeKind::Link: return FromLatin1("Link");
	case NodeKind::List: return FromLatin1("List");
	case NodeKind::ListItem: return FromLatin1("ListItem");
	case NodeKind::Blockquote: return FromLatin1("Blockquote");
	case NodeKind::ThematicBreak: return FromLatin1("ThematicBreak");
	case NodeKind::Table: return FromLatin1("Table");
	case NodeKind::TableRow: return FromLatin1("TableRow");
	case NodeKind::TableCell: return FromLatin1("TableCell");
	case NodeKind::HtmlInline: return FromLatin1("HtmlInline");
	case NodeKind::HtmlBlock: return FromLatin1("HtmlBlock");
	case NodeKind::DisplayMath: return FromLatin1("DisplayMath");
	case NodeKind::InlineMath: return FromLatin1("InlineMath");
	case NodeKind::SoftBreak: return FromLatin1("SoftBreak");
	case NodeKind::LineBreak: return FromLatin1("LineBreak");
	case NodeKind::Unsupported: return FromLatin1("Unsupported");
	}
	return FromLatin1("Unsupported");
}

[[nodiscard]] QString MathKindName(MathKind kind) {
	switch (kind) {
	case MathKind::Inline: return FromLatin1("Inline");
	case MathKind::Display: return FromLatin1("Display");
	}
	return FromLatin1("Inline");
}

[[nodiscard]] QString ListKindName(ListKind kind) {
	switch (kind) {
	case ListKind::Bullet: return FromLatin1("Bullet");
	case ListKind::Ordered: return FromLatin1("Ordered");
	}
	return FromLatin1("Bullet");
}

[[nodiscard]] QString ListDelimiterName(ListDelimiter delimiter) {
	switch (delimiter) {
	case ListDelimiter::None: return FromLatin1("None");
	case ListDelimiter::Period: return FromLatin1("Period");
	case ListDelimiter::Parenthesis: return FromLatin1("Parenthesis");
	}
	return FromLatin1("None");
}

[[nodiscard]] QString TaskStateName(TaskState state) {
	switch (state) {
	case TaskState::None: return FromLatin1("None");
	case TaskState::Unchecked: return FromLatin1("Unchecked");
	case TaskState::Checked: return FromLatin1("Checked");
	}
	return FromLatin1("None");
}

[[nodiscard]] QString TableAlignmentName(TableAlignment alignment) {
	switch (alignment) {
	case TableAlignment::None: return FromLatin1("None");
	case TableAlignment::Left: return FromLatin1("Left");
	case TableAlignment::Center: return FromLatin1("Center");
	case TableAlignment::Right: return FromLatin1("Right");
	}
	return FromLatin1("None");
}

[[nodiscard]] QString RangeString(const SourceRange &range) {
	if (!range.available) {
		return FromLatin1("unavailable");
	}
	return FromLatin1("%1:%2-%3:%4[%5,%6]").arg(
		range.startLine
	).arg(
		range.startColumn
	).arg(
		range.endLine
	).arg(
		range.endColumn
	).arg(
		range.startOffset
	).arg(
		range.endOffset);
}

[[nodiscard]] QString EscapedValue(QString value) {
	value.replace(FromLatin1("\\"), FromLatin1("\\\\"));
	value.replace(FromLatin1("\r"), FromLatin1("\\r"));
	value.replace(FromLatin1("\n"), FromLatin1("\\n"));
	value.replace(FromLatin1("\t"), FromLatin1("\\t"));
	return value;
}

void AddRequiredStringAttribute(
		QString *line,
		const char *name,
		const QString &value) {
	line->append(FromLatin1(" "));
	line->append(FromLatin1(name));
	line->append(FromLatin1("=\""));
	line->append(EscapedValue(value));
	line->append(FromLatin1("\""));
}

void AddStringAttribute(QString *line, const char *name, const QString &value) {
	if (value.isEmpty()) {
		return;
	}
	AddRequiredStringAttribute(line, name, value);
}

void AddIntAttribute(QString *line, const char *name, int value) {
	line->append(FromLatin1(" "));
	line->append(FromLatin1(name));
	line->append(FromLatin1("="));
	line->append(QString::number(value));
}

void AddBoolAttribute(QString *line, const char *name, bool value) {
	if (!value) {
		return;
	}
	line->append(FromLatin1(" "));
	line->append(FromLatin1(name));
	line->append(FromLatin1("="));
	line->append(BoolString(value));
}

[[nodiscard]] QString TableAlignmentsString(
		const std::vector<TableAlignment> &alignments) {
	auto names = QStringList();
	for (const auto alignment : alignments) {
		names.append(TableAlignmentName(alignment));
	}
	return names.join(FromLatin1(","));
}

void DumpNode(
		const MarkdownNode &node,
		int depth,
		QStringList *lines) {
	auto line = FromLatin1("node");
	AddIntAttribute(&line, "depth", depth);
	AddStringAttribute(&line, "kind", NodeKindName(node.kind));
	AddStringAttribute(&line, "range", RangeString(node.range));
	AddIntAttribute(&line, "children", static_cast<int>(node.children.size()));
	AddStringAttribute(&line, "text", node.text);
	AddStringAttribute(&line, "url", node.url);
	AddStringAttribute(&line, "title", node.title);
	AddStringAttribute(&line, "info", node.info);
	AddStringAttribute(&line, "raw", node.raw);
	AddStringAttribute(&line, "unsupportedKind", node.unsupportedKind);
	if (node.headingLevel != 0) {
		AddIntAttribute(&line, "headingLevel", node.headingLevel);
	}
	if (node.listStart != 0) {
		AddIntAttribute(&line, "listStart", node.listStart);
	}
	if (node.tableColumn != -1) {
		AddIntAttribute(&line, "tableColumn", node.tableColumn);
	}
	if (node.formulaIndex != -1) {
		AddIntAttribute(&line, "formulaIndex", node.formulaIndex);
	}
	if (node.kind == NodeKind::List) {
		AddStringAttribute(&line, "listKind", ListKindName(node.listKind));
		AddStringAttribute(
			&line,
			"listDelimiter",
			ListDelimiterName(node.listDelimiter));
	}
	if (node.taskState != TaskState::None) {
		AddStringAttribute(&line, "taskState", TaskStateName(node.taskState));
	}
	AddBoolAttribute(&line, "tight", node.tight);
	AddBoolAttribute(&line, "autolink", node.autolink);
	AddBoolAttribute(&line, "tableHeader", node.tableHeader);
	if (!node.tableAlignments.empty()) {
		AddStringAttribute(
			&line,
			"tableAlignments",
			TableAlignmentsString(node.tableAlignments));
	}
	lines->append(line);
	for (const auto &child : node.children) {
		DumpNode(child, depth + 1, lines);
	}
}

} // namespace

PreparedDocument EmptyDocument(QString sourceName) {
	auto document = PreparedDocument();
	document.sourceName = std::move(sourceName);
	document.document.kind = NodeKind::Document;
	return document;
}

QString DumpForDebug(const PreparedDocument &document) {
	auto lines = QStringList();
	lines.append(FromLatin1("sourceName=\"%1\"").arg(
		EscapedValue(document.sourceName)));
	lines.append(FromLatin1("title=\"%1\"").arg(
		EscapedValue(document.title)));
	lines.append(FromLatin1("sourceLength=%1").arg(
		static_cast<qlonglong>(document.sourceText.size())));
	lines.append(FromLatin1("empty=%1").arg(BoolString(document.empty)));
	lines.append(FromLatin1("cmarkNodeCount=%1").arg(
		document.stats.cmarkNodeCount));
	lines.append(FromLatin1("convertedNodeCount=%1").arg(
		document.stats.convertedNodeCount));
	lines.append(FromLatin1("maxDepth=%1").arg(
		document.stats.maxDepth));
	lines.append(FromLatin1("inlineFormulaCount=%1").arg(
		document.stats.inlineFormulaCount));
	lines.append(FromLatin1("displayFormulaCount=%1").arg(
		document.stats.displayFormulaCount));
	lines.append(FromLatin1("tablesSeen=%1").arg(
		BoolString(document.stats.tablesSeen)));
	lines.append(FromLatin1("taskListsSeen=%1").arg(
		BoolString(document.stats.taskListsSeen)));
	lines.append(FromLatin1("strikethroughSeen=%1").arg(
		BoolString(document.stats.strikethroughSeen)));
	lines.append(FromLatin1("autolinksSeen=%1").arg(
		BoolString(document.stats.autolinksSeen)));
	lines.append(FromLatin1("footnotesSeen=%1").arg(
		BoolString(document.stats.footnotesSeen)));
	lines.append(FromLatin1("warnings=%1").arg(
		static_cast<qlonglong>(document.warnings.size())));
	for (auto i = qsizetype(0); i != document.warnings.size(); ++i) {
		lines.append(FromLatin1("warning index=%1 text=\"%2\"").arg(
			i + 1
		).arg(
			EscapedValue(document.warnings[i])));
	}
	lines.append(FromLatin1("nodes=preorder"));
	DumpNode(document.document, 0, &lines);
	lines.append(FromLatin1("formulas=%1").arg(
		static_cast<qlonglong>(document.formulas.size())));
	for (const auto &formula : document.formulas) {
		auto line = FromLatin1("formula");
		AddIntAttribute(&line, "index", formula.index);
		AddStringAttribute(&line, "kind", MathKindName(formula.kind));
		AddStringAttribute(&line, "range", RangeString(formula.range));
		AddRequiredStringAttribute(&line, "parent", formula.parentNodeKind);
		AddRequiredStringAttribute(&line, "tex", formula.tex);
		lines.append(line);
	}
	return lines.join(FromLatin1("\n"));
}

} // namespace Iv::Markdown
