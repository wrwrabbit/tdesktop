/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_article.h"
#include "iv/markdown/iv_markdown_article_layout_structure.h"
#include "iv/markdown/iv_markdown_article_paint.h"
#include "iv/markdown/iv_markdown_article_selection.h"
#include "iv/markdown/iv_markdown_article_text.h"
#include "iv/markdown/iv_markdown_prepare_links.h"
#include "ui/style/style_core_scale.h"
#include "ui/basic_click_handlers.h"

#include "styles/style_iv.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace Iv::Markdown {
namespace {

constexpr auto kArticleMaxWidth = 32767;

struct PendingHighlightKey {
	QString text;
	QString language;
};

[[nodiscard]] bool operator==(
		const PendingHighlightKey &a,
		const PendingHighlightKey &b) {
	return (a.text == b.text) && (a.language == b.language);
}

struct PendingHighlightKeyHasher {
	[[nodiscard]] size_t operator()(
			const PendingHighlightKey &key) const noexcept {
		auto result = size_t(qHash(key.text));
		result = (result * 1315423911U) ^ size_t(qHash(key.language));
		return result;
	}
};

struct PendingHighlightEntry {
	PendingHighlightKey key;
	std::vector<LaidOutBlock*> blocks;
};

[[nodiscard]] PendingHighlightKey PendingHighlightKeyForBlock(
		const LaidOutBlock &block) {
	return {
		.text = CodeBlockDisplayText(block.copyText),
		.language = block.codeLanguage,
	};
}

[[nodiscard]] Ui::Text::StateResult TextStateAtLeaf(
		const Ui::Text::String &leaf,
		QRect rect,
		int width,
		QPoint point,
		Ui::Text::StateRequest::Flags flags,
		style::align align = style::al_left,
		bool clampToRect = false) {
	if (rect.isEmpty()) {
		return {};
	}
	if (!rect.contains(point)) {
		if (!clampToRect) {
			return {};
		}
		point.setX(std::clamp(point.x(), rect.left(), rect.right()));
		point.setY(std::clamp(point.y(), rect.top(), rect.bottom()));
	}
	auto request = Ui::Text::StateRequest();
	request.align = align;
	request.flags = flags | Ui::Text::StateRequest::Flag::BreakEverywhere;
	const auto availableWidth = std::max(width, 1);
	return leaf.getState(
		point - rect.topLeft(),
		TextGeometry(availableWidth),
		request);
}

[[nodiscard]] SegmentSpan FullSegmentSpan(
		const std::vector<SelectableSegment> &segments) {
	return { 0, int(segments.size()) };
}

void RebuildVisibleSegmentLookup(
		const std::vector<SelectableSegment> &segments,
		std::vector<int> *tops,
		std::vector<int> *bottoms) {
	if (!tops || !bottoms) {
		return;
	}
	tops->clear();
	bottoms->clear();
	tops->reserve(segments.size());
	bottoms->reserve(segments.size());
	auto runningBottom = std::numeric_limits<int>::lowest();
	for (const auto &segment : segments) {
		tops->push_back(segment.outerRect.top());
		runningBottom = std::max(runningBottom, segment.outerRect.bottom());
		bottoms->push_back(runningBottom);
	}
}

[[nodiscard]] SegmentSpan LookupVisibleSegmentSpan(
		const std::vector<int> &tops,
		const std::vector<int> &bottoms,
		LogicalVisibleRange range) {
	if (tops.empty() || bottoms.empty() || (range.bottom <= range.top)) {
		return {};
	}
	const auto from = int(std::lower_bound(
		bottoms.begin(),
		bottoms.end(),
		range.top) - bottoms.begin());
	if (from >= int(tops.size())) {
		return {};
	}
	const auto till = int(std::upper_bound(
		tops.begin() + from,
		tops.end(),
		range.bottom - 1) - tops.begin());
	return (from < till) ? SegmentSpan{ from, till } : SegmentSpan();
}

[[nodiscard]] std::optional<PreparedLink> PreparedLinkForMediaActivation(
		const MediaActivation &activation) {
	if (activation.kind != MediaActivationKind::ExternalUrl
		|| activation.url.isEmpty()) {
		return std::nullopt;
	}
	if (const auto prepared = ClassifiedLink(0, activation.url, nullptr);
		prepared.kind == PreparedLinkKind::External) {
		return prepared;
	}
	return PreparedLink{
		.kind = PreparedLinkKind::External,
		.target = activation.url,
		.copyText = UrlClickHandler::EncodeForOpening(activation.url),
		.entityType = EntityType::Url,
		.shown = EntityLinkShown::Full,
	};
}

[[nodiscard]] std::optional<PreparedLink> PreparedLinkForDetailsBlock(
		const SelectableSegment &segment) {
	if (!segment.block
		|| segment.block->kind != PreparedBlockKind::Details
		|| segment.block->anchorId.isEmpty()) {
		return std::nullopt;
	}
	return PreparedLink{
		.kind = PreparedLinkKind::ToggleDetails,
		.target = segment.block->anchorId,
	};
}

[[nodiscard]] MarkdownArticleHitTestResult HitSegmentBoundary(
		const SelectableSegment &segment,
		int offset) {
	auto result = MarkdownArticleHitTestResult();
	result.segmentIndex = segment.index;
	result.forcedOffset = std::clamp(offset, 0, SegmentLength(segment));
	result.state.uponSymbol = true;
	result.state.afterSymbol = (result.forcedOffset > 0);
	return result;
}

[[nodiscard]] MarkdownArticleHitTestResult HitTextSegment(
		const SelectableSegment &segment,
		QPoint point,
		Ui::Text::StateRequest::Flags flags) {
	if (!segment.isTextLeaf() || !segment.outerRect.contains(point)) {
		return {};
	}
	const auto insideText = segment.textRect.contains(point);
	if (!insideText
		&& !(flags & Ui::Text::StateRequest::Flag::LookupSymbol)) {
		return {};
	}
	auto result = MarkdownArticleHitTestResult();
	result.segmentIndex = segment.index;
	result.state = TextStateAtLeaf(
		*segment.leaf,
		segment.textRect,
		segment.textWidth,
		point,
		flags,
		segment.align,
		!insideText);
	if (!insideText) {
		result.state.link = nullptr;
	}
	result.preparedLink = ExtractPreparedLink(result.state.link);
	if (!result.preparedLink
		&& (flags & Ui::Text::StateRequest::Flag::LookupLink)) {
		if (const auto prepared = PreparedLinkForDetailsBlock(segment)) {
			result.preparedLink = prepared;
			if (!insideText) {
				result.state.link = CreatePreparedLinkHandler(*prepared);
			}
		}
	}
	result.direct = true;
	return result;
}

[[nodiscard]] MarkdownArticleHitTestResult HitBlockSegment(
		const SelectableSegment &segment,
		QPoint point,
		Ui::Text::StateRequest::Flags flags) {
	if (segment.isTextLeaf()
		|| !(flags & Ui::Text::StateRequest::Flag::LookupSymbol)
		|| !segment.outerRect.contains(point)) {
		return {};
	}
	const auto after = (point.y() > segment.outerRect.center().y())
		|| ((point.y() == segment.outerRect.center().y())
			&& (point.x() >= segment.outerRect.center().x()));
	auto result = HitSegmentBoundary(
		segment,
		after ? SegmentLength(segment) : 0);
	const auto applyActivation = [&](const MediaActivation &activation) {
		result.mediaActivation = activation;
		if (const auto prepared = PreparedLinkForMediaActivation(activation)) {
			result.preparedLink = prepared;
			result.state.link = CreatePreparedLinkHandler(*prepared);
		}
	};
	if (segment.block) {
		if (segment.block->kind == PreparedBlockKind::RelatedArticle
			&& segment.block->preparedLink) {
			result.preparedLink = segment.block->preparedLink;
			result.state.link = segment.block->preparedLinkHandler;
			result.mediaActivation = {};
		} else if (segment.block->mediaBlock) {
			if (const auto link = segment.block->mediaBlock->linkAt(point)) {
				result.state.link = link;
				result.preparedLink = std::nullopt;
				result.mediaActivation = {};
			} else {
				applyActivation(segment.block->mediaBlock->activationAt(point));
			}
		} else if (!segment.block->actionRect.isEmpty()
			&& segment.block->actionRect.contains(point)
			&& segment.block->channelRuntime
			&& segment.block->channelRuntime->joinVisible()) {
			applyActivation(segment.block->actionActivation);
		} else {
			applyActivation(segment.block->activation);
		}
	}
	result.direct = true;
	return result;
}

[[nodiscard]] MarkdownArticleHitTestResult HitSegmentFallback(
		const std::vector<SelectableSegment> &segments,
		SegmentSpan span,
		QPoint point) {
	if (segments.empty() || span.empty()) {
		return {};
	}
	for (auto i = span.from; i != span.till; ++i) {
		const auto &segment = segments[i];
		const auto &rect = segment.outerRect;
		if (point.y() < rect.top()) {
			return HitSegmentBoundary(segment, 0);
		}
		if (point.y() <= rect.bottom()) {
			if (point.x() < rect.left()) {
				return HitSegmentBoundary(segment, 0);
			} else if (point.x() > rect.right()) {
				return HitSegmentBoundary(
					segment,
					SegmentLength(segment));
			}
		}
	}
	return HitSegmentBoundary(
		segments[span.till - 1],
		SegmentLength(segments[span.till - 1]));
}

[[nodiscard]] bool ToggleDetailsBlock(
		std::vector<PreparedBlock> *blocks,
		const QString &anchorId) {
	if (!blocks) {
		return false;
	}
	for (auto &block : *blocks) {
		if (block.kind == PreparedBlockKind::Details
			&& block.anchorId == anchorId) {
			block.collapsed = !block.collapsed;
			return true;
		}
		if (ToggleDetailsBlock(&block.children, anchorId)) {
			return true;
		}
	}
	return false;
}

void ClearColorizedFormulaImages(std::vector<LaidOutBlock> *blocks) {
	if (!blocks) {
		return;
	}
	for (auto &block : *blocks) {
		block.colorizedFormulaImage = QImage();
		block.colorizedFormulaColor = QColor();
		block.colorizedFormulaSize = QSize();
		ClearColorizedFormulaImages(&block.children);
	}
}

} // namespace

