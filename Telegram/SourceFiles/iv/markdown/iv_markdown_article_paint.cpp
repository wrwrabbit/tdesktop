#include "iv/markdown/iv_markdown_article_paint.h"

#include "iv/markdown/iv_markdown_article_text.h"

#include "ui/dynamic_image.h"
#include "ui/widgets/checkbox.h"

#include <algorithm>
#include <cmath>
#include <QtGui/QPainterPath>

#include "styles/palette.h"
#include "styles/style_iv.h"
#include "styles/style_widgets.h"

namespace Iv::Markdown {
namespace {

[[nodiscard]] bool PaintDynamicImage(
		Painter &p,
		const std::shared_ptr<Ui::DynamicImage> &image,
		QRect rect) {
	if (!image || rect.isEmpty()) {
		return false;
	}
	if (const auto frame = image->image(std::max(rect.width(), rect.height()));
		!frame.isNull()) {
		p.drawImage(rect, frame);
		return true;
	}
	return false;
}

void SubscribeDynamicImage(
		const std::shared_ptr<Ui::DynamicImage> &image,
		const Fn<void()> &repaint,
		bool *subscribed) {
	if (!image || !repaint || *subscribed) {
		return;
	}
	*subscribed = true;
	image->subscribeToUpdates(repaint);
}

void PaintTextLeaf(
		Painter &p,
		const Ui::Text::String &leaf,
		const MarkdownArticlePaintCaches &caches,
		QRect rect,
		int width,
		QRect clip,
		style::align align = style::al_left,
		std::optional<TextSelection> selection = std::nullopt) {
	const auto availableWidth = std::max(width, 1);
	leaf.draw(p, {
		.position = rect.topLeft(),
		.availableWidth = availableWidth,
		.geometry = TextGeometry(availableWidth),
		.align = align,
		.clip = clip,
		.palette = &p.textPalette(),
		.pre = caches.pre,
		.blockquote = caches.blockquote,
		.colors = caches.colors,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = crl::now(),
		.selection = selection.value_or(TextSelection()),
	});
}

void PaintTaskMarker(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &markdown,
		int outerWidth) {
	const auto rect = block.markerRect;
	if (rect.isEmpty()) {
		return;
	}
	auto view = Ui::CheckView(
		markdown.list.taskCheck,
		block.taskState == TaskState::Checked);
	view.finishAnimating();
	view.paint(p, rect.left(), rect.top(), outerWidth);
}

void PaintBulletMarker(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &markdown) {
	const auto radius = markdown.list.bulletRadius;
	if (radius <= 0) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(markdown.list.bulletFg->c);
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

void PaintTableBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &markdown,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	const auto tableClip = clip.intersected(block.visibleTableRect);
	if (tableClip.isEmpty()) {
		return;
	}

	p.save();
	p.setClipRect(tableClip);

	const auto &table = st::defaultTable;
	const auto half = table.border / 2.;
	const auto inner = QRectF(block.tableRect).marginsRemoved(
		{ half, half, half, half });
	const auto radius = table.radius;
	auto outerPath = QPainterPath();
	outerPath.addRoundedRect(inner, radius, radius);

	auto headerBottom = 0;
	for (const auto &row : block.tableRows) {
		if (!row.header) {
			break;
		}
		headerBottom = row.outer.y() + row.outer.height();
	}
	if (headerBottom > 0) {
		auto hq = PainterHighQualityEnabler(p);
		p.setClipRect(
			QRect(
				block.tableRect.x(),
				block.tableRows.front().outer.y(),
				block.tableRect.width(),
				headerBottom - block.tableRows.front().outer.y()),
			Qt::IntersectClip);
		p.setBrush(table.headerBg);
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(inner, radius, radius);
		p.setClipping(false);
		p.setClipRect(tableClip);
	}

	if (table.border > 0 && !block.tableRect.isEmpty()) {
		auto path = outerPath;
		for (auto i = 1, count = int(block.tableRows.size()); i != count; ++i) {
			const auto y = block.tableRows[i].outer.y() - half;
			path.moveTo(inner.x(), y);
			path.lineTo(inner.x() + inner.width(), y);
		}

		auto separatorLeft = block.tableRect.x() + table.border;
		for (auto i = 0, count = int(block.tableColumnWidths.size()); i + 1 < count; ++i) {
			separatorLeft += block.tableColumnWidths[i];
			const auto x = separatorLeft + half;
			path.moveTo(x, inner.y());
			path.lineTo(x, inner.y() + inner.height());
			separatorLeft += table.border;
		}

		auto hq = PainterHighQualityEnabler(p);
		auto pen = table.borderFg->p;
		pen.setWidth(table.border);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
	}

	p.setPen(markdown.textColor->c);
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
				caches,
				cell.textRect,
				cell.textWidth,
				tableClip,
				cell.align,
				TextSelectionForSegmentIndex(
					selectionState,
					cell.segmentIndex));
		}
	}

	if (block.segmentIndex >= 0
		&& WholeSegmentSelected(selectionState, block.segmentIndex)) {
		p.save();
		p.setClipPath(outerPath, Qt::IntersectClip);
		p.fillRect(block.tableRect, p.textPalette().selectOverlay);
		p.restore();
	}

	if (block.overflowed) {
		const auto indicatorWidth = std::min(
			std::max(markdown.table.overflowWidth, 1),
			block.visibleTableRect.width());
		p.save();
		p.setClipPath(outerPath, Qt::IntersectClip);
		p.fillRect(
			QRect(
				block.visibleTableRect.x()
					+ block.visibleTableRect.width()
					- indicatorWidth,
				block.visibleTableRect.y(),
				indicatorWidth,
				block.visibleTableRect.height()),
			markdown.table.overflowFg->c);
		p.restore();
	}

	p.restore();
}

void PaintDisplayMathBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		const style::Markdown &markdown,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	const auto formulaClip = clip.intersected(block.visibleFormulaRect);
	if (formulaClip.isEmpty()) {
		return;
	}

	p.save();
	p.setClipRect(formulaClip);

	const auto formula = PreparedFormulaFor(formulas, block.formulaIndex);
	p.setPen(markdown.textColor->c);
	const auto rendered = EnsureFormulaRendered(
		formula,
		FormulaRasterSlot(renderedFormulas, block.formulaIndex),
		renderer,
		devicePixelRatio);
	if (rendered.success) {
		p.drawImage(
			block.formulaRect.topLeft(),
			ColorizedDisplayFormulaImage(
				block,
				rendered,
				p.pen().color()));
	}
	if (!rendered.success) {
		const auto radius = markdown.displayMath.fallbackRadius;
		p.setPen(Qt::NoPen);
		p.setBrush(markdown.displayMath.fallbackBg->c);
		if (radius > 0) {
			auto hq = PainterHighQualityEnabler(p);
			p.drawRoundedRect(block.formulaRect, radius, radius);
		} else {
			p.fillRect(block.formulaRect, markdown.displayMath.fallbackBg->c);
		}
		p.setPen(markdown.textColor->c);
		PaintTextLeaf(
			p,
			block.fallbackLeaf,
			caches,
			block.textRect,
			block.textWidth,
			formulaClip);
	}

	if (block.segmentIndex >= 0
		&& WholeSegmentSelected(selectionState, block.segmentIndex)) {
		p.fillRect(block.visibleFormulaRect, p.textPalette().selectOverlay);
	}

	if (block.overflowed) {
		const auto indicatorWidth = std::min(
			std::max(markdown.displayMath.overflowWidth, 1),
			block.visibleFormulaRect.width());
		p.fillRect(
			QRect(
				block.visibleFormulaRect.x()
					+ block.visibleFormulaRect.width()
					- indicatorWidth,
				block.visibleFormulaRect.y(),
				indicatorWidth,
				block.visibleFormulaRect.height()),
			markdown.displayMath.overflowFg->c);
	}

	p.restore();
}

void PaintQuoteBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const style::Markdown &markdown,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	const auto quoteClip = clip.intersected(block.outer);
	if (quoteClip.isEmpty()) {
		return;
	}

	if (caches.blockquote) {
		const auto &quoteStyle = markdown.body.blockquote;
		Ui::Text::ValidateQuotePaintCache(*caches.blockquote, quoteStyle);

		p.save();
		p.setClipRect(quoteClip);
		Ui::Text::FillQuotePaint(
			p,
			block.outer,
			*caches.blockquote,
			quoteStyle);
		p.restore();
	}

	PaintBlocks(
		p,
		block.children,
		formulas,
		renderedFormulas,
		renderer,
		devicePixelRatio,
		outerWidth,
		markdown,
		caches,
		selectionState,
		clip.intersected(block.contentRect));
}

void PaintPlaceholderBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &markdown,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	const auto visible = clip.intersected(block.visibleMediaRect);
	if (!visible.isEmpty()) {
		p.save();
		p.setClipRect(visible);
		auto hq = PainterHighQualityEnabler(p);
		const auto max = block.labelLeaf.maxWidth();
		const auto radius = markdown.placeholder.padding.left();
		p.setBrush(st::windowBgOver);
		p.setPen(Qt::NoPen);
		const auto skip = (max < block.labelRect.width())
			? ((block.labelRect.width() - max) / 2)
			: 0;
		p.drawRoundedRect(
			block.labelRect.marginsRemoved(
				{ skip, 0, skip, 0 }
			).marginsAdded(markdown.placeholder.padding),
			radius,
			radius);
		p.setPen(st::windowSubTextFg->c);
		PaintTextLeaf(
			p,
			block.labelLeaf,
			caches,
			block.labelRect,
			block.labelWidth,
			visible,
			style::al_center);
		if (block.segmentIndex >= 0
			&& WholeSegmentSelected(selectionState, block.segmentIndex)) {
			p.fillRect(block.visibleMediaRect, p.textPalette().selectOverlay);
		}
		p.restore();
	}
	if (!block.textRect.isEmpty()) {
		p.setPen(markdown.textColor->c);
		PaintTextLeaf(
			p,
			block.leaf,
			caches,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.secondarySegmentIndex));
	}
}

void PaintPhotoProgress(
		Painter &p,
		QRect rect,
		const style::MarkdownPhoto &style,
		double progress) {
	const auto size = std::min({
		style.progressSize,
		rect.width(),
		rect.height(),
	});
	if (size <= 0) {
		return;
	}
	const auto thickness = std::max(style.progressWidth, 1);
	const auto ring = QRect(
		rect.center().x() - (size / 2),
		rect.center().y() - (size / 2),
		size,
		size);
	auto hq = PainterHighQualityEnabler(p);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(0, 0, 0, 96), thickness));
	p.drawEllipse(ring);
	p.setPen(QPen(st::windowFg->c, thickness));
	p.drawArc(
		ring,
		90 * 16,
		-int(std::round(360. * 16. * std::clamp(progress, 0., 1.))));
}

void PaintPhotoBlock(
		Painter &p,
		const LaidOutBlock &block,
		const style::Markdown &markdown,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	const auto visible = clip.intersected(block.visibleMediaRect);
	if (!visible.isEmpty()) {
		p.save();
		p.setClipRect(visible);
		p.fillRect(block.mediaRect, st::windowBgOver->c);
		SubscribeDynamicImage(
			block.thumbnailImage,
			caches.repaint,
			&block.thumbnailSubscribed);
		SubscribeDynamicImage(
			block.fullImage,
			caches.repaint,
			&block.fullSubscribed);
		const auto paintedThumb = PaintDynamicImage(
			p,
			block.thumbnailImage,
			block.mediaRect);
		const auto paintedFull = PaintDynamicImage(
			p,
			block.fullImage,
			block.mediaRect);
		if (!paintedThumb && !paintedFull) {
			p.setPen(st::windowSubTextFg->c);
			p.drawText(
				block.mediaRect,
				Qt::AlignCenter | Qt::TextWordWrap,
				block.copyText);
		}
		if (block.photoRuntime && block.photoRuntime->loading()) {
			PaintPhotoProgress(
				p,
				block.mediaRect,
				markdown.photo,
				block.photoRuntime->progress());
		}
		if (block.segmentIndex >= 0
			&& WholeSegmentSelected(selectionState, block.segmentIndex)) {
			p.fillRect(block.visibleMediaRect, p.textPalette().selectOverlay);
		}
		p.restore();
	}
	if (!block.textRect.isEmpty()) {
		p.setPen(markdown.textColor->c);
		PaintTextLeaf(
			p,
			block.leaf,
			caches,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.secondarySegmentIndex));
	}
}

void PaintDetailsBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const style::Markdown &markdown,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	const auto visible = clip.intersected(block.outer);
	if (visible.isEmpty()) {
		return;
	}

	const auto &details = markdown.details;
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
			(block.bodyRect.isEmpty() ? details.headerBg : details.bodyBg)->c);
		p.save();
		p.setClipPath(outerPath, Qt::IntersectClip);
		p.fillRect(block.headerRect, details.headerBg->c);
		if (!block.bodyRect.isEmpty()) {
			p.setPen(QPen(details.borderFg->c, details.border));
			const auto separatorY = block.bodyRect.top() + half;
			p.drawLine(
				QPointF(outer.left(), separatorY),
				QPointF(outer.right(), separatorY));
		}
		p.restore();

		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(details.borderFg->c, details.border));
		p.drawPath(outerPath);
	}
	if (!block.iconRect.isEmpty()) {
		details.icon.paint(
			p,
			block.iconRect.x(),
			block.iconRect.y(),
			outerWidth);
	}
	p.setPen(details.summaryFg->c);
	PaintTextLeaf(
		p,
		block.leaf,
		caches,
		block.textRect,
		block.textWidth,
		clip,
		style::al_left,
		TextSelectionForSegmentIndex(
			selectionState,
			block.segmentIndex));
	p.restore();

	if (!block.bodyRect.isEmpty()) {
		PaintBlocks(
			p,
			block.children,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			markdown,
			caches,
			selectionState,
			clip.intersected(block.bodyRect));
	}
}

void PaintBlock(
		Painter &p,
		const LaidOutBlock &block,
		std::vector<PreparedFormulaSlot> *formulas,
		std::vector<RenderedFormula> *renderedFormulas,
		MathRenderer *renderer,
		int devicePixelRatio,
		int outerWidth,
		const style::Markdown &markdown,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	if (!block.outer.intersects(clip)) {
		return;
	}

	switch (block.kind) {
	case PreparedBlockKind::Paragraph:
	case PreparedBlockKind::Heading:
		p.setPen(markdown.textColor->c);
		PaintTextLeaf(
			p,
			block.leaf,
			caches,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.segmentIndex));
		break;
	case PreparedBlockKind::CodeBlock:
		p.setPen(markdown.textColor->c);
		PaintTextLeaf(
			p,
			block.leaf,
			caches,
			block.textRect,
			block.textWidth,
			clip,
			style::al_left,
			TextSelectionForSegmentIndex(
				selectionState,
				block.segmentIndex));
		break;
	case PreparedBlockKind::Rule:
		p.fillRect(block.outer, st::defaultTable.borderFg->c);
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
			markdown,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::ListItem:
		if (block.taskState != TaskState::None) {
			PaintTaskMarker(p, block, markdown, outerWidth);
		} else if (block.listKind == ListKind::Ordered
			&& !block.markerRect.isEmpty()) {
			p.setPen(markdown.textColor->c);
			PaintTextLeaf(
				p,
				block.marker,
				caches,
				block.markerRect,
				block.markerWidth,
				clip);
		} else if (block.listKind == ListKind::Bullet) {
			PaintBulletMarker(p, block, markdown);
		}
		PaintBlocks(
			p,
			block.children,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			outerWidth,
			markdown,
			caches,
			selectionState,
			clip);
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
			markdown,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::DisplayMath:
		PaintDisplayMathBlock(
			p,
			block,
			formulas,
			renderedFormulas,
			renderer,
			devicePixelRatio,
			markdown,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::Table:
		PaintTableBlock(
			p,
			block,
			markdown,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::Photo:
		PaintPhotoBlock(
			p,
			block,
			markdown,
			caches,
			selectionState,
			clip);
		break;
	case PreparedBlockKind::Placeholder:
		PaintPlaceholderBlock(
			p,
			block,
			markdown,
			caches,
			selectionState,
			clip);
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
			markdown,
			caches,
			selectionState,
			clip);
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
		const style::Markdown &markdown,
		const MarkdownArticlePaintCaches &caches,
		const PaintSelectionState &selectionState,
		QRect clip) {
	for (const auto &block : blocks) {
		if (block.outer.bottom() < clip.top()) {
			continue;
		} else if (block.outer.top() > clip.bottom()) {
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
			markdown,
			caches,
			selectionState,
			clip);
	}
}

} // namespace Iv::Markdown
