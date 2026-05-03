#include "iv/markdown/iv_markdown_view.h"

#include "iv/markdown/iv_markdown_article.h"
#include "iv/iv_delegate.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QDebug>
#include <QtGui/QClipboard>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>

#include "base/weak_ptr.h"
#include "core/credits_amount.h"
#include "core/click_handler_types.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "ui/chat/chat_style.h"
#include "ui/click_handler.h"
#include "ui/integration.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_scale.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "styles/style_iv.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

namespace Iv::Markdown {
namespace {

[[nodiscard]] QString PrepareTerminalFailureName(
		PrepareTerminalFailure failure) {
	switch (failure) {
	case PrepareTerminalFailure::None:
		return u"none"_q;
	case PrepareTerminalFailure::InvalidRequest:
		return u"invalid-request"_q;
	case PrepareTerminalFailure::InvalidStyle:
		return u"invalid-style"_q;
	case PrepareTerminalFailure::DocumentTooLarge:
		return u"document-too-large"_q;
	case PrepareTerminalFailure::InternalError:
		return u"internal-error"_q;
	}
	return u"unknown"_q;
}

[[nodiscard]] QString PrepareFailureReasonText(
		const PrepareFailureStatus &failure) {
	return !failure.debugReason.isEmpty()
		? failure.debugReason
		: PrepareTerminalFailureName(failure.terminal);
}

void EnsureBlockquotePaintCache(
		std::unique_ptr<Ui::Text::QuotePaintCache> &cache,
		const style::color &color) {
	if (cache) {
		return;
	}
	cache = std::make_unique<Ui::Text::QuotePaintCache>();
	cache->bg = color->c;
	cache->bg.setAlpha(Ui::kDefaultBgOpacity * 255);
	cache->outlines[0] = color->c;
	cache->outlines[0].setAlpha(Ui::kDefaultOutline1Opacity * 255);
	cache->outlines[1] = cache->outlines[2] = QColor(0, 0, 0, 0);
	cache->header = color->c;
	cache->header.setAlpha(Ui::kDefaultOutline2Opacity * 255);
	cache->icon = color->c;
	cache->icon.setAlpha(Ui::kDefaultOutline3Opacity * 255);
}

void EnsurePrePaintCache(
		std::unique_ptr<Ui::Text::QuotePaintCache> &cache,
		const style::MarkdownQuotePaintColors &colors) {
	if (cache) {
		return;
	}
	cache = std::make_unique<Ui::Text::QuotePaintCache>();
	cache->bg = colors.preBg->c;
	cache->outlines[0] = colors.pre->c;
	cache->outlines[0].setAlpha(Ui::kDefaultOutline1Opacity * 255);
	cache->outlines[1] = cache->outlines[2] = QColor(0, 0, 0, 0);
	cache->header = colors.pre->c;
	cache->header.setAlpha(Ui::kDefaultOutline2Opacity * 255);
	cache->icon = colors.pre->c;
	cache->icon.setAlpha(Ui::kDefaultOutline3Opacity * 255);
}

[[nodiscard]] int CompareSelectionPositions(
		MarkdownArticleSelectionPosition a,
		MarkdownArticleSelectionPosition b) {
	if (a.segment != b.segment) {
		return (a.segment < b.segment) ? -1 : 1;
	}
	if (a.offset != b.offset) {
		return (a.offset < b.offset) ? -1 : 1;
	}
	return 0;
}

[[nodiscard]] MarkdownArticleSelection NormalizeSelection(
		MarkdownArticleSelection selection) {
	if (selection.empty()) {
		return {};
	}
	if (CompareSelectionPositions(selection.from, selection.to) > 0) {
		std::swap(selection.from, selection.to);
	}
	return selection;
}

[[nodiscard]] MarkdownArticleSelectionEndpoint MakeSelectionEndpoint(
		const MarkdownArticleHitTestResult &result) {
	return {
		.segment = result.segmentIndex,
		.direct = result.direct,
	};
}

class MarkdownDocumentWidget final
	: public Ui::RpWidget
	, public ClickHandlerHost {
public:
	explicit MarkdownDocumentWidget(QWidget *parent);

	void setLinkActivationCallback(
		std::function<void(const PreparedLink &, Qt::MouseButton)> callback);
	void setArticle(std::shared_ptr<MarkdownArticle> article);
	void setZoom(int value);
	void refreshPalette();
	void invalidateRasterCache();
	[[nodiscard]] int anchorTop(const QString &anchorId) const;
	[[nodiscard]] bool toggleDetails(const QString &anchorId);
	[[nodiscard]] int lastRelayoutMs() const;
	int resizeGetHeight(int newWidth) override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void clickHandlerActiveChanged(const ClickHandlerPtr &, bool) override;
	void clickHandlerPressedChanged(const ClickHandlerPtr &, bool) override;

private:
	enum DragAction {
		NoDrag = 0x00,
		PrepareDrag = 0x01,
		Dragging = 0x02,
		Selecting = 0x04,
	};

	[[nodiscard]] ClickHandlerPtr linkAt(QPoint point) const;
	[[nodiscard]] MarkdownArticleHitTestResult hitTest(
		QPoint point,
		Ui::Text::StateRequest::Flags flags) const;
	[[nodiscard]] MarkdownArticleSelection selectionForCopy() const;
	[[nodiscard]] MarkdownArticleSelectionEndpoints selectionEndpointsForCopy() const;
	[[nodiscard]] bool selectionContains(
		MarkdownArticleSelection selection,
		const MarkdownArticleHitTestResult &result) const;
	[[nodiscard]] int selectionOffsetFromHit(
		const MarkdownArticleHitTestResult &result) const;
	[[nodiscard]] MarkdownArticleSelection selectionFromHit(
		const MarkdownArticleHitTestResult &result) const;
	[[nodiscard]] TextForMimeData getSelectedText() const;
	void copySelectedText();

	void relayoutCurrentWidth(bool clearSelection);
	void forceRelayoutCurrentWidth();
	void updateHover(const MarkdownArticleHitTestResult &state);
	void resetSelection();
	void clearSelection();
	void resetTextPaintCaches();
	[[nodiscard]] Ui::Text::QuotePaintCache *ensurePrePaintCache();
	[[nodiscard]] Ui::Text::QuotePaintCache *ensureBlockquotePaintCache();
	[[nodiscard]] MarkdownArticlePaintCaches textPaintCaches();
	void dragActionStart(QPoint point, Qt::MouseButton button);
	MarkdownArticleHitTestResult dragActionUpdate(QPoint point);
	MarkdownArticleHitTestResult dragActionFinish(
		QPoint point,
		Qt::MouseButton button);
	void applyCursor(style::cursor cursor);
	[[nodiscard]] double zoomScale() const;

	std::shared_ptr<MarkdownArticle> _article;
	std::unique_ptr<Ui::Text::QuotePaintCache> _prePaintCache;
	std::unique_ptr<Ui::Text::QuotePaintCache> _blockquotePaintCache;
	std::function<void(const PreparedLink &, Qt::MouseButton)> _activateLink;
	MarkdownArticleSelection _selection;
	MarkdownArticleSelection _savedSelection;
	MarkdownArticleSelectionEndpoints _selectionEndpoints;
	MarkdownArticleSelectionEndpoints _savedSelectionEndpoints;
	TextSelectType _selectionType = TextSelectType::Letters;
	style::cursor _cursor = style::cur_default;
	DragAction _dragAction = NoDrag;
	QPoint _dragStartPosition;
	int _dragSegment = -1;
	int _dragSymbol = 0;
	TextSelection _dragExpandedSelection;
	int _lastRelayoutMs = 0;
	int _zoom = 100;
	base::unique_qptr<Ui::PopupMenu> _contextMenu;

};

MarkdownDocumentWidget::MarkdownDocumentWidget(
	QWidget *parent)
: Ui::RpWidget(parent) {
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
}

void MarkdownDocumentWidget::setLinkActivationCallback(
		std::function<void(const PreparedLink &, Qt::MouseButton)> callback) {
	_activateLink = std::move(callback);
}

void MarkdownDocumentWidget::setArticle(std::shared_ptr<MarkdownArticle> article) {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	_article = std::move(article);
	_lastRelayoutMs = 0;
	resetTextPaintCaches();
	resetSelection();
	forceRelayoutCurrentWidth();
}

void MarkdownDocumentWidget::setZoom(int value) {
	value = (value > 0) ? value : 100;
	if (_zoom == value) {
		return;
	}
	_zoom = value;
	clearSelection();
	forceRelayoutCurrentWidth();
}

void MarkdownDocumentWidget::refreshPalette() {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	resetTextPaintCaches();
	if (_article) {
		_article->invalidatePaletteCache();
	}
	update();
}

void MarkdownDocumentWidget::invalidateRasterCache() {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	if (_article) {
		_article->invalidateRasterCache();
	}
	relayoutCurrentWidth(false);
	update();
}

int MarkdownDocumentWidget::anchorTop(const QString &anchorId) const {
	const auto top = _article ? _article->anchorTop(anchorId) : -1;
	if (top < 0) {
		return -1;
	}
	return int(std::floor(top * zoomScale()));
}

bool MarkdownDocumentWidget::toggleDetails(const QString &anchorId) {
	if (!_article || !_article->toggleDetails(anchorId)) {
		return false;
	}
	clearSelection();
	forceRelayoutCurrentWidth();
	return true;
}

int MarkdownDocumentWidget::lastRelayoutMs() const {
	return _lastRelayoutMs;
}

int MarkdownDocumentWidget::resizeGetHeight(int newWidth) {
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	clearSelection();
	if (!_article) {
		return 1;
	}
	const auto scale = zoomScale();
	const auto layoutWidth = std::max(int(std::floor(newWidth / scale)), 1);
	auto timer = QElapsedTimer();
	timer.start();
	const auto layoutHeight = _article->resizeGetHeight(layoutWidth);
	_lastRelayoutMs = int(timer.elapsed());
	return std::max(int(std::ceil(layoutHeight * scale)), 1);
}

void MarkdownDocumentWidget::paintEvent(QPaintEvent *e) {
	if (!_article) {
		return;
	}
	auto p = Painter(this);
	p.setTextPalette(st::defaultMarkdown.textPalette);
	const auto caches = textPaintCaches();
	const auto scale = zoomScale();
	if (scale == 1.) {
		_article->paint(
			p,
			e->rect(),
			caches,
			_selection,
			&_selectionEndpoints);
		return;
	}
	const auto clip = QRect(
		int(std::floor(e->rect().x() / scale)),
		int(std::floor(e->rect().y() / scale)),
		int(std::ceil(e->rect().width() / scale)) + 1,
		int(std::ceil(e->rect().height() / scale)) + 1);
	p.save();
	p.scale(scale, scale);
	_article->paint(
		p,
		clip,
		caches,
		_selection,
		&_selectionEndpoints);
	p.restore();
}

void MarkdownDocumentWidget::keyPressEvent(QKeyEvent *e) {
	if (e == QKeySequence::Copy && !selectionForCopy().empty()) {
		copySelectedText();
		return;
	}
	Ui::RpWidget::keyPressEvent(e);
}

void MarkdownDocumentWidget::contextMenuEvent(QContextMenuEvent *e) {
	const auto globalPoint = (e->reason() == QContextMenuEvent::Mouse)
		? e->globalPos()
		: QCursor::pos();
	const auto localPoint = (e->reason() == QContextMenuEvent::Mouse)
		? e->pos()
		: mapFromGlobal(globalPoint);
	const auto state = hitTest(
		localPoint,
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol);
	const auto selection = selectionForCopy();
	const auto uponSelection = !selection.empty()
		&& ((e->reason() != QContextMenuEvent::Mouse)
			|| selectionContains(selection, state));
	const auto contextText = uponSelection
		? TextForMimeData()
		: (_article ? _article->textForContext(state) : TextForMimeData());
	const auto link = state.preparedLink;

	_contextMenu = base::make_unique_q<Ui::PopupMenu>(this);
	if (uponSelection) {
		_contextMenu->addAction(
			Ui::Integration::Instance().phraseContextCopySelected(),
			[=] { copySelectedText(); },
			&st::menuIconCopy);
	} else if (!contextText.empty()) {
		_contextMenu->addAction(
			tr::lng_context_copy_text(tr::now),
			[text = contextText] {
				TextUtilities::SetClipboardText(text);
			},
			&st::menuIconCopy);
	}

	if (link) {
		const auto copyText = [&] {
			if (!link->copyText.isEmpty()) {
				return link->copyText;
			}
			switch (link->kind) {
			case PreparedLinkKind::Anchor:
			case PreparedLinkKind::Footnote:
			case PreparedLinkKind::FootnoteBacklink:
				return link->target.isEmpty() ? QString() : (u"#"_q + link->target);
			case PreparedLinkKind::LocalFile:
				return link->fragment.isEmpty()
					? link->target
					: (link->target + u"#"_q + link->fragment);
			case PreparedLinkKind::External:
				return link->target;
			case PreparedLinkKind::RejectedRelative:
			case PreparedLinkKind::ToggleDetails:
				return QString();
			}
			return QString();
		}();
		if (!copyText.isEmpty()
			&& link->kind != PreparedLinkKind::RejectedRelative
			&& link->kind != PreparedLinkKind::ToggleDetails) {
			_contextMenu->addAction(
				tr::lng_context_copy_link(tr::now),
				[text = copyText] {
					QGuiApplication::clipboard()->setText(text);
				},
				&st::menuIconCopy);
		}
		switch (link->kind) {
		case PreparedLinkKind::RejectedRelative:
		case PreparedLinkKind::ToggleDetails:
			break;
		case PreparedLinkKind::External:
		case PreparedLinkKind::Anchor:
		case PreparedLinkKind::Footnote:
		case PreparedLinkKind::FootnoteBacklink:
		case PreparedLinkKind::LocalFile:
			_contextMenu->addAction(
				tr::lng_open_link(tr::now),
				[=, prepared = *link] {
					if (_activateLink) {
						_activateLink(prepared, Qt::LeftButton);
					}
				},
				&st::menuIconAddress);
			break;
		}
	}

	if (_contextMenu->empty()) {
		_contextMenu = nullptr;
		return;
	}
	_contextMenu->popup(globalPoint);
	e->accept();
}

void MarkdownDocumentWidget::mouseMoveEvent(QMouseEvent *e) {
	dragActionUpdate(e->pos());
}

void MarkdownDocumentWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		dragActionStart(e->pos(), e->button());
		return;
	}
	updateHover(hitTest(
		e->pos(),
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol));
	if (e->button() == Qt::MiddleButton) {
		ClickHandler::pressed();
	}
}