class MarkdownArticle::Impl final : public CodeBlockSyntaxHighlightTracker {
public:
	explicit Impl(std::shared_ptr<MathRenderer> renderer)
	: _renderer(std::move(renderer))
	, _inlineFormulaObjects(CreateInlineFormulaObjectCache(_renderer)) {
	}

	void setRenderer(std::shared_ptr<MathRenderer> renderer) {
		_renderer = std::move(renderer);
		SetInlineFormulaObjectCacheRenderer(_inlineFormulaObjects, _renderer);
		invalidateRasterCache();
		invalidateLayout();
	}

	void setMediaBlockHost(MediaBlockHost *host) {
		_mediaBlockHost = host;
		refreshMediaBlockHosts();
	}

	void setTextRepaintCallbacks(
			Fn<void()> repaint,
			Fn<void(QRect)> repaintRect) {
		_textRepaint = std::move(repaint);
		_textRepaintRect = std::move(repaintRect);
	}

	void setContent(MarkdownArticleContent content) {
		clearMediaBlocks();
		_content = std::move(content);
		ClearInlineFormulaObjectCache(_inlineFormulaObjects);
		resetFormulaRasterCache();
		invalidateLayout();
	}

	[[nodiscard]] int maxWidth() {
		if (_content.blocks.blocks.empty()) {
			return st::defaultMarkdown.pagePadding.left()
				+ st::defaultMarkdown.pagePadding.right();
		}
		const auto &markdown = st::defaultMarkdown;
		const auto &page = markdown.pagePadding;
		auto blocks = std::vector<LaidOutBlock>();
		const auto innerWidth = std::max(
			kArticleMaxWidth - page.left() - page.right(),
			1);
		auto repaintScope = InlineIvImageRepaintScope(
			_textRepaint,
			_textRepaintRect);
		const auto layoutBottom = LayoutBlocks(
			_content.blocks.blocks,
			&_content.formulas,
			&_formulaRenders,
			_renderer.get(),
			_inlineFormulaObjects.get(),
			_content.mediaRuntime,
			&blocks,
			markdown,
			page.left(),
			page.top(),
			innerWidth,
			LayoutContext{
				.allowAsyncSyntaxHighlighting = false,
			});
		(void)layoutBottom;
		return BlockMaxRight(blocks) + page.right();
	}

