#include "iv/markdown/iv_markdown_document.h"
#include "iv/markdown/iv_markdown_math_renderer.h"
#include "iv/markdown/iv_markdown_microtex.h"
#include "iv/markdown/iv_markdown_parse.h"
#include "iv/markdown/iv_markdown_prepare.h"

#include "ui/style/style_core.h"
#include "ui/style/style_core_scale.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QString>
#include <QtGui/QGuiApplication>

#include <rpl/never.h>

#include <atomic>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>

namespace {

using namespace Iv::Markdown;

struct Args {
	QString markdownPath;
	QString latexMarkdownPath;
	bool dump = false;
	bool inlineHtml = false;
	bool ok = true;
	QString error;
};

struct PreparedFixture {
	QString label;
	QString path;
	PreparedDocument parsed;
	PreparedResult prepared;
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

[[nodiscard]] QString AbsolutePath(const QString &path) {
	return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

[[nodiscard]] QString PrepareFailureReason(
		const PrepareFailureStatus &failure) {
	return !failure.debugReason.isEmpty()
		? failure.debugReason
		: QString::number(int(failure.terminal));
}

[[nodiscard]] PreparedResult PrepareParsedDocumentForTest(
		const PreparedDocument &document,
		const QString &sourcePath,
		const std::shared_ptr<MathRenderer> &renderer,
		MarkdownStyleSnapshot style = CaptureMarkdownStyleSnapshot()) {
	return PrepareSynchronously({
		.document = std::make_shared<const PreparedDocument>(document),
		.renderer = renderer,
		.style = std::move(style),
		.generation = 1,
		.sourcePath = AbsolutePath(sourcePath),
		.cancelled = std::make_shared<std::atomic_bool>(false),
	});
}

[[nodiscard]] int CountPreparedFormulaSlots(const PreparedResult &prepared) {
	auto result = 0;
	for (const auto &slot : prepared.formulas) {
		if (slot.present) {
			++result;
		}
	}
	return result;
}

void PrintPrepareSummary(
		const QString &label,
		const PreparedResult &prepared) {
	auto line = label;
	line.append(FromLatin1(" prepare_ms="));
	line.append(QString::number(prepared.debug.prepareMs));
	line.append(FromLatin1(" formula_ms="));
	line.append(QString::number(prepared.debug.formulaRenderMs));
	line.append(FromLatin1(" prepare_warnings="));
	line.append(QString::number(prepared.debug.prepareWarningCount));
	line.append(FromLatin1(" formula_warnings="));
	line.append(QString::number(prepared.debug.formulaWarningCount));
	line.append(FromLatin1(" prepared_formulas="));
	line.append(QString::number(CountPreparedFormulaSlots(prepared)));
	PrintLine(line);
}

[[nodiscard]] bool PrepareFixture(
		const QString &path,
		const QString &label,
		const std::shared_ptr<MathRenderer> &renderer,
		PreparedFixture *fixture) {
	auto parsed = PreparedDocument();
	if (!ParseFixture(path, label, &parsed)) {
		return false;
	}
	auto prepared = PrepareParsedDocumentForTest(parsed, path, renderer);
	if (prepared.cancelled) {
		PrintError(label + FromLatin1(" prepare-cancelled"));
		return false;
	}
	if (prepared.failure.failed()) {
		PrintError(
			label + FromLatin1(" prepare-failed: ")
				+ PrepareFailureReason(prepared.failure));
		return false;
	}
	PrintPrepareSummary(label, prepared);
	if (fixture) {
		fixture->label = label;
		fixture->path = AbsolutePath(path);
		fixture->parsed = std::move(parsed);
		fixture->prepared = std::move(prepared);
	}
	return true;
}

template <typename Callback>
void ForEachPreparedBlock(
		const std::vector<PreparedBlock> &blocks,
		Callback &&callback) {
	for (const auto &block : blocks) {
		callback(block);
		ForEachPreparedBlock(block.children, callback);
	}
}

template <typename Callback>
void ForEachPreparedLink(
		const std::vector<PreparedBlock> &blocks,
		Callback &&callback) {
	ForEachPreparedBlock(blocks, [&](const PreparedBlock &block) {
		for (const auto &link : block.links) {
			callback(link);
		}
		for (const auto &row : block.tableRows) {
			for (const auto &cell : row.cells) {
				for (const auto &link : cell.links) {
					callback(link);
				}
			}
		}
	});
}

template <typename Callback>
void ForEachPreparedInlineObject(
		const std::vector<PreparedBlock> &blocks,
		Callback &&callback) {
	ForEachPreparedBlock(blocks, [&](const PreparedBlock &block) {
		for (const auto &object : block.inlineObjects) {
			callback(object);
		}
		for (const auto &row : block.tableRows) {
			for (const auto &cell : row.cells) {
				for (const auto &object : cell.inlineObjects) {
					callback(object);
				}
			}
		}
	});
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
	const auto &limits = ParseLimitsForIv();
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

	const auto oversizedSource = QByteArray(limits.maxSourceBytes + 1, 'a');
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
	oversizedFormula.reserve(limits.maxFormulaBytes + 2);
	oversizedFormula.append('$');
	oversizedFormula.append(QByteArray(limits.maxFormulaBytes + 1, '+'));
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

void CheckPrepareCoverage(
		const PreparedFixture &markdownFixture,
		const PreparedFixture &latexFixture,
		bool *ok) {
	const auto &markdown = markdownFixture.prepared;
	const auto &latex = latexFixture.prepared;

	const auto preparedHasInlineCopySource = [&](
			const std::vector<PreparedBlock> &blocks,
			const QString &copySource) {
		auto found = false;
		ForEachPreparedInlineObject(blocks, [&](const PreparedInlineObject &object) {
			if (object.copySource == copySource) {
				found = true;
			}
		});
		return found;
	};
	const auto blockHasInlineCopySource = [](
			const PreparedBlock &block,
			const QString &copySource) {
		for (const auto &object : block.inlineObjects) {
			if (object.copySource == copySource) {
				return true;
			}
		}
		return false;
	};

	const auto inlineCopySourceFound = preparedHasInlineCopySource(
		markdown.blocks.blocks,
		FromLatin1("$a^2 + b^2 = c^2$"));
	Check(
		inlineCopySourceFound,
		FromLatin1("markdown-example.md prepared inline formula copySource"),
		ok);

	auto markdownTables = std::vector<const PreparedBlock*>();
	const PreparedBlock *markdownDisplayMath = nullptr;
	const PreparedBlock *detailsBlock = nullptr;
	const PreparedBlock *footnoteList = nullptr;
	ForEachPreparedBlock(markdown.blocks.blocks, [&](const PreparedBlock &block) {
		if (block.kind == PreparedBlockKind::Table) {
			markdownTables.push_back(&block);
		}
		if (!markdownDisplayMath
			&& block.kind == PreparedBlockKind::DisplayMath
			&& block.formulaTex.trimmed()
				== FromLatin1("\\int_0^1 x^2\\,dx = \\frac{1}{3}")) {
			markdownDisplayMath = &block;
		}
		if (!detailsBlock
			&& block.kind == PreparedBlockKind::Details
			&& block.text.text.contains(
				FromLatin1("Click to expand details/summary block"))) {
			detailsBlock = &block;
		}
		if (!footnoteList
			&& block.kind == PreparedBlockKind::List
			&& block.listKind == ListKind::Ordered
			&& block.children.size() >= 2
			&& block.children[0].anchorId == FromLatin1("fn-1")
			&& block.children[1].anchorId == FromLatin1("fn-2")) {
			footnoteList = &block;
		}
	});
	Check(
		markdownDisplayMath != nullptr,
		FromLatin1("markdown-example.md prepared display math formulaTex"),
		ok);
	Check(
		markdownTables.size() >= 2,
		FromLatin1("markdown-example.md prepared table count"),
		ok);
	if (markdownTables.size() >= 2) {
		const auto &firstTable = *markdownTables[0];
		const auto &secondTable = *markdownTables[1];
		Check(
			firstTable.tableColumnCount == 3,
			FromLatin1("markdown-example.md prepared first table column count"),
			ok);
		Check(
			firstTable.tableRows.size() == 4,
			FromLatin1("markdown-example.md prepared first table row count"),
			ok);
		if (firstTable.tableRows.size() == 4) {
			Check(
				firstTable.tableRows[0].header,
				FromLatin1("markdown-example.md prepared first table header row"),
				ok);
			Check(
				firstTable.tableRows[0].cells.size() == 3
					&& firstTable.tableRows[1].cells.size() == 3,
				FromLatin1("markdown-example.md prepared first table cell shape"),
				ok);
		}
		Check(
			secondTable.tableAlignments.size() == 3
				&& secondTable.tableAlignments[0] == TableAlignment::Left
				&& secondTable.tableAlignments[1] == TableAlignment::Center
				&& secondTable.tableAlignments[2] == TableAlignment::Right,
			FromLatin1("markdown-example.md prepared second table alignments"),
			ok);
	}
	Check(
		detailsBlock != nullptr,
		FromLatin1("markdown-example.md prepared details block"),
		ok);
	if (detailsBlock) {
		Check(
			detailsBlock->collapsed,
			FromLatin1("markdown-example.md prepared details collapsed"),
			ok);
		Check(
			!detailsBlock->children.empty()
				&& detailsBlock->children[0].kind == PreparedBlockKind::Paragraph
				&& detailsBlock->children[0].text.text.contains(
					FromLatin1("Hidden content inside details.")),
			FromLatin1("markdown-example.md prepared details body"),
			ok);
	}
	Check(
		footnoteList != nullptr,
		FromLatin1("markdown-example.md prepared footnote list"),
		ok);
	auto footnoteReferenceOne = false;
	auto footnoteReferenceTwo = false;
	auto footnoteBacklinkFound = false;
	ForEachPreparedLink(markdown.blocks.blocks, [&](const PreparedLink &link) {
		if (link.kind == PreparedLinkKind::Footnote
			&& link.target == FromLatin1("fn-1")) {
			footnoteReferenceOne = true;
		}
		if (link.kind == PreparedLinkKind::Footnote
			&& link.target == FromLatin1("fn-2")) {
			footnoteReferenceTwo = true;
		}
		if (link.kind == PreparedLinkKind::FootnoteBacklink
			&& !link.target.isEmpty()) {
			footnoteBacklinkFound = true;
		}
	});
	Check(
		footnoteReferenceOne && footnoteReferenceTwo,
		FromLatin1("markdown-example.md prepared footnote references"),
		ok);
	Check(
		footnoteBacklinkFound,
		FromLatin1("markdown-example.md prepared footnote backlink"),
		ok);

	auto latexTables = std::vector<const PreparedBlock*>();
	const PreparedBlock *latexHeadingBlock = nullptr;
	ForEachPreparedBlock(latex.blocks.blocks, [&](const PreparedBlock &block) {
		if (block.kind == PreparedBlockKind::Table) {
			latexTables.push_back(&block);
		}
		if (!latexHeadingBlock
			&& block.kind == PreparedBlockKind::Heading
			&& block.headingLevel == 4
			&& block.text.text.contains(FromLatin1("The equation"))) {
			latexHeadingBlock = &block;
		}
	});
	const auto latexTableCopySourceFound
		= preparedHasInlineCopySource(
			latex.blocks.blocks,
			FromLatin1("$x^n$"))
		|| preparedHasInlineCopySource(
			latex.blocks.blocks,
			FromLatin1("$\\frac{x^{n+1}}{n+1}$"));
	Check(
		!latexTables.empty(),
		FromLatin1("latex-markdown-test.md prepared table count"),
		ok);
	if (!latexTables.empty()) {
		const auto &table = *latexTables[0];
		Check(
			table.tableColumnCount == 3,
			FromLatin1("latex-markdown-test.md prepared table column count"),
			ok);
		Check(
			table.tableRows.size() == 5,
			FromLatin1("latex-markdown-test.md prepared table row count"),
			ok);
		if (table.tableRows.size() == 5) {
			Check(
				table.tableRows[0].header,
				FromLatin1("latex-markdown-test.md prepared table header row"),
				ok);
			Check(
				table.tableRows[1].cells.size() == 3,
				FromLatin1("latex-markdown-test.md prepared table cell count"),
				ok);
		}
	}
	Check(
		latexTableCopySourceFound,
		FromLatin1("latex-markdown-test.md prepared table inline formula copySource"),
		ok);
	Check(
		preparedHasInlineCopySource(
			latex.blocks.blocks,
			FromLatin1("$\\int_0^1 x \\, dx$")),
		FromLatin1("latex-markdown-test.md prepared line 71 inline copySource"),
		ok);
	Check(
		preparedHasInlineCopySource(
			latex.blocks.blocks,
			FromLatin1(
				"$\\mathbb{E}[X] = \\int_{-\\infty}^{\\infty} x f(x) \\, dx$")),
		FromLatin1("latex-markdown-test.md prepared line 259 inline copySource"),
		ok);
	Check(
		preparedHasInlineCopySource(
			latex.blocks.blocks,
			FromLatin1("$ax^2 + bx + c = 0$")),
		FromLatin1("latex-markdown-test.md prepared line 322 inline copySource"),
		ok);
	Check(
		preparedHasInlineCopySource(
			latex.blocks.blocks,
			FromLatin1("$a\\,b$")),
		FromLatin1("latex-markdown-test.md prepared line 381 inline copySource"),
		ok);
	Check(
		preparedHasInlineCopySource(
			latex.blocks.blocks,
			FromLatin1("$a\\:b$")),
		FromLatin1("latex-markdown-test.md prepared line 383 inline copySource"),
		ok);
	Check(
		preparedHasInlineCopySource(
			latex.blocks.blocks,
			FromLatin1("$a\\;b$")),
		FromLatin1("latex-markdown-test.md prepared line 385 inline copySource"),
		ok);
	Check(
		preparedHasInlineCopySource(
			latex.blocks.blocks,
			FromLatin1("$a\\!b$")),
		FromLatin1("latex-markdown-test.md prepared line 391 inline copySource"),
		ok);
	Check(
		latexHeadingBlock != nullptr,
		FromLatin1("latex-markdown-test.md prepared heading block"),
		ok);
	if (latexHeadingBlock) {
		Check(
			blockHasInlineCopySource(
				*latexHeadingBlock,
				FromLatin1("$ax^2 + bx + c = 0$")),
			FromLatin1("latex-markdown-test.md prepared heading inline object"),
			ok);
		Check(
			!latexHeadingBlock->text.text.contains(
				FromLatin1("$ax^2 + bx + c = 0$")),
			FromLatin1("latex-markdown-test.md prepared heading removes raw formula"),
			ok);
	}

	const auto &limits = PrepareTableRenderLimitsForIv();
	auto overflowTable = QByteArray("| A | B |\n| --- | --- |\n");
	for (auto i = 0; i != limits.maxRows; ++i) {
		overflowTable.append("| row ");
		overflowTable.append(QByteArray::number(i));
		overflowTable.append(" | value |\n");
	}
	const auto overflowLabel = FromLatin1("generated-overflow-table.md");
	const auto overflowParsed = ParseMarkdownForIv(
		overflowTable,
		ParseOptions{ overflowLabel });
	Check(
		overflowParsed.ok,
		overflowLabel + FromLatin1(" parse failed: ") + overflowParsed.error,
		ok);
	if (overflowParsed.ok) {
		const auto overflowPrepared = PrepareParsedDocumentForTest(
			overflowParsed.document,
			overflowLabel,
			std::make_shared<MathRenderer>());
		Check(
			!overflowPrepared.cancelled,
			overflowLabel + FromLatin1(" prepare cancelled"),
			ok);
		Check(
			!overflowPrepared.failure.failed(),
			overflowLabel + FromLatin1(" prepare failure: ")
				+ PrepareFailureReason(overflowPrepared.failure),
			ok);
		auto overflowTableBlocks = 0;
		ForEachPreparedBlock(
			overflowPrepared.blocks.blocks,
			[&](const PreparedBlock &block) {
				if (block.kind == PreparedBlockKind::Table) {
					++overflowTableBlocks;
				}
			});
		Check(
			overflowPrepared.debug.prepareWarningCount > 0,
			overflowLabel + FromLatin1(" flatten warning count"),
			ok);
		Check(
			overflowTableBlocks == 0,
			overflowLabel + FromLatin1(" flattened table block removed"),
			ok);
		Check(
			!overflowPrepared.blocks.blocks.empty(),
			overflowLabel + FromLatin1(" flattened fallback blocks present"),
			ok);
	}
}

void CheckPrepareLinkClassification(
		const QString &sourcePath,
		bool *ok) {
	const auto label = FromLatin1("generated-relative-links.md");
	const auto parsed = ParseMarkdownForIv(
		QByteArray(
			"[Local](./docs/getting-started.md#section-1)\n"
			"[Rejected](../outside.md)\n"),
		ParseOptions{ label });
	Check(
		parsed.ok,
		label + FromLatin1(" parse failed: ") + parsed.error,
		ok);
	if (!parsed.ok) {
		return;
	}
	const auto prepared = PrepareParsedDocumentForTest(
		parsed.document,
		sourcePath,
		std::make_shared<MathRenderer>());
	Check(
		!prepared.cancelled,
		label + FromLatin1(" prepare cancelled"),
		ok);
	Check(
		!prepared.failure.failed(),
		label + FromLatin1(" prepare failure: ")
			+ PrepareFailureReason(prepared.failure),
		ok);
	const auto expectedLocalTarget = QDir(
		QFileInfo(sourcePath).absolutePath()).absoluteFilePath(
			FromLatin1("docs/getting-started.md"));
	auto foundLocal = false;
	auto foundRejected = false;
	ForEachPreparedLink(prepared.blocks.blocks, [&](const PreparedLink &link) {
		if (link.kind == PreparedLinkKind::LocalFile
			&& link.target == QDir::cleanPath(expectedLocalTarget)
			&& link.fragment == FromLatin1("section-1")
			&& link.copyText
				== FromLatin1("./docs/getting-started.md#section-1")) {
			foundLocal = true;
		}
		if (link.kind == PreparedLinkKind::RejectedRelative
			&& link.copyText == FromLatin1("../outside.md")) {
			foundRejected = true;
		}
	});
	Check(
		foundLocal,
		FromLatin1("generated-relative-links.md local markdown classification"),
		ok);
	Check(
		foundRejected,
		FromLatin1("generated-relative-links.md rejected relative classification"),
		ok);
}

void CheckPrepareRenderSmoke(
		const PreparedFixture &markdownFixture,
		const PreparedFixture &latexFixture,
		bool *ok) {
	Check(
		MicrotexBackendLinked(),
		FromLatin1("microtex backend should be linked"),
		ok);
	auto renderer = std::make_shared<MathRenderer>();
	const auto firstMarkdown = PrepareParsedDocumentForTest(
		markdownFixture.parsed,
		markdownFixture.path,
		renderer);
	const auto firstLatex = PrepareParsedDocumentForTest(
		latexFixture.parsed,
		latexFixture.path,
		renderer);
	Check(
		!firstMarkdown.cancelled && !firstLatex.cancelled,
		FromLatin1("prepare cache smoke first pass cancelled"),
		ok);
	Check(
		!firstMarkdown.failure.failed() && !firstLatex.failure.failed(),
		FromLatin1("prepare cache smoke first pass failed"),
		ok);
	const auto firstCounters = renderer->debugCounters();
	Check(
		renderer->cacheUsageBytes() > 0,
		FromLatin1("prepare cache smoke first pass cache bytes"),
		ok);
	renderer->resetDebugCounters();
	const auto secondMarkdown = PrepareParsedDocumentForTest(
		markdownFixture.parsed,
		markdownFixture.path,
		renderer);
	const auto secondLatex = PrepareParsedDocumentForTest(
		latexFixture.parsed,
		latexFixture.path,
		renderer);
	Check(
		!secondMarkdown.cancelled && !secondLatex.cancelled,
		FromLatin1("prepare cache smoke second pass cancelled"),
		ok);
	Check(
		!secondMarkdown.failure.failed() && !secondLatex.failure.failed(),
		FromLatin1("prepare cache smoke second pass failed"),
		ok);
	const auto secondCounters = renderer->debugCounters();
	const auto expectedHits = CountPreparedFormulaSlots(firstMarkdown)
		+ CountPreparedFormulaSlots(firstLatex);
	Check(
		secondCounters.hits >= expectedHits,
		FromLatin1("prepare cache smoke second pass cache hits"),
		ok);
	Check(
		secondCounters.misses == 0,
		FromLatin1("prepare cache smoke second pass cache misses"),
		ok);
	auto smokeLine = FromLatin1("prepare-cache-smoke");
	smokeLine.append(FromLatin1(" first_hits="));
	smokeLine.append(QString::number(firstCounters.hits));
	smokeLine.append(FromLatin1(" first_misses="));
	smokeLine.append(QString::number(firstCounters.misses));
	smokeLine.append(FromLatin1(" second_hits="));
	smokeLine.append(QString::number(secondCounters.hits));
	smokeLine.append(FromLatin1(" second_misses="));
	smokeLine.append(QString::number(secondCounters.misses));
	smokeLine.append(FromLatin1(" cache_bytes="));
	smokeLine.append(QString::number(secondCounters.cacheBytes));
	PrintLine(smokeLine);

	const auto failureLabel = FromLatin1("generated-formula-cap.md");
	const auto failureParsed = ParseMarkdownForIv(
		QByteArray("$$\nE = mc^2\n$$\n"),
		ParseOptions{ failureLabel });
	Check(
		failureParsed.ok,
		failureLabel + FromLatin1(" parse failed: ") + failureParsed.error,
		ok);
	if (!failureParsed.ok) {
		return;
	}
	auto failureStyle = CaptureMarkdownStyleSnapshot();
	failureStyle.displayMathMaxRenderWidth = 1;
	const auto failurePrepared = PrepareParsedDocumentForTest(
		failureParsed.document,
		failureLabel,
		std::make_shared<MathRenderer>(),
		std::move(failureStyle));
	Check(
		!failurePrepared.cancelled,
		failureLabel + FromLatin1(" prepare cancelled"),
		ok);
	Check(
		!failurePrepared.failure.failed(),
		failureLabel + FromLatin1(" terminal prepare failure"),
		ok);
	Check(
		failurePrepared.debug.formulaWarningCount > 0,
		failureLabel + FromLatin1(" formula warning count"),
		ok);
	auto failedFormulaFound = false;
	for (const auto &slot : failurePrepared.formulas) {
		if (!slot.present) {
			continue;
		}
		if (!slot.rendered.success
			&& (slot.rendered.tooLarge || slot.rendered.overflow)) {
			failedFormulaFound = true;
		}
	}
	Check(
		failedFormulaFound,
		failureLabel + FromLatin1(" formula cap fallback result"),
		ok);

	const auto &prepareLimits = PrepareLimitsForIv();
	auto blockLimitSource = QByteArray();
	for (auto i = 0; i != (prepareLimits.maxPreparedBlocks + 1); ++i) {
		blockLimitSource.append("Paragraph ");
		blockLimitSource.append(QByteArray::number(i));
		blockLimitSource.append("\n\n");
	}
	const auto blockLimitLabel = FromLatin1("generated-prepare-block-limit.md");
	const auto blockLimitParsed = ParseMarkdownForIv(
		blockLimitSource,
		ParseOptions{ blockLimitLabel });
	Check(
		blockLimitParsed.ok,
		blockLimitLabel + FromLatin1(" parse failed: ") + blockLimitParsed.error,
		ok);
	if (blockLimitParsed.ok) {
		const auto blockLimitPrepared = PrepareParsedDocumentForTest(
			blockLimitParsed.document,
			blockLimitLabel,
			std::make_shared<MathRenderer>());
		Check(
			!blockLimitPrepared.cancelled,
			blockLimitLabel + FromLatin1(" prepare cancelled"),
			ok);
		Check(
			blockLimitPrepared.failure.failed(),
			blockLimitLabel + FromLatin1(
				" missing real terminal prepare failure"),
			ok);
		Check(
			blockLimitPrepared.failure.terminal
				== PrepareTerminalFailure::DocumentTooLarge,
			blockLimitLabel + FromLatin1(
				" real terminal prepare failure kind"),
			ok);
		Check(
			PrepareFailureReason(blockLimitPrepared.failure)
				== FromLatin1("prepared-block-limit"),
			blockLimitLabel + FromLatin1(
				" real terminal prepare failure reason"),
			ok);
		Check(
			blockLimitPrepared.blocks.blocks.empty(),
			blockLimitLabel + FromLatin1(
				" real terminal prepare clears blocks"),
			ok);
	}

	const auto invalidStyleLabel = FromLatin1(
		"generated-invalid-style-internal.md");
	auto invalidStyle = CaptureMarkdownStyleSnapshot();
	invalidStyle.devicePixelRatio = 0;
	const auto invalidStylePrepared = PrepareParsedDocumentForTest(
		markdownFixture.parsed,
		markdownFixture.path,
		std::make_shared<MathRenderer>(),
		std::move(invalidStyle));
	Check(
		!invalidStylePrepared.cancelled,
		invalidStyleLabel + FromLatin1(" synthetic prepare cancelled"),
		ok);
	Check(
		invalidStylePrepared.failure.failed(),
		invalidStyleLabel + FromLatin1(
			" missing synthetic terminal prepare failure"),
		ok);
	Check(
		invalidStylePrepared.failure.terminal
			== PrepareTerminalFailure::InvalidStyle,
		invalidStyleLabel + FromLatin1(
			" synthetic terminal prepare failure kind"),
		ok);
	Check(
		PrepareFailureReason(invalidStylePrepared.failure)
			== FromLatin1("invalid-device-pixel-ratio"),
		invalidStyleLabel + FromLatin1(
			" synthetic terminal prepare failure reason"),
		ok);
	Check(
		invalidStylePrepared.blocks.blocks.empty(),
		invalidStyleLabel + FromLatin1(
			" synthetic terminal prepare clears blocks"),
		ok);
}

[[nodiscard]] int RunTests(int argc, char **argv) {
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

	auto fixtureRenderer = std::make_shared<MathRenderer>();
	auto markdownFixture = PreparedFixture();
	if (!PrepareFixture(
			args.markdownPath,
			FromLatin1("markdown-example.md"),
			fixtureRenderer,
			&markdownFixture)) {
		return 1;
	}
	if (args.dump) {
		PrintLine(DumpForDebug(markdownFixture.parsed));
	}

	auto latexFixture = PreparedFixture();
	if (!PrepareFixture(
			args.latexMarkdownPath,
			FromLatin1("latex-markdown-test.md"),
			fixtureRenderer,
			&latexFixture)) {
		return 1;
	}
	if (args.dump) {
		PrintLine(DumpForDebug(latexFixture.parsed));
	}

	const auto &markdown = markdownFixture.parsed;
	const auto &latex = latexFixture.parsed;
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
		CountFormulas(latex, MathKind::Inline) == 100,
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
	Check(
		HasFormulaOnLine(latex, 71, FromLatin1("\\int_0^1 x \\, dx")),
		FromLatin1("latex-markdown-test.md line 71 inline formula"),
		&ok);
	Check(
		HasFormulaOnLine(
			latex,
			259,
			FromLatin1(
				"\\mathbb{E}[X] = \\int_{-\\infty}^{\\infty} x f(x) \\, dx")),
		FromLatin1("latex-markdown-test.md line 259 inline formula"),
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
	}
	Check(
		HasFormulaOnLine(latex, 322, FromLatin1("ax^2 + bx + c = 0")),
		FromLatin1("latex-markdown-test.md line 322 inline formula"),
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
	Check(
		HasFormulaOnLine(latex, 381, FromLatin1("a\\,b")),
		FromLatin1("latex-markdown-test.md line 381 inline formula"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 383, FromLatin1("a\\:b")),
		FromLatin1("latex-markdown-test.md line 383 inline formula"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 385, FromLatin1("a\\;b")),
		FromLatin1("latex-markdown-test.md line 385 inline formula"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 391, FromLatin1("a\\!b")),
		FromLatin1("latex-markdown-test.md line 391 inline formula"),
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

	CheckPrepareCoverage(markdownFixture, latexFixture, &ok);
	CheckPrepareLinkClassification(markdownFixture.path, &ok);
	CheckPrepareRenderSmoke(markdownFixture, latexFixture, &ok);
	CheckInlineHtmlCoverage(args.dump, &ok);
	CheckValidationEdges(&ok);

	return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	QCoreApplication::setAttribute(Qt::AA_Use96Dpi);
	auto application = QGuiApplication(argc, argv);
	(void)application;

	style::SetDevicePixelRatio(1);
	style::StartManager(style::kScaleDefault);
	const auto result = RunTests(argc, argv);
	style::StopManager();
	return result;
}

namespace crl {

rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}

} // namespace crl