void MarkdownDocumentWidget::mouseReleaseEvent(QMouseEvent *e) {
	dragActionFinish(e->pos(), e->button());
	if (!rect().contains(e->pos())) {
		ClickHandler::clearActive(this);
		applyCursor(style::cur_default);
	}
}

void MarkdownDocumentWidget::mouseDoubleClickEvent(QMouseEvent *e) {
	dragActionStart(e->pos(), e->button());
	if (_dragAction != Selecting || _selectionType != TextSelectType::Letters) {
		return;
	}
	const auto state = hitTest(
		e->pos(),
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol);
	if (!_article
		|| !_article->segmentIsText(state.segmentIndex)
		|| !state.direct
		|| !state.state.uponSymbol) {
		return;
	}
	_dragSegment = state.segmentIndex;
	_dragSymbol = selectionOffsetFromHit(state);
	_selectionType = TextSelectType::Words;
	_selection = selectionFromHit(state);
	_savedSelection = {};
	_selectionEndpoints = {
		.from = MakeSelectionEndpoint(state),
		.to = MakeSelectionEndpoint(state),
	};
	_savedSelectionEndpoints = {};
	if (_selection.from.segment == _dragSegment
		&& _selection.to.segment == _dragSegment) {
		_dragExpandedSelection = TextSelection(
			uint16(_selection.from.offset),
			uint16(_selection.to.offset));
	}
	setFocus();
	updateHover(state);
	update();
}