	[[nodiscard]] int resizeGetHeight(int width) {
		relayout(width);
		return std::max(_height, 1);
	}

	void setVisibleTopBottom(int visibleTop, int visibleBottom) {
		if (visibleBottom <= visibleTop) {
			_visibleRange = std::nullopt;
			_visibleSegmentSpan = {};
			return;
		}
		_visibleRange = LogicalVisibleRange{
			.top = visibleTop,
			.bottom = visibleBottom,
		};
		refreshVisibleSegmentSpan();
	}

	void paint(
			Painter &p,
			QRect clip,
			MarkdownArticlePaintCaches caches,
			MarkdownArticleSelection selection,
			const MarkdownArticleSelectionEndpoints *endpoints) {
		const auto &markdown = st::defaultMarkdown;
		const auto selectionState = PaintSelectionState{
			.segments = &_segments,
			.selection = selection,
			.endpoints = endpoints,
		};
		PaintBlocks(
			p,
			_blocks,
			&_content.formulas,
			&_formulaRenders,
			_renderer.get(),
			currentDevicePixelRatio(),
			std::max(_width, 1),
			markdown,
			caches,
			selectionState,
			clip);
	}

	[[nodiscard]] MarkdownArticleHitTestResult hitTest(
			QPoint point,
			Ui::Text::StateRequest::Flags flags) const {
		const auto span = candidateSegmentSpan(point);
		for (auto i = span.from; i != span.till; ++i) {
			const auto &segment = _segments[i];
			if (const auto result = HitTextSegment(segment, point, flags);
				result.valid()) {
				return result;
			}
		}
		for (auto i = span.from; i != span.till; ++i) {
			const auto &segment = _segments[i];
			if (const auto result = HitBlockSegment(segment, point, flags);
				result.valid()) {
				return result;
			}
		}
		if (flags & Ui::Text::StateRequest::Flag::LookupSymbol) {
			return HitSegmentFallback(_segments, span, point);
		}
		return {};
	}

