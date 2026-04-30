#include "iv/markdown/iv_markdown_parse.h"

#include "iv/markdown/iv_markdown_math.h"

#include "base/basic_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cmark-gfm.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm-core-extensions.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Iv::Markdown {
namespace {

constexpr auto kMaxSourceBytes = 4 * 1024 * 1024;
constexpr auto kMaxCmarkNodes = 100000;
constexpr auto kMaxNesting = 128;
constexpr auto kMaxFormulaBytes = 64 * 1024;
constexpr auto kMaxFormulaCount = 10000;

struct ParserDeleter {
	void operator()(cmark_parser *parser) const;
};

struct NodeDeleter {
	void operator()(cmark_node *node) const;
};

using ParserPointer = std::unique_ptr<cmark_parser, ParserDeleter>;
using NodePointer = std::unique_ptr<cmark_node, NodeDeleter>;

struct ParserState {
	const QByteArray &normalizedSource;
	const std::vector<int> &lineStarts;
	std::vector<bool> *mask = nullptr;
	std::vector<MathScanBlock> *scanBlocks = nullptr;
	ParseStats *stats = nullptr;
	QStringList *warnings = nullptr;
	QString error;
	bool failed = false;
};

struct ParsedDetailsBlock {
	QString summary;
	QString body;
	bool open = false;
	bool ok = false;
};

void ParserDeleter::operator()(cmark_parser *parser) const {
	if (parser) {
		cmark_parser_free(parser);
	}
}

void NodeDeleter::operator()(cmark_node *node) const {
	if (node) {
		cmark_node_free(node);
	}
}

[[nodiscard]] QString FromLatin1(const char *value) {
	return QString::fromLatin1(value);
}

[[nodiscard]] QString ExtensionError(const char *prefix, const char *name) {
	return FromLatin1("%1-%2").arg(
		FromLatin1(prefix),
		FromLatin1(name));
}

void AddWarning(ParserState *state, QString warning) {
	if (state && state->warnings && !warning.isEmpty()) {
		state->warnings->push_back(std::move(warning));
	}
}

[[nodiscard]] unsigned char ByteAt(const QByteArray &source, int index) {
	return static_cast<unsigned char>(source.at(index));
}

template <std::size_t Size>
[[nodiscard]] bool HasPrefix(
		const QByteArray &source,
		const std::array<unsigned char, Size> &prefix) {
	if (source.size() < static_cast<int>(Size)) {
		return false;
	}
	for (auto i = std::size_t(0); i != Size; ++i) {
		if (ByteAt(source, static_cast<int>(i)) != prefix[i]) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] ParseResult Failure(QString sourceName, QString error) {
	auto result = ParseResult();
	result.document = EmptyDocument(std::move(sourceName));
	result.error = std::move(error);
	result.ok = false;
	return result;
}

[[nodiscard]] MarkdownSourceValidationResult ValidationFailure(
		QString sourceName,
		QString error) {
	auto result = MarkdownSourceValidationResult();
	result.source.sourceName = std::move(sourceName);
	result.error = std::move(error);
	result.ok = false;
	return result;
}

[[nodiscard]] bool HasUtf8Bom(const QByteArray &source) {
	constexpr auto kUtf8Bom = std::array<unsigned char, 3>{
		0xEF,
		0xBB,
		0xBF,
	};
	return HasPrefix(source, kUtf8Bom);
}

[[nodiscard]] bool HasUnsupportedUnicodeBom(const QByteArray &source) {
	constexpr auto kUtf32Boms = std::array{
		std::array<unsigned char, 4>{ 0x00, 0x00, 0xFE, 0xFF },
		std::array<unsigned char, 4>{ 0xFF, 0xFE, 0x00, 0x00 },
	};
	for (const auto &bom : kUtf32Boms) {
		if (HasPrefix(source, bom)) {
			return true;
		}
	}
	constexpr auto kUtf16Boms = std::array{
		std::array<unsigned char, 2>{ 0xFE, 0xFF },
		std::array<unsigned char, 2>{ 0xFF, 0xFE },
	};
	for (const auto &bom : kUtf16Boms) {
		if (HasPrefix(source, bom)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QByteArray StripUtf8Bom(QByteArray source) {
	if (HasUtf8Bom(source)) {
		source.remove(0, 3);
	}
	return source;
}

[[nodiscard]] bool IsAllowedControl(unsigned char byte) {
	return byte == '\t' || byte == '\n' || byte == '\r';
}

[[nodiscard]] bool LooksBinary(const QByteArray &source) {
	const auto size = static_cast<int>(source.size());
	if (size == 0) {
		return false;
	}
	auto controlBytes = 0;
	for (auto i = 0; i != size; ++i) {
		const auto byte = ByteAt(source, i);
		if (byte == 0) {
			return true;
		} else if ((byte < 0x20 || byte == 0x7F)
			&& !IsAllowedControl(byte)) {
			++controlBytes;
		}
	}
	return (controlBytes * 10) > size;
}

[[nodiscard]] bool IsUtf8Continuation(unsigned char byte) {
	return (byte & 0xC0) == 0x80;
}

[[nodiscard]] bool IsValidCodepoint(int codepoint, int minimum) {
	return codepoint >= minimum
		&& codepoint <= 0x10FFFF
		&& (codepoint < 0xD800 || codepoint > 0xDFFF);
}

[[nodiscard]] bool IsValidUtf8(const QByteArray &source) {
	const auto size = static_cast<int>(source.size());
	for (auto i = 0; i != size; ++i) {
		const auto byte = ByteAt(source, i);
		if (byte <= 0x7F) {
			continue;
		}
		auto extraBytes = 0;
		auto codepoint = 0;
		auto minimum = 0;
		if (byte >= 0xC2 && byte <= 0xDF) {
			extraBytes = 1;
			codepoint = byte & 0x1F;
			minimum = 0x80;
		} else if (byte >= 0xE0 && byte <= 0xEF) {
			extraBytes = 2;
			codepoint = byte & 0x0F;
			minimum = 0x800;
		} else if (byte >= 0xF0 && byte <= 0xF4) {
			extraBytes = 3;
			codepoint = byte & 0x07;
			minimum = 0x10000;
		} else {
			return false;
		}
		if (i + extraBytes >= size) {
			return false;
		}
		for (auto j = 1; j <= extraBytes; ++j) {
			const auto continuation = ByteAt(source, i + j);
			if (!IsUtf8Continuation(continuation)) {
				return false;
			}
			codepoint = (codepoint << 6) | (continuation & 0x3F);
		}
		if (!IsValidCodepoint(codepoint, minimum)) {
			return false;
		}
		i += extraBytes;
	}
	return true;
}

void EnsureCmarkExtensionsRegistered() {
	static std::once_flag once;
	std::call_once(once, [] {
		cmark_gfm_core_extensions_ensure_registered();
	});
}

[[nodiscard]] bool AttachExtensions(cmark_parser *parser, QString *error) {
	if (!parser) {
		if (error) {
			*error = FromLatin1("cmark-parser-failed");
		}
		return false;
	}
	EnsureCmarkExtensionsRegistered();
	constexpr auto kExtensions = std::array<const char *, 5>{
		"table",
		"strikethrough",
		"autolink",
		"tagfilter",
		"tasklist",
	};
	for (const auto name : kExtensions) {
		const auto extension = cmark_find_syntax_extension(name);
		if (!extension) {
			if (error) {
				*error = ExtensionError("cmark-extension-missing", name);
			}
			return false;
		}
		if (!cmark_parser_attach_syntax_extension(parser, extension)) {
			if (error) {
				*error = ExtensionError("cmark-extension-attach-failed", name);
			}
			return false;
		}
	}
	if (error) {
		error->clear();
	}
	return true;
}

[[nodiscard]] QString FromCmarkString(const char *value) {
	return value ? QString::fromUtf8(value) : QString();
}

[[nodiscard]] QString RawTypeString(cmark_node *node) {
	return node ? FromCmarkString(cmark_node_get_type_string(node)) : QString();
}

[[nodiscard]] bool IsCoreNodeType(cmark_node_type type) {
	constexpr auto kCoreTypes = std::array{
		CMARK_NODE_DOCUMENT,
		CMARK_NODE_BLOCK_QUOTE,
		CMARK_NODE_LIST,
		CMARK_NODE_ITEM,
		CMARK_NODE_CODE_BLOCK,
		CMARK_NODE_HTML_BLOCK,
		CMARK_NODE_CUSTOM_BLOCK,
		CMARK_NODE_PARAGRAPH,
		CMARK_NODE_HEADING,
		CMARK_NODE_THEMATIC_BREAK,
		CMARK_NODE_FOOTNOTE_DEFINITION,
		CMARK_NODE_TEXT,
		CMARK_NODE_SOFTBREAK,
		CMARK_NODE_LINEBREAK,
		CMARK_NODE_CODE,
		CMARK_NODE_HTML_INLINE,
		CMARK_NODE_CUSTOM_INLINE,
		CMARK_NODE_EMPH,
		CMARK_NODE_STRONG,
		CMARK_NODE_LINK,
		CMARK_NODE_IMAGE,
		CMARK_NODE_FOOTNOTE_REFERENCE,
	};
	return std::find(kCoreTypes.begin(), kCoreTypes.end(), type)
		!= kCoreTypes.end();
}

[[nodiscard]] QString CmarkKind(cmark_node *node) {
	if (!node) {
		return FromLatin1("unknown");
	}
	const auto type = cmark_node_get_type(node);
	const auto raw = RawTypeString(node);
	if (!IsCoreNodeType(type)) {
		return raw.isEmpty() ? FromLatin1("unknown") : raw;
	}
	switch (type) {
	case CMARK_NODE_DOCUMENT: return FromLatin1("document");
	case CMARK_NODE_BLOCK_QUOTE: return FromLatin1("block_quote");
	case CMARK_NODE_LIST: return FromLatin1("list");
	case CMARK_NODE_ITEM: return FromLatin1("item");
	case CMARK_NODE_CODE_BLOCK: return FromLatin1("code_block");
	case CMARK_NODE_HTML_BLOCK: return FromLatin1("html_block");
	case CMARK_NODE_CUSTOM_BLOCK: return FromLatin1("custom_block");
	case CMARK_NODE_PARAGRAPH: return FromLatin1("paragraph");
	case CMARK_NODE_HEADING: return FromLatin1("heading");
	case CMARK_NODE_THEMATIC_BREAK: return FromLatin1("thematic_break");
	case CMARK_NODE_FOOTNOTE_DEFINITION:
		return FromLatin1("footnote_definition");
	case CMARK_NODE_TEXT: return FromLatin1("text");
	case CMARK_NODE_SOFTBREAK: return FromLatin1("softbreak");
	case CMARK_NODE_LINEBREAK: return FromLatin1("linebreak");
	case CMARK_NODE_CODE: return FromLatin1("code");
	case CMARK_NODE_HTML_INLINE: return FromLatin1("html_inline");
	case CMARK_NODE_CUSTOM_INLINE: return FromLatin1("custom_inline");
	case CMARK_NODE_EMPH: return FromLatin1("emph");
	case CMARK_NODE_STRONG: return FromLatin1("strong");
	case CMARK_NODE_LINK: return FromLatin1("link");
	case CMARK_NODE_IMAGE: return FromLatin1("image");
	case CMARK_NODE_FOOTNOTE_REFERENCE:
		return FromLatin1("footnote_reference");
	case CMARK_NODE_NONE: return FromLatin1("none");
	}
	return raw.isEmpty() ? FromLatin1("unknown") : raw;
}

[[nodiscard]] SourceRange RangeFromCmarkLines(
		const std::vector<int> &lineStarts,
		int sourceSize,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	auto result = SourceRange();
	result.startLine = startLine;
	result.startColumn = startColumn;
	result.endLine = endLine;
	result.endColumn = endColumn;
	const auto linesCount = static_cast<int>(lineStarts.size());
	if (startLine <= 0
		|| endLine <= 0
		|| startLine > linesCount
		|| endLine > linesCount) {
		return result;
	}
	const auto maxOffset = std::max(sourceSize, 0);
	const auto startOffset = lineStarts[startLine - 1]
		+ std::max(0, startColumn - 1);
	const auto endOffset = lineStarts[endLine - 1]
		+ std::max(0, endColumn);
	result.available = true;
	result.startOffset = std::clamp(startOffset, 0, maxOffset);
	result.endOffset = std::clamp(endOffset, 0, maxOffset);
	return result;
}

[[nodiscard]] SourceRange NodeRange(
		cmark_node *node,
		const std::vector<int> &lineStarts,
		int sourceSize) {
	return node
		? RangeFromCmarkLines(
			lineStarts,
			sourceSize,
			cmark_node_get_start_line(node),
			cmark_node_get_start_column(node),
			cmark_node_get_end_line(node),
			cmark_node_get_end_column(node))
		: SourceRange();
}

[[nodiscard, maybe_unused]] QString SourceSlice(
		const QByteArray &source,
		const SourceRange &range) {
	if (!range.available) {
		return QString();
	}
	const auto sourceSize = static_cast<int>(source.size());
	const auto start = std::clamp(range.startOffset, 0, sourceSize);
	const auto end = std::clamp(range.endOffset, start, sourceSize);
	const auto slice = source.mid(start, end - start);
	return QString::fromUtf8(slice.constData(), slice.size());
}

void OffsetToPosition(
		const std::vector<int> &lineStarts,
		int offset,
		int *line,
		int *column) {
	const auto clampedOffset = std::max(offset, 0);
	auto resultLine = 1;
	auto resultColumn = clampedOffset + 1;
	if (!lineStarts.empty()) {
		auto i = std::upper_bound(
			lineStarts.begin(),
			lineStarts.end(),
			clampedOffset);
		if (i != lineStarts.begin()) {
			--i;
			resultLine = static_cast<int>(i - lineStarts.begin()) + 1;
			resultColumn = clampedOffset - *i + 1;
		}
	}
	if (line) {
		*line = resultLine;
	}
	if (column) {
		*column = resultColumn;
	}
}

[[nodiscard]] SourceRange RangeForOffsets(
		const std::vector<int> &lineStarts,
		int sourceSize,
		int startOffset,
		int endOffset) {
	auto startLine = 0;
	auto startColumn = 0;
	auto endLine = 0;
	auto endColumn = 0;
	OffsetToPosition(lineStarts, startOffset, &startLine, &startColumn);
	OffsetToPosition(
		lineStarts,
		std::max(startOffset, endOffset - 1),
		&endLine,
		&endColumn);
	return RangeFromLineColumns(
		lineStarts,
		sourceSize,
		startLine,
		startColumn,
		endLine,
		endColumn);
}

template <std::size_t Size>
[[nodiscard]] bool IsAnyKind(
		const QString &kind,
		const std::array<const char *, Size> &values) {
	for (const auto value : values) {
		if (kind == FromLatin1(value)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasCmarkSourceRange(cmark_node *node) {
	return node
		&& cmark_node_get_start_line(node) > 0
		&& cmark_node_get_end_line(node) > 0;
}

[[nodiscard]] bool IsBlockNodeType(cmark_node_type type) {
	return (type & CMARK_NODE_TYPE_MASK) == CMARK_NODE_TYPE_BLOCK;
}

void MarkMaskRange(std::vector<bool> *mask, const SourceRange &range) {
	if (!mask || !range.available) {
		return;
	}
	const auto size = static_cast<int>(mask->size());
	const auto start = std::clamp(range.startOffset, 0, size);
	const auto end = std::clamp(range.endOffset, start, size);
	for (auto i = start; i != end; ++i) {
		(*mask)[i] = true;
	}
}

[[nodiscard]] bool IsMathMaskedNode(cmark_node *node) {
	if (!node) {
		return false;
	}
	constexpr auto kMaskedTypes = std::array{
		CMARK_NODE_CODE,
		CMARK_NODE_CODE_BLOCK,
		CMARK_NODE_HTML_INLINE,
		CMARK_NODE_HTML_BLOCK,
	};
	const auto type = cmark_node_get_type(node);
	return std::find(kMaskedTypes.begin(), kMaskedTypes.end(), type)
		!= kMaskedTypes.end();
}

[[nodiscard]] bool IsTaskListItem(cmark_node *node) {
	if (!node
		|| cmark_node_get_type(node) != CMARK_NODE_ITEM
		|| RawTypeString(node) != FromLatin1("tasklist")) {
		return false;
	}
	(void)cmark_gfm_extensions_get_tasklist_item_checked(node);
	return true;
}

[[nodiscard]] bool IsBlockForMathParent(cmark_node *node) {
	if (!node) {
		return false;
	}
	const auto kind = CmarkKind(node);
	constexpr auto kBlockKinds = std::array<const char *, 13>{
		"paragraph",
		"heading",
		"list",
		"item",
		"block_quote",
		"code_block",
		"html_block",
		"thematic_break",
		"table",
		"table_header",
		"table_row",
		"table_cell",
		"footnote_definition",
	};
	if (IsAnyKind(kind, kBlockKinds)) {
		return true;
	}
	const auto type = cmark_node_get_type(node);
	return !IsCoreNodeType(type)
		&& IsBlockNodeType(type)
		&& HasCmarkSourceRange(node);
}

void RecordCapabilities(cmark_node *node, ParserState *state) {
	if (!node || !state || !state->stats) {
		return;
	}
	const auto kind = CmarkKind(node);
	constexpr auto kTableKinds = std::array<const char *, 4>{
		"table",
		"table_header",
		"table_row",
		"table_cell",
	};
	if (IsAnyKind(kind, kTableKinds)) {
		state->stats->tablesSeen = true;
	}
	if (IsTaskListItem(node)) {
		state->stats->taskListsSeen = true;
	}
	if (kind == FromLatin1("strikethrough")) {
		state->stats->strikethroughSeen = true;
	} else if (kind == FromLatin1("footnote_reference")
		|| kind == FromLatin1("footnote_definition")) {
		state->stats->footnotesSeen = true;
	}
}

[[nodiscard]] bool FailScanMetadata(ParserState *state, const char *error) {
	if (state) {
		state->error = FromLatin1(error);
		state->failed = true;
	}
	return false;
}

[[nodiscard]] bool CollectScanMetadata(
		cmark_node *node,
		ParserState *state,
		int depth) {
	if (!node || !state || state->failed) {
		return false;
	}
	if (state->stats) {
		state->stats->maxDepth = std::max(state->stats->maxDepth, depth);
	}
	if (depth > kMaxNesting) {
		return FailScanMetadata(state, "cmark-nesting-too-deep");
	}
	if (state->stats) {
		++state->stats->cmarkNodeCount;
		if (state->stats->cmarkNodeCount > kMaxCmarkNodes) {
			return FailScanMetadata(state, "too-many-cmark-nodes");
		}
	}
	RecordCapabilities(node, state);
	const auto range = NodeRange(
		node,
		state->lineStarts,
		static_cast<int>(state->normalizedSource.size()));
	if (IsBlockForMathParent(node) && state->scanBlocks) {
		state->scanBlocks->push_back(MathScanBlock{ range, CmarkKind(node) });
	}
	if (IsMathMaskedNode(node)) {
		MarkMaskRange(state->mask, range);
	}
	for (auto child = cmark_node_first_child(node); child;) {
		const auto next = cmark_node_next(child);
		if (!CollectScanMetadata(child, state, depth + 1)) {
			return false;
		}
		child = next;
	}
	return true;
}

[[nodiscard]] NodeKind ExtensionNodeKind(const QString &raw) {
	struct Entry {
		const char *name = nullptr;
		NodeKind kind = NodeKind::Unsupported;
	};
	constexpr auto kEntries = std::array{
		Entry{ "strikethrough", NodeKind::Strike },
		Entry{ "table", NodeKind::Table },
		Entry{ "table_header", NodeKind::TableRow },
		Entry{ "table_row", NodeKind::TableRow },
		Entry{ "table_cell", NodeKind::TableCell },
	};
	for (const auto &entry : kEntries) {
		if (raw == FromLatin1(entry.name)) {
			return entry.kind;
		}
	}
	return NodeKind::Unsupported;
}

[[nodiscard]] NodeKind NodeKindFor(cmark_node *node) {
	if (!node) {
		return NodeKind::Unsupported;
	}
	const auto type = cmark_node_get_type(node);
	switch (type) {
	case CMARK_NODE_DOCUMENT: return NodeKind::Document;
	case CMARK_NODE_BLOCK_QUOTE: return NodeKind::Blockquote;
	case CMARK_NODE_LIST: return NodeKind::List;
	case CMARK_NODE_ITEM: return NodeKind::ListItem;
	case CMARK_NODE_CODE_BLOCK: return NodeKind::CodeBlock;
	case CMARK_NODE_HTML_BLOCK: return NodeKind::HtmlBlock;
	case CMARK_NODE_PARAGRAPH: return NodeKind::Paragraph;
	case CMARK_NODE_HEADING: return NodeKind::Heading;
	case CMARK_NODE_THEMATIC_BREAK: return NodeKind::ThematicBreak;
	case CMARK_NODE_FOOTNOTE_DEFINITION: return NodeKind::FootnoteDefinition;
	case CMARK_NODE_TEXT: return NodeKind::Text;
	case CMARK_NODE_SOFTBREAK: return NodeKind::SoftBreak;
	case CMARK_NODE_LINEBREAK: return NodeKind::LineBreak;
	case CMARK_NODE_CODE: return NodeKind::InlineCode;
	case CMARK_NODE_HTML_INLINE: return NodeKind::HtmlInline;
	case CMARK_NODE_EMPH: return NodeKind::Emphasis;
	case CMARK_NODE_STRONG: return NodeKind::Strong;
	case CMARK_NODE_LINK: return NodeKind::Link;
	case CMARK_NODE_FOOTNOTE_REFERENCE: return NodeKind::FootnoteReference;
	default: break;
	}
	return ExtensionNodeKind(RawTypeString(node));
}

[[nodiscard]] ListKind ListKindFor(cmark_node *node) {
	return (node && cmark_node_get_list_type(node) == CMARK_ORDERED_LIST)
		? ListKind::Ordered
		: ListKind::Bullet;
}

[[nodiscard]] ListDelimiter ListDelimiterFor(cmark_node *node) {
	if (!node) {
		return ListDelimiter::None;
	}
	switch (cmark_node_get_list_delim(node)) {
	case CMARK_PERIOD_DELIM: return ListDelimiter::Period;
	case CMARK_PAREN_DELIM: return ListDelimiter::Parenthesis;
	case CMARK_NO_DELIM: return ListDelimiter::None;
	}
	return ListDelimiter::None;
}

[[nodiscard]] TaskState TaskStateFor(cmark_node *node) {
	if (!IsTaskListItem(node)) {
		return TaskState::None;
	}
	return cmark_gfm_extensions_get_tasklist_item_checked(node)
		? TaskState::Checked
		: TaskState::Unchecked;
}

[[nodiscard]] TableAlignment TableAlignmentFor(uint8_t value) {
	switch (value) {
	case 'l': return TableAlignment::Left;
	case 'c': return TableAlignment::Center;
	case 'r': return TableAlignment::Right;
	default: return TableAlignment::None;
	}
}

[[nodiscard]] std::vector<TableAlignment> TableAlignmentsFor(cmark_node *node) {
	auto result = std::vector<TableAlignment>();
	if (!node) {
		return result;
	}
	const auto columns = cmark_gfm_extensions_get_table_columns(node);
	if (!columns) {
		return result;
	}
	result.reserve(columns);
	const auto alignments = cmark_gfm_extensions_get_table_alignments(node);
	for (auto i = uint16_t(0); i != columns; ++i) {
		result.push_back(
			alignments
				? TableAlignmentFor(alignments[i])
				: TableAlignment::None);
	}
	return result;
}

[[nodiscard]] bool TableRowIsHeader(cmark_node *node) {
	return node && cmark_gfm_extensions_get_table_row_is_header(node) != 0;
}

[[nodiscard]] int TableCellColumn(cmark_node *node) {
	if (!node) {
		return -1;
	}
	auto result = 0;
	for (auto previous = cmark_node_previous(node); previous;) {
		if (NodeKindFor(previous) == NodeKind::TableCell) {
			++result;
		}
		previous = cmark_node_previous(previous);
	}
	return result;
}

[[nodiscard]] QString NormalizeFragmentId(QString fragment) {
	fragment = QString::fromUtf8(
		QByteArray::fromPercentEncoding(fragment.toUtf8()));
	fragment = fragment.trimmed().toLower();
	while (fragment.startsWith(u"#"_q)) {
		fragment.remove(0, 1);
	}
	return fragment;
}

[[nodiscard]] QString AnchorIdBaseFromText(QString text) {
	text = text.trimmed().toLower();
	auto result = QString();
	auto pendingHyphen = false;
	for (const auto ch : text) {
		if (ch.isLetterOrNumber()) {
			if (pendingHyphen && !result.isEmpty()) {
				result.append(QChar('-'));
			}
			result.append(ch);
			pendingHyphen = false;
		} else if (!result.isEmpty()) {
			pendingHyphen = true;
		}
	}
	if (result.isEmpty()) {
		return u"section"_q;
	}
	return result;
}

[[nodiscard]] QString FootnoteDefinitionAnchorId(int ordinal) {
	return (ordinal > 0) ? (u"fn-"_q + QString::number(ordinal)) : QString();
}

[[nodiscard]] QString ExtractFootnoteLabel(
		QString raw,
		bool definition) {
	raw = raw.trimmed();
	if (!raw.startsWith(u"[^"_q)) {
		return QString();
	}
	const auto closing = raw.indexOf(u']');
	if (closing <= 2) {
		return QString();
	}
	if (definition && (closing + 1 >= raw.size() || raw[closing + 1] != u':')) {
		return QString();
	}
	return raw.mid(2, closing - 2).trimmed();
}

[[nodiscard]] bool ParseDetailsOpenAttribute(QString raw) {
	raw = raw.trimmed().toLower();
	if (raw.isEmpty()) {
		return false;
	}
	return (raw == u"open"_q)
		|| (raw == u"open=\"\""_q)
		|| (raw == u"open=''"_q)
		|| (raw == u"open=\"open\""_q)
		|| (raw == u"open='open'"_q);
}

[[nodiscard]] ParsedDetailsBlock ParseDetailsBlock(QString raw) {
	auto result = ParsedDetailsBlock();
	raw = raw.trimmed();
	if (!raw.startsWith(u"<details"_q, Qt::CaseInsensitive)
		|| !raw.endsWith(u"</details>"_q, Qt::CaseInsensitive)) {
		return result;
	}
	const auto openingEnd = raw.indexOf(QChar('>'));
	if (openingEnd < 0) {
		return result;
	}
	const auto openingAttributes = raw.mid(8, openingEnd - 8);
	if (!openingAttributes.trimmed().isEmpty()
		&& !ParseDetailsOpenAttribute(openingAttributes)) {
		return result;
	}
	result.open = ParseDetailsOpenAttribute(openingAttributes);
	auto inner = raw.mid(
		openingEnd + 1,
		raw.size() - openingEnd - 11);
	if (inner.contains(u"<details"_q, Qt::CaseInsensitive)) {
		return result;
	}
	inner = inner.trimmed();
	if (!inner.startsWith(u"<summary>"_q, Qt::CaseInsensitive)) {
		return result;
	}
	const auto summaryClosing = inner.indexOf(
		u"</summary>"_q,
		0,
		Qt::CaseInsensitive);
	if (summaryClosing < 0) {
		return result;
	}
	result.summary = inner.mid(9, summaryClosing - 9).trimmed();
	result.body = inner.mid(summaryClosing + 10).trimmed();
	result.ok = !result.summary.isEmpty();
	return result;
}

[[nodiscard]] QString PlainText(const MarkdownNode &node) {
	auto result = node.text;
	for (const auto &child : node.children) {
		result.append(PlainText(child));
	}
	return result;
}

[[nodiscard]] bool LinkLooksAutolink(const MarkdownNode &node) {
	const auto text = PlainText(node);
	if (text.isEmpty() || node.url.isEmpty()) {
		return false;
	}
	if (text == node.url) {
		return true;
	}
	const auto mailto = FromLatin1("mailto:");
	return node.url.startsWith(mailto, Qt::CaseInsensitive)
		&& text == node.url.mid(mailto.size());
}

void FillNodeAttributes(
		cmark_node *node,
		ParserState *state,
		MarkdownNode *out) {
	switch (out->kind) {
	case NodeKind::Text:
	case NodeKind::InlineCode:
		out->text = FromCmarkString(cmark_node_get_literal(node));
		break;
	case NodeKind::CodeBlock:
		out->text = FromCmarkString(cmark_node_get_literal(node));
		out->info = FromCmarkString(cmark_node_get_fence_info(node));
		break;
	case NodeKind::HtmlBlock: {
		out->raw = FromCmarkString(cmark_node_get_literal(node));
		const auto trimmed = out->raw.trimmed();
		if (trimmed.startsWith(u"<!--"_q) && trimmed.endsWith(u"-->"_q)) {
			out->htmlBlockKind = HtmlBlockKind::Comment;
		} else if (trimmed.startsWith(u"<details"_q, Qt::CaseInsensitive)) {
			const auto details = ParseDetailsBlock(out->raw);
			if (details.ok) {
				out->htmlBlockKind = HtmlBlockKind::Details;
				out->detailsSummary = details.summary;
				out->detailsBody = details.body;
				out->detailsOpen = details.open;
			} else {
				out->htmlBlockKind = HtmlBlockKind::Unsupported;
				AddWarning(
					state,
					FromLatin1("Malformed details block at %1:%2").arg(
						out->range.startLine
					).arg(
						out->range.startColumn));
			}
		} else if (!trimmed.isEmpty()) {
			out->htmlBlockKind = HtmlBlockKind::Unsupported;
			AddWarning(
				state,
				FromLatin1("Unsupported HTML block at %1:%2").arg(
					out->range.startLine
				).arg(
					out->range.startColumn));
		}
	} break;
	case NodeKind::HtmlInline:
		out->raw = FromCmarkString(cmark_node_get_literal(node));
		break;
	case NodeKind::SoftBreak:
	case NodeKind::LineBreak:
		out->text = FromLatin1("\n");
		break;
	case NodeKind::Heading:
		out->headingLevel = cmark_node_get_heading_level(node);
		break;
	case NodeKind::List:
		out->listKind = ListKindFor(node);
		out->listDelimiter = ListDelimiterFor(node);
		out->listStart = cmark_node_get_list_start(node);
		out->tight = cmark_node_get_list_tight(node) != 0;
		break;
	case NodeKind::ListItem:
		out->taskState = TaskStateFor(node);
		break;
	case NodeKind::Table:
		out->tableAlignments = TableAlignmentsFor(node);
		break;
	case NodeKind::TableRow:
		out->tableHeader = TableRowIsHeader(node);
		break;
	case NodeKind::TableCell:
		out->tableColumn = TableCellColumn(node);
		break;
	case NodeKind::Link:
		out->url = FromCmarkString(cmark_node_get_url(node));
		out->title = FromCmarkString(cmark_node_get_title(node));
		break;
	case NodeKind::FootnoteReference:
		out->raw = SourceSlice(state->normalizedSource, out->range);
		if (const auto parent = cmark_node_parent_footnote_def(node)) {
			out->footnoteLabel = FromCmarkString(cmark_node_get_literal(parent));
		}
		if (out->footnoteLabel.isEmpty()) {
			out->footnoteLabel = ExtractFootnoteLabel(out->raw, false);
		}
		break;
	case NodeKind::FootnoteDefinition:
		out->raw = SourceSlice(state->normalizedSource, out->range);
		out->footnoteLabel = FromCmarkString(cmark_node_get_literal(node));
		if (out->footnoteLabel.isEmpty()) {
			out->footnoteLabel = ExtractFootnoteLabel(out->raw, true);
		}
		break;
	default:
		break;
	}
}

[[nodiscard]] bool ConvertNode(
		cmark_node *node,
		ParserState *state,
		int depth,
		MarkdownNode *out) {
	if (!node || !state || !out || state->failed) {
		return false;
	}
	if (depth > kMaxNesting) {
		return FailScanMetadata(state, "cmark-nesting-too-deep");
	}
	out->kind = NodeKindFor(node);
	out->range = NodeRange(
		node,
		state->lineStarts,
		static_cast<int>(state->normalizedSource.size()));
	if (out->kind == NodeKind::Unsupported) {
		out->unsupportedKind = CmarkKind(node);
		out->raw = SourceSlice(state->normalizedSource, out->range);
	}
	FillNodeAttributes(node, state, out);
	for (auto child = cmark_node_first_child(node); child;) {
		const auto next = cmark_node_next(child);
		auto converted = MarkdownNode();
		if (!ConvertNode(child, state, depth + 1, &converted)) {
			return false;
		}
		out->children.push_back(std::move(converted));
		child = next;
	}
	if (out->kind == NodeKind::Link) {
		out->autolink = LinkLooksAutolink(*out);
		if (out->autolink && state->stats) {
			state->stats->autolinksSeen = true;
		}
	}
	if (state->stats) {
		++state->stats->convertedNodeCount;
	}
	return true;
}

[[nodiscard]] MarkdownNode DisplayMathNode(
		const MathFormula &formula,
		int vectorIndex) {
	auto result = MarkdownNode();
	result.kind = NodeKind::DisplayMath;
	result.range = formula.range;
	result.text = formula.tex;
	result.formulaIndex = vectorIndex;
	return result;
}

[[nodiscard]] int RangeEndOffset(const SourceRange &range) {
	return range.available ? range.endOffset : std::numeric_limits<int>::min();
}

[[nodiscard]] int RangeStartOffset(const SourceRange &range) {
	return range.available
		? range.startOffset
		: std::numeric_limits<int>::max();
}

[[nodiscard]] bool IsBreakNode(NodeKind kind) {
	return kind == NodeKind::SoftBreak || kind == NodeKind::LineBreak;
}

void TrimBreakEdges(std::vector<MarkdownNode> *children) {
	if (!children) {
		return;
	}
	while (!children->empty() && IsBreakNode(children->front().kind)) {
		children->erase(children->begin());
	}
	while (!children->empty() && IsBreakNode(children->back().kind)) {
		children->pop_back();
	}
}

[[nodiscard]] bool RangeContains(
		const SourceRange &outer,
		const SourceRange &inner) {
	return outer.available
		&& inner.available
		&& outer.startOffset <= inner.startOffset
		&& outer.endOffset >= inner.endOffset;
}

[[nodiscard]] bool RangeOverlaps(
		const SourceRange &range,
		int clipStart,
		int clipEnd) {
	return range.available
		&& (range.endOffset > clipStart)
		&& (range.startOffset < clipEnd);
}

[[nodiscard]] bool FirstAvailableRange(
		const MarkdownNode &node,
		SourceRange *out);
[[nodiscard]] bool LastAvailableRange(
		const MarkdownNode &node,
		SourceRange *out);

[[nodiscard]] bool FirstAvailableRangeInChildren(
		const std::vector<MarkdownNode> &children,
		SourceRange *out) {
	for (const auto &child : children) {
		if (FirstAvailableRange(child, out)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool LastAvailableRangeInChildren(
		const std::vector<MarkdownNode> &children,
		SourceRange *out) {
	for (auto i = children.rbegin(); i != children.rend(); ++i) {
		if (LastAvailableRange(*i, out)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool FirstAvailableRange(
		const MarkdownNode &node,
		SourceRange *out) {
	if (node.range.available) {
		if (out) {
			*out = node.range;
		}
		return true;
	}
	return FirstAvailableRangeInChildren(node.children, out);
}

[[nodiscard]] bool LastAvailableRange(
		const MarkdownNode &node,
		SourceRange *out) {
	if (node.range.available) {
		if (out) {
			*out = node.range;
		}
		return true;
	}
	return LastAvailableRangeInChildren(node.children, out);
}

[[nodiscard]] bool RangeFromChildren(
		const std::vector<MarkdownNode> &children,
		SourceRange *out) {
	auto first = SourceRange();
	auto last = SourceRange();
	if (!FirstAvailableRangeInChildren(children, &first)
		|| !LastAvailableRangeInChildren(children, &last)) {
		return false;
	}
	auto result = first;
	result.endLine = last.endLine;
	result.endColumn = last.endColumn;
	result.endOffset = last.endOffset;
	if (out) {
		*out = result;
	}
	return true;
}

[[nodiscard]] bool ClipNodeToOffsets(
		const MarkdownNode &node,
		const QByteArray &source,
		const std::vector<int> &lineStarts,
		int clipStart,
		int clipEnd,
		MarkdownNode *out) {
	if (!out || clipEnd <= clipStart || IsBreakNode(node.kind)) {
		return false;
	}
	if (node.range.available && !RangeOverlaps(node.range, clipStart, clipEnd)) {
		return false;
	}
	if (node.children.empty()) {
		if (!node.range.available) {
			return false;
		}
		const auto clippedStart = std::max(node.range.startOffset, clipStart);
		const auto clippedEnd = std::min(node.range.endOffset, clipEnd);
		if (clippedEnd <= clippedStart) {
			return false;
		}
		*out = node;
		out->range = RangeForOffsets(
			lineStarts,
			static_cast<int>(source.size()),
			clippedStart,
			clippedEnd);
		if (clippedStart == node.range.startOffset
			&& clippedEnd == node.range.endOffset) {
			return true;
		}
		switch (node.kind) {
		case NodeKind::Text:
		case NodeKind::InlineCode:
			out->text = SourceSlice(source, out->range);
			break;
		case NodeKind::HtmlBlock:
		case NodeKind::HtmlInline:
		case NodeKind::Unsupported:
			out->raw = SourceSlice(source, out->range);
			break;
		default:
			return false;
		}
		return true;
	}
	auto result = node;
	result.children.clear();
	for (const auto &child : node.children) {
		if (IsBreakNode(child.kind)) {
			if (!result.children.empty()) {
				result.children.push_back(child);
			}
			continue;
		}
		auto clippedChild = MarkdownNode();
		if (ClipNodeToOffsets(
				child,
				source,
				lineStarts,
				clipStart,
				clipEnd,
				&clippedChild)) {
			result.children.push_back(std::move(clippedChild));
		}
	}
	TrimBreakEdges(&result.children);
	if (result.children.empty()) {
		return false;
	}
	auto clippedRange = SourceRange();
	if (RangeFromChildren(result.children, &clippedRange)) {
		result.range = clippedRange;
	}
	*out = std::move(result);
	return true;
}

[[nodiscard]] bool ParagraphSegment(
		const MarkdownNode &paragraph,
		const QByteArray &source,
		const std::vector<int> &lineStarts,
		int clipStart,
		int clipEnd,
		MarkdownNode *out) {
	if (clipEnd <= clipStart) {
		return false;
	}
	return ClipNodeToOffsets(
		paragraph,
		source,
		lineStarts,
		clipStart,
		clipEnd,
		out);
}

[[nodiscard]] std::vector<MarkdownNode> SplitParagraphAroundDisplayMath(
		const MarkdownNode &paragraph,
		const std::vector<MathFormula> &formulas,
		const std::vector<int> &displayIndexes,
		int begin,
		int end,
		const QByteArray &source,
		const std::vector<int> &lineStarts) {
	auto result = std::vector<MarkdownNode>();
	if (!paragraph.range.available) {
		result.push_back(paragraph);
		return result;
	}
	auto cursor = paragraph.range.startOffset;
	for (auto i = begin; i != end; ++i) {
		const auto formulaIndex = displayIndexes[i];
		const auto &formula = formulas[formulaIndex];
		if (!RangeContains(paragraph.range, formula.range)) {
			continue;
		}
		auto segment = MarkdownNode();
		if (ParagraphSegment(
				paragraph,
				source,
				lineStarts,
				cursor,
				formula.range.startOffset,
				&segment)) {
			result.push_back(std::move(segment));
		}
		result.push_back(DisplayMathNode(formula, formulaIndex));
		cursor = std::max(cursor, formula.range.endOffset);
	}
	auto tail = MarkdownNode();
	if (ParagraphSegment(
			paragraph,
			source,
			lineStarts,
			cursor,
			paragraph.range.endOffset,
			&tail)) {
		result.push_back(std::move(tail));
	}
	return result;
}

[[nodiscard]] std::vector<int> DisplayFormulaIndexes(
		const std::vector<MathFormula> &formulas) {
	auto result = std::vector<int>();
	const auto count = static_cast<int>(formulas.size());
	for (auto i = 0; i != count; ++i) {
		if (formulas[i].kind == MathKind::Display) {
			result.push_back(i);
		}
	}
	std::sort(
		result.begin(),
		result.end(),
		[&](int left, int right) {
			const auto leftOffset = RangeStartOffset(formulas[left].range);
			const auto rightOffset = RangeStartOffset(formulas[right].range);
			return (leftOffset != rightOffset)
				? (leftOffset < rightOffset)
				: (left < right);
		});
	return result;
}

[[nodiscard]] bool CanNormalizeDisplayMathChildren(NodeKind kind) {
	switch (kind) {
	case NodeKind::Document:
	case NodeKind::List:
	case NodeKind::ListItem:
	case NodeKind::Blockquote:
	case NodeKind::Table:
	case NodeKind::TableRow:
		return true;
	default:
		return false;
	}
}

void NormalizeDisplayMathChildren(
		MarkdownNode *node,
		const std::vector<MathFormula> &formulas,
		const std::vector<int> &displayIndexes,
		int begin,
		int end,
		const QByteArray &source,
		const std::vector<int> &lineStarts) {
	if (!node || begin >= end || node->children.empty()) {
		return;
	}
	auto originalChildren = std::move(node->children);
	auto children = std::vector<MarkdownNode>();
	children.reserve(originalChildren.size() + (end - begin));
	auto formula = begin;
	for (auto &child : originalChildren) {
		while (formula != end
			&& child.range.available
			&& RangeEndOffset(formulas[displayIndexes[formula]].range)
				<= child.range.startOffset) {
			const auto formulaIndex = displayIndexes[formula];
			children.push_back(DisplayMathNode(formulas[formulaIndex], formulaIndex));
			++formula;
		}
		auto childEnd = formula;
		while (childEnd != end) {
			const auto &formulaRange = formulas[displayIndexes[childEnd]].range;
			if (!RangeContains(child.range, formulaRange)) {
				break;
			}
			++childEnd;
		}
		if (formula != childEnd && child.kind == NodeKind::Paragraph) {
			auto split = SplitParagraphAroundDisplayMath(
				child,
				formulas,
				displayIndexes,
				formula,
				childEnd,
				source,
				lineStarts);
			for (auto &part : split) {
				children.push_back(std::move(part));
			}
			formula = childEnd;
			continue;
		}
		if (formula != childEnd && child.kind == NodeKind::TableCell) {
			children.push_back(std::move(child));
			formula = childEnd;
			continue;
		}
		if (formula != childEnd && CanNormalizeDisplayMathChildren(child.kind)) {
			NormalizeDisplayMathChildren(
				&child,
				formulas,
				displayIndexes,
				formula,
				childEnd,
				source,
				lineStarts);
			formula = childEnd;
		}
		children.push_back(std::move(child));
	}
	while (formula != end) {
		const auto formulaIndex = displayIndexes[formula];
		children.push_back(DisplayMathNode(formulas[formulaIndex], formulaIndex));
		++formula;
	}
	node->children = std::move(children);
}

[[nodiscard]] int CountNodes(const MarkdownNode &node) {
	auto result = 1;
	for (const auto &child : node.children) {
		result += CountNodes(child);
	}
	return result;
}

[[nodiscard]] int *FindNamedCounter(
		std::vector<std::pair<QString, int>> *entries,
		const QString &key) {
	if (!entries) {
		return nullptr;
	}
	for (auto &entry : *entries) {
		if (entry.first == key) {
			return &entry.second;
		}
	}
	return nullptr;
}

[[nodiscard]] int FindNamedValue(
		const std::vector<std::pair<QString, int>> &entries,
		const QString &key) {
	for (const auto &entry : entries) {
		if (entry.first == key) {
			return entry.second;
		}
	}
	return 0;
}

[[nodiscard]] bool ContainsAnchorId(
		const std::vector<QString> &anchors,
		const QString &value) {
	return std::find(anchors.begin(), anchors.end(), value) != anchors.end();
}

void AssignHeadingAnchors(
		MarkdownNode *node,
		std::vector<std::pair<QString, int>> *counts,
		QStringList *warnings) {
	if (!node) {
		return;
	}
	if (node->kind == NodeKind::Heading) {
		const auto base = AnchorIdBaseFromText(PlainText(*node));
		auto count = FindNamedCounter(counts, base);
		if (count) {
			++(*count);
			node->anchorId = base + u"-"_q + QString::number(*count);
			if (warnings) {
				warnings->push_back(FromLatin1(
					"Duplicate heading anchor \"%1\" remapped to \"%2\"").arg(
						base
					).arg(
						node->anchorId));
			}
		} else {
			counts->push_back({ base, 1 });
			node->anchorId = base;
		}
	}
	for (auto &child : node->children) {
		AssignHeadingAnchors(&child, counts, warnings);
	}
}

void AssignFootnoteDefinitionOrdinals(
		MarkdownNode *node,
		std::vector<std::pair<QString, int>> *definitions,
		int *nextOrdinal,
		QStringList *warnings) {
	if (!node || !definitions || !nextOrdinal) {
		return;
	}
	if (node->kind == NodeKind::FootnoteDefinition) {
		if (node->footnoteLabel.isEmpty()) {
			if (warnings) {
				warnings->push_back(FromLatin1(
					"Footnote definition without label at %1:%2").arg(
						node->range.startLine
					).arg(
						node->range.startColumn));
			}
		} else if (const auto existing = FindNamedValue(
				*definitions,
				node->footnoteLabel)) {
			node->footnoteOrdinal = existing;
			node->anchorId = FootnoteDefinitionAnchorId(existing);
			if (warnings) {
				warnings->push_back(FromLatin1(
					"Duplicate footnote definition \"%1\"").arg(
						node->footnoteLabel));
			}
		} else {
			node->footnoteOrdinal = *nextOrdinal;
			node->anchorId = FootnoteDefinitionAnchorId(*nextOrdinal);
			definitions->push_back({ node->footnoteLabel, *nextOrdinal });
			++(*nextOrdinal);
		}
	}
	for (auto &child : node->children) {
		AssignFootnoteDefinitionOrdinals(&child, definitions, nextOrdinal, warnings);
	}
}

void AssignFootnoteReferenceOrdinals(
		MarkdownNode *node,
		const std::vector<std::pair<QString, int>> &definitions,
		QStringList *warnings) {
	if (!node) {
		return;
	}
	if (node->kind == NodeKind::FootnoteReference) {
		if (node->footnoteLabel.isEmpty()) {
			if (warnings) {
				warnings->push_back(FromLatin1(
					"Footnote reference without label at %1:%2").arg(
						node->range.startLine
					).arg(
						node->range.startColumn));
			}
		} else if (const auto ordinal = FindNamedValue(
				definitions,
				node->footnoteLabel)) {
			node->footnoteOrdinal = ordinal;
		} else if (warnings) {
			warnings->push_back(FromLatin1(
				"Unresolved footnote reference \"%1\"").arg(
					node->footnoteLabel));
		}
	}
	for (auto &child : node->children) {
		AssignFootnoteReferenceOrdinals(&child, definitions, warnings);
	}
}

void CollectAnchorIds(
		const MarkdownNode &node,
		std::vector<QString> *anchors) {
	if (!anchors) {
		return;
	}
	if (!node.anchorId.isEmpty()
		&& (node.kind == NodeKind::Heading
			|| node.kind == NodeKind::FootnoteDefinition)) {
		anchors->push_back(node.anchorId);
	}
	for (const auto &child : node.children) {
		CollectAnchorIds(child, anchors);
	}
}

void ValidateLocalFragments(
		const MarkdownNode &node,
		const std::vector<QString> &anchors,
		QStringList *warnings) {
	if (node.kind == NodeKind::Link && node.url.startsWith(QChar('#'))) {
		const auto fragment = NormalizeFragmentId(node.url.mid(1));
		if (fragment.isEmpty() || !ContainsAnchorId(anchors, fragment)) {
			if (warnings) {
				warnings->push_back(FromLatin1(
					"Unresolved local fragment \"%1\"").arg(
						node.url));
			}
		}
	}
	for (const auto &child : node.children) {
		ValidateLocalFragments(child, anchors, warnings);
	}
}

void FinalizeDocumentSemantics(PreparedDocument *document) {
	if (!document) {
		return;
	}
	auto headingCounts = std::vector<std::pair<QString, int>>();
	AssignHeadingAnchors(
		&document->document,
		&headingCounts,
		&document->warnings);

	auto footnoteDefinitions = std::vector<std::pair<QString, int>>();
	auto nextFootnoteOrdinal = 1;
	AssignFootnoteDefinitionOrdinals(
		&document->document,
		&footnoteDefinitions,
		&nextFootnoteOrdinal,
		&document->warnings);
	AssignFootnoteReferenceOrdinals(
		&document->document,
		footnoteDefinitions,
		&document->warnings);

	auto anchors = std::vector<QString>();
	CollectAnchorIds(document->document, &anchors);
	ValidateLocalFragments(document->document, anchors, &document->warnings);
}

void NormalizeDisplayMathBlocks(
		PreparedDocument *document,
		const QByteArray &source,
		const std::vector<int> &lineStarts) {
	if (!document) {
		return;
	}
	const auto displayIndexes = DisplayFormulaIndexes(document->formulas);
	if (!displayIndexes.empty()) {
		NormalizeDisplayMathChildren(
			&document->document,
			document->formulas,
			displayIndexes,
			0,
			static_cast<int>(displayIndexes.size()),
			source,
			lineStarts);
	}
	document->stats.convertedNodeCount = CountNodes(document->document);
}

[[nodiscard]] QString FirstHeadingTitle(const MarkdownNode &node) {
	if (node.kind == NodeKind::Heading) {
		return PlainText(node).trimmed();
	}
	for (const auto &child : node.children) {
		const auto result = FirstHeadingTitle(child);
		if (!result.isEmpty()) {
			return result;
		}
	}
	return QString();
}

void FillFormulaStats(PreparedDocument *document) {
	if (!document) {
		return;
	}
	document->stats.inlineFormulaCount = 0;
	document->stats.displayFormulaCount = 0;
	for (const auto &formula : document->formulas) {
		switch (formula.kind) {
		case MathKind::Inline:
			++document->stats.inlineFormulaCount;
			break;
		case MathKind::Display:
			++document->stats.displayFormulaCount;
			break;
		}
	}
}

} // namespace

MarkdownSourceValidationResult ValidateMarkdownSourceForIv(
		const QByteArray &source,
		ParseOptions options) {
	if (source.size() > kMaxSourceBytes) {
		return ValidationFailure(
			std::move(options.sourceName),
			FromLatin1("source-too-large"));
	}
	if (HasUnsupportedUnicodeBom(source)) {
		return ValidationFailure(
			std::move(options.sourceName),
			FromLatin1("source-unsupported-bom"));
	}
	auto normalized = StripUtf8Bom(source);
	if (LooksBinary(normalized)) {
		return ValidationFailure(
			std::move(options.sourceName),
			FromLatin1("source-binary"));
	}
	if (!IsValidUtf8(normalized)) {
		return ValidationFailure(
			std::move(options.sourceName),
			FromLatin1("source-invalid-utf8"));
	}
	auto result = MarkdownSourceValidationResult();
	result.source.normalized = std::move(normalized);
	result.source.decoded = QString::fromUtf8(
		result.source.normalized.constData(),
		result.source.normalized.size());
	result.source.lineStarts = BuildLineStarts(result.source.normalized);
	result.source.sourceName = std::move(options.sourceName);
	return result;
}

ParseResult ParseMarkdownForIv(ValidatedMarkdownSource source) {
	auto mask = std::vector<bool>(source.normalized.size(), false);
	const auto parserOptions = CMARK_OPT_DEFAULT
		| CMARK_OPT_SOURCEPOS
		| CMARK_OPT_FOOTNOTES
		| CMARK_OPT_STRIKETHROUGH_DOUBLE_TILDE;
	auto parser = ParserPointer(cmark_parser_new(parserOptions));
	if (!parser) {
		return Failure(
			source.sourceName,
			FromLatin1("cmark-parser-failed"));
	}
	auto error = QString();
	if (!AttachExtensions(parser.get(), &error)) {
		return Failure(source.sourceName, std::move(error));
	}
	cmark_parser_feed(
		parser.get(),
		source.normalized.constData(),
		static_cast<std::size_t>(source.normalized.size()));
	auto root = NodePointer(cmark_parser_finish(parser.get()));
	if (!root) {
		return Failure(
			source.sourceName,
			FromLatin1("cmark-parser-failed"));
	}
	auto document = EmptyDocument(std::move(source.sourceName));
	document.sourceText = std::move(source.decoded);
	auto scanBlocks = std::vector<MathScanBlock>();
	auto state = ParserState{
		source.normalized,
		source.lineStarts,
		&mask,
		&scanBlocks,
		&document.stats,
		&document.warnings,
	};
	if (!CollectScanMetadata(root.get(), &state, 0)) {
		return Failure(std::move(document.sourceName), std::move(state.error));
	}
	if (!ExtractMathRegions(
			source.normalized,
			mask,
			source.lineStarts,
			scanBlocks,
			kMaxFormulaBytes,
			kMaxFormulaCount,
			&document.formulas,
			&error)) {
		return Failure(std::move(document.sourceName), std::move(error));
	}
	if (!ConvertNode(
			root.get(),
			&state,
			0,
			&document.document)) {
		return Failure(
			std::move(document.sourceName),
			state.error.isEmpty()
				? FromLatin1("cmark-conversion-failed")
				: std::move(state.error));
	}
	FillFormulaStats(&document);
	NormalizeDisplayMathBlocks(
		&document,
		source.normalized,
		source.lineStarts);
	FinalizeDocumentSemantics(&document);
	document.title = FirstHeadingTitle(document.document);
	document.empty = document.document.children.empty()
		&& document.formulas.empty();
	return ParseResult{ std::move(document), QString(), true };
}

ParseResult ParseMarkdownForIv(const QByteArray &source, ParseOptions options) {
	auto validated = ValidateMarkdownSourceForIv(source, std::move(options));
	return validated.ok
		? ParseMarkdownForIv(std::move(validated.source))
		: Failure(
			std::move(validated.source.sourceName),
			std::move(validated.error));
}

} // namespace Iv::Markdown