void MarkdownDocumentWidget::focusOutEvent(QFocusEvent *e) {
	if (!_selection.empty()) {
		_savedSelection = _selection;
		_savedSelectionEndpoints = _selectionEndpoints;
		_selection = {};
		_selectionEndpoints = {};
		update();
	}
	ClickHandler::clearActive(this);
	applyCursor(style::cur_default);
	Ui::RpWidget::focusOutEvent(e);
}

void MarkdownDocumentWidget::focusInEvent(QFocusEvent *e) {
	if (!_savedSelection.empty()) {
		_selection = _savedSelection;
		_selectionEndpoints = _savedSelectionEndpoints;
		_savedSelection = {};
		_savedSelectionEndpoints = {};
		update();
	}
	Ui::RpWidget::focusInEvent(e);
}

void MarkdownDocumentWidget::leaveEventHook(QEvent *e) {
	ClickHandler::clearActive(this);
	applyCursor((_dragAction == Selecting)
		? style::cur_text
		: style::cur_default);
	Ui::RpWidget::leaveEventHook(e);
}

void MarkdownDocumentWidget::clickHandlerActiveChanged(
		const ClickHandlerPtr &,
		bool) {
	update();
}

void MarkdownDocumentWidget::clickHandlerPressedChanged(
		const ClickHandlerPtr &,
		bool) {
	update();
}