	[[nodiscard]] int anchorTop(const QString &anchorId) const {
		for (const auto &entry : _anchors) {
			if (entry.first == anchorId) {
				return entry.second;
			}
		}
		return -1;
	}

	[[nodiscard]] bool toggleDetails(const QString &anchorId) {
		if (!ToggleDetailsBlock(&_content.blocks.blocks, anchorId)) {
			return false;
		}
		invalidateLayout();
		return true;
	}

	[[nodiscard]] bool segmentIsText(int index) const {
		const auto segment = FindSegment(&_segments, index);
		return segment && segment->isTextLeaf();
	}

	[[nodiscard]] int segmentLength(int index) const {
		const auto segment = FindSegment(&_segments, index);
		return segment ? SegmentLength(*segment) : 0;
	}

	[[nodiscard]] int selectionOffsetFromHit(
			const MarkdownArticleHitTestResult &result,
			TextSelectType selectionType) const {
		const auto segment = FindSegment(&_segments, result.segmentIndex);
		if (!segment) {
			return 0;
		}
		if (result.forcedOffset >= 0) {
			return std::clamp(result.forcedOffset, 0, SegmentLength(*segment));
		}
		auto offset = int(result.state.symbol);
		if (selectionType == TextSelectType::Letters
			&& result.state.afterSymbol) {
			++offset;
		}
		return std::clamp(offset, 0, SegmentLength(*segment));
	}

