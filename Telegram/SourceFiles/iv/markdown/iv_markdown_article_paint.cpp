/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_article_paint.h"
#include "iv/markdown/iv_markdown_article.h"
#include "iv/markdown/iv_markdown_article_text.h"
#include "ui/dynamic_image.h"
#include "ui/effects/path_shift_gradient.h"
#include "ui/style/style_core_scale.h"
#include "ui/widgets/checkbox.h"

#include "styles/palette.h"
#include "styles/style_iv.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainterPath>

#include <algorithm>
#include <cmath>
#include <optional>
#include <variant>

namespace Iv::Markdown {
namespace {

[[nodiscard]] QRectF CenterCropSourceRect(QSize source, QSize target) {
	const auto sourceWidth = std::max(source.width(), 1);
	const auto sourceHeight = std::max(source.height(), 1);
	const auto targetWidth = std::max(target.width(), 1);
	const auto targetHeight = std::max(target.height(), 1);
	const auto sourceRatio = float64(sourceWidth) / sourceHeight;
	const auto targetRatio = float64(targetWidth) / targetHeight;
	if (sourceRatio > targetRatio) {
		const auto width = sourceHeight * targetRatio;
		return QRectF(
			(sourceWidth - width) / 2.,
			0.,
			width,
			sourceHeight);
	}
	const auto height = sourceWidth / targetRatio;
	return QRectF(
		0.,
		(sourceHeight - height) / 2.,
		sourceWidth,
		height);
}

void PaintImageCenterCrop(Painter &p, QRect rect, const QImage &image) {
	p.drawImage(
		QRectF(rect),
		image,
		CenterCropSourceRect(image.size(), rect.size()));
}

[[nodiscard]] bool ImageCoversRect(const QImage &image, QRect rect) {
	const auto ratio = std::max(image.devicePixelRatio(), 1.);
	return (image.width() / ratio >= rect.width())
		&& (image.height() / ratio >= rect.height());
}

[[nodiscard]] bool PaintDynamicImage(
		Painter &p,
		const std::shared_ptr<Ui::DynamicImage> &image,
		QRect rect,
		bool requireCovering = false) {
	if (!image || rect.isEmpty()) {
		return false;
	}
	if (const auto frame = image->image(std::max(rect.width(), rect.height()));
		!frame.isNull()) {
		if (requireCovering && !ImageCoversRect(frame, rect)) {
			return false;
		}
		PaintImageCenterCrop(p, rect, frame);
		return true;
	}
	return false;
}

[[nodiscard]] bool PaintThumbnailImage(
		Painter &p,
		QRect rect,
		const std::shared_ptr<Ui::DynamicImage> &thumbnail,
		const std::shared_ptr<Ui::DynamicImage> &previousThumbnail) {
	return PaintDynamicImage(p, thumbnail, rect, true)
		|| PaintDynamicImage(p, previousThumbnail, rect, true)
		|| PaintDynamicImage(p, previousThumbnail, rect)
		|| PaintDynamicImage(p, thumbnail, rect);
}

[[nodiscard]] const style::Markdown &PaintStyle(
		const MarkdownArticlePaintContext &context,
		const style::Markdown &st) {
	return context.paintMarkdownStyle(st);
}

[[nodiscard]] MarkdownArticlePaintContext ClippedContext(
		const MarkdownArticlePaintContext &context,
		QRect clip) {
	auto result = context;
	result.clip = clip;
	return result;
}

[[nodiscard]] MarkdownArticlePaintContext RevealSuppressedContext(
		const MarkdownArticlePaintContext &context) {
	auto result = context;
	result.reveal = nullptr;
	return result;
}

[[nodiscard]] std::optional<int> ConsumeRevealLine(
		const MarkdownArticlePaintContext &context) {
	if (!context.reveal) {
		return std::nullopt;
	}
	return context.reveal->nextLine++;
}

[[nodiscard]] int CountGenericRevealBand(QRect rect) {
	return rect.isEmpty() ? 0 : 1;
}

[[nodiscard]] int CountTextRevealLines(
		const Ui::Text::String &leaf,
		QRect textRect,
		int textWidth) {
	if (textRect.isEmpty() || (textWidth <= 0)) {
		return 0;
	}
	return int(leaf.countLinesGeometry(textWidth, true).size());
}

[[nodiscard]] int CountRevealLinesForBlocks(
		const std::vector<LaidOutBlock> &blocks,
		const style::Markdown &st);

[[nodiscard]] int CountTableRevealLines(const LaidOutBlock &block) {
	if (block.visibleTableRect.isEmpty()) {
		return 0;
	}
	auto result = 0;
	for (const auto &row : block.tableRows) {
		if (row.outer.height() > 0) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] int CountMediaRevealLines(const LaidOutBlock &block) {
	return CountGenericRevealBand(block.visibleMediaRect)
		+ CountTextRevealLines(block.leaf, block.textRect, block.textWidth);
}

[[nodiscard]] int CountEmbedPostRevealLines(
		const LaidOutBlock &block,
		const style::Markdown &st) {
	auto result = 0;
	if (!block.headerRect.isEmpty()) {
		result += (block.mediaRect.width() > 0) ? 1 : 0;
	} else if (block.children.empty()) {
		result += CountGenericRevealBand(block.mediaRect);
	}
	result += CountRevealLinesForBlocks(block.children, st);
	result += CountTextRevealLines(block.leaf, block.textRect, block.textWidth);
	return result;
}

[[nodiscard]] int CountRevealLinesForBlock(
		const LaidOutBlock &block,
		const style::Markdown &st) {
	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Thinking:
	case PreparedBlockKind::Heading:
	case PreparedBlockKind::CodeBlock:
		return CountTextRevealLines(
			block.leaf,
			block.textRect,
			block.textWidth);
	case PreparedBlockKind::Rule:
		return CountGenericRevealBand(block.outer);
	case PreparedBlockKind::List:
	case PreparedBlockKind::ListItem:
	case PreparedBlockKind::Quote:
		return CountRevealLinesForBlocks(block.children, st);
	case PreparedBlockKind::DisplayMath:
		return CountGenericRevealBand(block.visibleFormulaRect);
	case PreparedBlockKind::Table:
		return CountTextRevealLines(
			block.leaf,
			block.textRect,
			block.textWidth) + CountTableRevealLines(block);
	case PreparedBlockKind::Details:
		return CountTextRevealLines(
			block.leaf,
			block.textRect,
			block.textWidth) + CountRevealLinesForBlocks(block.children, st);
	case PreparedBlockKind::Photo:
	case PreparedBlockKind::Video:
	case PreparedBlockKind::Audio:
	case PreparedBlockKind::Map:
	case PreparedBlockKind::Channel:
	case PreparedBlockKind::GroupedMedia:
	case PreparedBlockKind::Placeholder:
		return CountMediaRevealLines(block);
	case PreparedBlockKind::RelatedArticle:
		return CountGenericRevealBand(block.visibleMediaRect);
	case PreparedBlockKind::EmbedPost:
		return CountEmbedPostRevealLines(block, st);
	}
	Unexpected("Prepared block kind in CountRevealLinesForBlock.");
}

[[nodiscard]] int CountRevealLinesForBlocks(
		const std::vector<LaidOutBlock> &blocks,
		const style::Markdown &st) {
	auto result = 0;
	for (const auto &block : blocks) {
		result += CountRevealLinesForBlock(block, st);
	}
	return result;
}

void AdvanceRevealLinesForBlock(
		const MarkdownArticlePaintContext &context,
		const LaidOutBlock &block,
		const style::Markdown &st) {
	if (context.reveal) {
		context.reveal->nextLine += CountRevealLinesForBlock(block, st);
	}
}

template <typename Callback>
void PaintRevealBand(
		Painter &p,
		const MarkdownArticlePaintContext &context,
		QRect band,
		Callback paint) {
	if (band.isEmpty()) {
		return;
	}
	const auto lineIndex = ConsumeRevealLine(context);
	const auto visible = context.clip.intersected(band);
	if (visible.isEmpty()) {
		return;
	}

	const auto paintDirect = [&] {
		const auto local = ClippedContext(
			RevealSuppressedContext(context),
			visible);
		p.save();
		p.setClipRect(visible, Qt::IntersectClip);
		paint(p, local);
		p.restore();
	};
	if (!lineIndex) {
		paintDirect();
		return;
	}

	const auto reveal = context.reveal;
	if (*lineIndex > reveal->activeLine) {
		return;
	} else if (*lineIndex < reveal->activeLine
		|| !reveal->postprocess
		|| !reveal->postprocess->method) {
		paintDirect();
		return;
	}

	auto postprocess = reveal->postprocess->method(*lineIndex);
	if (!postprocess) {
		paintDirect();
		return;
	}

	const auto ratio = std::max(style::DevicePixelRatio(), 1);
	const auto cacheWidth = band.width() * ratio;
	const auto cacheHeight = band.height() * ratio;
	auto &cache = *reveal->postprocess->cache;
	if (cache.devicePixelRatio() != ratio
		|| cache.width() < cacheWidth
		|| cache.height() < cacheHeight) {
		cache = QImage(
			QSize(
				std::max(cache.width(), cacheWidth),
				std::max(cache.height(), cacheHeight)),
			QImage::Format_ARGB32_Premultiplied);
		cache.setDevicePixelRatio(ratio);
	}
	cache.fill(Qt::transparent);

	{
		auto cachePainter = Painter(&cache);
		cachePainter.setFont(p.font());
		cachePainter.setPen(p.pen());
		cachePainter.setBrush(p.brush());
		cachePainter.setRenderHints(p.renderHints());
		cachePainter.setInactive(p.inactive());
		cachePainter.setTextPalette(p.textPalette());
		cachePainter.translate(-band.x(), -band.y());
		cachePainter.setClipRect(band);
		const auto local = ClippedContext(
			RevealSuppressedContext(context),
			band);
		paint(cachePainter, local);
	}

	postprocess(cache);

	p.save();
	p.setClipRect(visible, Qt::IntersectClip);
	p.drawImage(
		QRectF(band),
		cache,
		QRectF(0, 0, cacheWidth, cacheHeight));
	p.restore();
}

void EnsureThinkingPaintCacheImage(
		QImage *image,
		QSize logicalSize,
		int ratio) {
	if (!image || logicalSize.isEmpty()) {
		return;
	}
	ratio = std::max(ratio, 1);
	const auto neededSize = QSize(
		logicalSize.width() * ratio,
		logicalSize.height() * ratio);
	if (image->devicePixelRatio() == ratio
		&& image->format() == QImage::Format_ARGB32_Premultiplied
		&& image->width() >= neededSize.width()
		&& image->height() >= neededSize.height()) {
		return;
	}
	*image = QImage(
		QSize(
			std::max(image->width(), neededSize.width()),
			std::max(image->height(), neededSize.height())),
		QImage::Format_ARGB32_Premultiplied);
	image->setDevicePixelRatio(ratio);
}

void FillThinkingGradientImage(
		Painter &targetPainter,
		QImage *image,
		QRect logicalRect,
		const Ui::PathShiftGradient::Background &background) {
	if (!image) {
		return;
	}
	image->fill(Qt::transparent);
	if (logicalRect.isEmpty()) {
		return;
	}
	auto imagePainter = Painter(image);
	const auto localRect = QRect(QPoint(), logicalRect.size());
	if (const auto color = std::get_if<style::color>(&background)) {
		imagePainter.fillRect(localRect, (*color)->c);
	} else {
		const auto gradient = v::get<QLinearGradient*>(background);
		auto copy = *gradient;
		const auto shift = targetPainter.worldTransform().dx()
			+ logicalRect.x();
		copy.setStart(copy.start().x() - shift, copy.start().y());
		copy.setFinalStop(
			copy.finalStop().x() - shift,
			copy.finalStop().y());
		imagePainter.fillRect(localRect, QBrush(copy));
	}
}

[[nodiscard]] QColor WithOpacity(style::color color, double opacity) {
	auto result = color->c;
	result.setAlphaF(result.alphaF() * std::clamp(opacity, 0., 1.));
	return result;
}

[[nodiscard]] QColor TableHeaderBg(const style::MarkdownTable &st) {
	return WithOpacity(st.headerBg, st.headerBgOpacity);
}

void RefreshBlockThumbnail(
		const LaidOutBlock &block,
		const MarkdownArticlePaintContext &context) {
	if (!block.photoRuntime || block.thumbnailRect.isEmpty()) {
		return;
	}
	const auto size = block.thumbnailRect.size();
	if (size.isEmpty() || block.thumbnailRequestSize == size) {
		return;
	}
	block.thumbnailRequestSize = size;
	if (const auto image = block.photoRuntime->thumbnail(size)) {
		if (image != block.thumbnailImage) {
			block.previousThumbnailImage = std::move(block.thumbnailImage);
			block.thumbnailImage = image;
		}
		if (image != block.subscribedThumbnailImage) {
			block.subscribedThumbnailImage = image;
			const auto repaint = context.caches.repaint;
			const auto repaintRect = context.caches.repaintRect;
			const auto rect = block.mediaRect;
			image->subscribeToUpdates([repaint, repaintRect, rect] {
				if (repaintRect && !rect.isEmpty()) {
					repaintRect(rect);
				} else if (repaint) {
					repaint();
				}
			});
		}
	}
}

void PaintTextLeaf(
		Painter &p,
		const Ui::Text::String &leaf,
		const MarkdownArticlePaintContext &context,
		QRect rect,
		int width,
		style::align align = style::al_left,
		std::optional<TextSelection> selection = std::nullopt,
		int elisionLines = 0) {
	const auto availableWidth = std::max(width, 1);
	auto linePostprocess = std::optional<Ui::Text::LinePostprocess>();
	if (context.reveal && !elisionLines) {
		const auto lines = leaf.countLinesGeometry(availableWidth, true);
		const auto baseLine = context.reveal->nextLine;
		context.reveal->nextLine += int(lines.size());
		if (const auto articlePostprocess = context.reveal->postprocess) {
			const auto activeLine = context.reveal->activeLine;
			linePostprocess.emplace(Ui::Text::LinePostprocess{
				.method = [=](int lineIndex) -> Fn<void(QImage&)> {
					const auto globalLine = baseLine + lineIndex;
					if (globalLine != activeLine
						|| !articlePostprocess->method) {
						return nullptr;
					}
					return articlePostprocess->method(globalLine);
				},
				.cache = articlePostprocess->cache,
			});
		}
	}
	p.save();
	if (!context.clip.isNull()) {
		p.setClipRect(context.clip, Qt::IntersectClip);
	}
	leaf.draw(p, {
		.position = rect.topLeft(),
		.availableWidth = availableWidth,
		.geometry = elisionLines
			? Ui::Text::SimpleGeometry(availableWidth, elisionLines, 0, true)
			: TextGeometry(availableWidth),
		.align = align,
		.clip = context.clip,
		.palette = &p.textPalette(),
		.pre = context.caches.pre,
		.blockquote = context.caches.blockquote,
		.colors = context.caches.colors,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = context.now,
		.selection = selection.value_or(TextSelection()),
		.elisionLines = elisionLines,
		.linePostprocess = linePostprocess ? &*linePostprocess : nullptr,
	});
	p.restore();
}

[[nodiscard]] std::optional<QColor> QuoteSupplementaryColor(
		const MarkdownArticlePaintContext &context) {
	if (!context.caches.blockquote) {
		return {};
	}
	return anim::color(
		context.caches.blockquote->bg,
		context.caches.blockquote->outlines[0],
		0.9);
}

void SetTextLeafPen(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	const auto &paintSt = PaintStyle(context, st);
	p.setPen(!block.supplementary
		? paintSt.textColor->c
		: context.caches.supplementaryColorOverride.value_or(
			paintSt.supplementaryTextColor->c));
}

void PaintThinkingTextLeafDirect(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	const auto &paintSt = PaintStyle(context, st);
	p.setPen(paintSt.supplementaryTextColor->c);
	PaintTextLeaf(
		p,
		block.leaf,
		context,
		block.textRect,
		block.textWidth,
		style::al_left,
		TextSelectionForSegmentIndex(
			context.selectionState,
			block.segmentIndex));
}

void PaintRelatedArticleTextLeaf(
		Painter &p,
		const Ui::Text::String &leaf,
		const MarkdownArticlePaintContext &context,
		QRect rect,
		int width,
		int elisionLines) {
	const auto textClip = context.clip.intersected(rect);
	if (textClip.isEmpty()) {
		return;
	}
	const auto clipped = ClippedContext(context, textClip);
	p.save();
	p.setClipRect(textClip, Qt::IntersectClip);
	PaintTextLeaf(
		p,
		leaf,
		clipped,
		rect,
		width,
		style::al_left,
		std::nullopt,
		elisionLines);
	p.restore();
}

void PaintTaskMarker(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context,
		int outerWidth) {
	const auto rect = block.markerRect;
	if (rect.isEmpty()) {
		return;
	}
	const auto &paintSt = PaintStyle(context, st);
	auto view = Ui::CheckView(
		paintSt.list.taskCheck,
		block.taskState == TaskState::Checked);
	view.finishAnimating();
	view.paint(p, rect.left(), rect.top(), outerWidth);
}

void PaintBulletMarker(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	const auto radius = st.list.bulletRadius;
	if (radius <= 0) {
		return;
	}
	const auto &paintSt = PaintStyle(context, st);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(paintSt.list.bulletFg->c);
	p.drawEllipse(QPointF(block.markerCenter), radius, radius);
}

[[nodiscard]] const QImage &ColorizedDisplayFormulaImage(
		const LaidOutBlock &block,
		const RenderedFormula &formula,
		QColor color) {
	const auto size = formula.image.size();
	if (block.colorizedFormulaImage.isNull()
		|| (block.colorizedFormulaSize != size)
		|| (block.colorizedFormulaColor != color)) {
		block.colorizedFormulaImage = QImage(
			size,
			QImage::Format_ARGB32_Premultiplied);
		style::colorizeImage(
			formula.image,
			color,
			&block.colorizedFormulaImage,
			QRect(),
			QPoint(),
			true);
		block.colorizedFormulaColor = color;
		block.colorizedFormulaSize = size;
	}
	return block.colorizedFormulaImage;
}

[[nodiscard]] int TableBorder(
		const LaidOutBlock &block,
		const style::Markdown &st) {
	return block.tableBordered ? st.table.border : 0;
}

struct TableOwnershipSlot {
	const LaidOutTableCell *cell = nullptr;
};

using TableOwnershipGrid = std::vector<std::vector<TableOwnershipSlot>>;

[[nodiscard]] TableOwnershipGrid BuildTableOwnershipGrid(
		const LaidOutBlock &block) {
	const auto rowCount = int(block.tableRows.size());
	const auto columnCount = int(block.tableColumnWidths.size());
	auto result = TableOwnershipGrid(
		std::max(rowCount, 0),
		std::vector<TableOwnershipSlot>(std::max(columnCount, 0)));
	for (auto row = 0; row != rowCount; ++row) {
		for (const auto &cell : block.tableRows[row].cells) {
			const auto fromRow = std::clamp(row, 0, rowCount);
			const auto toRow = std::clamp(row + cell.rowspan, 0, rowCount);
			const auto fromColumn = std::clamp(cell.column, 0, columnCount);
			const auto toColumn = std::clamp(
				cell.column + cell.colspan,
				0,
				columnCount);
			for (auto currentRow = fromRow; currentRow != toRow; ++currentRow) {
				for (auto currentColumn = fromColumn;
					currentColumn != toColumn;
					++currentColumn) {
					result[currentRow][currentColumn].cell = &cell;
				}
			}
		}
	}
	return result;
}

[[nodiscard]] QPainterPath TableShapePath(
		const LaidOutBlock &block,
		int border,
		int radius) {
	auto path = QPainterPath();
	if (block.tableRect.isEmpty()) {
		return path;
	}
	if (border > 0) {
		const auto half = border / 2.;
		path.addRoundedRect(
			QRectF(block.tableRect).marginsRemoved({
				half,
				half,
				half,
				half,
			}),
			radius,
			radius);
	} else {
		path.addRect(block.tableRect);
	}
	return path;
}

[[nodiscard]] auto TableColumnLefts(
		const LaidOutBlock &block,
		int columnCount,
		int border) -> std::vector<int> {
	auto result = std::vector<int>(
		std::max(columnCount, 0),
		block.tableRect.x() + border);
	auto separatorLeft = block.tableRect.x() + border;
	for (auto column = 0; column != columnCount; ++column) {
		result[column] = separatorLeft;
		if (column < int(block.tableColumnWidths.size())) {
			separatorLeft += block.tableColumnWidths[column] + border;
		}
	}
	return result;
}

void AddTableHorizontalBorderSegments(
		QPainterPath *path,
		const LaidOutBlock &block,
		const TableOwnershipGrid &ownership,
		const std::vector<int> &columnLefts,
		QRectF inner,
		float64 half) {
	const auto rowCount = int(ownership.size());
	const auto columnCount = rowCount ? int(ownership.front().size()) : 0;
	for (auto boundaryRow = 1; boundaryRow != rowCount; ++boundaryRow) {
		auto segmentStart = -1;
		for (auto column = 0; column != columnCount; ++column) {
			const auto split = ownership[boundaryRow - 1][column].cell
				!= ownership[boundaryRow][column].cell;
			if (split && (segmentStart < 0)) {
				segmentStart = column;
			} else if (!split && (segmentStart >= 0)) {
				const auto fromX = (segmentStart == 0)
					? inner.x()
					: (columnLefts[segmentStart] - half);
				const auto toX = columnLefts[column] - half;
				const auto y = block.tableRows[boundaryRow].outer.y() - half;
				path->moveTo(fromX, y);
				path->lineTo(toX, y);
				segmentStart = -1;
			}
		}
		if (segmentStart >= 0) {
			const auto fromX = (segmentStart == 0)
				? inner.x()
				: (columnLefts[segmentStart] - half);
			const auto y = block.tableRows[boundaryRow].outer.y() - half;
			path->moveTo(fromX, y);
			path->lineTo(inner.x() + inner.width(), y);
		}
	}
}

void AddTableVerticalBorderSegments(
		QPainterPath *path,
		const LaidOutBlock &block,
		const TableOwnershipGrid &ownership,
		const std::vector<int> &columnLefts,
		QRectF inner,
		float64 half) {
	const auto rowCount = int(ownership.size());
	const auto columnCount = rowCount ? int(ownership.front().size()) : 0;
	for (auto boundaryColumn = 1;
		boundaryColumn != columnCount;
		++boundaryColumn) {
		auto segmentStart = -1;
		for (auto row = 0; row != rowCount; ++row) {
			const auto split = ownership[row][boundaryColumn - 1].cell
				!= ownership[row][boundaryColumn].cell;
			if (split && (segmentStart < 0)) {
				segmentStart = row;
			} else if (!split && (segmentStart >= 0)) {
				const auto fromY = (segmentStart == 0)
					? inner.y()
					: (block.tableRows[segmentStart].outer.y() - half);
				const auto toY = block.tableRows[row].outer.y() - half;
				const auto x = columnLefts[boundaryColumn] - half;
				path->moveTo(x, fromY);
				path->lineTo(x, toY);
				segmentStart = -1;
			}
		}
		if (segmentStart >= 0) {
			const auto fromY = (segmentStart == 0)
				? inner.y()
				: (block.tableRows[segmentStart].outer.y() - half);
			const auto x = columnLefts[boundaryColumn] - half;
			path->moveTo(x, fromY);
			path->lineTo(x, inner.y() + inner.height());
		}
	}
}

[[nodiscard]] QPainterPath TableBorderPath(
		const LaidOutBlock &block,
		int border,
		QPainterPath path) {
	const auto ownership = BuildTableOwnershipGrid(block);
	const auto rowCount = int(ownership.size());
	const auto columnCount = rowCount
		? int(ownership.front().size())
		: int(block.tableColumnWidths.size());
	if (!rowCount || !columnCount) {
		return path;
	}
	const auto half = border / 2.;
	const auto columnLefts = TableColumnLefts(block, columnCount, border);
	const auto inner = QRectF(block.tableRect).marginsRemoved({
		half,
		half,
		half,
		half,
	});
	AddTableHorizontalBorderSegments(
		&path,
		block,
		ownership,
		columnLefts,
		inner,
		half);
	AddTableVerticalBorderSegments(
		&path,
		block,
		ownership,
		columnLefts,
		inner,
		half);
	return path;
}

[[nodiscard]] QRect TableRowRevealBand(
		const LaidOutBlock &block,
		const style::Markdown &st,
		int rowIndex) {
	if (block.visibleTableRect.isEmpty()
		|| rowIndex < 0
		|| rowIndex >= int(block.tableRows.size())) {
		return QRect();
	}
	const auto &row = block.tableRows[rowIndex];
	if (row.outer.height() <= 0) {
		return QRect();
	}
	const auto border = std::max(TableBorder(block, st), 0);
	const auto top = (rowIndex == 0)
		? block.visibleTableRect.y()
		: row.outer.y();
	const auto tableBottom = block.visibleTableRect.y()
		+ block.visibleTableRect.height();
	const auto bottom = std::min(
		row.outer.y() + row.outer.height() + border,
		tableBottom);
	if (bottom <= top) {
		return QRect();
	}
	return QRect(
		block.visibleTableRect.x(),
		top,
		block.visibleTableRect.width(),
		bottom - top);
}

[[nodiscard]] auto TableCellsForRowBand(
		const TableOwnershipGrid &ownership,
		int rowIndex,
		QRect rowBand) -> std::vector<const LaidOutTableCell*> {
	if (rowIndex < 0 || rowIndex >= int(ownership.size())) {
		return {};
	}
	auto result = std::vector<const LaidOutTableCell*>();
	result.reserve(ownership[rowIndex].size());
	for (const auto &slot : ownership[rowIndex]) {
		const auto cell = slot.cell;
		if (!cell || !cell->outer.intersects(rowBand)) {
			continue;
		} else if (std::find(result.begin(), result.end(), cell)
			!= result.end()) {
			continue;
		}
		result.push_back(cell);
	}
	return result;
}

[[nodiscard]] int TableCellOriginRow(
		const LaidOutBlock &block,
		const LaidOutTableCell *cell) {
	for (auto rowIndex = 0;
		rowIndex != int(block.tableRows.size());
		++rowIndex) {
		for (const auto &current : block.tableRows[rowIndex].cells) {
			if (&current == cell) {
				return rowIndex;
			}
		}
	}
	return -1;
}

void PaintTableCaption(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	if (!block.textRect.isEmpty()) {
		SetTextLeafPen(p, block, st, context);
		PaintTextLeaf(
			p,
			block.leaf,
			context,
			block.textRect,
			block.textWidth,
			style::al_left,
			TextSelectionForSegmentIndex(
				context.selectionState,
				block.secondarySegmentIndex));
	}
}

void PaintWholeTable(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	const auto tableClip = context.clip.intersected(block.visibleTableRect);
	if (tableClip.isEmpty()) {
		return;
	}
	const auto tableContext = ClippedContext(context, tableClip);

	const auto border = TableBorder(block, st);
	const auto radius = st.table.radius;
	const auto shapePath = TableShapePath(block, border, radius);
	const auto &paintSt = PaintStyle(context, st);

	p.save();
	p.setClipRect(tableClip);
	p.save();
	p.setClipPath(shapePath, Qt::IntersectClip);
	for (auto rowIndex = 0, rowCount = int(block.tableRows.size()); rowIndex != rowCount; ++rowIndex) {
		const auto striped = block.tableStriped && ((rowIndex % 2) == 0);
		for (const auto &cell : block.tableRows[rowIndex].cells) {
			if (!cell.outer.intersects(block.visibleTableRect)) {
				continue;
			}
			if (!cell.header && !striped) {
				continue;
			}
			p.fillRect(cell.outer, TableHeaderBg(paintSt.table));
		}
	}
	p.restore();

	if (border > 0 && !block.tableRect.isEmpty()) {
		const auto path = TableBorderPath(block, border, shapePath);
		auto hq = PainterHighQualityEnabler(p);
		auto pen = QPen(
			WithOpacity(
				paintSt.table.borderFg,
				paintSt.table.headerBgOpacity * 3),
			border);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
	}

	p.setPen(paintSt.textColor->c);
	for (const auto &row : block.tableRows) {
		if (!row.outer.intersects(block.visibleTableRect)) {
			continue;
		}
		for (const auto &cell : row.cells) {
			if (!cell.textRect.intersects(block.visibleTableRect)) {
				continue;
			}
			PaintTextLeaf(
				p,
				cell.leaf,
				tableContext,
				cell.textRect,
				cell.textWidth,
				cell.align,
				TextSelectionForSegmentIndex(
					context.selectionState,
					cell.segmentIndex));
		}
	}

	if (block.segmentIndex >= 0
		&& WholeSegmentSelected(
			context.selectionState,
			block.segmentIndex)) {
		p.save();
		p.setClipPath(shapePath, Qt::IntersectClip);
		p.fillRect(block.tableRect, p.textPalette().selectOverlay);
		p.restore();
	}

	if (block.overflowed) {
		const auto indicatorWidth = std::min(
			std::max(st.table.overflowWidth, 1),
			block.visibleTableRect.width());
		p.save();
		p.setClipPath(shapePath, Qt::IntersectClip);
		p.fillRect(
			QRect(
				block.visibleTableRect.x()
					+ block.visibleTableRect.width()
					- indicatorWidth,
				block.visibleTableRect.y(),
				indicatorWidth,
				block.visibleTableRect.height()),
			paintSt.table.overflowFg->c);
		p.restore();
	}

	p.restore();
}

void PaintTableRowBand(
		Painter &p,
		const LaidOutBlock &block,
		const TableOwnershipGrid &ownership,
		int rowIndex,
		QRect rowBand,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	const auto rowClip = context.clip.intersected(rowBand);
	if (rowClip.isEmpty()) {
		return;
	}

	const auto border = TableBorder(block, st);
	const auto radius = st.table.radius;
	const auto shapePath = TableShapePath(block, border, radius);
	const auto &paintSt = PaintStyle(context, st);
	const auto cells = TableCellsForRowBand(ownership, rowIndex, rowBand);
	const auto rowContext = ClippedContext(
		RevealSuppressedContext(context),
		rowClip);

	p.save();
	p.setClipRect(rowClip, Qt::IntersectClip);
	p.save();
	p.setClipPath(shapePath, Qt::IntersectClip);
	for (const auto cell : cells) {
		const auto originRow = TableCellOriginRow(block, cell);
		const auto striped = block.tableStriped
			&& (originRow >= 0)
			&& ((originRow % 2) == 0);
		if (!cell->header && !striped) {
			continue;
		}
		p.fillRect(cell->outer, TableHeaderBg(paintSt.table));
	}
	p.restore();

	if (border > 0 && !block.tableRect.isEmpty()) {
		const auto path = TableBorderPath(block, border, shapePath);
		auto hq = PainterHighQualityEnabler(p);
		auto pen = QPen(
			WithOpacity(
				paintSt.table.borderFg,
				paintSt.table.headerBgOpacity * 3),
			border);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
	}

	p.setPen(paintSt.textColor->c);
	for (const auto cell : cells) {
		if (!cell->textRect.intersects(rowBand)) {
			continue;
		}
		PaintTextLeaf(
			p,
			cell->leaf,
			rowContext,
			cell->textRect,
			cell->textWidth,
			cell->align,
			TextSelectionForSegmentIndex(
				context.selectionState,
				cell->segmentIndex));
	}

	if (block.segmentIndex >= 0
		&& WholeSegmentSelected(
			context.selectionState,
			block.segmentIndex)) {
		p.save();
		p.setClipPath(shapePath, Qt::IntersectClip);
		p.fillRect(block.tableRect, p.textPalette().selectOverlay);
		p.restore();
	}

	if (block.overflowed) {
		const auto indicatorWidth = std::min(
			std::max(st.table.overflowWidth, 1),
			block.visibleTableRect.width());
		p.save();
		p.setClipPath(shapePath, Qt::IntersectClip);
		p.fillRect(
			QRect(
				block.visibleTableRect.x()
					+ block.visibleTableRect.width()
					- indicatorWidth,
				block.visibleTableRect.y(),
				indicatorWidth,
				block.visibleTableRect.height()),
			paintSt.table.overflowFg->c);
		p.restore();
	}

	p.restore();
}

void PaintTableBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintTableCaption(p, block, st, context);
	if (!context.reveal) {
		PaintWholeTable(p, block, st, context);
		return;
	}