ClickHandlerPtr MarkdownDocumentWidget::linkAt(QPoint point) const {
	return hitTest(
		point,
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol).state.link;
}

MarkdownArticleHitTestResult MarkdownDocumentWidget::hitTest(
		QPoint point,
		Ui::Text::StateRequest::Flags flags) const {
	if (!_article) {
		return {};
	}
	const auto scale = zoomScale();
	if (scale != 1.) {
		point = QPoint(
			int(std::floor(point.x() / scale)),
			int(std::floor(point.y() / scale)));
	}
	return _article->hitTest(point, flags);
}

MarkdownArticleSelection MarkdownDocumentWidget::selectionForCopy() const {
	return !_selection.empty()
		? _selection
		: _contextMenu
		? _savedSelection
		: MarkdownArticleSelection();
}

MarkdownArticleSelectionEndpoints MarkdownDocumentWidget::selectionEndpointsForCopy() const {
	return !_selection.empty()
		? _selectionEndpoints
		: _contextMenu
		? _savedSelectionEndpoints
		: MarkdownArticleSelectionEndpoints();
}

bool MarkdownDocumentWidget::selectionContains(
		MarkdownArticleSelection selection,
		const MarkdownArticleHitTestResult &result) const {
	const auto endpoints = selectionEndpointsForCopy();
	return _article
		? _article->selectionContains(
			selection,
			&endpoints,
			result)
		: false;
}

