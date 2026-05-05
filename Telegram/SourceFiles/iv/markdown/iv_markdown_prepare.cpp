#include "iv/markdown/iv_markdown_prepare.h"

#include "iv/markdown/iv_markdown_parse.h"
#include "base/unixtime.h"
#include "iv/iv_prepare.h"
#include "lang/lang_keys.h"

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>

#include <algorithm>
#include <limits>
#include <utility>
#include "styles/style_iv.h"

namespace Iv::Markdown {

const MarkdownPrepareLimits &PrepareLimitsForIv() {
	static const auto result = MarkdownPrepareLimits{
		.tableRender = {
			.maxRows = 128,
			.maxColumns = 16,
			.maxCells = 1024,
		},
		.maxPreparedBlocks = 4096,
	};
	return result;
}

const MarkdownPrepareTableRenderLimits &PrepareTableRenderLimitsForIv() {
	return PrepareLimitsForIv().tableRender;
}

namespace {

constexpr auto kMaxVisualListDepth = 6;
constexpr auto kMaxVisualQuoteDepth = 3;

struct PrepareContext {
	int listDepth = 0;
	int quoteDepth = 0;
};

struct FootnoteDefinitionEntry {
	const MarkdownNode *node = nullptr;
};

struct PrepareState {
	const PrepareRequest *request = nullptr;
	MarkdownArticleContent result;
	QByteArray sourceUtf8;
	std::vector<FootnoteDefinitionEntry> footnoteDefinitions;
	std::vector<std::pair<QString, QString>> firstFootnoteReferences;
	int nextGeneratedId = 0;

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
			request->dimensions.displayMathTextSize,
			request->dimensions.displayMathMaxRenderWidth,
			request->dimensions.displayMathMaxRenderHeight);
	}

	void addPrepareWarning() {
		++result.debug.prepareWarningCount;
	}

	void addFormulaWarning() {
		++result.debug.formulaWarningCount;
	}

	void addPrepareWarnings(int count) {
		result.debug.prepareWarningCount += count;
	}

	void addFormulaWarnings(int count) {
		result.debug.formulaWarningCount += count;
	}

	void setTerminalFailure(
			PrepareTerminalFailure terminal,
			QString debugReason) {
		if (result.failure.failed()) {
			return;
		}
		result.failure.terminal = terminal;
		result.failure.debugReason = std::move(debugReason);
	}

	[[nodiscard]] QString formulaSourceText(int index) const {
		if (!request
			|| !request->document
			|| index < 0
			|| index >= int(request->document->formulas.size())) {
			return QString();
		}
		const auto &range = request->document->formulas[index].range;
		const auto size = int(sourceUtf8.size());
		const auto from = std::clamp(range.startOffset, 0, size);
		const auto till = std::clamp(range.endOffset, from, size);
		return QString::fromUtf8(sourceUtf8.constData() + from, till - from);
	}

	[[nodiscard]] QString firstFootnoteReferenceAnchor(
			const QString &label) const {
		for (const auto &entry : firstFootnoteReferences) {
			if (entry.first == label) {
				return entry.second;
			}
		}
		return QString();
	}

	[[nodiscard]] QString rememberFootnoteReferenceAnchor(
			const QString &label,
			QString *blockAnchorId) {
		if (label.isEmpty()) {
			return QString();
		}
		if (const auto existing = firstFootnoteReferenceAnchor(label); !existing.isEmpty()) {
			return existing;
		}
		auto anchorId = (blockAnchorId && !blockAnchorId->isEmpty())
			? *blockAnchorId
			: (u"fnref-"_q + QString::number(++nextGeneratedId));
		if (blockAnchorId && blockAnchorId->isEmpty()) {
			*blockAnchorId = anchorId;
		}
		firstFootnoteReferences.push_back({ label, anchorId });
		return anchorId;
	}
};

struct InlineFormulaSource {
	int formulaIndex = -1;
	SourceRange range;
	QString copySource;
};

