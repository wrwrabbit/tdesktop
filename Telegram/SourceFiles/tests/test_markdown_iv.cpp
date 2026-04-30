#include "iv/markdown/iv_markdown_document.h"
#include "iv/markdown/iv_markdown_parse.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QString>

#include <iostream>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace {

using namespace Iv::Markdown;

constexpr auto kValidationSourceLimit = 4 * 1024 * 1024;
constexpr auto kValidationFormulaLimit = 64 * 1024;

struct Args {
	QString markdownPath;
	QString latexMarkdownPath;
	bool dump = false;
	bool inlineHtml = false;
	bool ok = true;
	QString error;
};

[[nodiscard]] QString FromLatin1(const char *value) {
	return QString::fromLatin1(value);
}

void PrintStreamLine(std::ostream &stream, const QString &line) {
	const auto bytes = line.toUtf8();
	stream.write(bytes.constData(), static_cast<std::streamsize>(bytes.size()));
	stream << '\n';
}

void PrintLine(const QString &line) {
	PrintStreamLine(std::cout, line);
}

void PrintError(const QString &line) {
	PrintStreamLine(std::cerr, line);
}

[[nodiscard]] Args ParseArgs(int argc, char **argv) {
	auto result = Args();
	for (auto i = 1; i != argc; ++i) {
		const auto argument = QString::fromLocal8Bit(argv[i]);
		if (argument == FromLatin1("--dump")) {
			result.dump = true;
		} else if (argument == FromLatin1("--inline-html")) {
			result.inlineHtml = true;
		} else if (argument == FromLatin1("--markdown")
			|| argument == FromLatin1("--latex-md")) {
			if (i + 1 == argc) {
				result.ok = false;
				result.error = FromLatin1("missing value for ") + argument;
				return result;
			}
			const auto path = QString::fromLocal8Bit(argv[++i]);
			if (argument == FromLatin1("--markdown")) {
				result.markdownPath = path;
			} else {
				result.latexMarkdownPath = path;
			}
		} else {
			result.ok = false;
			result.error = FromLatin1("unknown argument: ") + argument;
			return result;
		}
	}
	return result;
}

[[nodiscard]] QString DefaultFixturePath(const QString &name) {
	const auto applicationDir = QDir(QCoreApplication::applicationDirPath());
	const auto applicationCandidate = applicationDir.filePath(name);
	if (QFileInfo::exists(applicationCandidate)) {
		return applicationCandidate;
	}
	const auto outDebug = QDir::current().filePath(
		FromLatin1("out/Debug/") + name);
	if (QFileInfo::exists(outDebug)) {
		return outDebug;
	}
	const auto repoFixtureFromApplication = QDir::cleanPath(
		applicationDir.filePath(
			FromLatin1("../../Telegram/MarkdownMathProbes/fixtures/") + name));
	if (QFileInfo::exists(repoFixtureFromApplication)) {
		return repoFixtureFromApplication;
	}
	const auto repoFixtureFromCurrent = QDir::current().filePath(
		FromLatin1("Telegram/MarkdownMathProbes/fixtures/") + name);
	if (QFileInfo::exists(repoFixtureFromCurrent)) {
		return repoFixtureFromCurrent;
	}
	return outDebug;
}

[[nodiscard]] bool ReadFile(const QString &path, QByteArray *bytes) {
	if (!bytes) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	*bytes = file.readAll();
	return true;
}

[[nodiscard]] int CountNodes(const MarkdownNode &node) {
	auto result = 1;
	for (const auto &child : node.children) {
		result += CountNodes(child);
	}
	return result;
}