int MarkdownDocumentWidget::selectionOffsetFromHit(
		const MarkdownArticleHitTestResult &result) const {
	return _article
		? _article->selectionOffsetFromHit(result, _selectionType)
		: 0;
}

MarkdownArticleSelection MarkdownDocumentWidget::selectionFromHit(
		const MarkdownArticleHitTestResult &result) const {
	if (!_article || _dragSegment < 0 || !result.valid()) {
		return {};
	}
	auto first = _dragSymbol;
	auto second = selectionOffsetFromHit(result);
	if (_selectionType != TextSelectType::Letters
		&& !_dragExpandedSelection.empty()
		&& result.segmentIndex != _dragSegment) {
		first = (CompareSelectionPositions(
			MarkdownArticleSelectionPosition{ result.segmentIndex, second },
			MarkdownArticleSelectionPosition{ _dragSegment, _dragSymbol }) < 0)
			? _dragExpandedSelection.to
			: _dragExpandedSelection.from;
	}
	if (result.segmentIndex == _dragSegment
		&& _article->segmentIsText(_dragSegment)) {
		const auto adjusted = _article->adjustSelection(
			_dragSegment,
			TextSelection(
				uint16(std::min(first, second)),
				uint16(std::max(first, second))),
			_selectionType);
		return {
			{ _dragSegment, adjusted.from },
			{ _dragSegment, adjusted.to },
		};
	}
	return NormalizeSelection({
		{ _dragSegment, first },
		{ result.segmentIndex, second },
	});
}

TextForMimeData MarkdownDocumentWidget::getSelectedText() const {
	const auto endpoints = selectionEndpointsForCopy();
	return _article
		? _article->textForSelection(
			selectionForCopy(),
			&endpoints)
		: TextForMimeData();
}

void MarkdownDocumentWidget::copySelectedText() {
	if (const auto text = getSelectedText(); !text.empty()) {
		TextUtilities::SetClipboardText(text);
	}
}

void MarkdownDocumentWidget::relayoutCurrentWidth(bool clearSelection) {
	if (clearSelection) {
		this->clearSelection();
	}
	if (!_article) {
		_lastRelayoutMs = 0;
		return;
	}
	const auto scale = zoomScale();
	const auto layoutWidth = std::max(int(std::floor(width() / scale)), 1);
	auto timer = QElapsedTimer();
	timer.start();
	const auto articleHeight = _article->resizeGetHeight(layoutWidth);
	(void)articleHeight;
	_lastRelayoutMs = int(timer.elapsed());
}

void MarkdownDocumentWidget::forceRelayoutCurrentWidth() {
	resizeToWidth(width());
	update();
}

void MarkdownDocumentWidget::updateHover(
		const MarkdownArticleHitTestResult &state) {
	const auto changed = ClickHandler::setActive(state.state.link, this);
	auto cursor = style::cur_default;
	if (_dragAction == NoDrag) {
		if (state.state.link) {
			cursor = style::cur_pointer;
		} else if (state.direct) {
			cursor = style::cur_text;
		}
	} else {
		if (_dragAction == Selecting) {
			const auto selection = selectionFromHit(state);
			const auto endpoints = MarkdownArticleSelectionEndpoints{
				.from = _selectionEndpoints.from.valid()
					? _selectionEndpoints.from
					: MarkdownArticleSelectionEndpoint{ _dragSegment, false },
				.to = MakeSelectionEndpoint(state),
			};
			const auto endpointsChanged
				= (_selectionEndpoints.from.segment != endpoints.from.segment)
				|| (_selectionEndpoints.from.direct != endpoints.from.direct)
				|| (_selectionEndpoints.to.segment != endpoints.to.segment)
				|| (_selectionEndpoints.to.direct != endpoints.to.direct);
			if (_selection != selection || endpointsChanged) {
				_selection = selection;
				_selectionEndpoints = endpoints;
				_savedSelection = {};
				_savedSelectionEndpoints = {};
				setFocus();
				update();
			} else {
				_selectionEndpoints = endpoints;
			}
			cursor = style::cur_text;
		} else if (ClickHandler::getPressed()) {
			cursor = style::cur_pointer;
		}
	}
	if (changed || cursor != _cursor) {
		applyCursor(cursor);
	}
}

void MarkdownDocumentWidget::resetSelection() {
	_selection = {};
	_savedSelection = {};
	_selectionEndpoints = {};
	_savedSelectionEndpoints = {};
	_selectionType = TextSelectType::Letters;
	_dragAction = NoDrag;
	_dragStartPosition = QPoint();
	_dragSegment = -1;
	_dragSymbol = 0;
	_dragExpandedSelection = {};
}