	[[nodiscard]] TextSelection adjustSelection(
			int segmentIndex,
			TextSelection selection,
			TextSelectType selectionType) const {
		const auto segment = FindSegment(&_segments, segmentIndex);
		if (!segment || !segment->isTextLeaf()) {
			return selection;
		}
		return segment->leaf->adjustSelection(selection, selectionType);
	}

	[[nodiscard]] bool selectionContains(
			MarkdownArticleSelection selection,
			const MarkdownArticleSelectionEndpoints *endpoints,
			const MarkdownArticleHitTestResult &result) const {
		const auto segment = FindSegment(&_segments, result.segmentIndex);
		if (!segment || selection.empty() || !result.valid()) {
			return false;
		}
		const auto selectionState = PaintSelectionState{
			.segments = &_segments,
			.selection = selection,
			.endpoints = endpoints,
		};
		if (segment->tableSegmentIndex >= 0
			&& TableSegmentSelected(selectionState, segment->tableSegmentIndex)) {
			return true;
		}
		if (!segment->isTextLeaf()) {
			return WholeSegmentSelected(*segment, selectionState);
		}
		const auto textSelection = TextSelectionForSegment(*segment, selectionState);
		if (!textSelection || textSelection->empty()) {
			return false;
		}
		const auto offset = selectionOffsetFromHit(result, TextSelectType::Letters);
		return (offset >= textSelection->from) && (offset < textSelection->to);
	}

	[[nodiscard]] TextForMimeData textForContext(
			const MarkdownArticleHitTestResult &result) const {
		if (!result.valid() || !result.direct) {
			return TextForMimeData();
		}
		const auto segment = FindSegment(&_segments, result.segmentIndex);
		return segment ? TextForSegment(*segment) : TextForMimeData();
	}

	[[nodiscard]] TextForMimeData textForSelection(
			MarkdownArticleSelection selection,
			const MarkdownArticleSelectionEndpoints *endpoints) const {
		return TextForSelectedSegments(_segments, selection, endpoints);
	}

	[[nodiscard]] bool highlightProcessDone(
			Spellchecker::HighlightProcessId processId) {
		const auto i = _pendingHighlightEntries.find(processId);
		if (i == end(_pendingHighlightEntries)) {
			return false;
		}
		auto entry = std::move(i->second);
		_pendingHighlightEntries.erase(i);
		_pendingHighlightProcesses.erase(entry.key);

		auto rebuilt = false;
		for (const auto block : entry.blocks) {
			RepopulateCodeBlockLeaf(*block, st::defaultMarkdown, true, this);
			registerPendingHighlightBlock(*block);
			rebuilt = true;
		}
		return rebuilt;
	}

	void invalidatePaletteCache() {
		InvalidateInlineFormulaPaletteCache(_inlineFormulaObjects);
		ClearColorizedFormulaImages(&_blocks);
	}

	void invalidateRasterCache() {
		resetFormulaRasterCache();
		InvalidateInlineFormulaRasterCache(_inlineFormulaObjects);
		ClearColorizedFormulaImages(&_blocks);
	}

	[[nodiscard]] MediaBlockHost *mediaBlockHost() const {
		return _mediaBlockHost;
	}

	void invalidateLayout() {
		_width = -1;
		_height = 0;
		clearPendingHighlightBlockPointers();
		_blocks.clear();
		_anchors.clear();
		_segments.clear();
		_visibleSegmentSpan = {};
		_segmentTops.clear();
		_segmentBottoms.clear();
	}

private:
	[[nodiscard]] int currentDevicePixelRatio() const {
		return std::max(style::DevicePixelRatio(), 1);
	}

	void rebuildVisibleSegmentLookup() {
		RebuildVisibleSegmentLookup(
			_segments,
			&_segmentTops,
			&_segmentBottoms);
		refreshVisibleSegmentSpan();
	}