[[nodiscard]] bool HasKind(const MarkdownNode &node, NodeKind kind) {
	if (node.kind == kind) {
		return true;
	}
	for (const auto &child : node.children) {
		if (HasKind(child, kind)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasTextContaining(
		const MarkdownNode &node,
		const QString &text) {
	if (node.text.contains(text) || node.raw.contains(text)) {
		return true;
	}
	for (const auto &child : node.children) {
		if (HasTextContaining(child, text)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasExactInlineHtmlTriplet(
		const MarkdownNode &node,
		const QString &openingTag,
		const QString &innerText,
		const QString &closingTag) {
	const auto count = int(node.children.size());
	for (auto i = 0; (i + 2) < count; ++i) {
		const auto &opening = node.children[i];
		const auto &text = node.children[i + 1];
		const auto &closing = node.children[i + 2];
		if (opening.kind == NodeKind::HtmlInline
			&& opening.raw == openingTag
			&& text.kind == NodeKind::Text
			&& text.text == innerText
			&& closing.kind == NodeKind::HtmlInline
			&& closing.raw == closingTag) {
			return true;
		}
	}
	for (const auto &child : node.children) {
		if (HasExactInlineHtmlTriplet(
				child,
				openingTag,
				innerText,
				closingTag)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasTaskState(const MarkdownNode &node, TaskState state) {
	if (node.taskState == state) {
		return true;
	}
	for (const auto &child : node.children) {
		if (HasTaskState(child, state)) {
			return true;
		}
	}
	return false;
}

void CollectTables(
		const MarkdownNode &node,
		std::vector<const MarkdownNode*> *out) {
	if (!out) {
		return;
	}
	if (node.kind == NodeKind::Table) {
		out->push_back(&node);
	}
	for (const auto &child : node.children) {
		CollectTables(child, out);
	}
}

[[nodiscard]] int TableHeaderRowCount(const MarkdownNode &table) {
	auto result = 0;
	for (const auto &row : table.children) {
		if (row.kind == NodeKind::TableRow && row.tableHeader) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] bool HasTableHeaderRow(const MarkdownNode &table) {
	return TableHeaderRowCount(table) > 0;
}

[[nodiscard]] bool HasSequentialTableColumns(const MarkdownNode &table) {
	for (const auto &row : table.children) {
		if (row.kind != NodeKind::TableRow) {
			return false;
		}
		auto expectedColumn = 0;
		for (const auto &cell : row.children) {
			if (cell.kind != NodeKind::TableCell
				|| cell.tableColumn != expectedColumn) {
				return false;
			}
			++expectedColumn;
		}
	}
	return !table.children.empty();
}

[[nodiscard]] bool HasTableAlignments(
		const MarkdownNode &table,
		const std::vector<TableAlignment> &expected) {
	if (table.tableAlignments.size() != expected.size()) {
		return false;
	}
	for (auto i = 0, count = int(expected.size()); i != count; ++i) {
		if (table.tableAlignments[i] != expected[i]) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] int TableColumnCount(const MarkdownNode &table) {
	auto result = 0;
	for (const auto &row : table.children) {
		if (row.kind != NodeKind::TableRow) {
			return 0;
		}
		const auto count = int(row.children.size());
		if (count > result) {
			result = count;
		}
	}
	return result;
}

[[nodiscard]] int CountDisplayMathNodes(const MarkdownNode &node) {
	auto result = (node.kind == NodeKind::DisplayMath) ? 1 : 0;
	for (const auto &child : node.children) {
		result += CountDisplayMathNodes(child);
	}
	return result;
}

[[nodiscard]] bool SameRange(
		const SourceRange &range,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	return range.available
		&& (range.startLine == startLine)
		&& (range.startColumn == startColumn)
		&& (range.endLine == endLine)
		&& (range.endColumn == endColumn);
}

[[nodiscard]] bool CoversLineRange(
		const SourceRange &range,
		int firstLine,
		int lastLine) {
	return range.available
		&& (range.startLine <= firstLine)
		&& (range.endLine >= lastLine);
}

[[nodiscard]] const MarkdownNode *FindNodeByKindAndRange(
		const MarkdownNode &node,
		NodeKind kind,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	if (node.kind == kind
		&& SameRange(
			node.range,
			startLine,
			startColumn,
			endLine,
			endColumn)) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindNodeByKindAndRange(
				child,
				kind,
				startLine,
				startColumn,
				endLine,
				endColumn)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] int CountNodesByKindAndRange(
		const MarkdownNode &node,
		NodeKind kind,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	auto result = (node.kind == kind
		&& SameRange(
			node.range,
			startLine,
			startColumn,
			endLine,
			endColumn))
		? 1
		: 0;
	for (const auto &child : node.children) {
		result += CountNodesByKindAndRange(
			child,
			kind,
			startLine,
			startColumn,
			endLine,
			endColumn);
	}
	return result;
}

using NodeKindPath = std::initializer_list<NodeKind>;
using NodeKindPathIter = NodeKindPath::const_iterator;

[[nodiscard]] const MarkdownNode *FindNodeByPathAndRange(
		const MarkdownNode &node,
		NodeKindPathIter begin,
		NodeKindPathIter end,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	if (begin == end || node.kind != *begin) {
		return nullptr;
	}
	if (std::next(begin) == end) {
		return SameRange(
			node.range,
			startLine,
			startColumn,
			endLine,
			endColumn)
			? &node
			: nullptr;
	}
	const auto next = std::next(begin);
	for (const auto &child : node.children) {
		if (const auto found = FindNodeByPathAndRange(
				child,
				next,
				end,
				startLine,
				startColumn,
				endLine,
				endColumn)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] const MarkdownNode *FindNodeByPathAndRange(
		const MarkdownNode &node,
		NodeKindPath path,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	return path.size()
		? FindNodeByPathAndRange(
			node,
			path.begin(),
			path.end(),
			startLine,
			startColumn,
			endLine,
			endColumn)
		: nullptr;
}

[[nodiscard]] const MarkdownNode *FindNodeByKindAndLineRange(
		const MarkdownNode &node,
		NodeKind kind,
		int firstLine,
		int lastLine) {
	if (node.kind == kind && CoversLineRange(node.range, firstLine, lastLine)) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindNodeByKindAndLineRange(
				child,
				kind,
				firstLine,
				lastLine)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] const MarkdownNode *FindHtmlInlineByRaw(
		const MarkdownNode &node,
		const QString &raw) {
	if (node.kind == NodeKind::HtmlInline && node.raw == raw) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindHtmlInlineByRaw(child, raw)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] const MarkdownNode *FindHtmlBlockContaining(
		const MarkdownNode &node,
		const QString &text) {
	if (node.kind == NodeKind::HtmlBlock && node.raw.contains(text)) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindHtmlBlockContaining(child, text)) {
			return found;
		}
	}
	return nullptr;
}

void CollectNodesByKind(
		const MarkdownNode &node,
		NodeKind kind,
		std::vector<const MarkdownNode*> *out) {
	if (!out) {
		return;
	}
	if (node.kind == kind) {
		out->push_back(&node);
	}
	for (const auto &child : node.children) {
		CollectNodesByKind(child, kind, out);
	}
}

[[nodiscard]] const MarkdownNode *FindLinkByTarget(
		const MarkdownNode &node,
		const QString &target) {
	if (node.kind == NodeKind::Link && node.url == target) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindLinkByTarget(child, target)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] bool WarningContains(
		const PreparedDocument &document,
		const QString &snippet) {
	for (const auto &warning : document.warnings) {
		if (warning.contains(snippet, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] int CountFormulas(
		const PreparedDocument &document,
		MathKind kind) {
	auto result = 0;
	for (const auto &formula : document.formulas) {
		if (formula.kind == kind) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] bool HasFormulaOnLine(
		const PreparedDocument &document,
		int line,
		const QString &tex) {
	for (const auto &formula : document.formulas) {
		if (formula.range.available
			&& formula.range.startLine == line
			&& formula.tex == tex) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasFormulaInLineRange(
		const PreparedDocument &document,
		int firstLine,
		int lastLine) {
	if (lastLine < firstLine) {
		return false;
	}
	for (const auto &formula : document.formulas) {
		if (!formula.range.available) {
			continue;
		}
		if (formula.range.startLine <= lastLine
			&& formula.range.endLine >= firstLine) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] int CountFormulasInLineRange(
		const PreparedDocument &document,
		MathKind kind,
		int firstLine,
		int lastLine) {
	auto result = 0;
	for (const auto &formula : document.formulas) {
		if (!formula.range.available || formula.kind != kind) {
			continue;
		}
		if (formula.range.startLine <= lastLine
			&& formula.range.endLine >= firstLine) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] QString YesNo(bool value) {
	return FromLatin1(value ? "yes" : "no");
}

[[nodiscard]] bool HasBothTaskStates(const PreparedDocument &document) {
	return HasTaskState(document.document, TaskState::Checked)
		&& HasTaskState(document.document, TaskState::Unchecked);
}

[[nodiscard]] bool ExclusionsPass(const PreparedDocument &document) {
	return !HasFormulaInLineRange(document, 281, 281)
		&& HasFormulaOnLine(document, 285, FromLatin1("5x + 3"))
		&& !HasFormulaInLineRange(document, 332, 340);
}

[[nodiscard]] bool HasFormula(
		const PreparedDocument &document,
		MathKind kind,
		const QString &tex) {
	for (const auto &formula : document.formulas) {
		if (formula.kind == kind && formula.tex == tex) {
			return true;
		}
	}
	return false;
}

void Check(bool condition, const QString &message, bool *ok);

[[nodiscard]] MarkdownSourceValidationResult CheckValidationSuccess(
		const QByteArray &source,
		const QString &label,
		bool *ok) {
	auto validated = ValidateMarkdownSourceForIv(source, ParseOptions{ label });
	Check(
		validated.ok,
		label + FromLatin1(" validation failed: ") + validated.error,
		ok);
	return validated;
}

void CheckValidationFailure(
		const QByteArray &source,
		const QString &label,
		const QString &expectedError,
		bool *ok) {
	const auto validated = ValidateMarkdownSourceForIv(
		source,
		ParseOptions{ label });
	Check(
		!validated.ok,
		label + FromLatin1(" validation should fail"),
		ok);
	if (!validated.ok) {
		Check(
			validated.error == expectedError,
			label + FromLatin1(" validation error should be ")
				+ expectedError
				+ FromLatin1(", got ")
				+ validated.error,
			ok);
	}
}

void CheckMatchingParseCounts(
		const PreparedDocument &legacy,
		const PreparedDocument &validated,
		const QString &label,
		bool *ok) {
	Check(
		legacy.stats.cmarkNodeCount == validated.stats.cmarkNodeCount,
		label + FromLatin1(" validated path cmark node count"),
		ok);
	Check(
		CountNodes(legacy.document) == CountNodes(validated.document),
		label + FromLatin1(" validated path converted node count"),
		ok);
	Check(
		legacy.formulas.size() == validated.formulas.size(),
		label + FromLatin1(" validated path formula count"),
		ok);
	Check(
		CountFormulas(legacy, MathKind::Inline)
			== CountFormulas(validated, MathKind::Inline),
		label + FromLatin1(" validated path inline formula count"),
		ok);
	Check(
		CountFormulas(legacy, MathKind::Display)
			== CountFormulas(validated, MathKind::Display),
		label + FromLatin1(" validated path display formula count"),
		ok);
	Check(
		CountDisplayMathNodes(legacy.document)
			== CountDisplayMathNodes(validated.document),
		label + FromLatin1(" validated path display math node count"),
		ok);
}

void AppendSummaryCounts(QString *line, const PreparedDocument &document) {
	line->append(FromLatin1(" nodes="));
	line->append(QString::number(document.stats.cmarkNodeCount));
	line->append(FromLatin1(" converted="));
	line->append(QString::number(CountNodes(document.document)));
	line->append(FromLatin1(" formulas_inline="));
	line->append(QString::number(CountFormulas(document, MathKind::Inline)));
	line->append(FromLatin1(" formulas_display="));
	line->append(QString::number(CountFormulas(document, MathKind::Display)));
}

void PrintSummary(const PreparedDocument &document, const QString &label) {
	auto line = label;
	AppendSummaryCounts(&line, document);
	line.append(FromLatin1(" tables="));
	line.append(YesNo(HasKind(document.document, NodeKind::Table)));
	if (label == FromLatin1("markdown-example.md")) {
		line.append(FromLatin1(" tasks="));
		line.append(YesNo(HasBothTaskStates(document)));
		line.append(FromLatin1(" strike="));
		line.append(YesNo(HasKind(document.document, NodeKind::Strike)));
	} else if (label == FromLatin1("latex-markdown-test.md")) {
		line.append(FromLatin1(" exclusions="));
		line.append(YesNo(ExclusionsPass(document)));
	}
	PrintLine(line);
}

[[nodiscard]] bool ParseFixture(
		const QString &path,
		const QString &label,
		PreparedDocument *document) {
	auto bytes = QByteArray();
	if (!ReadFile(path, &bytes)) {
		PrintError(label + FromLatin1(" read-failed: ") + path);
		return false;
	}
	auto parsed = ParseMarkdownForIv(bytes, ParseOptions{ label });
	if (!parsed.ok) {
		PrintError(label + FromLatin1(" parse-failed: ") + parsed.error);
		return false;
	}
	auto validated = ValidateMarkdownSourceForIv(
		bytes,
		ParseOptions{ label });
	if (!validated.ok) {
		PrintError(label + FromLatin1(" validate-failed: ") + validated.error);
		return false;
	}
	auto parsedValidated = ParseMarkdownForIv(std::move(validated.source));
	if (!parsedValidated.ok) {
		PrintError(
			label + FromLatin1(" validated-parse-failed: ")
				+ parsedValidated.error);
		return false;
	}
	auto countsOk = true;
	CheckMatchingParseCounts(
		parsed.document,
		parsedValidated.document,
		label,
		&countsOk);
	if (!countsOk) {
		return false;
	}
	PrintSummary(parsed.document, label);
	if (document) {
		*document = std::move(parsed.document);
	}
	return true;
}

void Check(bool condition, const QString &message, bool *ok) {
	if (condition) {
		return;
	}
	if (ok) {
		*ok = false;
	}
	PrintError(FromLatin1("assertion failed: ") + message);
}

void CheckParseSuccess(
		const QByteArray &source,
		const QString &label,
		bool *ok) {
	const auto parsed = ParseMarkdownForIv(source, ParseOptions{ label });
	Check(
		parsed.ok,
		label + FromLatin1(" parse failed: ") + parsed.error,
		ok);
}

void CheckParseFailure(
		const QByteArray &source,
		const QString &label,
		const QString &expectedError,
		bool *ok) {
	const auto parsed = ParseMarkdownForIv(source, ParseOptions{ label });
	Check(
		!parsed.ok,
		label + FromLatin1(" should fail"),
		ok);
	if (!parsed.ok) {
		Check(
			parsed.error == expectedError,
			label + FromLatin1(" error should be ")
				+ expectedError
				+ FromLatin1(", got ")
				+ parsed.error,
			ok);
	}
}

void CheckValidationEdges(bool *ok) {
	auto utf8BomSource = QByteArray::fromHex("EFBBBF");
	utf8BomSource.append("# Title\n");
	auto validatedUtf8Bom = CheckValidationSuccess(
		utf8BomSource,
		FromLatin1("utf8 bom"),
		ok);
	if (validatedUtf8Bom.ok) {
		Check(
			validatedUtf8Bom.source.normalized == QByteArray("# Title\n"),
			FromLatin1("utf8 bom normalized bytes"),
			ok);
		const auto parsedValidated = ParseMarkdownForIv(
			std::move(validatedUtf8Bom.source));
		Check(
			parsedValidated.ok,
			FromLatin1("utf8 bom validated parse failed: ")
				+ parsedValidated.error,
			ok);
	}
	CheckParseSuccess(
		utf8BomSource,
		FromLatin1("utf8 bom"),
		ok);

	CheckValidationFailure(
		QByteArray::fromHex("FFFE2300"),
		FromLatin1("utf16 bom"),
		FromLatin1("source-unsupported-bom"),
		ok);
	CheckParseFailure(
		QByteArray::fromHex("FFFE2300"),
		FromLatin1("utf16 bom"),
		FromLatin1("source-unsupported-bom"),
		ok);
	CheckValidationFailure(
		QByteArray("a\0b", 3),
		FromLatin1("nul byte"),
		FromLatin1("source-binary"),
		ok);
	CheckParseFailure(
		QByteArray("a\0b", 3),
		FromLatin1("nul byte"),
		FromLatin1("source-binary"),
		ok);
	CheckValidationFailure(
		QByteArray::fromHex("C328"),
		FromLatin1("invalid utf8"),
		FromLatin1("source-invalid-utf8"),
		ok);
	CheckParseFailure(
		QByteArray::fromHex("C328"),
		FromLatin1("invalid utf8"),
		FromLatin1("source-invalid-utf8"),
		ok);

	const auto oversizedSource = QByteArray(kValidationSourceLimit + 1, 'a');
	CheckValidationFailure(
		oversizedSource,
		FromLatin1("source size"),
		FromLatin1("source-too-large"),
		ok);
	CheckParseFailure(
		oversizedSource,
		FromLatin1("source size"),
		FromLatin1("source-too-large"),
		ok);

	auto oversizedFormula = QByteArray();
	oversizedFormula.reserve(kValidationFormulaLimit + 2);
	oversizedFormula.append('$');
	oversizedFormula.append(QByteArray(kValidationFormulaLimit + 1, '+'));
	oversizedFormula.append('$');
	CheckParseFailure(
		oversizedFormula,
		FromLatin1("formula size"),
		FromLatin1("formula-too-large"),
		ok);

	const auto generated = QByteArray(
		"Inline code `$code$`.\n"
		"```\n"
		"$block$\n"
		"```\n"
		"Escaped \\$ and price $5.99$.\n"
		"Real $x + y$ done.\n");
	const auto parsed = ParseMarkdownForIv(
		generated,
		ParseOptions{ FromLatin1("generated-edge-checks.md") });
	Check(
		parsed.ok,
		FromLatin1("generated exclusions parse failed: ") + parsed.error,
		ok);
	if (parsed.ok) {
		Check(
			static_cast<int>(parsed.document.formulas.size()) == 1,
			FromLatin1("generated exclusions formula count"),
			ok);
		Check(
			CountFormulas(parsed.document, MathKind::Inline) == 1,
			FromLatin1("generated exclusions inline formula count"),
			ok);
		Check(
			CountFormulas(parsed.document, MathKind::Display) == 0,
			FromLatin1("generated exclusions display formula count"),
			ok);
		Check(
			HasFormula(parsed.document, MathKind::Inline, FromLatin1("x + y")),
			FromLatin1("generated exclusions real formula"),
			ok);
	}
}

void CheckInlineHtmlCoverage(bool dump, bool *ok) {
	const auto source = QByteArray(
		"H<sub>2</sub>O\n"
		"E = mc<sup>2</sup>\n"
		"<mark>Highlighted text using HTML mark</mark>\n"
		"<br>\n");
	const auto label = FromLatin1("generated-inline-html.md");
	const auto parsed = ParseMarkdownForIv(source, ParseOptions{ label });
	Check(
		parsed.ok,
		label + FromLatin1(" parse failed: ") + parsed.error,
		ok);
	if (!parsed.ok) {
		return;
	}
	const auto validated = CheckValidationSuccess(source, label, ok);
	if (!validated.ok) {
		return;
	}
	const auto parsedValidated = ParseMarkdownForIv(std::move(validated.source));
	Check(
		parsedValidated.ok,
		label + FromLatin1(" validated parse failed: ")
			+ parsedValidated.error,
		ok);
	if (!parsedValidated.ok) {
		return;
	}
	CheckMatchingParseCounts(
		parsed.document,
		parsedValidated.document,
		label,
		ok);
	if (dump) {
		PrintLine(DumpForDebug(parsed.document));
	}
	const auto &document = parsed.document.document;
	const auto subscriptParagraph = FindNodeByKindAndLineRange(
		document,
		NodeKind::Paragraph,
		1,
		1);
	Check(
		subscriptParagraph != nullptr,
		label + FromLatin1(" subscript paragraph range"),
		ok);
	if (subscriptParagraph) {
		Check(
			HasExactInlineHtmlTriplet(
				*subscriptParagraph,
				FromLatin1("<sub>"),
				FromLatin1("2"),
				FromLatin1("</sub>")),
			label + FromLatin1(" subscript HTML triplet"),
			ok);
	}
	const auto superscriptParagraph = FindNodeByKindAndLineRange(
		document,
		NodeKind::Paragraph,
		2,
		2);
	Check(
		superscriptParagraph != nullptr,
		label + FromLatin1(" superscript paragraph range"),
		ok);
	if (superscriptParagraph) {
		Check(
			HasExactInlineHtmlTriplet(
				*superscriptParagraph,
				FromLatin1("<sup>"),
				FromLatin1("2"),
				FromLatin1("</sup>")),
			label + FromLatin1(" superscript HTML triplet"),
			ok);
	}
	const auto markParagraph = FindNodeByKindAndLineRange(
		document,
		NodeKind::Paragraph,
		3,
		3);
	Check(
		markParagraph != nullptr,
		label + FromLatin1(" mark paragraph range"),
		ok);
	if (markParagraph) {
		Check(
			HasExactInlineHtmlTriplet(
				*markParagraph,
				FromLatin1("<mark>"),
				FromLatin1("Highlighted text using HTML mark"),
				FromLatin1("</mark>")),
			label + FromLatin1(" mark HTML triplet"),
			ok);
	}
	const auto htmlLineBreakParagraph = FindNodeByKindAndLineRange(
		document,
		NodeKind::Paragraph,
		4,
		4);
	Check(
		htmlLineBreakParagraph != nullptr,
		label + FromLatin1(" html line break paragraph range"),
		ok);
	if (htmlLineBreakParagraph) {
		const auto brInline = FindHtmlInlineByRaw(
			*htmlLineBreakParagraph,
			FromLatin1("<br>"));
		Check(
			brInline != nullptr,
			label + FromLatin1(" html line break raw node"),
			ok);
		if (brInline) {
			Check(
				brInline->text.isEmpty() && brInline->children.empty(),
				label + FromLatin1(" html line break lone HtmlInline"),
				ok);
		}
	}
}

void CheckFixtureSemanticCoverage(
		const PreparedDocument &document,
		const QString &path,
		bool *ok) {
	auto footnoteReferences = std::vector<const MarkdownNode*>();
	CollectNodesByKind(
		document.document,
		NodeKind::FootnoteReference,
		&footnoteReferences);
	Check(
		footnoteReferences.size() >= 2,
		FromLatin1("markdown-example.md footnote reference count"),
		ok);
	if (footnoteReferences.size() >= 2) {
		Check(
			footnoteReferences[0]->footnoteLabel == FromLatin1("1")
				&& footnoteReferences[0]->footnoteOrdinal == 1,
			FromLatin1("markdown-example.md first footnote reference label"),
			ok);
		Check(
			footnoteReferences[1]->footnoteLabel == FromLatin1("long-note")
				&& footnoteReferences[1]->footnoteOrdinal == 2,
			FromLatin1("markdown-example.md second footnote reference label"),
			ok);
	}

	auto footnoteDefinitions = std::vector<const MarkdownNode*>();
	CollectNodesByKind(
		document.document,
		NodeKind::FootnoteDefinition,
		&footnoteDefinitions);
	Check(
		footnoteDefinitions.size() >= 2,
		FromLatin1("markdown-example.md footnote definition count"),
		ok);
	if (footnoteDefinitions.size() >= 2) {
		Check(
			footnoteDefinitions[0]->anchorId == FromLatin1("fn-1")
				&& footnoteDefinitions[1]->anchorId == FromLatin1("fn-2"),
			FromLatin1("markdown-example.md footnote anchors"),
			ok);
	}

	const auto headingsLink = FindLinkByTarget(document.document, FromLatin1("#headings"));
	Check(
		headingsLink != nullptr,
		FromLatin1("markdown-example.md toc fragment link"),
		ok);

	const auto relativeLink = FindLinkByTarget(
		document.document,
		FromLatin1("./docs/getting-started.md"));
	Check(
		relativeLink != nullptr,
		FromLatin1("markdown-example.md relative link parse"),
		ok);

	const auto headings = FindNodeByKindAndLineRange(
		document.document,
		NodeKind::Heading,
		27,
		27);
	Check(
		headings != nullptr && headings->anchorId == FromLatin1("headings"),
		FromLatin1("markdown-example.md headings anchor id"),
		ok);
	const auto definitionLists = FindNodeByKindAndLineRange(
		document.document,
		NodeKind::Heading,
		266,
		266);
	Check(
		definitionLists != nullptr
			&& definitionLists->anchorId
				== FromLatin1("definition-lists-renderer-dependent"),
		FromLatin1("markdown-example.md punctuation heading anchor id"),
		ok);

	const auto details = FindNodeByKindAndLineRange(
		document.document,
		NodeKind::HtmlBlock,
		261,
		264);
	Check(
		details != nullptr,
		FromLatin1("markdown-example.md details block range"),
		ok);
	if (details) {
		Check(
			details->htmlBlockKind == HtmlBlockKind::Details
				&& details->detailsSummary
					== FromLatin1("Click to expand details/summary block"),
			FromLatin1("markdown-example.md details classification"),
			ok);
	}
	const auto comment = FindHtmlBlockContaining(
		document.document,
		FromLatin1("markdown-renderer-test"));
	Check(
		comment != nullptr && comment->htmlBlockKind == HtmlBlockKind::Comment,
		FromLatin1("markdown-example.md comment classification"),
		ok);
	Check(
		WarningContains(document, FromLatin1("Unsupported HTML block")),
		FromLatin1("markdown-example.md unsupported html warning"),
		ok);

	const auto duplicateHeadings = ParseMarkdownForIv(
		QByteArray("## Same\n## Same\n"),
		ParseOptions{ FromLatin1("generated-duplicate-headings.md") });
	Check(
		duplicateHeadings.ok,
		FromLatin1("generated duplicate headings parse failed"),
		ok);
	if (duplicateHeadings.ok) {
		auto duplicateNodes = std::vector<const MarkdownNode*>();
		CollectNodesByKind(
			duplicateHeadings.document.document,
			NodeKind::Heading,
			&duplicateNodes);
		Check(
			duplicateNodes.size() == 2
				&& duplicateNodes[0]->anchorId == FromLatin1("same")
				&& duplicateNodes[1]->anchorId == FromLatin1("same-2"),
			FromLatin1("generated duplicate headings anchors"),
			ok);
	}

}

} // namespace

int main(int argc, char **argv) {
	auto application = QCoreApplication(argc, argv);
	(void)application;

	auto args = ParseArgs(argc, argv);
	if (!args.ok) {
		PrintError(args.error);
		return 1;
	}
	if (args.inlineHtml) {
		auto ok = true;
		CheckInlineHtmlCoverage(args.dump, &ok);
		return ok ? 0 : 1;
	}
	if (args.markdownPath.isEmpty()) {
		args.markdownPath = DefaultFixturePath(FromLatin1("markdown-example.md"));
	}
	if (args.latexMarkdownPath.isEmpty()) {
		args.latexMarkdownPath = DefaultFixturePath(
			FromLatin1("latex-markdown-test.md"));
	}

	auto markdown = PreparedDocument();
	if (!ParseFixture(
			args.markdownPath,
			FromLatin1("markdown-example.md"),
			&markdown)) {
		return 1;
	}
	if (args.dump) {
		PrintLine(DumpForDebug(markdown));
	}

	auto latex = PreparedDocument();
	if (!ParseFixture(
			args.latexMarkdownPath,
			FromLatin1("latex-markdown-test.md"),
			&latex)) {
		return 1;
	}
	if (args.dump) {
		PrintLine(DumpForDebug(latex));
	}

	auto ok = true;
	Check(
		markdown.stats.cmarkNodeCount == 562,
		FromLatin1("markdown-example.md cmark node count"),
		&ok);
	Check(
		CountFormulas(markdown, MathKind::Inline) == 1,
		FromLatin1("markdown-example.md inline formula count"),
		&ok);
	Check(
		CountFormulas(markdown, MathKind::Display) == 1,
		FromLatin1("markdown-example.md display formula count"),
		&ok);
	Check(
		CountDisplayMathNodes(markdown.document) == 1,
		FromLatin1("markdown-example.md display math node count"),
		&ok);
	Check(
		FindNodeByKindAndRange(
			markdown.document,
			NodeKind::DisplayMath,
			281,
			1,
			283,
			2) != nullptr,
		FromLatin1("markdown-example.md display formula range"),
		&ok);
	Check(
		CountNodesByKindAndRange(
			markdown.document,
			NodeKind::Paragraph,
			281,
			1,
			283,
			2) == 0,
		FromLatin1("markdown-example.md duplicate display paragraph removed"),
		&ok);
	Check(
		HasKind(markdown.document, NodeKind::Table),
		FromLatin1("markdown-example.md table coverage"),
		&ok);
	Check(
		HasKind(markdown.document, NodeKind::Strike),
		FromLatin1("markdown-example.md strikethrough coverage"),
		&ok);
	Check(
		HasTaskState(markdown.document, TaskState::Checked),
		FromLatin1("markdown-example.md checked task"),
		&ok);
	Check(
		HasTaskState(markdown.document, TaskState::Unchecked),
		FromLatin1("markdown-example.md unchecked task"),
		&ok);
	auto markdownTables = std::vector<const MarkdownNode*>();
	CollectTables(markdown.document, &markdownTables);
	Check(
		int(markdownTables.size()) >= 2,
		FromLatin1("markdown-example.md table count"),
		&ok);
	if (markdownTables.size() >= 2) {
		const auto &firstTable = *markdownTables[0];
		const auto &secondTable = *markdownTables[1];
		Check(
			HasTableHeaderRow(firstTable),
			FromLatin1("markdown-example.md first table header row"),
			&ok);
		Check(
			TableHeaderRowCount(firstTable) == 1,
			FromLatin1("markdown-example.md first table header row count"),
			&ok);
		Check(
			TableColumnCount(firstTable) == 3,
			FromLatin1("markdown-example.md first table column count"),
			&ok);
		Check(
			HasSequentialTableColumns(firstTable),
			FromLatin1("markdown-example.md first table column order"),
			&ok);
		Check(
			HasTableAlignments(
				secondTable,
				{
					TableAlignment::Left,
					TableAlignment::Center,
					TableAlignment::Right,
				}),
			FromLatin1("markdown-example.md second table alignments"),
			&ok);
	}
	CheckFixtureSemanticCoverage(markdown, args.markdownPath, &ok);
	Check(
		latex.stats.cmarkNodeCount == 532,
		FromLatin1("latex-markdown-test.md cmark node count"),
		&ok);
	Check(
		CountFormulas(latex, MathKind::Inline) == 99,
		FromLatin1("latex-markdown-test.md inline formula count"),
		&ok);
	Check(
		CountFormulas(latex, MathKind::Display) == 31,
		FromLatin1("latex-markdown-test.md display formula count"),
		&ok);
	Check(
		CountDisplayMathNodes(latex.document) == 31,
		FromLatin1("latex-markdown-test.md display math node count"),
		&ok);
	Check(
		FindNodeByPathAndRange(
			latex.document,
			{
				NodeKind::Document,
				NodeKind::List,
				NodeKind::ListItem,
				NodeKind::DisplayMath,
			},
			299,
			4,
			301,
			5) != nullptr,
		FromLatin1("latex-markdown-test.md list display formula nested"),
		&ok);
	Check(
		FindNodeByPathAndRange(
			latex.document,
			{
				NodeKind::Document,
				NodeKind::DisplayMath,
			},
			299,
			4,
			301,
			5) == nullptr,
		FromLatin1("latex-markdown-test.md list display formula not hoisted"),
		&ok);
	Check(
		FindNodeByPathAndRange(
			latex.document,
			{
				NodeKind::Document,
				NodeKind::Blockquote,
				NodeKind::DisplayMath,
			},
			307,
			3,
			309,
			4) != nullptr,
		FromLatin1("latex-markdown-test.md blockquote display formula nested"),
		&ok);
	Check(
		FindNodeByPathAndRange(
			latex.document,
			{
				NodeKind::Document,
				NodeKind::DisplayMath,
			},
			307,
			3,
			309,
			4) == nullptr,
		FromLatin1("latex-markdown-test.md blockquote display formula not hoisted"),
		&ok);
	Check(
		HasKind(latex.document, NodeKind::Table),
		FromLatin1("latex-markdown-test.md table coverage"),
		&ok);
	auto latexTables = std::vector<const MarkdownNode*>();
	CollectTables(latex.document, &latexTables);
	Check(
		!latexTables.empty(),
		FromLatin1("latex-markdown-test.md table count"),
		&ok);
	if (!latexTables.empty()) {
		const auto &table = *latexTables[0];
		Check(
			HasTableHeaderRow(table),
			FromLatin1("latex-markdown-test.md table header row"),
			&ok);
		Check(
			TableColumnCount(table) == 3,
			FromLatin1("latex-markdown-test.md table column count"),
			&ok);
		Check(
			HasSequentialTableColumns(table),
			FromLatin1("latex-markdown-test.md table column order"),
			&ok);
	}
	const auto tableMath = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::Table,
		313,
		318);
	Check(
		tableMath != nullptr,
		FromLatin1("latex-markdown-test.md table range"),
		&ok);
	if (tableMath) {
		Check(
			!HasKind(*tableMath, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md table display math stays literal"),
			&ok);
		Check(
			!HasKind(*tableMath, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md table inline math stays literal"),
			&ok);
	}
	Check(
		CountFormulasInLineRange(latex, MathKind::Inline, 315, 318) == 12,
		FromLatin1("latex-markdown-test.md table inline formulas preserved"),
		&ok);
	Check(
		CountFormulasInLineRange(latex, MathKind::Display, 315, 318) == 0,
		FromLatin1("latex-markdown-test.md table display formulas absent"),
		&ok);
	const auto headerMath = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::Heading,
		322,
		322);
	Check(
		headerMath != nullptr,
		FromLatin1("latex-markdown-test.md heading range"),
		&ok);
	if (headerMath) {
		Check(
			!HasKind(*headerMath, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md heading display math stays inline"),
			&ok);
		Check(
			!HasKind(*headerMath, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md heading inline math stays literal"),
			&ok);
	}
	Check(
		headerMath
			&& HasTextContaining(
				*headerMath,
				FromLatin1("$ax^2 + bx + c = 0$")),
		FromLatin1("latex-markdown-test.md heading inline formula preserved"),
		&ok);
	const auto strongMath = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::Strong,
		326,
		326);
	Check(
		strongMath != nullptr,
		FromLatin1("latex-markdown-test.md strong range"),
		&ok);
	if (strongMath) {
		Check(
			!HasKind(*strongMath, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md strong display math stays inline"),
			&ok);
		Check(
			!HasKind(*strongMath, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md strong inline math stays literal"),
			&ok);
	}
	Check(
		HasFormulaOnLine(latex, 326, FromLatin1("E = mc^2")),
		FromLatin1("latex-markdown-test.md strong inline formula preserved"),
		&ok);
	const auto emphasisMath = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::Emphasis,
		328,
		328);
	Check(
		emphasisMath != nullptr,
		FromLatin1("latex-markdown-test.md emphasis range"),
		&ok);
	if (emphasisMath) {
		Check(
			!HasKind(*emphasisMath, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md emphasis display math stays inline"),
			&ok);
		Check(
			!HasKind(*emphasisMath, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md emphasis inline math stays literal"),
			&ok);
	}
	Check(
		HasFormulaOnLine(latex, 328, FromLatin1("\\pi \\approx 3.14")),
		FromLatin1("latex-markdown-test.md emphasis inline formula preserved"),
		&ok);
	const auto fencedCode = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::CodeBlock,
		332,
		334);
	Check(
		fencedCode != nullptr,
		FromLatin1("latex-markdown-test.md fenced code range"),
		&ok);
	if (fencedCode) {
		Check(
			!HasKind(*fencedCode, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md fenced code display math stays literal"),
			&ok);
		Check(
			!HasKind(*fencedCode, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md fenced code inline math stays literal"),
			&ok);
	}
	const auto inlineCode = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::InlineCode,
		336,
		336);
	Check(
		inlineCode != nullptr,
		FromLatin1("latex-markdown-test.md inline code range"),
		&ok);
	if (inlineCode) {
		Check(
			!HasKind(*inlineCode, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md inline code display math stays literal"),
			&ok);
		Check(
			!HasKind(*inlineCode, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md inline code math stays literal"),
			&ok);
	}
	Check(
		CountFormulasInLineRange(latex, MathKind::Inline, 332, 336) == 0,
		FromLatin1("latex-markdown-test.md code inline formulas excluded"),
		&ok);
	Check(
		CountFormulasInLineRange(latex, MathKind::Display, 332, 336) == 0,
		FromLatin1("latex-markdown-test.md code display formulas excluded"),
		&ok);
	Check(
		!HasFormulaInLineRange(latex, 281, 281),
		FromLatin1("latex-markdown-test.md line 281 exclusion"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 285, FromLatin1("5x + 3")),
		FromLatin1("latex-markdown-test.md line 285 formula"),
		&ok);
	Check(
		!HasFormulaInLineRange(latex, 332, 340),
		FromLatin1("latex-markdown-test.md lines 332-340 exclusions"),
		&ok);

	CheckInlineHtmlCoverage(args.dump, &ok);
	CheckValidationEdges(&ok);

	return ok ? 0 : 1;
}