	const auto ownership = BuildTableOwnershipGrid(block);
	for (auto rowIndex = 0;
		rowIndex != int(block.tableRows.size());
		++rowIndex) {
		const auto rowBand = TableRowRevealBand(block, st, rowIndex);
		PaintRevealBand(
			p,
			context,
			rowBand,
			[&](Painter &p, const MarkdownArticlePaintContext &rowContext) {
				PaintTableRowBand(
					p,
					block,
					ownership,
					rowIndex,
					rowBand,
					st,
					rowContext);
			});
	}
}

void PaintDisplayMathBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintRevealBand(
		p,
		context,
		block.visibleFormulaRect,
		[&](Painter &p, const MarkdownArticlePaintContext &formulaContext) {
			const auto &paintSt = PaintStyle(formulaContext, st);
			const auto formula = PreparedFormulaFor(formulas, block.formulaIndex);
			p.setPen(paintSt.textColor->c);
			const auto rendered = EnsureFormulaRendered(
				formula,
				FormulaRasterSlot(renderedFormulas, block.formulaIndex),
				renderer,
				devicePixelRatio,
				st);
			if (rendered.success) {
				p.drawImage(
					block.formulaRect.topLeft(),
					ColorizedDisplayFormulaImage(
						block,
						rendered,
						p.pen().color()));
			}
			if (!rendered.success) {
				const auto radius = st.displayMath.fallbackRadius;
				p.setPen(Qt::NoPen);
				p.setBrush(paintSt.displayMath.fallbackBg->c);
				if (radius > 0) {
					auto hq = PainterHighQualityEnabler(p);
					p.drawRoundedRect(block.formulaRect, radius, radius);
				} else {
					p.fillRect(block.formulaRect, paintSt.displayMath.fallbackBg->c);
				}
				p.setPen(paintSt.textColor->c);
				PaintTextLeaf(
					p,
					block.fallbackLeaf,
					formulaContext,
					block.textRect,
					block.textWidth);
			}

			if (block.segmentIndex >= 0
				&& WholeSegmentSelected(
					formulaContext.selectionState,
					block.segmentIndex)) {
				p.fillRect(
					block.visibleFormulaRect,
					p.textPalette().selectOverlay);
			}

			if (block.overflowed) {
				const auto indicatorWidth = std::min(
					std::max(st.displayMath.overflowWidth, 1),
					block.visibleFormulaRect.width());
				p.fillRect(
					QRect(
						block.visibleFormulaRect.x()
							+ block.visibleFormulaRect.width()
							- indicatorWidth,
						block.visibleFormulaRect.y(),
						indicatorWidth,
						block.visibleFormulaRect.height()),
					paintSt.displayMath.overflowFg->c);
			}
		});
}

void PaintQuoteBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	const auto quoteClip = context.clip.intersected(block.outer);
	if (quoteClip.isEmpty()) {
		return;
	}

	if (context.caches.blockquote) {
		const auto &quoteStyle = st.body.blockquote;
		Ui::Text::ValidateQuotePaintCache(
			*context.caches.blockquote,
			quoteStyle);

		p.save();
		p.setClipRect(quoteClip);
		Ui::Text::FillQuotePaint(
			p,
			block.outer,
			*context.caches.blockquote,
			quoteStyle);
		p.restore();
	}

	auto local = ClippedContext(
		context,
		context.clip.intersected(block.contentRect));
	local.caches.supplementaryColorOverride = QuoteSupplementaryColor(context);
	PaintBlocks(
		p,
		block.children,
		formulas,
		renderedFormulas,
		renderer,
		devicePixelRatio,
		outerWidth,
		st,
		local);
}

void PaintPlaceholderBlock(
		Painter &p,
		const LaidOutBlock &block,
		int outerWidth,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintRevealBand(
		p,
		context,
		block.visibleMediaRect,
		[&](Painter &p, const MarkdownArticlePaintContext &visibleContext) {
			auto hq = PainterHighQualityEnabler(p);
			if (block.activation.kind == MediaActivationKind::Embed
				&& block.placeholderRuntime) {
				const auto border = st.placeholder.border;
				const auto radius = st.placeholder.radius;
				const auto borderSkip = border / 2;
				const auto borderRect = block.mediaRect.marginsRemoved(QMargins(
					borderSkip,
					borderSkip,
					borderSkip,
					borderSkip));
				const auto active = ClickHandler::showAsActive(
					block.placeholderRuntime->clickHandler);
				const auto pressed = ClickHandler::showAsPressed(
					block.placeholderRuntime->clickHandler);
				p.setPen(Qt::NoPen);
				p.setBrush(st.placeholder.bg);
				p.drawRoundedRect(block.mediaRect, radius, radius);
				if (active || pressed) {
					p.setBrush(st.placeholder.bgActive);
					p.drawRoundedRect(block.mediaRect, radius, radius);
				}
				if (const auto &ripple = block.placeholderRuntime->ripple) {
					ripple->paint(
						p,
						block.mediaRect.x(),
						block.mediaRect.y(),
						outerWidth,
						&st.placeholder.rippleBg->c);
				}
				auto pen = QPen(st.placeholder.borderFg->c);
				pen.setWidth(border);
				p.setPen(pen);
				p.setBrush(Qt::NoBrush);
				p.drawRoundedRect(borderRect, radius, radius);
				if (block.placeholderRuntime->loading) {
					const auto size = QSize(
						st.placeholder.spinnerSize,
						st.placeholder.spinnerSize);
					const auto spinner = style::centerrect(
						block.mediaRect,
						QRect(QPoint(), size));
					Ui::InfiniteRadialAnimation::Draw(
						p,
						block.placeholderRuntime->loadingAnimation.computeState(),
						spinner.topLeft(),
						spinner.size(),
						outerWidth,
						QPen(st.placeholder.spinnerFg->c),
						st.placeholder.spinnerWidth);
				} else {
					p.setPen(st.placeholder.labelFgActive->c);
					PaintTextLeaf(
						p,
						block.labelLeaf,
						visibleContext,
						block.labelRect,
						block.labelWidth,
						style::al_center);
				}
			} else {
				const auto max = block.labelLeaf.maxWidth();
				const auto radius = st.placeholder.radius;
				p.setBrush(st.placeholder.bg);
				p.setPen(Qt::NoPen);
				const auto skip = (max < block.labelRect.width())
					? ((block.labelRect.width() - max) / 2)
					: 0;
				p.drawRoundedRect(
					block.labelRect.marginsRemoved(
						{ skip, 0, skip, 0 }
					).marginsAdded(st.placeholder.padding),
					radius,
					radius);
				p.setPen(st.placeholder.labelFg->c);
				PaintTextLeaf(
					p,
					block.labelLeaf,
					visibleContext,
					block.labelRect,
					block.labelWidth,
					style::al_center);
			}
			if (block.segmentIndex >= 0
				&& WholeSegmentSelected(
					visibleContext.selectionState,
					block.segmentIndex)) {
				p.fillRect(block.visibleMediaRect, p.textPalette().selectOverlay);
			}
		});
	if (!block.textRect.isEmpty()) {
		SetTextLeafPen(p, block, st, context);
		PaintTextLeaf(
			p,
			block.leaf,
			context,
			block.textRect,
			block.textWidth,
			style::al_left,
			TextSelectionForSegmentIndex(
				context.selectionState,
				block.secondarySegmentIndex));
	}
}