	void refreshVisibleSegmentSpan() {
		_visibleSegmentSpan = _visibleRange
			? LookupVisibleSegmentSpan(
				_segmentTops,
				_segmentBottoms,
				*_visibleRange)
			: SegmentSpan();
	}

	void clearMediaBlocks() {
		for (const auto &[id, block] : _mediaBlocks) {
			if (block) {
				block->setHost(nullptr);
			}
		}
		_mediaBlocks.clear();
	}

	void refreshMediaBlockHosts() {
		for (const auto &[id, block] : _mediaBlocks) {
			if (block) {
				block->setHost(_mediaBlockHost);
			}
		}
	}

	[[nodiscard]] std::shared_ptr<MediaBlock> getOrCreateMediaBlock(
			const PreparedBlock &prepared) {
		switch (prepared.kind) {
		case PreparedBlockKind::Photo:
			return getOrCreateMediaBlock(
				prepared.photo.id,
				[=] {
					return CreatePhotoMediaBlock(
						prepared.photo,
						_content.mediaRuntime);
				});
		case PreparedBlockKind::Video:
			return getOrCreateMediaBlock(
				prepared.video.id,
				[=] {
					return CreateVideoMediaBlock(
						prepared.video,
						_content.mediaRuntime);
				});
		case PreparedBlockKind::Map:
			return getOrCreateMediaBlock(
				prepared.map.id,
				[=] {
					return CreateMapMediaBlock(
						prepared.map,
						_content.mediaRuntime);
				});
		case PreparedBlockKind::Audio:
			return getOrCreateMediaBlock(
				prepared.audio.id,
				[=] {
					return CreateAudioMediaBlock(
						prepared.audio,
						_content.mediaRuntime);
				});
		case PreparedBlockKind::Channel:
			return getOrCreateMediaBlock(
				prepared.channel.id,
				[=] {
					return CreateChannelMediaBlock(
						prepared.channel,
						_content.mediaRuntime);
				});
		case PreparedBlockKind::GroupedMedia:
			return getOrCreateMediaBlock(
				prepared.groupedMedia.id,
				[=] {
					return CreateGroupedMediaBlock(
						prepared.groupedMedia,
						_content.mediaRuntime);
				});
		default:
			return nullptr;
		}
	}

	template <typename Factory>
	[[nodiscard]] std::shared_ptr<MediaBlock> getOrCreateMediaBlock(
			PreparedMediaBlockId id,
			Factory &&factory) {
		if (!id) {
			return nullptr;
		}
		if (const auto i = _mediaBlocks.find(id.value);
			i != end(_mediaBlocks)) {
			return i->second;
		}
		auto block = factory();
		if (block) {
			block->setHost(_mediaBlockHost);
		}
		_mediaBlocks.emplace(id.value, block);
		return block;
	}

	[[nodiscard]] Spellchecker::HighlightProcessId tryHighlightSyntax(
			const QString &displayText,
			const QString &language,
			TextWithEntities &marked) override {
		const auto key = PendingHighlightKey{
			.text = displayText,
			.language = language,
		};
		if (const auto i = _pendingHighlightProcesses.find(key);
			i != end(_pendingHighlightProcesses)) {
			return i->second;
		}
		const auto processId = Spellchecker::TryHighlightSyntax(marked);
		if (processId) {
			registerPendingHighlightProcess(key, processId);
		}
		return processId;
	}

	[[nodiscard]] SegmentSpan candidateSegmentSpan(QPoint point) const {
		if (_visibleRange
			&& (_visibleRange->top <= point.y())
			&& (point.y() < _visibleRange->bottom)) {
			return _visibleSegmentSpan.empty()
				? FullSegmentSpan(_segments)
				: _visibleSegmentSpan;
		}
		return FullSegmentSpan(_segments);
	}

	void clearPendingHighlightBlockPointers() {
		for (auto &entry : _pendingHighlightEntries) {
			entry.second.blocks.clear();
		}
	}