void MarkdownDocumentWidget::clearSelection() {
	const auto hadSelection = !_selection.empty()
		|| !_savedSelection.empty()
		|| (_dragAction != NoDrag);
	resetSelection();
	if (hadSelection) {
		update();
	}
}

void MarkdownDocumentWidget::resetTextPaintCaches() {
	_prePaintCache = nullptr;
	_blockquotePaintCache = nullptr;
}

Ui::Text::QuotePaintCache *MarkdownDocumentWidget::ensurePrePaintCache() {
	EnsurePrePaintCache(_prePaintCache, st::defaultMarkdown.quotePaintColors);
	return _prePaintCache.get();
}

Ui::Text::QuotePaintCache *MarkdownDocumentWidget::ensureBlockquotePaintCache() {
	EnsureBlockquotePaintCache(
		_blockquotePaintCache,
		st::defaultMarkdown.quotePaintColors.blockquote);
	return _blockquotePaintCache.get();
}

MarkdownArticlePaintCaches MarkdownDocumentWidget::textPaintCaches() {
	return {
		.pre = ensurePrePaintCache(),
		.blockquote = ensureBlockquotePaintCache(),
	};
}

void MarkdownDocumentWidget::dragActionStart(
		QPoint point,
		Qt::MouseButton button) {
	const auto state = hitTest(
		point,
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol);
	updateHover(state);
	if (button != Qt::LeftButton) {
		return;
	}
	ClickHandler::pressed();
	_dragAction = NoDrag;
	_dragExpandedSelection = {};
	_dragSegment = -1;
	_dragSymbol = 0;
	if (ClickHandler::getPressed()) {
		_dragStartPosition = point;
		_dragAction = PrepareDrag;
		return;
	}
	if (!state.valid()) {
		clearSelection();
		return;
	}
	_dragSegment = state.segmentIndex;
	_dragSymbol = selectionOffsetFromHit(state);
	_selection = {
		{ _dragSegment, _dragSymbol },
		{ _dragSegment, _dragSymbol },
	};
	_savedSelection = {};
	_selectionEndpoints = {
		.from = MakeSelectionEndpoint(state),
		.to = MakeSelectionEndpoint(state),
	};
	_savedSelectionEndpoints = {};
	_dragAction = Selecting;
	update();
}

MarkdownArticleHitTestResult MarkdownDocumentWidget::dragActionUpdate(QPoint point) {
	const auto state = hitTest(
		point,
		Ui::Text::StateRequest::Flag::LookupLink
			| Ui::Text::StateRequest::Flag::LookupSymbol);
	if (_dragAction == PrepareDrag
		&& (point - _dragStartPosition).manhattanLength()
			>= QApplication::startDragDistance()) {
		_dragAction = Dragging;
	}
	updateHover(state);
	return state;
}

MarkdownArticleHitTestResult MarkdownDocumentWidget::dragActionFinish(
		QPoint point,
		Qt::MouseButton button) {
	const auto state = dragActionUpdate(point);
	auto activated = ClickHandler::unpressed();
	if (_dragAction == Dragging
		|| (_dragAction == Selecting && !_selection.empty())) {
		activated = nullptr;
	} else if (_dragAction == PrepareDrag && button != Qt::RightButton) {
		clearSelection();
	}
	_dragAction = NoDrag;
	_selectionType = TextSelectType::Letters;
	_dragExpandedSelection = {};
	updateHover(state);
	if (activated
		&& (button == Qt::LeftButton || button == Qt::MiddleButton)) {
		if (state.preparedLink && _activateLink) {
			_activateLink(*state.preparedLink, button);
		} else {
			ActivateClickHandler(window(), activated, button);
		}
	}
	if (QGuiApplication::clipboard()->supportsSelection()
		&& !_selection.empty()) {
		if (const auto text = getSelectedText(); !text.empty()) {
			TextUtilities::SetClipboardText(text, QClipboard::Selection);
		}
	}
	return state;
}

void MarkdownDocumentWidget::applyCursor(style::cursor cursor) {
	if (_cursor != cursor) {
		_cursor = cursor;
		setCursor(_cursor);
	}
}

double MarkdownDocumentWidget::zoomScale() const {
	return std::max(_zoom, 1) / 100.;
}