[[nodiscard]] QPainterPath RoundedRectPath(QRect rect, int radius) {
	auto path = QPainterPath();
	path.addRoundedRect(QRectF(rect), radius, radius);
	return path;
}

void PaintEmbedPostBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	const auto &style = st.embedPost;
	const auto paintHeader = [&](
			Painter &p,
			const MarkdownArticlePaintContext &headerContext) {
		RefreshBlockThumbnail(block, headerContext);
		if (style.accentWidth > 0) {
			p.fillRect(
				QRect(
					block.mediaRect.x(),
					block.mediaRect.y(),
					style.accentWidth,
					block.mediaRect.height()),
				style.accentFg->c);
		}
		if (block.photoRuntime && !block.thumbnailRect.isEmpty()) {
			auto hq = PainterHighQualityEnabler(p);
			const auto avatarPath = RoundedRectPath(
				block.thumbnailRect,
				style.avatarRadius);
			p.fillPath(avatarPath, st.photo.fallbackBg->c);
			p.save();
			p.setClipPath(
				avatarPath,
				Qt::IntersectClip);
			(void)PaintThumbnailImage(
				p,
				block.thumbnailRect,
				block.thumbnailImage,
				block.previousThumbnailImage);
			p.restore();
		}
		if (!block.labelRect.isEmpty()) {
			p.setPen(style.authorFg->c);
			PaintTextLeaf(
				p,
				block.labelLeaf,
				headerContext,
				block.labelRect,
				block.labelWidth,
				style::al_left,
				TextSelectionForSegmentIndex(
					headerContext.selectionState,
					block.segmentIndex));
		}
		if (!block.subtitleRect.isEmpty()) {
			p.setPen(style.dateFg->c);
			PaintTextLeaf(
				p,
				block.subtitleLeaf,
				headerContext,
				block.subtitleRect,
				block.subtitleWidth,
				style::al_left,
				TextSelectionForSegmentIndex(
					headerContext.selectionState,
					block.secondarySegmentIndex));
		}
	};
	if (context.reveal) {
		if (!block.headerRect.isEmpty()) {
			const auto headerBottom = block.headerRect.y()
				+ block.headerRect.height();
			PaintRevealBand(
				p,
				context,
				QRect(
					block.mediaRect.x(),
					block.mediaRect.y(),
					block.mediaRect.width(),
					headerBottom - block.mediaRect.y()),
				paintHeader);
		} else if (block.children.empty()) {
			PaintRevealBand(p, context, block.mediaRect, paintHeader);
		}
	} else {
		const auto mediaClip = context.clip.intersected(block.mediaRect);
		if (!mediaClip.isEmpty()) {
			const auto mediaContext = ClippedContext(context, mediaClip);
			p.save();
			p.setClipRect(mediaClip);
			paintHeader(p, mediaContext);
			p.restore();
		}
	}
	if (context.reveal
		&& style.accentWidth > 0
		&& !block.children.empty()) {
		const auto top = !block.headerRect.isEmpty()
			? block.headerRect.y() + block.headerRect.height()
			: block.mediaRect.y();
		const auto bottom = block.mediaRect.y() + block.mediaRect.height();
		const auto accentRect = QRect(
			block.mediaRect.x(),
			top,
			style.accentWidth,
			std::max(bottom - top, 0));
		const auto accentClip = context.clip.intersected(accentRect);
		if (!accentClip.isEmpty()) {
			p.save();
			p.setClipRect(accentClip);
			p.fillRect(accentRect, style.accentFg->c);
			p.restore();
		}
	}
	if (!block.bodyRect.isEmpty()) {
		const auto bodyContext = ClippedContext(
			context,
			context.clip.intersected(block.bodyRect));
		PaintBlocks(
			p,
			block.children,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			st,
			bodyContext);
	}
	if (!block.textRect.isEmpty()) {
		SetTextLeafPen(p, block, st, context);
		PaintTextLeaf(
			p,
			block.leaf,
			context,
			block.textRect,
			block.textWidth,
			style::al_left,
			TextSelectionForSegmentIndex(
				context.selectionState,
				block.tertiarySegmentIndex));
	}
}