struct InlineFormulaContext {
	const std::vector<InlineFormulaSource> *formulas = nullptr;
	QString *blockAnchorId = nullptr;
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

[[nodiscard]] QString InvalidStyleReason(
		const MarkdownPrepareDimensions &dimensions) {
	Q_UNUSED(dimensions);
	return QString();
}

void ClearPreparedOutput(MarkdownArticleContent *result) {
	result->blocks.blocks.clear();
	result->formulas.clear();
}

[[nodiscard]] QString InternalLinkData(uint16 index) {
	return u"internal:index"_q + QChar(index);
}

[[nodiscard]] QString EncodeInlineTextObjectField(const QString &value) {
	return QString::fromLatin1(value.toUtf8().toPercentEncoding());
}

[[nodiscard]] QString DecodeInlineTextObjectField(const QString &value) {
	return QString::fromUtf8(QByteArray::fromPercentEncoding(value.toLatin1()));
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

[[nodiscard]] bool HasUrlScheme(const QString &target) {
	if (target.isEmpty()) {
		return false;
	}
	const auto colon = target.indexOf(QChar(':'));
	if (colon <= 0) {
		return false;
	}
	const auto slash = target.indexOf(QChar('/'));
	const auto question = target.indexOf(QChar('?'));
	const auto hash = target.indexOf(QChar('#'));
	auto limit = target.size();
	for (const auto value : { slash, question, hash }) {
		if (value >= 0) {
			limit = std::min(limit, value);
		}
	}
	if (colon >= limit) {
		return false;
	}
	if (!target[0].isLetter()) {
		return false;
	}
	for (auto i = 1; i != colon; ++i) {
		const auto ch = target[i];
		if (!ch.isLetterOrNumber() && ch != QChar('+') && ch != QChar('-')
			&& ch != QChar('.')) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool LooksLikeWindowsDrivePath(const QString &target) {
	return target.size() >= 2
		&& target[0].isLetter()
		&& target[1] == QChar(':');
}

[[nodiscard]] bool LooksLikeFileUrl(const QString &target) {
	return target.size() >= 5
		&& target.left(5).compare(u"file:"_q, Qt::CaseInsensitive) == 0;
}

[[nodiscard]] bool LooksLikeFilesystemTarget(const QString &target) {
	return target.startsWith(u"/"_q)
		|| target.startsWith(u"\\"_q)
		|| target.startsWith(u"//"_q)
		|| target.startsWith(u"\\\\"_q)
		|| LooksLikeWindowsDrivePath(target)
		|| LooksLikeFileUrl(target);
}

[[nodiscard]] QString ComparablePath(QString path) {
	path = QDir::fromNativeSeparators(QDir::cleanPath(path));
	return path.toLower();
}

[[nodiscard]] bool IsContainedPath(
		const QString &baseDirectory,
		const QString &resolvedPath) {
	const auto base = ComparablePath(baseDirectory);
	const auto resolved = ComparablePath(resolvedPath);
	return (resolved == base) || resolved.startsWith(base + u"/"_q);
}

[[nodiscard]] QString DetailsAnchorId(PrepareState *state) {
	return u"details-"_q + QString::number(++state->nextGeneratedId);
}

[[nodiscard]] QString FootnoteDefinitionAnchor(const MarkdownNode &node) {
	return !node.anchorId.isEmpty()
		? node.anchorId
		: (node.footnoteOrdinal > 0
			? (u"fn-"_q + QString::number(node.footnoteOrdinal))
			: QString());
}

[[nodiscard]] PreparedLink ClassifiedLink(
		uint16 index,
		QString target,
		const PrepareState *state) {
	auto result = PreparedLink();
	result.index = index;
	result.copyText = target;
	if (target.startsWith(QChar('#'))) {
		result.kind = PreparedLinkKind::Anchor;
		result.target = NormalizeFragmentId(target.mid(1));
		return result;
	}

	auto fragmentIndex = target.indexOf(QChar('#'));
	if (fragmentIndex >= 0) {
		result.fragment = NormalizeFragmentId(target.mid(fragmentIndex + 1));
		target = target.left(fragmentIndex);
	}
	result.target = target;

	if (target.isEmpty()) {
		result.kind = PreparedLinkKind::Anchor;
		result.target = result.fragment;
		result.fragment = QString();
		return result;
	}
	if (LooksLikeFilesystemTarget(target)) {
		result.kind = PreparedLinkKind::RejectedRelative;
		return result;
	}
	if (HasUrlScheme(target)) {
		result.kind = PreparedLinkKind::External;
		return result;
	}
	if (!state
		|| !state->request
		|| state->request->sourcePath.isEmpty()) {
		result.kind = PreparedLinkKind::RejectedRelative;
		return result;
	}
	if (target.contains(QChar('?'))) {
		result.kind = PreparedLinkKind::RejectedRelative;
		return result;
	}
	const auto baseDirectory = QFileInfo(state->request->sourcePath).absolutePath();
	if (baseDirectory.isEmpty()) {
		result.kind = PreparedLinkKind::RejectedRelative;
		return result;
	}
	const auto resolved = QDir(baseDirectory).absoluteFilePath(target);
	const auto cleanedResolved = QDir::cleanPath(resolved);
	if (!IsContainedPath(baseDirectory, cleanedResolved)
		|| !LooksLikeMarkdownFile(cleanedResolved)) {
		result.kind = PreparedLinkKind::RejectedRelative;
		return result;
	}
	result.kind = PreparedLinkKind::LocalFile;
	result.target = cleanedResolved;
	return result;
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

struct DecodedDisplaySpan {
	int offset = -1;
	int length = 0;
};

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

[[nodiscard]] DecodedDisplaySpan DisplaySpanForSourceRange(
		const MarkdownNode &node,
		const QString &value,
		const SourceRange &range,
		const PrepareState *state) {
	auto result = DecodedDisplaySpan();
	if (!state
		|| !node.range.available
		|| !range.available
		|| range.startOffset < node.range.startOffset
		|| range.endOffset < range.startOffset
		|| range.endOffset > node.range.endOffset) {
		return result;
	}
	const auto offset = DisplayOffsetForSourceOffset(
		node,
		value,
		range.startOffset,
		state);
	if (offset < 0) {
		return result;
	}
	const auto decoded = DecodeMarkdownTextPrefix(
		state->sourceUtf8.mid(
			range.startOffset,
			range.endOffset - range.startOffset));
	const auto length = int(decoded.size());
	if ((offset + length) > value.size()
		|| value.mid(offset, length) != decoded) {
		return result;
	}
	result.offset = offset;
	result.length = length;
	return result;
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

[[nodiscard]] int FlowFormulaTextSize(
		PreparedBlockKind kind,
		int headingLevel,
		const MarkdownPrepareDimensions &dimensions) {
	if (kind != PreparedBlockKind::Heading) {
		return dimensions.bodyTextSize;
	}
	const auto index = std::clamp(headingLevel, 1, 6) - 1;
	if (index < int(dimensions.headingTextSizes.size())) {
		return dimensions.headingTextSizes[index];
	}
	return dimensions.bodyTextSize;
}

[[nodiscard]] int TableCellFormulaTextSize(
		bool header,
		const MarkdownPrepareDimensions &dimensions) {
	return header
		? dimensions.tableHeaderTextSize
		: dimensions.bodyTextSize;
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

		const auto displaySpan = DisplaySpanForSourceRange(
			node,
			value,
			formula.range,
			state);
		if (displaySpan.offset < 0 || displaySpan.length <= 0) {
			++inlineFormulas->next;
			continue;
		}
		const auto found = from + displaySpan.offset - removedLength;
		text->text.replace(
			found,
			displaySpan.length,
			QString(QChar::ObjectReplacementCharacter));
		const auto &source = state->request->document->formulas[formula.formulaIndex];
		const auto entityData = SerializeInlineTextObjectEntity({
			.kind = InlineTextObjectKind::Formula,
			.data = InlineTextObjectFormulaData{
				.copySource = formula.copySource,
				.trimmedTex = source.tex.trimmed(),
			},
		});
		if (!entityData.isEmpty()) {
			text->entities.push_back(EntityInText(
				EntityType::CustomEmoji,
				found,
				1,
				entityData));
		}
		removedLength += (displaySpan.length - 1);

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
		const auto preparedLink = ClassifiedLink(uint16(index), node.url, state);
		text->entities.push_back(
			EntityInText(
				EntityType::CustomUrl,
				from,
				length,
				InternalLinkData(uint16(index))));
		links->push_back({
			.index = uint16(index),
			.kind = preparedLink.kind,
			.target = preparedLink.target,
			.fragment = preparedLink.fragment,
			.copyText = preparedLink.copyText,
		});
	} break;
	case NodeKind::FootnoteReference: {
		if (node.footnoteOrdinal <= 0) {
			const auto fallback = !node.raw.isEmpty()
				? node.raw
				: (u"[^"_q + node.footnoteLabel + u"]"_q);
			AppendTextWithInlineFormulas(
				node,
				fallback,
				text,
				inlineFormulas,
				state);
			break;
		}
		const auto index = links->size() + 1;
		const auto display = QString::number(node.footnoteOrdinal);
		text->append(display);
		if (text->text.size() > from) {
			text->entities.push_back(EntityInText(
				EntityType::Superscript,
				from,
				text->text.size() - from));
		}
		if (index <= std::numeric_limits<uint16>::max()) {
			text->entities.push_back(EntityInText(
				EntityType::CustomUrl,
				from,
				display.size(),
				InternalLinkData(uint16(index))));
			links->push_back({
				.index = uint16(index),
				.kind = PreparedLinkKind::Footnote,
				.target = FootnoteDefinitionAnchor(node),
				.copyText = u"#"_q + FootnoteDefinitionAnchor(node),
			});
			if (inlineFormulas) {
				const auto remembered = state->rememberFootnoteReferenceAnchor(
					node.footnoteLabel,
					inlineFormulas->blockAnchorId);
				static_cast<void>(remembered);
			}
		}
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
		PrepareState *state) {
	const auto textSize = TableCellFormulaTextSize(
		header,
		state->request->dimensions);
	auto formulas = CollectInlineFormulas(cell, state);
	auto inlineFormulas = InlineFormulaContext{
		.formulas = &formulas,
		.textSize = textSize,
		.renderWidthCap = ScaleFormulaCap(
			state->request->dimensions.displayMathMaxRenderWidth,
			textSize,
			state->request->dimensions.displayMathTextSize),
		.renderHeightCap = ScaleFormulaCap(
			state->request->dimensions.displayMathMaxRenderHeight,
			textSize,
			state->request->dimensions.displayMathTextSize),
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
		PrepareContext context,
		PrepareState *state) {
	const auto &limits = PrepareLimitsForIv().tableRender;
	if (context.listDepth > 0 || context.quoteDepth > 0) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	if (node.children.empty()) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	const auto rowCount = int(node.children.size());
	if (rowCount > limits.maxRows) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	auto cellCount = 0;
	for (const auto &row : node.children) {
		if (row.kind != NodeKind::TableRow || row.children.empty()) {
			if (state) {
				state->addPrepareWarning();
			}
			return true;
		}
		const auto width = EffectiveTableRowWidth(row);
		if (!width || width > limits.maxColumns) {
			if (state) {
				state->addPrepareWarning();
			}
			return true;
		}
		cellCount += width;
		if (cellCount > limits.maxCells) {
			if (state) {
				state->addPrepareWarning();
			}
			return true;
		}
	}
	const auto columnCount = EffectiveTableColumnCount(node);
	if (!columnCount || columnCount > limits.maxColumns) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	if ((rowCount * columnCount) > limits.maxCells) {
		if (state) {
			state->addPrepareWarning();
		}
		return true;
	}
	return false;
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
	if (ShouldFlattenTable(node, context, state) || !columnCount) {
		return PrepareFallbackBlocks(node, context, state);
	}

	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Table;
	block.tableColumnCount = columnCount;
	block.tableAlignments = NormalizedTableAlignments(node, columnCount);
	block.tableRows.reserve(node.children.size());

	for (const auto &rowNode : node.children) {
		if (rowNode.kind != NodeKind::TableRow) {
			return PrepareFallbackBlocks(node, context, state);
		}

		auto row = PreparedTableRow();
		row.header = rowNode.tableHeader;
		row.cells.reserve(columnCount);

		auto expectedColumn = 0;
		for (const auto &cellNode : rowNode.children) {
			if (cellNode.kind != NodeKind::TableCell) {
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
		QString anchorId = QString(),
		bool collapsed = false,
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
	block.anchorId = std::move(anchorId);
	block.collapsed = collapsed;
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

void CollectFootnoteDefinitions(
		const MarkdownNode &node,
		std::vector<FootnoteDefinitionEntry> *definitions) {
	if (!definitions) {
		return;
	}
	if (node.kind == NodeKind::FootnoteDefinition && node.footnoteOrdinal > 0) {
		if (node.footnoteOrdinal > int(definitions->size())) {
			definitions->resize(node.footnoteOrdinal);
		}
		auto &entry = (*definitions)[node.footnoteOrdinal - 1];
		if (!entry.node) {
			entry.node = &node;
		}
	}
	for (const auto &child : node.children) {
		CollectFootnoteDefinitions(child, definitions);
	}
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
		AppendPrepared(PrepareBlocks(child, context, state), &result);
	}
	return result;
}

void AppendFootnoteBacklink(PreparedBlock *block, const QString &target) {
	if (!block || target.isEmpty()) {
		return;
	}
	const auto index = block->links.size() + 1;
	if (index > std::numeric_limits<uint16>::max()) {
		return;
	}
	if (!block->text.text.isEmpty()) {
		block->text.append(QChar(' '));
	}
	const auto from = block->text.text.size();
	const auto label = u"[back]"_q;
	block->text.append(label);
	block->text.entities.push_back(EntityInText(
		EntityType::CustomUrl,
		from,
		label.size(),
		InternalLinkData(uint16(index))));
	block->links.push_back({
		.index = uint16(index),
		.kind = PreparedLinkKind::FootnoteBacklink,
		.target = target,
		.copyText = u"#"_q + target,
	});
	SortEntities(&block->text);
}

void AppendFootnotes(
		std::vector<PreparedBlock> *blocks,
		PrepareState *state) {
	if (!blocks || !state || state->footnoteDefinitions.empty()) {
		return;
	}
	auto list = PreparedBlock();
	list.kind = PreparedBlockKind::List;
	list.listKind = ListKind::Ordered;
	list.listDelimiter = ListDelimiter::Period;
	list.startNumber = 1;
	for (const auto &entry : state->footnoteDefinitions) {
		if (!entry.node) {
			return;
		}
		auto item = PreparedBlock();
		item.kind = PreparedBlockKind::ListItem;
		item.listKind = ListKind::Ordered;
		item.listDelimiter = ListDelimiter::Period;
		item.orderedNumber = entry.node->footnoteOrdinal;
		item.anchorId = FootnoteDefinitionAnchor(*entry.node);
		item.children = PrepareChildren(*entry.node, {}, state);
		if (item.children.empty()) {
			item.children.push_back(EmptyParagraphBlock());
		}
		const auto backlink = state->firstFootnoteReferenceAnchor(
			entry.node->footnoteLabel);
		if (!item.children.empty()
			&& item.children.back().kind == PreparedBlockKind::Paragraph) {
			AppendFootnoteBacklink(&item.children.back(), backlink);
		} else if (!backlink.isEmpty()) {
			auto paragraph = EmptyParagraphBlock();
			AppendFootnoteBacklink(&paragraph, backlink);
			item.children.push_back(std::move(paragraph));
		}
		list.children.push_back(std::move(item));
	}
	if (list.children.empty()) {
		return;
	}
	blocks->push_back(PrepareRuleBlock());
	blocks->push_back(std::move(list));
}

[[nodiscard]] std::vector<PreparedBlock> PrepareNestedDetailsBody(
		const MarkdownNode &node,
		PrepareState *state) {
	const auto fallback = [&] {
		if (state) {
			state->addPrepareWarning();
		}
		auto blocks = std::vector<PreparedBlock>();
		AppendRichBlock(
			&blocks,
			PreparedBlockKind::Paragraph,
			0,
			TextWithEntities::Simple(node.detailsBody),
			std::vector<PreparedLink>());
		return blocks;
	};
	if (node.detailsBody.isEmpty()) {
		return {};
	}
	const auto parsed = ParseMarkdownForIv(
		node.detailsBody.toUtf8(),
		ParseOptions{ state->request->document->sourceName + u"#details"_q });
	if (!parsed.ok
		|| !parsed.document.formulas.empty()
		|| parsed.document.stats.footnotesSeen) {
		return fallback();
	}
	auto nestedRequest = PrepareRequest{
		.document = std::make_shared<const PreparedDocument>(parsed.document),
		.renderer = state->request->renderer,
		.dimensions = state->request->dimensions,
		.sourcePath = state->request->sourcePath,
	};
	auto nested = PrepareSynchronously(std::move(nestedRequest));
	state->addPrepareWarnings(nested.debug.prepareWarningCount);
	state->addFormulaWarnings(nested.debug.formulaWarningCount);
	return nested.failure.failed()
		? fallback()
		: std::move(nested.blocks.blocks);
}

[[nodiscard]] std::vector<PreparedBlock> PrepareDetailsBlocks(
		const MarkdownNode &node,
		PrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Details;
	block.anchorId = DetailsAnchorId(state);
	block.collapsed = !node.detailsOpen;
	block.text.text = (node.detailsOpen ? u"v "_q : u"> "_q)
		+ node.detailsSummary;
	if (!block.text.text.isEmpty()) {
		block.text.entities.push_back(EntityInText(
			EntityType::CustomUrl,
			0,
			block.text.text.size(),
			InternalLinkData(1)));
		block.links.push_back({
			.index = 1,
			.kind = PreparedLinkKind::ToggleDetails,
			.target = block.anchorId,
		});
	}
	block.children = PrepareNestedDetailsBody(node, state);
	return { std::move(block) };
}

[[nodiscard]] std::vector<PreparedBlock> PrepareDocumentBlocks(
		const MarkdownNode &node,
		PrepareState *state) {
	auto result = PrepareChildren(node, {}, state);
	AppendFootnotes(&result, state);
	return result;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFlowBlock(
		const MarkdownNode &node,
		PreparedBlockKind kind,
		PrepareState *state) {
	auto result = std::vector<PreparedBlock>();
	auto anchorId = (kind == PreparedBlockKind::Heading)
		? node.anchorId
		: QString();
	auto text = TextWithEntities();
	auto links = std::vector<PreparedLink>();
	const auto textSize = FlowFormulaTextSize(
		kind,
		(kind == PreparedBlockKind::Heading) ? node.headingLevel : 0,
		state->request->dimensions);
	auto formulas = CollectInlineFormulas(node, state);
	auto inlineFormulas = InlineFormulaContext{
		.formulas = &formulas,
		.blockAnchorId = &anchorId,
		.textSize = textSize,
		.renderWidthCap = ScaleFormulaCap(
			state->request->dimensions.displayMathMaxRenderWidth,
			textSize,
			state->request->dimensions.displayMathTextSize),
		.renderHeightCap = ScaleFormulaCap(
			state->request->dimensions.displayMathMaxRenderHeight,
			textSize,
			state->request->dimensions.displayMathTextSize),
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
	AppendRichBlock(
		&result,
		kind,
		(kind == PreparedBlockKind::Heading) ? node.headingLevel : 0,
		std::move(text),
		std::move(links),
		std::move(anchorId));
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
		AppendPrepared(PrepareBlocks(child, childContext, state), &block.children);
	}
	if (block.children.empty()) {
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
		if (child.kind == NodeKind::ListItem) {
			auto item = PrepareListItemBlock(
				child,
				context,
				block,
				(node.listKind == ListKind::Ordered) ? nextNumber : 0,
				state);
			block.children.push_back(std::move(item));
			if (node.listKind == ListKind::Ordered) {
				++nextNumber;
			}
		} else {
			AppendPrepared(PrepareBlocks(child, childContext, state), &block.children);
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
	return block;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareFallbackBlocks(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	if (node.kind == NodeKind::HtmlBlock) {
		if (node.htmlBlockKind == HtmlBlockKind::Comment) {
			return {};
		} else if (node.htmlBlockKind == HtmlBlockKind::Details) {
			return PrepareDetailsBlocks(node, state);
		}
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
		std::vector<PreparedLink>());
	return result;
}

[[nodiscard]] std::vector<PreparedBlock> PrepareBlocks(
		const MarkdownNode &node,
		PrepareContext context,
		PrepareState *state) {
	switch (node.kind) {
	case NodeKind::Document:
		return PrepareDocumentBlocks(node, state);
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
	case NodeKind::FootnoteDefinition:
		return {};
	case NodeKind::CodeBlock:
		return { PrepareCodeBlock(node) };
	case NodeKind::ThematicBreak:
		return { PrepareRuleBlock() };
	case NodeKind::List: {
		auto block = PrepareListBlock(node, context, state);
		return { std::move(block) };
	} break;
	case NodeKind::ListItem: {
		auto list = PreparedBlock();
		list.kind = PreparedBlockKind::List;
		list.actualDepth = context.listDepth;
		list.visualDepth = CappedListDepth(list.actualDepth);
		list.depthClamped = (list.actualDepth > list.visualDepth);
		auto block = PrepareListItemBlock(node, context, list, 0, state);
		return { std::move(block) };
	} break;
	case NodeKind::Blockquote: {
		auto block = PrepareQuoteBlock(node, context, state);
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
	state->footnoteDefinitions.clear();
	CollectFootnoteDefinitions(document.document, &state->footnoteDefinitions);
	result.blocks = PrepareBlocks(document.document, {}, state);
	return result;
}

[[nodiscard]] int CountPreparedBlocks(const std::vector<PreparedBlock> &blocks) {
	auto result = 0;
	for (const auto &block : blocks) {
		++result;
		result += CountPreparedBlocks(block.children);
	}
	return result;
}

[[nodiscard]] int FormulaSlotCount(const PreparedDocument &document) {
	auto result = 0;
	for (const auto &formula : document.formulas) {
		result = std::max(result, formula.index + 1);
	}
	return result;
}

[[nodiscard]] PreparedFormulaMeasurementSignature FormulaMeasurementSignature(
		const PreparedFormulaSlot &slot,
		const MarkdownPrepareDimensions &dimensions) {
	return {
		.trimmedTex = slot.trimmedTex.trimmed(),
		.kind = slot.kind,
		.textSize = slot.textSize
			? slot.textSize
			: dimensions.displayMathTextSize,
		.renderWidthCap = slot.renderWidthCap
			? slot.renderWidthCap
			: dimensions.displayMathMaxRenderWidth,
		.renderHeightCap = slot.renderHeightCap
			? slot.renderHeightCap
			: dimensions.displayMathMaxRenderHeight,
	};
}

[[nodiscard]] std::shared_ptr<const MeasuredFormula>
FindDocumentFormulaMeasurement(
		const std::shared_ptr<const PreparedDocument> &document,
		int index,
		const PreparedFormulaMeasurementSignature &signature) {
	const auto cache = document ? document->formulaMeasurementCache : nullptr;
	if (!cache) {
		return nullptr;
	}
	if (index >= 0 && index < int(cache->slots.size())) {
		const auto &entry = cache->slots[index];
		if (entry.data && entry.signature == signature) {
			cache->bySignature.emplace(signature, entry.data);
			return entry.data;
		}
	}
	if (const auto i = cache->bySignature.find(signature)
		; i != end(cache->bySignature)) {
		if (index >= 0) {
			if (index >= int(cache->slots.size())) {
				cache->slots.resize(index + 1);
			}
			cache->slots[index] = {
				.signature = signature,
				.data = i->second,
			};
		}
		return i->second;
	}
	return nullptr;
}

void RememberDocumentFormulaMeasurement(
		const std::shared_ptr<const PreparedDocument> &document,
		int index,
		PreparedFormulaMeasurementSignature signature,
		std::shared_ptr<const MeasuredFormula> data) {
	const auto cache = document ? document->formulaMeasurementCache : nullptr;
	if (!cache || index < 0 || !data) {
		return;
	}
	auto shared = std::move(data);
	if (const auto i = cache->bySignature.find(signature)
		; i != end(cache->bySignature)) {
		shared = i->second;
	} else {
		cache->bySignature.emplace(signature, shared);
	}
	if (index >= int(cache->slots.size())) {
		cache->slots.resize(index + 1);
	}
	cache->slots[index] = {
		.signature = std::move(signature),
		.data = std::move(shared),
	};
}

void MeasurePreparedFormulas(PrepareState *state) {
	const auto &dimensions = state->request->dimensions;
	auto ownedRenderer = std::shared_ptr<MathRenderer>();
	auto renderer = state->request ? state->request->renderer.get() : nullptr;
	if (!renderer) {
		ownedRenderer = std::make_shared<MathRenderer>();
		renderer = ownedRenderer.get();
	}
	auto timer = QElapsedTimer();
	timer.start();
	for (auto i = 0, count = int(state->result.formulas.size()); i != count; ++i) {
		auto &slot = state->result.formulas[i];
		if (!slot.present) {
			continue;
		}
		const auto signature = FormulaMeasurementSignature(slot, dimensions);
		if (const auto cached = FindDocumentFormulaMeasurement(
				state->request->document,
				i,
				signature)) {
			slot.measuredData = cached;
			slot.measured = *cached;
		} else {
			auto data = std::make_shared<MeasuredFormula>(renderer->measureFormula({
				.trimmedTex = signature.trimmedTex,
				.kind = signature.kind,
				.textSize = signature.textSize,
				.renderWidthCap = signature.renderWidthCap,
				.renderHeightCap = signature.renderHeightCap,
			}));
			slot.measuredData = data;
			slot.measured = *data;
			RememberDocumentFormulaMeasurement(
				state->request->document,
				i,
				signature,
				std::move(data));
		}
		if (!slot.measured.success) {
			state->addFormulaWarning();
		}
	}
	state->result.debug.formulaMeasureMs = int(timer.elapsed());
	state->result.debug.formulaRenderMs
		= state->result.debug.formulaMeasureMs;
}

struct NativeIvPhotoInfo {
	uint64 id = 0;
	int width = 0;
	int height = 0;
};

struct NativeIvPrepareState {
	MarkdownArticleContent result;
	std::vector<NativeIvPhotoInfo> photos;
	int nextGeneratedId = 0;

	void setFailure(
			PrepareTerminalFailure terminal,
			QString debugReason) {
		if (result.failure.failed()) {
			return;
		}
		result.failure.terminal = terminal;
		result.failure.debugReason = std::move(debugReason);
	}

	[[nodiscard]] bool blocked() const {
		return result.failure.failed();
	}
};

struct PreparedIvRichText {
	TextWithEntities text;
	std::vector<PreparedLink> links;
};

[[nodiscard]] bool PrepareNativeIvBlocks(
	const QVector<MTPPageBlock> &blocks,
	std::vector<PreparedBlock> *result,
	NativeIvPrepareState *state);
[[nodiscard]] bool PrepareNativeIvBlock(
	const MTPPageBlock &block,
	std::vector<PreparedBlock> *result,
	NativeIvPrepareState *state);
[[nodiscard]] bool AppendNativeIvRichText(
	const MTPRichText &text,
	TextWithEntities *result,
	std::vector<PreparedLink> *links,
	QString *blockAnchorId,
	NativeIvPrepareState *state);

void ShiftEntities(EntitiesInText *entities, int delta) {
	if (!delta) {
		return;
	}
	for (auto &entity : *entities) {
		entity = EntityInText(
			entity.type(),
			entity.offset() + delta,
			entity.length(),
			entity.data());
	}
}

void PrependText(TextWithEntities *text, QString prefix) {
	if (prefix.isEmpty()) {
		return;
	}
	text->text.prepend(prefix);
	ShiftEntities(&text->entities, prefix.size());
}

[[nodiscard]] QString NativeIvDateText(TimeId date) {
	return langDateTimeFull(base::unixtime::parse(date));
}

[[nodiscard]] QString NativeIvDetailsAnchorId(NativeIvPrepareState *state) {
	return u"details-"_q + QString::number(++state->nextGeneratedId);
}

void RememberNativeIvPhoto(
		NativeIvPrepareState *state,
		const MTPPhoto &photo) {
	auto info = NativeIvPhotoInfo{
		.id = photo.match([](const auto &data) {
			return data.vid().v;
		}),
	};
	photo.match([](const MTPDphotoEmpty &) {
	}, [&](const MTPDphoto &data) {
		auto width = 0;
		auto height = 0;
		const auto assign = [&](int w, int h) {
			if (w > 0 && h > 0) {
				width = w;
				height = h;
			}
		};
		for (const auto &size : data.vsizes().v) {
			size.match([&](const MTPDphotoSizeEmpty &) {
			}, [&](const MTPDphotoSize &data) {
				if (data.vtype().v == u"y"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"x"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"w"_q) {
					assign(data.vw().v, data.vh().v);
				}
			}, [&](const MTPDphotoCachedSize &data) {
				if (data.vtype().v == u"y"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"x"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"w"_q) {
					assign(data.vw().v, data.vh().v);
				}
			}, [&](const MTPDphotoStrippedSize &) {
			}, [&](const MTPDphotoSizeProgressive &data) {
				if (data.vtype().v == u"y"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"x"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"w"_q) {
					assign(data.vw().v, data.vh().v);
				}
			}, [&](const MTPDphotoPathSize &) {
			});
		}
		info.width = width;
		info.height = height;
	});
	if (!info.id) {
		return;
	}
	for (auto &existing : state->photos) {
		if (existing.id == info.id) {
			existing = info;
			return;
		}
	}
	state->photos.push_back(info);
}

[[nodiscard]] const NativeIvPhotoInfo *FindNativeIvPhoto(
		uint64 id,
		const NativeIvPrepareState &state) {
	for (const auto &photo : state.photos) {
		if (photo.id == id) {
			return &photo;
		}
	}
	return nullptr;
}

[[nodiscard]] bool AddNativeIvPreparedLink(
		TextWithEntities *text,
		std::vector<PreparedLink> *links,
		int from,
		int length,
		QString target) {
	if (!length || target.isEmpty()) {
		return true;
	}
	const auto index = links->size() + 1;
	if (index > std::numeric_limits<uint16>::max()) {
		return true;
	}
	const auto prepared = ClassifiedLink(uint16(index), target, nullptr);
	if (prepared.kind == PreparedLinkKind::RejectedRelative
		|| prepared.kind == PreparedLinkKind::LocalFile) {
		return true;
	}
	text->entities.push_back(EntityInText(
		EntityType::CustomUrl,
		from,
		length,
		InternalLinkData(uint16(index))));
	links->push_back({
		.index = uint16(index),
		.kind = prepared.kind,
		.target = prepared.target,
		.fragment = prepared.fragment,
		.copyText = prepared.copyText,
	});
	return true;
}

[[nodiscard]] bool AddNativeIvEntity(
		TextWithEntities *text,
		int from,
		EntityType type) {
	const auto length = text->text.size() - from;
	if (length <= 0) {
		return true;
	}
	text->entities.push_back(EntityInText(type, from, length));
	return true;
}

[[nodiscard]] bool AppendNativeIvRichText(
		const MTPRichText &text,
		TextWithEntities *result,
		std::vector<PreparedLink> *links,
		QString *blockAnchorId,
		NativeIvPrepareState *state) {
	if (state->blocked()) {
		return false;
	}
	return text.match([&](const MTPDtextEmpty &) {
		return true;
	}, [&](const MTPDtextPlain &data) {
		result->append(qs(data.vtext()));
		return true;
	}, [&](const MTPDtextConcat &data) {
		for (const auto &part : data.vtexts().v) {
			if (!AppendNativeIvRichText(
					part,
					result,
					links,
					blockAnchorId,
					state)) {
				return false;
			}
		}
		return true;
	}, [&](const MTPDtextImage &data) {
		const auto replacementText = u"[image]"_q;
		if (!data.vdocument_id().v || data.vw().v <= 0 || data.vh().v <= 0) {
			result->append(replacementText);
			return true;
		}
		const auto entityData = SerializeInlineTextObjectEntity({
			.kind = InlineTextObjectKind::IvImage,
			.data = InlineTextObjectIvImageData{
				.documentId = uint64(data.vdocument_id().v),
				.width = data.vw().v,
				.height = data.vh().v,
				.replacementText = replacementText,
			},
		});
		if (entityData.isEmpty()) {
			result->append(replacementText);
			return true;
		}
		const auto from = result->text.size();
		result->append(QChar::ObjectReplacementCharacter);
		result->entities.push_back(EntityInText(
			EntityType::CustomEmoji,
			from,
			1,
			entityData));
		return true;
	}, [&](const MTPDtextBold &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Bold);
	}, [&](const MTPDtextItalic &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Italic);
	}, [&](const MTPDtextUnderline &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Underline);
	}, [&](const MTPDtextStrike &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::StrikeOut);
	}, [&](const MTPDtextFixed &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Code);
	}, [&](const MTPDtextUrl &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		if (result->text.size() == from) {
			result->append(qs(data.vurl()));
		}
		return AddNativeIvPreparedLink(
			result,
			links,
			from,
			result->text.size() - from,
			qs(data.vurl()));
	}, [&](const MTPDtextEmail &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		if (result->text.size() == from) {
			result->append(qs(data.vemail()));
		}
		return AddNativeIvPreparedLink(
			result,
			links,
			from,
			result->text.size() - from,
			u"mailto:"_q + qs(data.vemail()));
	}, [&](const MTPDtextSubscript &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Subscript);
	}, [&](const MTPDtextSuperscript &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Superscript);
	}, [&](const MTPDtextMarked &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Marked);
	}, [&](const MTPDtextPhone &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		if (result->text.size() == from) {
			result->append(qs(data.vphone()));
		}
		return AddNativeIvPreparedLink(
			result,
			links,
			from,
			result->text.size() - from,
			u"tel:"_q + qs(data.vphone()));
	}, [&](const MTPDtextAnchor &data) {
		if (blockAnchorId && blockAnchorId->isEmpty()) {
			*blockAnchorId = NormalizeFragmentId(qs(data.vname()));
		}
		return AppendNativeIvRichText(
			data.vtext(),
			result,
			links,
			blockAnchorId,
			state);
	});
}

[[nodiscard]] bool PrepareNativeIvRichText(
		const MTPRichText &text,
		PreparedIvRichText *result,
		QString *blockAnchorId,
		NativeIvPrepareState *state) {
	return AppendNativeIvRichText(
		text,
		&result->text,
		&result->links,
		blockAnchorId,
		state);
}

[[nodiscard]] bool PrepareNativeIvCaption(
		const MTPPageCaption &caption,
		PreparedIvRichText *result,
		QString *blockAnchorId,
		NativeIvPrepareState *state) {
	if (!AppendNativeIvRichText(
			caption.data().vtext(),
			&result->text,
			&result->links,
			blockAnchorId,
			state)) {
		return false;
	}
	auto credit = PreparedIvRichText();
	if (!PrepareNativeIvRichText(
			caption.data().vcredit(),
			&credit,
			blockAnchorId,
			state)) {
		return false;
	}
	if (!credit.text.text.isEmpty()) {
		if (!result->text.text.isEmpty()) {
			result->text.append(QChar('\n'));
		}
		if (!AppendNativeIvRichText(
				caption.data().vcredit(),
				&result->text,
				&result->links,
				blockAnchorId,
				state)) {
			return false;
		}
	}
	return true;
}

void SortPreparedIvRichText(PreparedIvRichText *text) {
	SortEntities(&text->text);
}

[[nodiscard]] bool AppendPreparedIvRichBlock(
		std::vector<PreparedBlock> *result,
		PreparedBlockKind kind,
		int headingLevel,
		PreparedIvRichText prepared,
		QString anchorId = QString(),
		bool allowEmpty = false) {
	SortPreparedIvRichText(&prepared);
	AppendRichBlock(
		result,
		kind,
		headingLevel,
		std::move(prepared.text),
		std::move(prepared.links),
		std::move(anchorId),
		false,
		allowEmpty);
	return true;
}

[[nodiscard]] bool AppendNativeIvFlowBlock(
		std::vector<PreparedBlock> *result,
		PreparedBlockKind kind,
		int headingLevel,
		const MTPRichText &text,
		NativeIvPrepareState *state,
		bool allowEmpty = false) {
	auto prepared = PreparedIvRichText();
	auto anchorId = QString();
	if (!PrepareNativeIvRichText(text, &prepared, &anchorId, state)) {
		return false;
	}
	return AppendPreparedIvRichBlock(
		result,
		kind,
		headingLevel,
		std::move(prepared),
		std::move(anchorId),
		allowEmpty);
}

bool PrepareNativeIvPlainPlaceholderBlock(
		QString label,
		std::vector<PreparedBlock> *result) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Placeholder;
	block.placeholder.label = std::move(label);
	block.placeholder.copyText = block.placeholder.label;
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] bool PrepareNativeIvPlaceholderBlock(
		QString label,
		const MTPPageCaption &caption,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state);

[[nodiscard]] bool PrepareNativeIvPhotoBlock(
		const MTPDpageBlockPhoto &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	const auto info = FindNativeIvPhoto(uint64(data.vphoto_id().v), *state);
	if (!info || info->width <= 0 || info->height <= 0) {
		return PrepareNativeIvPlaceholderBlock(
			u"Photo Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}
	auto caption = PreparedIvRichText();
	auto anchorId = QString();
	if (!PrepareNativeIvCaption(data.vcaption(), &caption, &anchorId, state)) {
		return false;
	}
	SortPreparedIvRichText(&caption);
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Photo;
	block.text = std::move(caption.text);
	block.links = std::move(caption.links);
	block.anchorId = std::move(anchorId);
	block.photo.photoId = info->id;
	block.photo.width = info->width;
	block.photo.height = info->height;
	block.photo.urlOverride = data.vurl() ? qs(*data.vurl()) : QString();
	block.photo.viewerOpen = true;
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] QString NativeIvPlaceholderCopyText(
		const QString &label,
		const TextWithEntities &caption) {
	return caption.text.isEmpty()
		? label
		: (label + u"\n"_q + caption.text);
}

[[nodiscard]] bool PrepareNativeIvPlaceholderBlock(
		QString label,
		const MTPPageCaption &caption,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto prepared = PreparedIvRichText();
	auto anchorId = QString();
	if (!PrepareNativeIvCaption(caption, &prepared, &anchorId, state)) {
		return state->result.failure.failed()
			? false
			: PrepareNativeIvPlainPlaceholderBlock(std::move(label), result);
	}
	SortPreparedIvRichText(&prepared);
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Placeholder;
	block.text = std::move(prepared.text);
	block.links = std::move(prepared.links);
	block.anchorId = std::move(anchorId);
	block.placeholder.label = label;
	block.placeholder.copyText = NativeIvPlaceholderCopyText(
		block.placeholder.label,
		block.text);
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] bool PrepareNativeIvQuoteBlock(
		const MTPRichText &text,
		const MTPRichText &caption,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Quote;
	auto body = PreparedIvRichText();
	if (!PrepareNativeIvRichText(text, &body, &block.anchorId, state)) {
		return false;
	}
	if (!AppendPreparedIvRichBlock(
		&block.children,
		PreparedBlockKind::Paragraph,
		0,
		std::move(body))) {
		return false;
	}
	auto cite = PreparedIvRichText();
	if (!PrepareNativeIvRichText(caption, &cite, &block.anchorId, state)) {
		return false;
	}
	if (!AppendPreparedIvRichBlock(
		&block.children,
		PreparedBlockKind::Paragraph,
		0,
		std::move(cite))) {
		return false;
	}
	if (block.children.empty()) {
		block.children.push_back(EmptyParagraphBlock());
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] bool PrepareNativeIvList(
		const QVector<MTPPageListItem> &items,
		PreparedBlock *result,
		NativeIvPrepareState *state) {
	for (const auto &item : items) {
		auto block = PreparedBlock();
		block.kind = PreparedBlockKind::ListItem;
		block.listKind = result->listKind;
		block.listDelimiter = result->listDelimiter;
		const auto ok = item.match([&](const MTPDpageListItemText &data) {
			auto prepared = PreparedIvRichText();
			if (!PrepareNativeIvRichText(
					data.vtext(),
					&prepared,
					&block.anchorId,
					state)) {
				return false;
			}
			return AppendPreparedIvRichBlock(
				&block.children,
				PreparedBlockKind::Paragraph,
				0,
				std::move(prepared));
		}, [&](const MTPDpageListItemBlocks &data) {
			return PrepareNativeIvBlocks(data.vblocks().v, &block.children, state);
		});
		if (!ok) {
			return false;
		}
		if (block.children.empty()) {
			block.children.push_back(EmptyParagraphBlock());
		}
		result->children.push_back(std::move(block));
	}
	return true;
}

[[nodiscard]] bool ParseOrderedNumber(
		const QString &value,
		int *result) {
	auto ok = false;
	const auto parsed = value.toInt(&ok);
	if (!ok) {
		return false;
	}
	*result = parsed;
	return true;
}

[[nodiscard]] int NextNativeIvOrderedNumber(const PreparedBlock &result) {
	return result.children.empty()
		? result.startNumber
		: (result.children.back().orderedNumber + 1);
}

[[nodiscard]] bool PrepareNativeIvOrderedList(
		const QVector<MTPPageListOrderedItem> &items,
		PreparedBlock *result,
		NativeIvPrepareState *state) {
	auto firstNumber = true;
	for (const auto &item : items) {
		auto block = PreparedBlock();
		block.kind = PreparedBlockKind::ListItem;
		block.listKind = result->listKind;
		block.listDelimiter = result->listDelimiter;
		const auto ok = item.match([&](const MTPDpageListOrderedItemText &data) {
			auto orderedNumber = 0;
			if (!ParseOrderedNumber(qs(data.vnum()), &orderedNumber)) {
				orderedNumber = NextNativeIvOrderedNumber(*result);
			}
			block.orderedNumber = orderedNumber;
			auto prepared = PreparedIvRichText();
			if (!PrepareNativeIvRichText(
					data.vtext(),
					&prepared,
					&block.anchorId,
					state)) {
				return false;
			}
			return AppendPreparedIvRichBlock(
				&block.children,
				PreparedBlockKind::Paragraph,
				0,
				std::move(prepared));
		}, [&](const MTPDpageListOrderedItemBlocks &data) {
			auto orderedNumber = 0;
			if (!ParseOrderedNumber(qs(data.vnum()), &orderedNumber)) {
				orderedNumber = NextNativeIvOrderedNumber(*result);
			}
			block.orderedNumber = orderedNumber;
			return PrepareNativeIvBlocks(data.vblocks().v, &block.children, state);
		});
		if (!ok) {
			return false;
		}
		if (firstNumber) {
			result->startNumber = block.orderedNumber;
			firstNumber = false;
		}
		if (block.children.empty()) {
			block.children.push_back(EmptyParagraphBlock());
		}
		result->children.push_back(std::move(block));
	}
	return true;
}

[[nodiscard]] TableAlignment NativeIvTableAlignment(
		const MTPDpageTableCell &cell) {
	if (cell.is_align_right()) {
		return TableAlignment::Right;
	} else if (cell.is_align_center()) {
		return TableAlignment::Center;
	}
	return TableAlignment::Left;
}

[[nodiscard]] bool PrepareNativeIvTableBlock(
		const MTPDpageBlockTable &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto title = PreparedIvRichText();
	auto anchorId = QString();
	if (!PrepareNativeIvRichText(data.vtitle(), &title, &anchorId, state)) {
		return false;
	}
	if (!AppendPreparedIvRichBlock(
		result,
		PreparedBlockKind::Paragraph,
		0,
		std::move(title),
		std::move(anchorId))) {
		return false;
	}
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Table;
	for (const auto &row : data.vrows().v) {
		auto preparedRow = PreparedTableRow();
		auto header = std::optional<bool>();
		auto column = 0;
		const auto ok = row.match([&](const MTPDpageTableRow &data) {
			for (const auto &cell : data.vcells().v) {
				auto preparedCell = PreparedTableCell();
				const auto cellOk = cell.match([&](const MTPDpageTableCell &data) {
					if ((data.vcolspan() && data.vcolspan()->v > 1)
						|| (data.vrowspan() && data.vrowspan()->v > 1)) {
						return false;
					}
					const auto cellHeader = data.is_header();
					if (header && *header != cellHeader) {
						return false;
					}
					header = cellHeader;
					preparedCell.column = column++;
					preparedCell.alignment = NativeIvTableAlignment(data);
					if (data.vtext()) {
						auto rich = PreparedIvRichText();
						if (!PrepareNativeIvRichText(
								*data.vtext(),
								&rich,
								nullptr,
								state)) {
							return false;
						}
						SortPreparedIvRichText(&rich);
						preparedCell.text = std::move(rich.text);
						preparedCell.links = std::move(rich.links);
					}
					return true;
				});
				if (!cellOk) {
					return false;
				}
				preparedRow.cells.push_back(std::move(preparedCell));
			}
			return true;
		});
		if (!ok) {
			return state->result.failure.failed()
				? false
				: PrepareNativeIvPlainPlaceholderBlock(
					u"Table Placeholder"_q,
					result);
		}
		preparedRow.header = header.value_or(false);
		if (!block.tableColumnCount) {
			block.tableColumnCount = column;
			block.tableAlignments.resize(column, TableAlignment::Left);
		} else if (block.tableColumnCount != column) {
			return PrepareNativeIvPlainPlaceholderBlock(
				u"Table Placeholder"_q,
				result);
		}
		for (const auto &cell : preparedRow.cells) {
			if (cell.column >= 0 && cell.column < int(block.tableAlignments.size())) {
				block.tableAlignments[cell.column] = cell.alignment;
			}
		}
		block.tableRows.push_back(std::move(preparedRow));
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] bool PrepareNativeIvDetailsBlock(
		const MTPDpageBlockDetails &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto summary = PreparedIvRichText();
	auto anchorId = NativeIvDetailsAnchorId(state);
	if (!PrepareNativeIvRichText(
			data.vtitle(),
			&summary,
			&anchorId,
			state)) {
		return false;
	}
	if (!summary.links.empty()) {
		const auto isLink = [](const EntityInText &entity) {
			return entity.type() == EntityType::CustomUrl;
		};
		const auto from = std::remove_if(
			summary.text.entities.begin(),
			summary.text.entities.end(),
			isLink);
		summary.text.entities.erase(from, summary.text.entities.end());
		summary.links.clear();
	}
	PrependText(&summary.text, data.is_open() ? u"v "_q : u"> "_q);
	SortPreparedIvRichText(&summary);
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Details;
	block.anchorId = std::move(anchorId);
	block.collapsed = !data.is_open();
	block.text = std::move(summary.text);
	block.links = std::move(summary.links);
	const auto toggleIndex = block.links.size() + 1;
	if (toggleIndex <= std::numeric_limits<uint16>::max()) {
		block.text.entities.push_back(EntityInText(
			EntityType::CustomUrl,
			0,
			block.text.text.size(),
			InternalLinkData(uint16(toggleIndex))));
		block.links.push_back({
			.index = uint16(toggleIndex),
			.kind = PreparedLinkKind::ToggleDetails,
			.target = block.anchorId,
		});
	}
	if (!PrepareNativeIvBlocks(data.vblocks().v, &block.children, state)) {
		return false;
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] bool PrepareNativeIvBlocks(
		const QVector<MTPPageBlock> &blocks,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	for (const auto &block : blocks) {
		if (!PrepareNativeIvBlock(block, result, state)) {
			if (state->result.failure.failed()) {
				return false;
			}
			PrepareNativeIvPlainPlaceholderBlock(
				u"Unsupported Block Placeholder"_q,
				result);
		}
	}
	return !state->result.failure.failed();
}

[[nodiscard]] bool PrepareNativeIvBlock(
		const MTPPageBlock &block,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	if (state->blocked()) {
		return false;
	}
	return block.match([&](const MTPDpageBlockUnsupported &) {
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Unsupported Block Placeholder"_q,
			result);
	}, [&](const MTPDpageBlockTitle &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			1,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockSubtitle &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			2,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockAuthorDate &data) {
		auto prepared = PreparedIvRichText();
		auto anchorId = QString();
		if (!PrepareNativeIvRichText(
				data.vauthor(),
				&prepared,
				&anchorId,
				state)) {
			return false;
		}
		if (const auto date = data.vpublished_date().v) {
			if (!prepared.text.text.isEmpty()) {
				prepared.text.append(u" • "_q);
			}
			prepared.text.append(NativeIvDateText(date));
		}
		return AppendPreparedIvRichBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			std::move(prepared),
			std::move(anchorId));
	}, [&](const MTPDpageBlockHeader &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			3,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockSubheader &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			4,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockParagraph &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockPreformatted &data) {
		auto prepared = PreparedIvRichText();
		auto anchorId = QString();
		if (!PrepareNativeIvRichText(
				data.vtext(),
				&prepared,
				&anchorId,
				state)) {
			return false;
		}
		auto block = PreparedBlock();
		block.kind = PreparedBlockKind::CodeBlock;
		block.anchorId = std::move(anchorId);
		block.codeLanguage = qs(data.vlanguage()).trimmed();
		block.text.text = prepared.text.text;
		result->push_back(std::move(block));
		return true;
	}, [&](const MTPDpageBlockFooter &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockDivider &) {
		result->push_back(PrepareRuleBlock());
		return true;
	}, [&](const MTPDpageBlockAnchor &data) {
		const auto anchorId = NormalizeFragmentId(qs(data.vname()));
		if (anchorId.isEmpty()) {
			return true;
		}
		auto prepared = PreparedIvRichText();
		return AppendPreparedIvRichBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			std::move(prepared),
			anchorId,
			true);
	}, [&](const MTPDpageBlockList &data) {
		auto prepared = PreparedBlock();
		prepared.kind = PreparedBlockKind::List;
		prepared.listKind = ListKind::Bullet;
		return PrepareNativeIvList(data.vitems().v, &prepared, state)
			? (result->push_back(std::move(prepared)), true)
			: false;
	}, [&](const MTPDpageBlockBlockquote &data) {
		return PrepareNativeIvQuoteBlock(
			data.vtext(),
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockPullquote &data) {
		return PrepareNativeIvQuoteBlock(
			data.vtext(),
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockPhoto &data) {
		return PrepareNativeIvPhotoBlock(data, result, state);
	}, [&](const MTPDpageBlockVideo &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Video Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockCover &data) {
		return PrepareNativeIvBlock(data.vcover(), result, state);
	}, [&](const MTPDpageBlockEmbed &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Embed Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockEmbedPost &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Embed Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockCollage &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Collage placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockSlideshow &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Grouped Media Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockChannel &) {
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Channel Placeholder"_q,
			result);
	}, [&](const MTPDpageBlockAudio &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Audio File Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockKicker &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			5,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockTable &data) {
		return PrepareNativeIvTableBlock(data, result, state);
	}, [&](const MTPDpageBlockOrderedList &data) {
		auto prepared = PreparedBlock();
		prepared.kind = PreparedBlockKind::List;
		prepared.listKind = ListKind::Ordered;
		prepared.listDelimiter = ListDelimiter::Period;
		prepared.startNumber = 1;
		return PrepareNativeIvOrderedList(data.vitems().v, &prepared, state)
			? (result->push_back(std::move(prepared)), true)
			: false;
	}, [&](const MTPDpageBlockDetails &data) {
		return PrepareNativeIvDetailsBlock(data, result, state);
	}, [&](const MTPDpageBlockRelatedArticles &) {
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Related Articles Placeholder"_q,
			result);
	}, [&](const MTPDpageBlockMap &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Map Placeholder"_q,
			data.vcaption(),
			result,
			state);
	});
}

} // namespace

QString SerializeInlineTextObjectEntity(const InlineTextObjectEntity &object) {
	switch (object.kind) {
	case InlineTextObjectKind::Formula: {
		const auto data = std::get_if<InlineTextObjectFormulaData>(&object.data);
		if (!data) {
			return QString();
		}
		return u"iv-markdown:inline-text-object;formula;"_q
			+ EncodeInlineTextObjectField(data->copySource)
			+ u";"_q
			+ EncodeInlineTextObjectField(data->trimmedTex);
	} break;
	case InlineTextObjectKind::IvImage: {
		const auto data = std::get_if<InlineTextObjectIvImageData>(&object.data);
		if (!data) {
			return QString();
		}
		return u"iv-markdown:inline-text-object;iv-image;"_q
			+ QString::number(data->documentId)
			+ u";"_q
			+ QString::number(data->width)
			+ u";"_q
			+ QString::number(data->height)
			+ u";"_q
			+ EncodeInlineTextObjectField(data->replacementText);
	} break;
	}
	return QString();
}

std::optional<InlineTextObjectEntity> ParseInlineTextObjectEntity(
		const QString &data) {
	const auto parts = data.split(QChar(';'), Qt::KeepEmptyParts);
	if (parts.size() < 2
		|| parts[0] != u"iv-markdown:inline-text-object"_q) {
		return std::nullopt;
	}
	if (parts[1] == u"formula"_q) {
		if (parts.size() != 4) {
			return std::nullopt;
		}
		return InlineTextObjectEntity{
			.kind = InlineTextObjectKind::Formula,
			.data = InlineTextObjectFormulaData{
				.copySource = DecodeInlineTextObjectField(parts[2]),
				.trimmedTex = DecodeInlineTextObjectField(parts[3]),
			},
		};
	} else if (parts[1] == u"iv-image"_q) {
		if (parts.size() != 6) {
			return std::nullopt;
		}
		auto documentIdOk = false;
		auto widthOk = false;
		auto heightOk = false;
		const auto documentId = parts[2].toULongLong(&documentIdOk);
		const auto width = parts[3].toInt(&widthOk);
		const auto height = parts[4].toInt(&heightOk);
		if (!documentIdOk || !widthOk || !heightOk) {
			return std::nullopt;
		}
		return InlineTextObjectEntity{
			.kind = InlineTextObjectKind::IvImage,
			.data = InlineTextObjectIvImageData{
				.documentId = documentId,
				.width = width,
				.height = height,
				.replacementText = DecodeInlineTextObjectField(parts[5]),
			},
		};
	}
	return std::nullopt;
}

MarkdownPrepareDimensions CaptureMarkdownPrepareDimensions() {
	auto result = MarkdownPrepareDimensions();
	const auto &markdown = st::defaultMarkdown;
	result.bodyTextSize = TextSizeForFormula(markdown.body);
	result.headingTextSizes = {
		TextSizeForFormula(markdown.heading1),
		TextSizeForFormula(markdown.heading2),
		TextSizeForFormula(markdown.heading3),
		TextSizeForFormula(markdown.heading4),
		TextSizeForFormula(markdown.heading5),
		TextSizeForFormula(markdown.heading6),
	};
	result.tableHeaderTextSize = TextSizeForFormula(markdown.table.headerStyle);
	result.displayMathTextSize = markdown.displayMath.textSize;
	result.displayMathMaxRenderWidth = markdown.displayMath.maxRenderWidth;
	result.displayMathMaxRenderHeight = markdown.displayMath.maxRenderHeight;
	return result;
}

MarkdownArticleContent PrepareSynchronously(PrepareRequest request) {
	auto state = PrepareState();
	auto timer = QElapsedTimer();
	timer.start();
	state.request = &request;
	const auto finish = [&] {
		state.result.debug.prepareMs = int(timer.elapsed());
		return std::move(state.result);
	};

	if (!request.document) {
		state.setTerminalFailure(
			PrepareTerminalFailure::InvalidRequest,
			u"missing-document"_q);
		return finish();
	}
	if (const auto invalidStyle = InvalidStyleReason(request.dimensions);
		!invalidStyle.isEmpty()) {
		state.setTerminalFailure(
			PrepareTerminalFailure::InvalidStyle,
			invalidStyle);
		return finish();
	}

	state.sourceUtf8 = request.document->sourceText.toUtf8();
	state.result.formulas.resize(FormulaSlotCount(*request.document));
	state.result.debug.sourceWarningCount = int(request.document->warnings.size());

	state.result.blocks = PrepareRenderData(*request.document, &state);
	if (CountPreparedBlocks(state.result.blocks.blocks)
		> PrepareLimitsForIv().maxPreparedBlocks) {
		state.setTerminalFailure(
			PrepareTerminalFailure::DocumentTooLarge,
			u"prepared-block-limit"_q);
		ClearPreparedOutput(&state.result);
		return finish();
	}
	MeasurePreparedFormulas(&state);
	if (state.result.failure.failed()) {
		ClearPreparedOutput(&state.result);
	}
	return finish();
}

NativeInstantViewPrepareResult TryPrepareNativeInstantView(
		NativeInstantViewPrepareRequest request) {
	auto state = NativeIvPrepareState();
	auto timer = QElapsedTimer();
	timer.start();
	state.result.mediaRuntime = std::move(request.mediaRuntime);
	const auto finish = [&](NativeInstantViewPrepareResultKind kind, QString reason) {
		state.result.debug.prepareMs = int(timer.elapsed());
		return NativeInstantViewPrepareResult{
			.kind = kind,
			.content = std::move(state.result),
			.debugReason = std::move(reason),
		};
	};

	if (!request.source) {
		state.setFailure(
			PrepareTerminalFailure::InvalidRequest,
			u"missing-native-iv-source"_q);
		ClearPreparedOutput(&state.result);
		return finish(
			NativeInstantViewPrepareResultKind::Failure,
			state.result.failure.debugReason);
	}

	for (const auto &photo : request.source->page.data().vphotos().v) {
		RememberNativeIvPhoto(&state, photo);
	}
	if (request.source->webpagePhoto) {
		RememberNativeIvPhoto(&state, *request.source->webpagePhoto);
	}

	if (!PrepareNativeIvBlocks(
			request.source->page.data().vblocks().v,
			&state.result.blocks.blocks,
			&state)) {
		if (state.result.failure.failed()) {
			ClearPreparedOutput(&state.result);
			return finish(
				NativeInstantViewPrepareResultKind::Failure,
				state.result.failure.debugReason);
		}
		PrepareNativeIvPlainPlaceholderBlock(
			u"Unsupported Block Placeholder"_q,
			&state.result.blocks.blocks);
	}
	if (CountPreparedBlocks(state.result.blocks.blocks)
		> PrepareLimitsForIv().maxPreparedBlocks) {
		state.setFailure(
			PrepareTerminalFailure::DocumentTooLarge,
			u"prepared-block-limit"_q);
		ClearPreparedOutput(&state.result);
		return finish(
			NativeInstantViewPrepareResultKind::Failure,
			state.result.failure.debugReason);
	}
	return finish(
		NativeInstantViewPrepareResultKind::Supported,
		QString());
}

} // namespace Iv::Markdown