	void registerPendingHighlightProcess(
			const PendingHighlightKey &key,
			Spellchecker::HighlightProcessId processId) {
		_pendingHighlightProcesses[key] = processId;
		auto &entry = _pendingHighlightEntries[processId];
		entry.key = key;
	}

	void registerPendingHighlightBlock(LaidOutBlock &block) {
		if (!block.syntaxHighlightProcessId) {
			return;
		}
		if (!_pendingHighlightEntries.contains(block.syntaxHighlightProcessId)) {
			registerPendingHighlightProcess(
				PendingHighlightKeyForBlock(block),
				block.syntaxHighlightProcessId);
		}
		_pendingHighlightEntries[block.syntaxHighlightProcessId].blocks.push_back(
			&block);
	}

	void registerPendingHighlightBlocks(std::vector<LaidOutBlock> &blocks) {
		for (auto &block : blocks) {
			registerPendingHighlightBlock(block);
			registerPendingHighlightBlocks(block.children);
		}
	}

	void resetFormulaRasterCache() {
		_formulaRenders.clear();
		_formulaRenders.resize(_content.formulas.size());
	}

	void relayout(int width) {
		width = std::max(width, 1);
		if (_width == width) {
			return;
		}
		_width = width;
		clearPendingHighlightBlockPointers();
		_blocks.clear();
		_anchors.clear();
		_segments.clear();
		_visibleSegmentSpan = {};
		_segmentTops.clear();
		_segmentBottoms.clear();

		const auto &markdown = st::defaultMarkdown;
		const auto &page = markdown.pagePadding;
		const auto innerWidth = std::max(width - page.left() - page.right(), 1);
		auto repaintScope = InlineIvImageRepaintScope(
			_textRepaint,
			_textRepaintRect);
		auto context = LayoutContext{
			.syntaxHighlightTracker = this,
		};
		context.mediaBlockFactory = [=](const PreparedBlock &prepared) {
			return getOrCreateMediaBlock(prepared);
		};
		const auto y = LayoutBlocks(
			_content.blocks.blocks,
			&_content.formulas,
			&_formulaRenders,
			_renderer.get(),
			_inlineFormulaObjects.get(),
			_content.mediaRuntime,
			&_blocks,
			markdown,
			page.left(),
			page.top(),
			innerWidth,
			std::move(context));
		_height = y + page.bottom();
		registerPendingHighlightBlocks(_blocks);
		CollectAnchors(_blocks, &_anchors);
		CollectSelectableSegments(&_blocks, &_segments);
		rebuildVisibleSegmentLookup();
	}

	mutable MarkdownArticleContent _content;
	std::vector<RenderedFormula> _formulaRenders;
	std::shared_ptr<MathRenderer> _renderer;
	std::shared_ptr<InlineFormulaObjectCache> _inlineFormulaObjects;
	MediaBlockHost *_mediaBlockHost = nullptr;
	Fn<void()> _textRepaint;
	Fn<void(QRect)> _textRepaintRect;
	int _width = -1;
	int _height = 0;
	std::vector<LaidOutBlock> _blocks;
	std::unordered_map<uint64, std::shared_ptr<MediaBlock>> _mediaBlocks;
	std::unordered_map<
		PendingHighlightKey,
		Spellchecker::HighlightProcessId,
		PendingHighlightKeyHasher> _pendingHighlightProcesses;
	std::unordered_map<
		Spellchecker::HighlightProcessId,
		PendingHighlightEntry> _pendingHighlightEntries;
	std::vector<std::pair<QString, int>> _anchors;
	std::vector<SelectableSegment> _segments;
	std::optional<LogicalVisibleRange> _visibleRange;
	SegmentSpan _visibleSegmentSpan;
	std::vector<int> _segmentTops;
	std::vector<int> _segmentBottoms;

};

MarkdownArticle::MarkdownArticle(std::shared_ptr<MathRenderer> renderer)
: _impl(std::make_unique<Impl>(std::move(renderer))) {
}