void PaintMediaCaption(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	if (block.textRect.isEmpty()) {
		return;
	}
	SetTextLeafPen(p, block, st, context);
	PaintTextLeaf(
		p,
		block.leaf,
		context,
		block.textRect,
		block.textWidth,
		style::al_left,
		TextSelectionForSegmentIndex(
			context.selectionState,
			block.secondarySegmentIndex));
}

void PaintPersistentMediaBlock(
		Painter &p,
		const LaidOutBlock &block,
		const MarkdownArticlePaintContext &context) {
	PaintRevealBand(
		p,
		context,
		block.visibleMediaRect,
		[&](Painter &p, const MarkdownArticlePaintContext &mediaContext) {
			if (block.mediaBlock) {
				block.mediaBlock->paint(p, mediaContext);
			}
			if (block.segmentIndex >= 0
				&& WholeSegmentSelected(
					mediaContext.selectionState,
					block.segmentIndex)) {
				p.fillRect(block.mediaRect, p.textPalette().selectOverlay);
			}
		});
}

void PaintCardSurface(
		Painter &p,
		QRect rect,
		int border,
		const style::color &borderFg,
		const style::color &bg,
		int radius) {
	if (rect.isEmpty()) {
		return;
	}
	if (border <= 0) {
		if (radius > 0) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(bg->c);
			p.drawRoundedRect(rect, radius, radius);
		} else {
			p.fillRect(rect, bg->c);
		}
		return;
	}
	const auto half = border / 2.;
	const auto inner = QRectF(rect).marginsRemoved({
		half,
		half,
		half,
		half,
	});
	if (radius > 0) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(borderFg->c, border));
		p.setBrush(bg->c);
		p.drawRoundedRect(inner, radius, radius);
	} else {
		p.fillRect(rect, bg->c);
		p.setPen(QPen(borderFg->c, border));
		p.setBrush(Qt::NoBrush);
		p.drawRect(inner);
	}
}

void PaintAudioBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintPersistentMediaBlock(p, block, context);
	PaintMediaCaption(p, block, st, context);
}

void PaintChannelBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintPersistentMediaBlock(p, block, context);
	PaintMediaCaption(p, block, st, context);
}

void PaintRelatedArticleBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintRevealBand(
		p,
		context,
		block.visibleMediaRect,
		[&](Painter &p, const MarkdownArticlePaintContext &visibleContext) {
			const auto &style = st.relatedArticle;
			RefreshBlockThumbnail(block, visibleContext);
			PaintCardSurface(
				p,
				block.mediaRect,
				style.border,
				style.borderFg,
				style.bg,
				style.radius);
			if (!block.thumbnailRect.isEmpty()) {
				p.fillRect(block.thumbnailRect, style.bg->c);
				if (style.thumbnailRadius > 0) {
					auto hq = PainterHighQualityEnabler(p);
					auto path = RoundedRectPath(
						block.thumbnailRect,
						style.thumbnailRadius);
					p.save();
					p.setClipPath(path, Qt::IntersectClip);
					(void)PaintThumbnailImage(
						p,
						block.thumbnailRect,
						block.thumbnailImage,
						block.previousThumbnailImage);
					p.restore();
				} else {
					(void)PaintThumbnailImage(
						p,
						block.thumbnailRect,
						block.thumbnailImage,
						block.previousThumbnailImage);
				}
			}
			if (!block.labelRect.isEmpty()) {
				p.setPen(st.textColor->c);
				PaintRelatedArticleTextLeaf(
					p,
					block.labelLeaf,
					visibleContext,
					block.labelRect,
					block.labelWidth,
					style.titleLines);
			}
			if (!block.subtitleRect.isEmpty()) {
				p.setPen(st.textColor->c);
				PaintRelatedArticleTextLeaf(
					p,
					block.subtitleLeaf,
					visibleContext,
					block.subtitleRect,
					block.subtitleWidth,
					style.subtitleLines);
			}
			if (!block.actionRect.isEmpty()) {
				p.setPen(st.supplementaryTextColor->c);
				PaintRelatedArticleTextLeaf(
					p,
					block.actionLeaf,
					visibleContext,
					block.actionRect,
					block.actionWidth,
					style.footerLines);
			}
			if (block.segmentIndex >= 0
				&& WholeSegmentSelected(
					visibleContext.selectionState,
					block.segmentIndex)) {
				p.fillRect(
					block.visibleMediaRect,
					p.textPalette().selectOverlay);
			}
			if (style.separator > 0) {
				p.fillRect(
					QRect(
						block.mediaRect.x(),
						block.mediaRect.y()
							+ block.mediaRect.height()
							- style.separator,
						block.mediaRect.width(),
						style.separator),
					style.separatorFg->c);
			}
		});
}

void PaintPhotoBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintPersistentMediaBlock(p, block, context);
	PaintMediaCaption(p, block, st, context);
}

void PaintVideoBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintPersistentMediaBlock(p, block, context);
	PaintMediaCaption(p, block, st, context);
}

void PaintMapBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintPersistentMediaBlock(p, block, context);
	PaintMediaCaption(p, block, st, context);
}

void PaintGroupedMediaBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	PaintRevealBand(
		p,
		context,
		block.visibleMediaRect,
		[&](Painter &p, const MarkdownArticlePaintContext &mediaContext) {
			if (block.mediaBlock) {
				block.mediaBlock->paint(p, mediaContext);
				if ((block.segmentIndex >= 0)
					&& WholeSegmentSelected(
						mediaContext.selectionState,
						block.segmentIndex)) {
					const auto &style = PaintStyle(
						mediaContext,
						st).groupedMedia;
					auto overlay = p.textPalette().selectOverlay->c;
					overlay.setAlphaF(std::clamp(
						style.overlayOpacity,
						0.,
						1.));
					p.fillPath(
						RoundedRectPath(block.mediaRect, style.radius),
						overlay);
				}
			}
		});
	PaintMediaCaption(p, block, st, context);
}

void PaintDetailsBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	const auto visible = context.clip.intersected(block.outer);
	if (visible.isEmpty()) {
		return;
	}

	const auto &details = st.details;
	const auto &paintSt = PaintStyle(context, st);
	const auto &paintDetails = paintSt.details;
	const auto headerBg = TableHeaderBg(paintSt.table);
	const auto half = details.border / 2.;
	const auto outer = QRectF(block.outer).marginsRemoved({
		half,
		half,
		half,
		half,
	});
	auto outerPath = QPainterPath();
	outerPath.addRoundedRect(outer, details.radius, details.radius);

	p.save();
	p.setClipRect(visible);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.fillPath(
			outerPath,
			paintDetails.bodyBg->c);
		p.save();
		p.setClipPath(outerPath, Qt::IntersectClip);
		p.fillRect(block.headerRect, TableHeaderBg(paintSt.table));
		auto pen = QPen(
			WithOpacity(
				paintSt.table.borderFg,
				paintSt.table.headerBgOpacity * 3),
			details.border);
		if (!block.bodyRect.isEmpty()) {
			p.setPen(pen);
			const auto separatorY = block.bodyRect.top() + half;
			p.drawLine(
				QPointF(outer.left(), separatorY),
				QPointF(outer.right(), separatorY));
		}
		p.restore();

		p.setBrush(Qt::NoBrush);
		p.setPen(pen);
		p.drawPath(outerPath);
	}
	if (!block.iconRect.isEmpty()) {
		paintDetails.icon.paint(
			p,
			block.iconRect.x(),
			block.iconRect.y(),
			outerWidth);
	}
	p.setPen(paintDetails.summaryFg->c);
	PaintTextLeaf(
		p,
		block.leaf,
		context,
		block.textRect,
		block.textWidth,
		style::al_left,
		TextSelectionForSegmentIndex(
			context.selectionState,
			block.segmentIndex));
	p.restore();

	if (!block.bodyRect.isEmpty()) {
		const auto bodyContext = ClippedContext(
			context,
			context.clip.intersected(block.bodyRect));
		PaintBlocks(
			p,
			block.children,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			st,
			bodyContext);
	}
}