class MarkdownPreviewRoot final : public Ui::RpWidget {
public:
	MarkdownPreviewRoot(
		QWidget *parent,
		const PreparedDocument &document,
		Fn<void(Event)> callback,
		const OpenOptions &options);

private:
	void prepareArticle();
	void activateLink(const PreparedLink &link, Qt::MouseButton button);
	void applyPreparedContent(MarkdownArticleContent prepared, int prepareMs);
	[[nodiscard]] bool scrollToAnchor(const QString &anchorId);
	void updateChildrenGeometry(QSize size);
	void updateFailureGeometry();
	void logPreparationSummary(
		const PrepareFailureStatus &failure,
		const PrepareDebugStats &debug,
		int prepareMs,
		int layoutMs) const;

	const OpenOptions _options;
	const std::shared_ptr<const PreparedDocument> _document;
	const Fn<void(Event)> _callback;
	Ui::ScrollArea *_scroll = nullptr;
	MarkdownDocumentWidget *_body = nullptr;
	Ui::FlatLabel *_failure = nullptr;
	Ui::LinkButton *_failureOpen = nullptr;
	std::shared_ptr<MathRenderer> _renderer;
	QString _pendingFragment;
	int _devicePixelRatio = 0;

};

MarkdownPreviewRoot::MarkdownPreviewRoot(
	QWidget *parent,
	const PreparedDocument &document,
	Fn<void(Event)> callback,
	const OpenOptions &options)
: Ui::RpWidget(parent)
, _options(options)
, _document(std::make_shared<PreparedDocument>(document))
, _callback(std::move(callback))
, _renderer(std::make_shared<MathRenderer>())
, _pendingFragment(options.initialFragment) {
	_scroll = Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll);
	_body = _scroll->setOwnedWidget(object_ptr<MarkdownDocumentWidget>(_scroll));
	_failure = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_markdown_preview_cant(tr::now),
		st::defaultMarkdown.failure.label);
	_failureOpen = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_markdown_preview_open_file(tr::now));

	_scroll->hide();
	if (_body) {
		_body->hide();
		_body->setLinkActivationCallback([=](
				const PreparedLink &link,
				Qt::MouseButton button) {
			activateLink(link, button);
		});
		if (_options.delegate) {
			_body->setZoom(_options.delegate->ivZoom());
		}
	}
	_failure->hide();
	_failureOpen->hide();
	_failureOpen->setClickedCallback([=] {
		if (!_options.sourcePath.isEmpty()) {
			File::Launch(_options.sourcePath);
		}
	});

	sizeValue() | rpl::on_next([=](QSize size) {
		updateChildrenGeometry(size);
	}, lifetime());

	style::PaletteChanged() | rpl::on_next([=] {
		if (_body && !_body->isHidden()) {
			_body->refreshPalette();
		}
	}, lifetime());

	screenValue() | rpl::on_next([=](not_null<QScreen*>) {
		const auto devicePixelRatio = style::DevicePixelRatio();
		if (devicePixelRatio == _devicePixelRatio) {
			return;
		}
		_devicePixelRatio = devicePixelRatio;
		if (_body && !_body->isHidden()) {
			_body->invalidateRasterCache();
		}
	}, lifetime());

	if (_options.delegate) {
		_options.delegate->ivZoomValue(
		) | rpl::on_next([=](int value) {
			if (_body) {
				_body->setZoom(value);
			}
		}, lifetime());
	}

	_devicePixelRatio = style::DevicePixelRatio();
	prepareArticle();
}

void MarkdownPreviewRoot::prepareArticle() {
	if (_renderer) {
		_renderer->resetDebugCounters();
	}
	auto timer = QElapsedTimer();
	timer.start();
	auto prepared = PrepareSynchronously({
		.document = _document,
		.renderer = _renderer,
		.dimensions = CaptureMarkdownPrepareDimensions(),
		.sourcePath = _options.sourcePath,
	});
	applyPreparedContent(std::move(prepared), int(timer.elapsed()));
}

void MarkdownPreviewRoot::activateLink(
		const PreparedLink &link,
		Qt::MouseButton button) {
	if (button != Qt::LeftButton && button != Qt::MiddleButton) {
		return;
	}
	switch (link.kind) {
	case PreparedLinkKind::External:
		HiddenUrlClickHandler::Open(link.target);
		break;
	case PreparedLinkKind::Anchor:
	case PreparedLinkKind::Footnote:
	case PreparedLinkKind::FootnoteBacklink:
		if (!scrollToAnchor(link.target)) {
			DEBUG_LOG(("Native Markdown IV: unresolved anchor: %1").arg(
				link.target));
		}
		break;
	case PreparedLinkKind::LocalFile: {
	} break;
	case PreparedLinkKind::RejectedRelative:
		DEBUG_LOG(("Native Markdown IV: "
			"rejected relative markdown link: %1").arg(
			link.target));
		break;
	case PreparedLinkKind::ToggleDetails:
		if (_body && !_body->toggleDetails(link.target)) {
			DEBUG_LOG(("Native Markdown IV: failed details toggle: %1").arg(
				link.target));
		}
		break;
	}
}