MarkdownArticle::~MarkdownArticle() = default;
MarkdownArticle::MarkdownArticle(MarkdownArticle &&) noexcept = default;
MarkdownArticle &MarkdownArticle::operator=(MarkdownArticle &&) noexcept = default;

void MarkdownArticle::setRenderer(std::shared_ptr<MathRenderer> renderer) {
	_impl->setRenderer(std::move(renderer));
}

void MarkdownArticle::setMediaBlockHost(MediaBlockHost *host) {
	_impl->setMediaBlockHost(host);
}

void MarkdownArticle::setTextRepaintCallbacks(
		Fn<void()> repaint,
		Fn<void(QRect)> repaintRect) {
	_impl->setTextRepaintCallbacks(
		std::move(repaint),
		std::move(repaintRect));
}

void MarkdownArticle::setContent(MarkdownArticleContent content) {
	_impl->setContent(std::move(content));
}

void MarkdownArticle::invalidateLayout() {
	_impl->invalidateLayout();
}

int MarkdownArticle::maxWidth() const {
	return const_cast<Impl*>(_impl.get())->maxWidth();
}

int MarkdownArticle::resizeGetHeight(int width) {
	return _impl->resizeGetHeight(width);
}

void MarkdownArticle::setVisibleTopBottom(int visibleTop, int visibleBottom) {
	_impl->setVisibleTopBottom(visibleTop, visibleBottom);
}

void MarkdownArticle::paint(
		Painter &p,
		QRect clip,
		MarkdownArticlePaintCaches caches,
		MarkdownArticleSelection selection,
		const MarkdownArticleSelectionEndpoints *endpoints) const {
	_impl->paint(
		p,
		clip,
		caches,
		selection,
		endpoints);
}

MarkdownArticleHitTestResult MarkdownArticle::hitTest(
		QPoint point,
		Ui::Text::StateRequest::Flags flags) const {
	return _impl->hitTest(point, flags);
}

int MarkdownArticle::anchorTop(const QString &anchorId) const {
	return _impl->anchorTop(anchorId);
}

bool MarkdownArticle::toggleDetails(const QString &anchorId) {
	return _impl->toggleDetails(anchorId);
}

bool MarkdownArticle::segmentIsText(int index) const {
	return _impl->segmentIsText(index);
}

int MarkdownArticle::segmentLength(int index) const {
	return _impl->segmentLength(index);
}

int MarkdownArticle::selectionOffsetFromHit(
		const MarkdownArticleHitTestResult &result,
		TextSelectType selectionType) const {
	return _impl->selectionOffsetFromHit(result, selectionType);
}

TextSelection MarkdownArticle::adjustSelection(
		int segmentIndex,
		TextSelection selection,
		TextSelectType selectionType) const {
	return _impl->adjustSelection(segmentIndex, selection, selectionType);
}

bool MarkdownArticle::selectionContains(
		MarkdownArticleSelection selection,
		const MarkdownArticleSelectionEndpoints *endpoints,
		const MarkdownArticleHitTestResult &result) const {
	return _impl->selectionContains(selection, endpoints, result);
}

TextForMimeData MarkdownArticle::textForContext(
		const MarkdownArticleHitTestResult &result) const {
	return _impl->textForContext(result);
}

TextForMimeData MarkdownArticle::textForSelection(
		MarkdownArticleSelection selection,
		const MarkdownArticleSelectionEndpoints *endpoints) const {
	return _impl->textForSelection(selection, endpoints);
}

bool MarkdownArticle::highlightProcessDone(
		Spellchecker::HighlightProcessId processId) {
	return _impl->highlightProcessDone(processId);
}

void MarkdownArticle::invalidatePaletteCache() {
	_impl->invalidatePaletteCache();
}

void MarkdownArticle::invalidateRasterCache() {
	_impl->invalidateRasterCache();
}

MediaBlockHost *MarkdownArticle::mediaBlockHost() const {
	return _impl->mediaBlockHost();
}

} // namespace Iv::Markdown