void PaintThinkingBlock(
		Painter &p,
		const LaidOutBlock &block,
		int devicePixelRatio,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	const auto &paintSt = PaintStyle(context, st);
	const auto baseColor = paintSt.supplementaryTextColor;
	const auto selection = TextSelectionForSegmentIndex(
		context.selectionState,
		block.segmentIndex);
	const auto logicalRect = block.outer;
	const auto visible = context.clip.intersected(logicalRect);
	if (visible.isEmpty()) {
		AdvanceRevealLinesForBlock(context, block, st);
		return;
	}

	const auto thinking = context.caches.thinking;
	const auto pathShiftGradient = context.caches.pathShiftGradient;
	if (!thinking || !pathShiftGradient) {
		PaintThinkingTextLeafDirect(p, block, st, context);
		return;
	}
	if (selection && !selection->empty()) {
		PaintThinkingTextLeafDirect(p, block, st, context);
		return;
	}

	const auto ratio = std::max(devicePixelRatio, 1);
	const auto cacheWidth = logicalRect.width() * ratio;
	const auto cacheHeight = logicalRect.height() * ratio;
	EnsureThinkingPaintCacheImage(
		&thinking->mask,
		logicalRect.size(),
		ratio);
	EnsureThinkingPaintCacheImage(
		&thinking->gradient,
		logicalRect.size(),
		ratio);
	thinking->mask.fill(Qt::transparent);

	{
		auto maskPainter = Painter(&thinking->mask);
		maskPainter.setFont(p.font());
		maskPainter.setPen(p.pen());
		maskPainter.setBrush(p.brush());
		maskPainter.setRenderHints(p.renderHints());
		maskPainter.setInactive(p.inactive());
		maskPainter.setTextPalette(p.textPalette());
		maskPainter.translate(-logicalRect.topLeft());
		maskPainter.setClipRect(logicalRect);
		maskPainter.setPen(baseColor->c);
		PaintTextLeaf(
			maskPainter,
			block.leaf,
			ClippedContext(context, logicalRect),
			block.textRect,
			block.textWidth,
			style::al_left,
			TextSelection());
	}

	auto highlight = std::optional<style::owned_color>();
	highlight.emplace(anim::color(baseColor->c, paintSt.textColor->c, 0.45));
	pathShiftGradient->overrideColors(baseColor, highlight->color());
	const auto localRect = QRect(QPoint(), logicalRect.size());
	const auto painted = pathShiftGradient->paint(
		[&](const Ui::PathShiftGradient::Background &background) {
			FillThinkingGradientImage(
				p,
				&thinking->gradient,
				logicalRect,
				background);
			auto gradientPainter = Painter(&thinking->gradient);
			gradientPainter.setCompositionMode(
				QPainter::CompositionMode_DestinationIn);
			gradientPainter.drawImage(
				QRectF(localRect),
				thinking->mask,
				QRectF(0, 0, cacheWidth, cacheHeight));
			gradientPainter.setCompositionMode(
				QPainter::CompositionMode_SourceOver);
			return true;
		});
	pathShiftGradient->clearOverridenColors();
	if (!painted) {
		return;
	}

	p.save();
	p.setClipRect(visible, Qt::IntersectClip);
	p.drawImage(
		QRectF(logicalRect),
		thinking->gradient,
		QRectF(0, 0, cacheWidth, cacheHeight));
	p.restore();
}

void PaintBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	if (!block.outer.intersects(context.clip)
		&& block.kind != PreparedBlockKind::Thinking) {
		return;
	}
	const auto &paintSt = PaintStyle(context, st);

	switch (block.kind) {
	case PreparedBlockKind::Thinking:
		PaintThinkingBlock(p, block, devicePixelRatio, st, context);
		break;
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		if (!block.headerRect.isEmpty()) {
			p.fillRect(block.headerRect, paintSt.relatedArticle.headerBg->c);
		}
		SetTextLeafPen(p, block, st, context);
		PaintTextLeaf(
			p,
			block.leaf,
			context,
			block.textRect,
			block.textWidth,
			style::al_left,
			TextSelectionForSegmentIndex(
				context.selectionState,
				block.segmentIndex));
		break;
	case PreparedBlockKind::CodeBlock:
		p.setPen(paintSt.textColor->c);
		PaintTextLeaf(
			p,
			block.leaf,
			context,
			block.textRect,
			block.textWidth,
			style::al_left,
			TextSelectionForSegmentIndex(
				context.selectionState,
				block.segmentIndex));
		break;
	case PreparedBlockKind::Rule:
		PaintRevealBand(
			p,
			context,
			block.outer,
			[&](Painter &p, const MarkdownArticlePaintContext &) {
				p.fillRect(block.outer, paintSt.rule.fg->c);
			});
		break;
	case PreparedBlockKind::List:
		PaintBlocks(
			p,
			block.children,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			st,
			context);
		break;
	case PreparedBlockKind::ListItem:
		if (!context.reveal
			|| context.reveal->activeLine >= context.reveal->nextLine) {
			const auto markerContext = RevealSuppressedContext(context);
			if (block.taskState != TaskState::None) {
				PaintTaskMarker(p, block, st, markerContext, outerWidth);
			} else if (block.listKind == ListKind::Ordered
				&& !block.markerRect.isEmpty()) {
				p.setPen(paintSt.textColor->c);
				PaintTextLeaf(
					p,
					block.marker,
					markerContext,
					block.markerRect,
					block.markerWidth);
			} else if (block.listKind == ListKind::Bullet) {
				PaintBulletMarker(p, block, st, markerContext);
			}
		}
		PaintBlocks(
			p,
			block.children,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			st,
			context);
		break;
	case PreparedBlockKind::Quote:
		PaintQuoteBlock(
			p,
			block,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			st,
			context);
		break;
	case PreparedBlockKind::DisplayMath:
		PaintDisplayMathBlock(
			p,
			block,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			st,
			context);
		break;
	case PreparedBlockKind::Table:
		PaintTableBlock(p, block, st, context);
		break;
	case PreparedBlockKind::Photo:
		PaintPhotoBlock(p, block, st, context);
		break;
	case PreparedBlockKind::Video:
		PaintVideoBlock(p, block, st, context);
		break;
	case PreparedBlockKind::Audio:
		PaintAudioBlock(p, block, st, context);
		break;
	case PreparedBlockKind::Map:
		PaintMapBlock(p, block, st, context);
		break;
	case PreparedBlockKind::Channel:
		PaintChannelBlock(p, block, st, context);
		break;
	case PreparedBlockKind::RelatedArticle:
		PaintRelatedArticleBlock(p, block, st, context);
		break;
	case PreparedBlockKind::EmbedPost:
		PaintEmbedPostBlock(
			p,
			block,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			st,
			context);
		break;
	case PreparedBlockKind::Placeholder:
		PaintPlaceholderBlock(p, block, outerWidth, st, context);
		break;
	case PreparedBlockKind::GroupedMedia:
		PaintGroupedMediaBlock(p, block, st, context);
		break;
	case PreparedBlockKind::Details:
		PaintDetailsBlock(
			p,
			block,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			st,
			context);
		break;
	}
}

} // namespace

void PaintBlocks(
		Painter &p,
		const std::vector<LaidOutBlock> &blocks,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const style::Markdown &st,
		const MarkdownArticlePaintContext &context) {
	for (const auto &block : blocks) {
		if (block.outer.bottom() < context.clip.top()) {
			AdvanceRevealLinesForBlock(context, block, st);
			continue;
		} else if (block.outer.top() > context.clip.bottom()) {
			break;
		}
		PaintBlock(
			p,
			block,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			st,
			context);
	}
}

} // namespace Iv::Markdown
