#include "iv/markdown/iv_markdown_prepare.h"

#include "base/call_delayed.h"

#include <QtCore/QByteArray>

#include <algorithm>
#include <limits>
#include <utility>

#include "ui/style/style_core.h"

#include "styles/palette.h"
#include "styles/style_iv.h"

namespace Iv::Markdown {
namespace {

constexpr auto kMaxVisualListDepth = 6;
constexpr auto kMaxVisualQuoteDepth = 3;
constexpr auto kMaxRenderedTableRows = 128;
constexpr auto kMaxRenderedTableColumns = 16;
constexpr auto kMaxRenderedTableCells = 1024;

struct PrepareContext {
	int listDepth = 0;
	int quoteDepth = 0;
};

struct PrepareState {
	const PrepareRequest *request = nullptr;
	PreparedResult result;
	QByteArray sourceUtf8;

	[[nodiscard]] bool cancelled() {
		if (!request->cancelled) {
			return false;
		} else if (!request->cancelled->load(std::memory_order_relaxed)) {
			return false;
		}
		result.cancelled = true;
		return true;
	}

	void rememberFormula(
			int index,
			MathKind kind,
			QString formulaTex,
			int textSize,
			int renderWidthCap,
			int renderHeightCap) {
		if (index < 0) {
			return;
		}
		if (index >= int(result.formulas.size())) {
			result.formulas.resize(index + 1);
		}
		auto &slot = result.formulas[index];
		slot.trimmedTex = formulaTex.trimmed();
		slot.kind = kind;
		slot.textSize = textSize;
		slot.renderWidthCap = renderWidthCap;
		slot.renderHeightCap = renderHeightCap;
		slot.present = true;
	}

	void rememberFormula(const PreparedBlock &block) {
		rememberFormula(
			block.formulaIndex,
			block.mathKind,
			block.formulaTex,
			result.style.displayMathTextSize,
			result.style.displayMathMaxRenderWidth,
			result.style.displayMathMaxRenderHeight);
	}