void MarkdownPreviewRoot::applyPreparedContent(
		MarkdownArticleContent prepared,
		int prepareMs) {
	const auto failure = prepared.failure;
	const auto debug = prepared.debug;
	if (failure.failed()) {
		_scroll->hide();
		if (_body) {
			_body->hide();
		}
		_failure->show();
		if (_options.sourcePath.isEmpty()) {
			_failureOpen->hide();
		} else {
			_failureOpen->show();
		}
		_failure->raise();
		_failureOpen->raise();
		updateFailureGeometry();
		logPreparationSummary(failure, debug, prepareMs, 0);
		return;
	}

	if (!_body) {
		logPreparationSummary(failure, debug, prepareMs, 0);
		return;
	}

	auto article = std::make_shared<MarkdownArticle>(_renderer);
	article->setContent(std::move(prepared));
	updateChildrenGeometry(size());
	_body->setArticle(std::move(article));
	if (_options.delegate) {
		_body->setZoom(_options.delegate->ivZoom());
	}
	_scroll->show();
	_body->show();
	_failure->hide();
	_failureOpen->hide();
	if (!_pendingFragment.isEmpty()) {
		const auto scrolled = scrollToAnchor(_pendingFragment);
		static_cast<void>(scrolled);
		_pendingFragment.clear();
	}
	logPreparationSummary(
		failure,
		debug,
		prepareMs,
		_body->lastRelayoutMs());
}

bool MarkdownPreviewRoot::scrollToAnchor(const QString &anchorId) {
	if (!_body || !_scroll || anchorId.isEmpty()) {
		return false;
	}
	const auto top = _body->anchorTop(anchorId);
	if (top < 0) {
		return false;
	}
	_scroll->scrollToY(top, top + 1);
	return true;
}

void MarkdownPreviewRoot::updateChildrenGeometry(QSize size) {
	_scroll->setGeometry(QRect(QPoint(), size));
	if (_body) {
		_body->resizeToWidth(_scroll->width());
	}
	updateFailureGeometry();
}

void MarkdownPreviewRoot::updateFailureGeometry() {
	const auto availableWidth = std::max(width(), 1);
	const auto failureWidth = std::min(
		availableWidth,
		st::defaultMarkdown.failure.width);
	_failure->resizeToWidth(failureWidth);
	_failureOpen->resizeToNaturalWidth(failureWidth);
	const auto openVisible = !_failureOpen->isHidden();
	const auto totalHeight = _failure->height()
		+ (openVisible
			? (st::defaultMarkdown.failure.skip + _failureOpen->height())
			: 0);
	const auto top = std::max((height() - totalHeight) / 2, 0);
	_failure->moveToLeft(
		(availableWidth - failureWidth) / 2,
		top,
		availableWidth);
	if (openVisible) {
		_failureOpen->moveToLeft(
			(availableWidth - _failureOpen->width()) / 2,
			top + _failure->height() + st::defaultMarkdown.failure.skip,
			availableWidth);
	}
}

void MarkdownPreviewRoot::logPreparationSummary(
		const PrepareFailureStatus &failure,
		const PrepareDebugStats &debug,
		int prepareMs,
		int layoutMs) const {
#ifndef NDEBUG
	const auto counters = _renderer ? _renderer->debugCounters() : FormulaDebugCounters();
	const auto reason = PrepareFailureReasonText(failure);
	DEBUG_LOG((
		failure.failed()
			? "Native Markdown IV: unexpected preview prepare failure (%1 ms prepare, %2 ms layout, %3 ms formulas, cache hits=%4 misses=%5 bytes=%6, terminal=%7): %8"
			: "Native Markdown IV: preview prepare success (%1 ms prepare, %2 ms layout, %3 ms formulas, cache hits=%4 misses=%5 bytes=%6, terminal=%7): %8"
		).arg(prepareMs
		).arg(layoutMs
		).arg(debug.formulaMeasureMs
		).arg(counters.hits
		).arg(counters.misses
		).arg(qlonglong(counters.cacheBytes)
		).arg(reason
		).arg(_options.sourcePath));
#else
	Q_UNUSED(failure);
	Q_UNUSED(debug);
	Q_UNUSED(prepareMs);
	Q_UNUSED(layoutMs);
#endif
}

} // namespace

std::unique_ptr<Ui::RpWidget> CreateMarkdownPreviewWidget(
		QWidget *parent,
		const PreparedDocument &document,
		Fn<void(Event)> callback,
		const OpenOptions &options) {
	return std::make_unique<MarkdownPreviewRoot>(
		parent,
		document,
		std::move(callback),
		options);
}

} // namespace Iv::Markdown
