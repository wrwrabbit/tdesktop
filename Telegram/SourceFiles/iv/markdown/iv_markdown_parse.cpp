#include "iv/markdown/iv_markdown_parse.h"

#include "iv/markdown/iv_markdown_math.h"

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

[[nodiscard]] bool ValidateSource(
		const QByteArray &source,
		QByteArray *normalized,
		QString *decoded,
		QString *error) {
	const auto fail = [=](const char *value) {
		if (error) {
			*error = FromLatin1(value);
		}
		return false;
	};
	if (source.size() > kMaxSourceBytes) {
		return fail("source-too-large");
	}
	if (HasUnsupportedUnicodeBom(source)) {
		return fail("source-unsupported-bom");
	}
	auto normalizedSource = StripUtf8Bom(source);
	if (LooksBinary(normalizedSource)) {
		return fail("source-binary");
	}
	if (!IsValidUtf8(normalizedSource)) {
		return fail("source-invalid-utf8");
	}
	if (decoded) {
		*decoded = QString::fromUtf8(
			normalizedSource.constData(),
			normalizedSource.size());
	}
	if (normalized) {
		*normalized = std::move(normalizedSource);
	}
	if (error) {
		error->clear();
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
	case CMARK_NODE_TEXT: return NodeKind::Text;
	case CMARK_NODE_SOFTBREAK: return NodeKind::SoftBreak;
	case CMARK_NODE_LINEBREAK: return NodeKind::LineBreak;
	case CMARK_NODE_CODE: return NodeKind::InlineCode;
	case CMARK_NODE_HTML_INLINE: return NodeKind::HtmlInline;
	case CMARK_NODE_EMPH: return NodeKind::Emphasis;
	case CMARK_NODE_STRONG: return NodeKind::Strong;
	case CMARK_NODE_LINK: return NodeKind::Link;
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

void FillNodeAttributes(cmark_node *node, MarkdownNode *out) {
	switch (out->kind) {
	case NodeKind::Text:
	case NodeKind::InlineCode:
		out->text = FromCmarkString(cmark_node_get_literal(node));
		break;
	case NodeKind::CodeBlock:
		out->text = FromCmarkString(cmark_node_get_literal(node));
		out->info = FromCmarkString(cmark_node_get_fence_info(node));
		break;
	case NodeKind::HtmlBlock:
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
	FillNodeAttributes(node, out);
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

[[nodiscard]] int RangeStartOffset(const SourceRange &range) {
	return range.available
		? range.startOffset
		: std::numeric_limits<int>::max();
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

void InsertDisplayMathBlocks(PreparedDocument *document) {
	if (!document) {
		return;
	}
	const auto displayIndexes = DisplayFormulaIndexes(document->formulas);
	if (displayIndexes.empty()) {
		return;
	}
	auto originalChildren = std::move(document->document.children);
	auto children = std::vector<MarkdownNode>();
	children.reserve(originalChildren.size() + displayIndexes.size());
	auto displayIndex = std::size_t(0);
	const auto appendDisplayBefore = [&](int offset) {
		while (displayIndex != displayIndexes.size()) {
			const auto formulaIndex = displayIndexes[displayIndex];
			const auto formulaOffset = RangeStartOffset(
				document->formulas[formulaIndex].range);
			if (formulaOffset > offset) {
				break;
			}
			children.push_back(DisplayMathNode(
				document->formulas[formulaIndex],
				formulaIndex));
			++displayIndex;
		}
	};
	for (auto &child : originalChildren) {
		appendDisplayBefore(RangeStartOffset(child.range));
		children.push_back(std::move(child));
	}
	appendDisplayBefore(std::numeric_limits<int>::max());
	const auto displayCount = static_cast<int>(displayIndexes.size());
	document->stats.convertedNodeCount += displayCount;
	document->document.children = std::move(children);
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

ParseResult ParseMarkdownForIv(const QByteArray &source, ParseOptions options) {
	auto normalized = QByteArray();
	auto decoded = QString();
	auto error = QString();
	if (!ValidateSource(source, &normalized, &decoded, &error)) {
		return Failure(std::move(options.sourceName), std::move(error));
	}
	const auto lineStarts = BuildLineStarts(normalized);
	auto mask = std::vector<bool>(normalized.size(), false);
	const auto parserOptions = CMARK_OPT_DEFAULT
		| CMARK_OPT_SOURCEPOS
		| CMARK_OPT_FOOTNOTES
		| CMARK_OPT_STRIKETHROUGH_DOUBLE_TILDE;
	auto parser = ParserPointer(cmark_parser_new(parserOptions));
	if (!parser) {
		return Failure(
			std::move(options.sourceName),
			FromLatin1("cmark-parser-failed"));
	}
	if (!AttachExtensions(parser.get(), &error)) {
		return Failure(std::move(options.sourceName), std::move(error));
	}
	cmark_parser_feed(
		parser.get(),
		normalized.constData(),
		static_cast<std::size_t>(normalized.size()));
	auto root = NodePointer(cmark_parser_finish(parser.get()));
	if (!root) {
		return Failure(
			std::move(options.sourceName),
			FromLatin1("cmark-parser-failed"));
	}
	auto document = EmptyDocument(std::move(options.sourceName));
	document.sourceText = std::move(decoded);
	auto scanBlocks = std::vector<MathScanBlock>();
	auto state = ParserState{
		normalized,
		lineStarts,
		&mask,
		&scanBlocks,
		&document.stats,
		&document.warnings,
	};
	if (!CollectScanMetadata(root.get(), &state, 0)) {
		return Failure(std::move(document.sourceName), std::move(state.error));
	}
	if (!ExtractMathRegions(
			normalized,
			mask,
			lineStarts,
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
	document.title = FirstHeadingTitle(document.document);
	document.empty = document.document.children.empty()
		&& document.formulas.empty();
	InsertDisplayMathBlocks(&document);
	document.empty = document.document.children.empty()
		&& document.formulas.empty();
	return ParseResult{ std::move(document), QString(), true };
}

} // namespace Iv::Markdown