	[[nodiscard]] QString formulaSourceText(int index) const {
		if (!request
			|| !request->document
			|| index < 0
			|| index >= int(request->document->formulas.size())) {
			return QString();
		}
		const auto &range = request->document->formulas[index].range;
		const auto from = std::clamp(range.startOffset, 0, sourceUtf8.size());
		const auto till = std::clamp(range.endOffset, from, sourceUtf8.size());
		return QString::fromUtf8(sourceUtf8.constData() + from, till - from);
	}
};

struct InlineFormulaSource {
	int formulaIndex = -1;
	SourceRange range;
	QString copySource;
};

struct InlineFormulaContext {
	const std::vector<InlineFormulaSource> *formulas = nullptr;
	std::vector<PreparedInlineObject> *prepared = nullptr;
	int next = 0;
	int textSize = 0;
	int renderWidthCap = 0;
	int renderHeightCap = 0;
};

enum class RawInlineTag {
	None,
	SubOpen,
	SubClose,
	SupOpen,
	SupClose,
	MarkOpen,
	MarkClose,
};

void ClearPreparedOutput(PreparedResult *result) {
	result->blocks.blocks.clear();
	result->formulas.clear();
}

[[nodiscard]] QString InternalLinkData(uint16 index) {
	return u"internal:index"_q + QChar(index);
}

[[nodiscard]] int CappedListDepth(int depth) {
	return std::min(depth, kMaxVisualListDepth);
}

[[nodiscard]] int CappedQuoteDepth(int depth) {
	return std::min(depth, kMaxVisualQuoteDepth);
}

[[nodiscard]] QString FirstInfoToken(const QString &info) {
	const auto trimmed = info.trimmed();
	for (auto i = 0; i != trimmed.size(); ++i) {
		if (trimmed[i].isSpace()) {
			return trimmed.left(i);
		}
	}
	return trimmed;
}

void SortEntities(TextWithEntities *text) {
	auto &entities = text->entities;
	std::sort(
		entities.begin(),
		entities.end(),
		[](const EntityInText &left, const EntityInText &right) {
			if (left.offset() != right.offset()) {
				return left.offset() < right.offset();
			} else if (left.length() != right.length()) {
				return left.length() > right.length();
			}
			return int(left.type()) < int(right.type());
		});
}

[[nodiscard]] bool RangeContains(
		const SourceRange &outer,
		const SourceRange &inner) {
	return outer.available
		&& inner.available
		&& outer.startOffset <= inner.startOffset
		&& outer.endOffset >= inner.endOffset;
}

[[nodiscard]] bool IsEscapableAscii(char ch) {
	const auto value = uchar(ch);
	return (value >= 0x21 && value <= 0x2F)
		|| (value >= 0x3A && value <= 0x40)
		|| (value >= 0x5B && value <= 0x60)
		|| (value >= 0x7B && value <= 0x7E);
}

[[nodiscard]] bool AppendHtmlEntityText(
		const QByteArray &entity,
		QString *result) {
	if (entity == "amp") {
		result->append(QChar('&'));
	} else if (entity == "lt") {
		result->append(QChar('<'));
	} else if (entity == "gt") {
		result->append(QChar('>'));
	} else if (entity == "quot") {
		result->append(QChar('"'));
	} else if (entity == "apos") {
		result->append(QChar('\''));
	} else if (entity.startsWith("#x") || entity.startsWith("#X")) {
		auto ok = false;
		const auto value = entity.mid(2).toUInt(&ok, 16);
		if (!ok || value > 0xFFFF) {
			return false;
		}
		result->append(QChar(ushort(value)));
	} else if (entity.startsWith("#")) {
		auto ok = false;
		const auto value = entity.mid(1).toUInt(&ok, 10);
		if (!ok || value > 0xFFFF) {
			return false;
		}
		result->append(QChar(ushort(value)));
	} else {
		return false;
	}
	return true;
}

[[nodiscard]] QString DecodeMarkdownTextPrefix(QByteArray bytes) {
	auto result = QString();
	auto plainFrom = 0;
	const auto flushPlain = [&](int till) {
		if (till > plainFrom) {
			result.append(QString::fromUtf8(
				bytes.constData() + plainFrom,
				till - plainFrom));
		}
	};
	for (auto i = 0; i != bytes.size();) {
		if (bytes[i] == '\\'
			&& (i + 1) < bytes.size()
			&& IsEscapableAscii(bytes[i + 1])) {
			flushPlain(i);
			result.append(QChar(ushort(uchar(bytes[i + 1]))));
			i += 2;
			plainFrom = i;
		} else if (bytes[i] == '&') {
			const auto semicolon = bytes.indexOf(';', i + 1);
			if (semicolon > i && semicolon - i <= 32) {
				auto entityText = QString();
				if (AppendHtmlEntityText(
						bytes.mid(i + 1, semicolon - i - 1),
						&entityText)) {
					flushPlain(i);
					result.append(entityText);
					i = semicolon + 1;
					plainFrom = i;
					continue;
				}
			}
			++i;
		} else {
			++i;
		}
	}
	flushPlain(bytes.size());
	return result;
}

[[nodiscard]] int DisplayOffsetForSourceOffset(
		const MarkdownNode &node,
		const QString &value,
		int sourceOffset,
		const PrepareState *state) {
	if (!state
		|| !node.range.available
		|| sourceOffset < node.range.startOffset
		|| sourceOffset > node.range.endOffset) {
		return -1;
	}
	const auto prefixSize = sourceOffset - node.range.startOffset;
	if (!prefixSize) {
		return 0;
	}
	const auto prefix = state->sourceUtf8.mid(
		node.range.startOffset,
		prefixSize);
	const auto displayPrefix = DecodeMarkdownTextPrefix(prefix);
	return value.startsWith(displayPrefix) ? displayPrefix.size() : -1;
}

[[nodiscard]] int TextSizeForFormula(const style::TextStyle &textStyle) {
	return std::max(textStyle.font->height, 1);
}

[[nodiscard]] int ScaleFormulaCap(int cap, int textSize, int baseTextSize) {
	if (cap <= 0) {
		return 0;
	}
	const auto numerator = int64(cap) * std::max(textSize, 1);
	const auto denominator = std::max(baseTextSize, 1);
	return std::max(int((numerator + denominator - 1) / denominator), 1);
}

[[nodiscard]] const style::TextStyle &FlowTextStyle(
		PreparedBlockKind kind,
		int headingLevel,
		const MarkdownStyleSnapshot &style) {
	if (kind != PreparedBlockKind::Heading) {
		return style.paragraphStyle;
	}
	switch (std::clamp(headingLevel, 1, 6)) {
	case 1: return style.heading1Style;
	case 2: return style.heading2Style;
	case 3: return style.heading3Style;
	case 4: return style.heading4Style;
	case 5: return style.heading5Style;
	case 6: return style.heading6Style;
	}
	return style.heading6Style;
}

[[nodiscard]] const style::TextStyle &TableCellTextStyle(
		bool header,
		const MarkdownStyleSnapshot &style) {
	return header ? style.tableHeaderStyle : style.paragraphStyle;
}

[[nodiscard]] std::vector<InlineFormulaSource> CollectInlineFormulas(
		const MarkdownNode &node,
		PrepareState *state) {
	auto result = std::vector<InlineFormulaSource>();
	if (!state
		|| !state->request
		|| !state->request->document
		|| !node.range.available) {
		return result;
	}
	const auto &formulas = state->request->document->formulas;
	for (auto i = 0, count = int(formulas.size()); i != count; ++i) {
		const auto &formula = formulas[i];
		if (formula.kind != MathKind::Inline
			|| !RangeContains(node.range, formula.range)) {
			continue;
		}
		auto copySource = state->formulaSourceText(i);
		if (copySource.isEmpty()) {
			copySource = u"$"_q + formula.tex + u"$"_q;
		}
		result.push_back({
			.formulaIndex = i,
			.range = formula.range,
			.copySource = std::move(copySource),
		});
	}
	return result;
}

void ReplaceInlineFormulasInAppendedText(
		const MarkdownNode &node,
		const QString &value,
		int from,
		TextWithEntities *text,
		InlineFormulaContext *inlineFormulas,
		PrepareState *state) {
	if (!text
		|| !inlineFormulas
		|| !inlineFormulas->formulas
		|| !inlineFormulas->prepared
		|| !state
		|| inlineFormulas->next >= int(inlineFormulas->formulas->size())
		|| !node.range.available) {
		return;
	}
	auto removedLength = 0;
	while (inlineFormulas->next < int(inlineFormulas->formulas->size())) {
		const auto &formula = (*inlineFormulas->formulas)[inlineFormulas->next];
		if (formula.range.endOffset <= node.range.startOffset) {
			++inlineFormulas->next;
			continue;
		} else if (!RangeContains(node.range, formula.range)) {
			break;
		}

		const auto originalOffset = DisplayOffsetForSourceOffset(
			node,
			value,
			formula.range.startOffset,
			state);
		const auto sourceLength = formula.copySource.size();
		if (originalOffset < 0
			|| value.mid(originalOffset, sourceLength) != formula.copySource) {
			++inlineFormulas->next;
			continue;
		}
		const auto found = from + originalOffset - removedLength;
		text->text.replace(
			found,
			sourceLength,
			QString(QChar::ObjectReplacementCharacter));
		inlineFormulas->prepared->push_back({
			.position = found,
			.formulaIndex = formula.formulaIndex,
			.sourceLength = sourceLength,
			.copySource = formula.copySource,
		});
		removedLength += (sourceLength - 1);

		const auto &source = state->request->document->formulas[formula.formulaIndex];
		state->rememberFormula(
			formula.formulaIndex,
			MathKind::Inline,
			source.tex,
			inlineFormulas->textSize,
			inlineFormulas->renderWidthCap,
			inlineFormulas->renderHeightCap);
		++inlineFormulas->next;
	}
}

void AppendTextWithInlineFormulas(
		const MarkdownNode &node,
		const QString &value,
		TextWithEntities *text,
		InlineFormulaContext *inlineFormulas,
		PrepareState *state) {
	const auto from = text->text.size();
	text->append(value);
	ReplaceInlineFormulasInAppendedText(
		node,
		value,
		from,
		text,
		inlineFormulas,
		state);
}

[[nodiscard]] RawInlineTag ParseRawInlineTag(const MarkdownNode &node) {
	if (node.kind != NodeKind::HtmlInline) {
		return RawInlineTag::None;
	} else if (node.raw == u"<sub>"_q) {
		return RawInlineTag::SubOpen;
	} else if (node.raw == u"</sub>"_q) {
		return RawInlineTag::SubClose;
	} else if (node.raw == u"<sup>"_q) {
		return RawInlineTag::SupOpen;
	} else if (node.raw == u"</sup>"_q) {
		return RawInlineTag::SupClose;
	} else if (node.raw == u"<mark>"_q) {
		return RawInlineTag::MarkOpen;
	} else if (node.raw == u"</mark>"_q) {
		return RawInlineTag::MarkClose;
	}
	return RawInlineTag::None;
}

[[nodiscard]] bool IsOpeningRawInlineTag(RawInlineTag tag) {
	return (tag == RawInlineTag::SubOpen)
		|| (tag == RawInlineTag::SupOpen)
		|| (tag == RawInlineTag::MarkOpen);
}

[[nodiscard]] RawInlineTag MatchingClosingRawInlineTag(RawInlineTag tag) {
	switch (tag) {
	case RawInlineTag::SubOpen: return RawInlineTag::SubClose;
	case RawInlineTag::SupOpen: return RawInlineTag::SupClose;
	case RawInlineTag::MarkOpen: return RawInlineTag::MarkClose;
	default: return RawInlineTag::None;
	}
}

[[nodiscard]] EntityType EntityTypeForRawInlineTag(RawInlineTag tag) {
	switch (tag) {
	case RawInlineTag::SubOpen: return EntityType::Subscript;
	case RawInlineTag::SupOpen: return EntityType::Superscript;
	case RawInlineTag::MarkOpen: return EntityType::Marked;
	default: return EntityType::Invalid;
	}
}

[[nodiscard]] int FindMatchingRawInlineTag(
		const std::vector<MarkdownNode> &nodes,
		int from,
		int till,
		RawInlineTag openingTag) {
	if (!IsOpeningRawInlineTag(openingTag)) {
		return -1;
	}
	auto stack = std::vector<RawInlineTag>();
	stack.push_back(openingTag);
	for (auto i = from; i != till; ++i) {
		const auto tag = ParseRawInlineTag(nodes[i]);
		if (tag == RawInlineTag::None) {
			continue;
		} else if (IsOpeningRawInlineTag(tag)) {
			stack.push_back(tag);
		} else if (stack.empty()
			|| MatchingClosingRawInlineTag(stack.back()) != tag) {
			return -1;
		} else if (stack.size() == 1) {
			return i;
		} else {
			stack.pop_back();
		}
	}
	return -1;
}

void AppendInline(
	const MarkdownNode &node,
	TextWithEntities *text,
	std::vector<PreparedLink> *links,
	InlineFormulaContext *inlineFormulas,
	PrepareState *state);

void AppendInlineRange(
		const std::vector<MarkdownNode> &nodes,
		int from,
		int till,
		TextWithEntities *text,
		std::vector<PreparedLink> *links,
		InlineFormulaContext *inlineFormulas,
		PrepareState *state) {
	for (auto i = from; i != till; ++i) {
		if (state->cancelled()) {
			return;
		}
		const auto &node = nodes[i];
		const auto tag = ParseRawInlineTag(node);
		if (IsOpeningRawInlineTag(tag)) {
			const auto closing = FindMatchingRawInlineTag(
				nodes,
				i + 1,
				till,
				tag);
			if (closing > i) {
				const auto entityFrom = text->text.size();
				AppendInlineRange(
					nodes,
					i + 1,
					closing,
					text,
					links,
					inlineFormulas,
					state);
				if (state->cancelled()) {
					return;
				}
				const auto entityLength = text->text.size() - entityFrom;
				if (entityLength > 0) {
					text->entities.push_back(EntityInText(
						EntityTypeForRawInlineTag(tag),
						entityFrom,
						entityLength));
				}
				i = closing;
				continue;
			}
		}
		AppendInline(node, text, links, inlineFormulas, state);
	}
}

void AppendInline(
		const MarkdownNode &node,
		TextWithEntities *text,
		std::vector<PreparedLink> *links,
		InlineFormulaContext *inlineFormulas,
		PrepareState *state) {
	if (state->cancelled()) {
		return;
	}

	const auto from = text->text.size();
	switch (node.kind) {
	case NodeKind::Text:
	case NodeKind::InlineMath:
		AppendTextWithInlineFormulas(
			node,
			node.text,
			text,
			inlineFormulas,
			state);
		break;
	case NodeKind::SoftBreak:
		text->append(QChar(' '));
		break;
	case NodeKind::LineBreak:
		text->append(QChar('\n'));
		break;
	case NodeKind::Emphasis:
		AppendInlineRange(
			node.children,
			0,
			int(node.children.size()),
			text,
			links,
			inlineFormulas,
			state);
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::Italic,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::Strong:
		AppendInlineRange(
			node.children,
			0,
			int(node.children.size()),
			text,
			links,
			inlineFormulas,
			state);
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::Bold,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::Strike:
		AppendInlineRange(
			node.children,
			0,
			int(node.children.size()),
			text,
			links,
			inlineFormulas,
			state);
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::StrikeOut,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::InlineCode:
		if (!node.text.isEmpty()) {
			text->append(node.text);
		} else {
			AppendInlineRange(
				node.children,
				0,
				int(node.children.size()),
				text,
				links,
				inlineFormulas,
				state);
		}
		if (text->text.size() > from) {
			text->entities.push_back(
				EntityInText(
					EntityType::Code,
					from,
					text->text.size() - from));
		}
		break;
	case NodeKind::Link: {
		AppendInlineRange(
			node.children,
			0,
			int(node.children.size()),
			text,
			links,
			inlineFormulas,
			state);
		if (text->text.size() == from && !node.url.isEmpty()) {
			text->append(node.url);
		}
		const auto length = text->text.size() - from;
		if (length <= 0 || node.url.isEmpty()) {
			break;
		}
		const auto index = links->size() + 1;
		if (index > std::numeric_limits<uint16>::max()) {
			break;
		}
		text->entities.push_back(
			EntityInText(
				EntityType::CustomUrl,
				from,
				length,
				InternalLinkData(uint16(index))));
		links->push_back({
			.index = uint16(index),
			.target = node.url,
		});
	} break;
	case NodeKind::HtmlInline:
	case NodeKind::Unsupported:
		if (!node.raw.isEmpty()) {
			AppendTextWithInlineFormulas(
				node,
				node.raw,
				text,
				inlineFormulas,
				state);
		} else if (!node.children.empty()) {
			AppendInlineRange(
				node.children,
				0,
				int(node.children.size()),
				text,
				links,
				inlineFormulas,
				state);
		} else if (!node.text.isEmpty()) {
			AppendTextWithInlineFormulas(
				node,
				node.text,
				text,
				inlineFormulas,
				state);
		}
		break;
	default:
		if (!node.children.empty()) {
			AppendInlineRange(
				node.children,
				0,
				int(node.children.size()),
				text,
				links,
				inlineFormulas,
				state);
		} else if (!node.text.isEmpty()) {
			AppendTextWithInlineFormulas(
				node,
				node.text,
				text,
				inlineFormulas,
				state);
		} else if (!node.raw.isEmpty()) {
			AppendTextWithInlineFormulas(
				node,
				node.raw,
				text,
				inlineFormulas,
				state);
		}
		break;
	}
}

void PrepareTableCellText(
		const MarkdownNode &cell,
		bool header,
		TextWithEntities *text,
		std::vector<PreparedLink> *links,
		std::vector<PreparedInlineObject> *inlineObjects,
		PrepareState *state) {
	const auto &renderStyle = TableCellTextStyle(header, state->result.style);
	const auto textSize = TextSizeForFormula(renderStyle);
	auto formulas = CollectInlineFormulas(cell, state);
	auto inlineFormulas = InlineFormulaContext{
		.formulas = &formulas,
		.prepared = inlineObjects,
		.textSize = textSize,
		.renderWidthCap = ScaleFormulaCap(
			state->result.style.displayMathMaxRenderWidth,
			textSize,
			state->result.style.displayMathTextSize),
		.renderHeightCap = ScaleFormulaCap(
			state->result.style.displayMathMaxRenderHeight,
			textSize,
			state->result.style.displayMathTextSize),
	};
	if (!cell.children.empty()) {
		AppendInlineRange(
			cell.children,
			0,
			int(cell.children.size()),
			text,
			links,
			&inlineFormulas,
			state);
	} else if (!cell.text.isEmpty()) {
		AppendTextWithInlineFormulas(
			cell,
			cell.text,
			text,
			&inlineFormulas,
			state);
	} else if (!cell.raw.isEmpty()) {
		AppendTextWithInlineFormulas(
			cell,
			cell.raw,
			text,
			&inlineFormulas,
			state);
	}
	SortEntities(text);
}

[[nodiscard]] int EffectiveTableRowWidth(const MarkdownNode &row) {
	auto result = 0;
	auto expectedColumn = 0;
	for (const auto &cell : row.children) {
		if (cell.kind != NodeKind::TableCell) {
			return 0;
		}
		const auto column = (cell.tableColumn >= 0)
			? cell.tableColumn
			: expectedColumn;
		if (column != expectedColumn) {
			return 0;
		}
		result = std::max(result, column + 1);
		++expectedColumn;
	}
	return result;
}

[[nodiscard]] int EffectiveTableColumnCount(const MarkdownNode &node) {
	auto result = int(node.tableAlignments.size());
	for (const auto &row : node.children) {
		if (row.kind != NodeKind::TableRow) {
			return 0;
		}
		const auto width = EffectiveTableRowWidth(row);
		if (!width) {
			return 0;
		}
		result = std::max(result, width);
	}
	return result;
}

[[nodiscard]] std::vector<TableAlignment> NormalizedTableAlignments(
		const MarkdownNode &node,
		int columnCount) {
	auto result = std::vector<TableAlignment>(
		std::max(columnCount, 0),
		TableAlignment::None);
	const auto limit = std::min(columnCount, int(node.tableAlignments.size()));
	for (auto i = 0; i != limit; ++i) {
		result[i] = node.tableAlignments[i];
	}
	return result;
}

[[nodiscard]] bool ShouldFlattenTable(
		const MarkdownNode &node,
		PrepareContext context) {
	if (context.listDepth > 0 || context.quoteDepth > 0) {
		return true;
	}
	if (node.children.empty()) {
		return true;
	}
	const auto rowCount = int(node.children.size());
	if (rowCount > kMaxRenderedTableRows) {
		return true;
	}
	auto cellCount = 0;
	for (const auto &row : node.children) {
		if (row.kind != NodeKind::TableRow || row.children.empty()) {
			return true;
		}
		const auto width = EffectiveTableRowWidth(row);
		if (!width || width > kMaxRenderedTableColumns) {
			return true;
		}
		cellCount += width;
		if (cellCount > kMaxRenderedTableCells) {
			return true;
		}
	}
	const auto columnCount = EffectiveTableColumnCount(node);
	if (!columnCount || columnCount > kMaxRenderedTableColumns) {
		return true;
	}
	return (rowCount * columnCount) > kMaxRenderedTableCells;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFallbackBlocks(
	const MarkdownNode &node,
	PrepareContext context,
	PrepareState *state);

[[nodiscard]] std::vector<PreparedBlock> PrepareTableBlocks(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	const auto columnCount = EffectiveTableColumnCount(node);
	if (ShouldFlattenTable(node, context) || !columnCount) {
		return PrepareFallbackBlocks(node, context, state);
	}

	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Table;
	block.tableColumnCount = columnCount;
	block.tableAlignments = NormalizedTableAlignments(node, columnCount);
	block.tableRows.reserve(node.children.size());

	for (const auto &rowNode : node.children) {
		if (state->cancelled()) {
			return {};
		} else if (rowNode.kind != NodeKind::TableRow) {
			return PrepareFallbackBlocks(node, context, state);
		}

		auto row = PreparedTableRow();
		row.header = rowNode.tableHeader;
		row.cells.reserve(columnCount);

		auto expectedColumn = 0;
		for (const auto &cellNode : rowNode.children) {
			if (state->cancelled()) {
				return {};
			} else if (cellNode.kind != NodeKind::TableCell) {
				return PrepareFallbackBlocks(node, context, state);
			}
			const auto column = (cellNode.tableColumn >= 0)
				? cellNode.tableColumn
				: expectedColumn;
			if (column != expectedColumn || column >= columnCount) {
				return PrepareFallbackBlocks(node, context, state);
			}

			auto cell = PreparedTableCell();
			cell.column = column;
			cell.alignment = block.tableAlignments[column];
			PrepareTableCellText(
				cellNode,
				rowNode.tableHeader,
				&cell.text,
				&cell.links,
				&cell.inlineObjects,
				state);
			row.cells.push_back(std::move(cell));
			++expectedColumn;
		}
		for (auto column = expectedColumn; column != columnCount; ++column) {
			auto cell = PreparedTableCell();
			cell.column = column;
			cell.alignment = block.tableAlignments[column];
			row.cells.push_back(std::move(cell));
		}
		block.tableRows.push_back(std::move(row));
	}
	return { std::move(block) };
}

void AppendPrepared(
		std::vector<PreparedBlock> &&from,
		std::vector<PreparedBlock> *to) {
	for (auto &block : from) {
		to->push_back(std::move(block));
	}
}

void AppendRichBlock(
		std::vector<PreparedBlock> *blocks,
		PreparedBlockKind kind,
		int headingLevel,
		TextWithEntities text,
		std::vector<PreparedLink> links,
		std::vector<PreparedInlineObject> inlineObjects,
		bool allowEmpty = false) {
	SortEntities(&text);
	if (text.text.isEmpty() && !allowEmpty) {
		return;
	}
	auto block = PreparedBlock();
	block.kind = kind;
	block.headingLevel = headingLevel;
	block.text = std::move(text);
	block.links = std::move(links);
	block.inlineObjects = std::move(inlineObjects);
	blocks->push_back(std::move(block));
}

[[nodiscard]] PreparedBlock EmptyParagraphBlock() {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Paragraph;
	return block;
}

[[nodiscard]] PreparedBlock PrepareCodeBlock(const MarkdownNode &node) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::CodeBlock;
	block.text.text = node.text;
	block.codeLanguage = FirstInfoToken(node.info);
	return block;
}

[[nodiscard]] PreparedBlock PrepareRuleBlock() {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Rule;
	return block;
}

[[nodiscard]] PreparedBlock PrepareDisplayMathBlock(
		const MarkdownNode &node,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::DisplayMath;
	block.formulaTex = !node.text.isEmpty() ? node.text : node.raw;
	block.mathKind = MathKind::Display;
	block.formulaIndex = node.formulaIndex;
	state->rememberFormula(block);
	return block;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareBlocks(
	const MarkdownNode &node,
	PrepareContext context,
	PrepareState *state);

[[nodiscard]] std::vector<PreparedBlock> PrepareChildren(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	auto result = std::vector<PreparedBlock>();
	for (const auto &child : node.children) {
		if (state->cancelled()) {
			return {};
		}
		AppendPrepared(PrepareBlocks(child, context, state), &result);
	}
	return result;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFlowBlock(
		const MarkdownNode &node,
		PreparedBlockKind kind,
		PrepareState *state) {
	auto result = std::vector<PreparedBlock>();
	auto text = TextWithEntities();
	auto links = std::vector<PreparedLink>();
	auto inlineObjects = std::vector<PreparedInlineObject>();
	const auto &renderStyle = FlowTextStyle(
		kind,
		(kind == PreparedBlockKind::Heading) ? node.headingLevel : 0,
		state->result.style);
	const auto textSize = TextSizeForFormula(renderStyle);
	auto formulas = CollectInlineFormulas(node, state);
	auto inlineFormulas = InlineFormulaContext{
		.formulas = &formulas,
		.prepared = &inlineObjects,
		.textSize = textSize,
		.renderWidthCap = ScaleFormulaCap(
			state->result.style.displayMathMaxRenderWidth,
			textSize,
			state->result.style.displayMathTextSize),
		.renderHeightCap = ScaleFormulaCap(
			state->result.style.displayMathMaxRenderHeight,
			textSize,
			state->result.style.displayMathTextSize),
	};
	if (!node.children.empty()) {
		AppendInlineRange(
			node.children,
			0,
			int(node.children.size()),
			&text,
			&links,
			&inlineFormulas,
			state);
	} else if (!node.text.isEmpty()) {
		AppendTextWithInlineFormulas(
			node,
			node.text,
			&text,
			&inlineFormulas,
			state);
	}
	if (state->cancelled()) {
		return {};
	}
	AppendRichBlock(
		&result,
		kind,
		(kind == PreparedBlockKind::Heading) ? node.headingLevel : 0,
		std::move(text),
		std::move(links),
		std::move(inlineObjects));
	return result;
}

[[nodiscard]] PreparedBlock PrepareListItemBlock(
		const MarkdownNode &node,
		PrepareContext context,
		const PreparedBlock &list,
		int orderedNumber,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::ListItem;
	block.listKind = list.listKind;
	block.listDelimiter = list.listDelimiter;
	block.taskState = node.taskState;
	block.orderedNumber = orderedNumber;
	block.actualDepth = list.actualDepth;
	block.visualDepth = list.visualDepth;
	block.depthClamped = list.depthClamped;

	auto childContext = context;
	childContext.listDepth = context.listDepth + 1;
	for (const auto &child : node.children) {
		if (state->cancelled()) {
			return {};
		}
		AppendPrepared(PrepareBlocks(child, childContext, state), &block.children);
		if (state->result.cancelled) {
			return {};
		}
	}
	if (block.children.empty() && !state->result.cancelled) {
		block.children.push_back(EmptyParagraphBlock());
	}
	return block;
}

[[nodiscard]] PreparedBlock PrepareListBlock(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::List;
	block.listKind = node.listKind;
	block.listDelimiter = node.listDelimiter;
	block.startNumber = (node.listKind == ListKind::Ordered && node.listStart > 0)
		? node.listStart
		: 1;
	block.actualDepth = context.listDepth;
	block.visualDepth = CappedListDepth(block.actualDepth);
	block.depthClamped = (block.actualDepth > block.visualDepth);
	block.tight = node.tight;

	auto nextNumber = block.startNumber;
	auto childContext = context;
	childContext.listDepth = context.listDepth + 1;
	for (const auto &child : node.children) {
		if (state->cancelled()) {
			return {};
		}
		if (child.kind == NodeKind::ListItem) {
			auto item = PrepareListItemBlock(
				child,
				context,
				block,
				(node.listKind == ListKind::Ordered) ? nextNumber : 0,
				state);
			if (state->result.cancelled) {
				return {};
			}
			block.children.push_back(std::move(item));
			if (node.listKind == ListKind::Ordered) {
				++nextNumber;
			}
		} else {
			AppendPrepared(PrepareBlocks(child, childContext, state), &block.children);
			if (state->result.cancelled) {
				return {};
			}
		}
	}
	return block;
}

[[nodiscard]] PreparedBlock PrepareQuoteBlock(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Quote;
	block.actualDepth = context.quoteDepth;
	block.visualDepth = CappedQuoteDepth(block.actualDepth);
	block.depthClamped = (block.actualDepth > block.visualDepth);

	auto childContext = context;
	childContext.quoteDepth = context.quoteDepth + 1;
	block.children = PrepareChildren(node, childContext, state);
	if (state->result.cancelled) {
		return {};
	}
	return block;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFallbackBlocks(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	if (state->cancelled()) {
		return {};
	}
	if (!node.children.empty()) {
		return PrepareChildren(node, context, state);
	}
	const auto text = !node.text.isEmpty() ? node.text : node.raw;
	if (text.isEmpty()) {
		return {};
	}
	auto result = std::vector<PreparedBlock>();
	AppendRichBlock(
		&result,
		PreparedBlockKind::Paragraph,
		0,
		TextWithEntities::Simple(text),
		std::vector<PreparedLink>(),
		std::vector<PreparedInlineObject>());
	return result;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareBlocks(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	if (state->cancelled()) {
		return {};
	}

	switch (node.kind) {
	case NodeKind::Document:
	case NodeKind::TableRow:
	case NodeKind::TableCell:
	case NodeKind::HtmlBlock:
	case NodeKind::Unsupported:
		return PrepareFallbackBlocks(node, context, state);
	case NodeKind::DisplayMath:
		return { PrepareDisplayMathBlock(node, state) };
	case NodeKind::Paragraph:
		return PrepareFlowBlock(node, PreparedBlockKind::Paragraph, state);
	case NodeKind::Heading:
		return PrepareFlowBlock(node, PreparedBlockKind::Heading, state);
	case NodeKind::CodeBlock:
		return { PrepareCodeBlock(node) };
	case NodeKind::ThematicBreak:
		return { PrepareRuleBlock() };
	case NodeKind::List: {
		auto block = PrepareListBlock(node, context, state);
		if (state->result.cancelled) {
			return {};
		}
		return { std::move(block) };
	} break;
	case NodeKind::ListItem: {
		auto list = PreparedBlock();
		list.kind = PreparedBlockKind::List;
		list.actualDepth = context.listDepth;
		list.visualDepth = CappedListDepth(list.actualDepth);
		list.depthClamped = (list.actualDepth > list.visualDepth);
		auto block = PrepareListItemBlock(node, context, list, 0, state);
		if (state->result.cancelled) {
			return {};
		}
		return { std::move(block) };
	} break;
	case NodeKind::Blockquote: {
		auto block = PrepareQuoteBlock(node, context, state);
		if (state->result.cancelled) {
			return {};
		}
		return { std::move(block) };
	} break;
	case NodeKind::Table:
		return PrepareTableBlocks(node, context, state);
	default:
		return PrepareFallbackBlocks(node, context, state);
	}
	return {};
}

[[nodiscard]] PreparedRenderDocument PrepareRenderData(
		const PreparedDocument &document,
		PrepareState *state) {
	auto result = PreparedRenderDocument();
	result.blocks = PrepareBlocks(document.document, {}, state);
	return result;
}

[[nodiscard]] int FormulaSlotCount(const PreparedDocument &document) {
	auto result = 0;
	for (const auto &formula : document.formulas) {
		result = std::max(result, formula.index + 1);
	}
	return result;
}

[[nodiscard]] QColor Resolve(style::color color) {
	return color->c;
}

[[nodiscard]] bool RenderPreparedFormulas(PrepareState *state) {
	const auto &style = state->result.style;
	auto renderer = MathRenderer();
	for (auto &slot : state->result.formulas) {
		if (!slot.present) {
			continue;
		}
		if (state->cancelled()) {
			return false;
		}
		slot.rendered = renderer.renderFormula({
			.trimmedTex = slot.trimmedTex,
			.kind = slot.kind,
			.textSize = slot.textSize
				? slot.textSize
				: style.displayMathTextSize,
			.renderWidthCap = slot.renderWidthCap
				? slot.renderWidthCap
				: style.displayMathMaxRenderWidth,
			.renderHeightCap = slot.renderHeightCap
				? slot.renderHeightCap
				: style.displayMathMaxRenderHeight,
			.foreground = style.displayMathForegroundColor,
			.devicePixelRatio = style.devicePixelRatio,
		}, style.paletteVersion);
		if (state->cancelled()) {
			return false;
		}
	}
	return true;
}

} // namespace

MarkdownStyleSnapshot CaptureMarkdownStyleSnapshot() {
	auto result = MarkdownStyleSnapshot();
	result.textPalette = {
		.link = Resolve(st::ivMarkdownTextPalette.linkFg),
		.mono = Resolve(st::ivMarkdownTextPalette.monoFg),
		.spoiler = Resolve(st::ivMarkdownTextPalette.spoilerFg),
		.selectBackground = Resolve(st::ivMarkdownTextPalette.selectBg),
		.selectText = Resolve(st::ivMarkdownTextPalette.selectFg),
		.selectLink = Resolve(st::ivMarkdownTextPalette.selectLinkFg),
		.selectMono = Resolve(st::ivMarkdownTextPalette.selectMonoFg),
		.selectSpoiler = Resolve(st::ivMarkdownTextPalette.selectSpoilerFg),
		.selectOverlay = Resolve(st::ivMarkdownTextPalette.selectOverlay),
		.linkAlwaysActive = st::ivMarkdownTextPalette.linkAlwaysActive,
	};
	result.paragraphStyle = st::ivMarkdownParagraphStyle;
	result.heading1Style = st::ivMarkdownHeading1Style;
	result.heading2Style = st::ivMarkdownHeading2Style;
	result.heading3Style = st::ivMarkdownHeading3Style;
	result.heading4Style = st::ivMarkdownHeading4Style;
	result.heading5Style = st::ivMarkdownHeading5Style;
	result.heading6Style = st::ivMarkdownHeading6Style;
	result.codeStyle = st::ivMarkdownCodeStyle;
	result.codeLanguageStyle = st::ivMarkdownCodeLanguageStyle;
	result.displayMathFallbackStyle = st::ivMarkdownDisplayMathFallbackStyle;
	result.tableHeaderStyle = st::ivMarkdownTableHeaderStyle;
	result.pagePadding = st::ivMarkdownPagePadding;
	result.quotePadding = st::ivMarkdownQuotePadding;
	result.codePadding = st::ivMarkdownCodePadding;
	result.displayMathPadding = st::ivMarkdownDisplayMathPadding;
	result.displayMathFallbackPadding = st::ivMarkdownDisplayMathFallbackPadding;
	result.tableCellPadding = st::ivMarkdownTableCellPadding;
	result.displayMathAlign = st::ivMarkdownDisplayMathAlign;
	result.defaultTextColor = Resolve(st::windowFg);
	result.codeLanguageColor = Resolve(st::ivMarkdownCodeLanguageFg);
	result.taskMarkerColor = Resolve(st::ivMarkdownTaskMarkerFg);
	result.taskMarkerCheckColor = Resolve(st::ivMarkdownTaskMarkerCheckFg);
	result.quoteBorderColor = Resolve(st::ivMarkdownQuoteBorderFg);
	result.codeBackgroundColor = Resolve(st::ivMarkdownCodeBg);
	result.markBackgroundColor = Resolve(st::ivMarkdownMarkBackground);
	result.ruleColor = Resolve(st::ivMarkdownRuleFg);
	result.displayMathForegroundColor = Resolve(st::windowFg);
	result.displayMathFallbackBackgroundColor = Resolve(
		st::ivMarkdownDisplayMathFallbackBg);
	result.displayMathOverflowColor = Resolve(st::ivMarkdownDisplayMathOverflowFg);
	result.tableBorderColor = Resolve(st::ivMarkdownTableBorderFg);
	result.tableHeaderBackgroundColor = Resolve(st::ivMarkdownTableHeaderBg);
	result.tableOverflowColor = Resolve(st::ivMarkdownTableOverflowFg);
	result.subscriptScale = st::ivMarkdownSubscriptScale;
	result.superscriptScale = st::ivMarkdownSuperscriptScale;
	result.paragraphSkip = st::ivMarkdownParagraphSkip;
	result.headingSkip = st::ivMarkdownHeadingSkip;
	result.codeSkip = st::ivMarkdownCodeSkip;
	result.ruleSkip = st::ivMarkdownRuleSkip;
	result.displayMathSkip = st::ivMarkdownDisplayMathSkip;
	result.tableSkip = st::ivMarkdownTableSkip;
	result.quoteSkip = st::ivMarkdownQuoteSkip;
	result.listIndent = st::ivMarkdownListIndent;
	result.listContinuationIndent = st::ivMarkdownListContinuationIndent;
	result.listMarkerWidth = st::ivMarkdownListMarkerWidth;
	result.listMarkerSkip = st::ivMarkdownListMarkerSkip;
	result.taskMarkerSize = st::ivMarkdownTaskMarkerSize;
	result.taskMarkerBorder = st::ivMarkdownTaskMarkerBorder;
	result.quoteIndent = st::ivMarkdownQuoteIndent;
	result.quoteBorder = st::ivMarkdownQuoteBorder;
	result.codeRadius = st::ivMarkdownCodeRadius;
	result.codeLanguageSkip = st::ivMarkdownCodeLanguageSkip;
	result.subscriptBaselineOffset = st::ivMarkdownSubscriptBaselineOffset;
	result.superscriptBaselineOffset = st::ivMarkdownSuperscriptBaselineOffset;
	result.ruleHeight = st::ivMarkdownRuleHeight;
	result.displayMathTextSize = st::ivMarkdownDisplayMathTextSize;
	result.displayMathMaxRenderWidth = st::ivMarkdownDisplayMathMaxRenderWidth;
	result.displayMathMaxRenderHeight = st::ivMarkdownDisplayMathMaxRenderHeight;
	result.displayMathFallbackRadius = st::ivMarkdownDisplayMathFallbackRadius;
	result.displayMathOverflowWidth = st::ivMarkdownDisplayMathOverflowWidth;
	result.tableBorder = st::ivMarkdownTableBorder;
	result.tableMinColumnWidth = st::ivMarkdownTableMinColumnWidth;
	result.tableOverflowWidth = st::ivMarkdownTableOverflowWidth;
	result.paletteVersion = style::PaletteVersion();
	result.devicePixelRatio = style::DevicePixelRatio();
	return result;
}

PreparedResult PrepareSynchronously(PrepareRequest request) {
	auto state = PrepareState();
	state.request = &request;
	state.result.style = request.style;
	state.result.generation = request.generation;

	if (!request.document) {
		return state.result;
	}

	state.sourceUtf8 = request.document->sourceText.toUtf8();
	state.result.formulas.resize(FormulaSlotCount(*request.document));
	if (state.cancelled()) {
		return std::move(state.result);
	}

	state.result.blocks = PrepareRenderData(*request.document, &state);
	if (state.result.cancelled) {
		ClearPreparedOutput(&state.result);
		return std::move(state.result);
	}
	if (!RenderPreparedFormulas(&state)) {
		ClearPreparedOutput(&state.result);
	}
	return std::move(state.result);
}

void PrepareAsync(PrepareRequest request, Fn<void(PreparedResult)> done) {
	if (!done) {
		return;
	}
	base::call_delayed(0, [
		request = std::move(request),
		done = std::move(done)
	]() mutable {
		done(PrepareSynchronously(std::move(request)));
	});
}

} // namespace Iv::Markdown
