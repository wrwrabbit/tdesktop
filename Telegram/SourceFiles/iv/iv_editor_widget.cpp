/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/iv_editor_widget.h"

#include "data/data_msg_id.h"
#include "ui/image/image_location.h"
#include "data/data_types.h"
#include "chat_helpers/message_field.h"
#include "data/stickers/data_custom_emoji.h"
#include "iv/markdown/iv_markdown_prepare_native_richtext.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/color_contrast.h"
#include "ui/painter.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/fields/input_field.h"
#include "styles/palette.h"
#include "styles/style_chat.h"
#include "styles/style_iv.h"

#include <QtCore/QDate>
#include <QtCore/QPointer>
#include <QtGui/QFocusEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>

#include "window/window_session_controller.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Iv::Editor {
namespace {

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

[[nodiscard]] bool UseDarkPrePaintBackground() {
	const auto withBg = [](const QColor &color) {
		return Ui::CountContrast(st::windowBg->c, color);
	};
	return withBg({ 0, 0, 0 }) < withBg({ 255, 255, 255 });
}

void EnsurePrePaintCache(
		std::unique_ptr<Ui::Text::QuotePaintCache> &cache,
		const style::color &color) {
	if (cache) {
		return;
	}
	cache = std::make_unique<Ui::Text::QuotePaintCache>();
	if (UseDarkPrePaintBackground()) {
		cache->bg = QColor(0, 0, 0, 192);
	} else {
		cache->bg = color->c;
		cache->bg.setAlpha(Ui::kDefaultBgOpacity * 255);
	}
	cache->outlines[0] = color->c;
	cache->outlines[0].setAlpha(Ui::kDefaultOutline1Opacity * 255);
	cache->outlines[1] = cache->outlines[2] = QColor(0, 0, 0, 0);
	cache->header = color->c;
	cache->header.setAlpha(Ui::kDefaultOutline2Opacity * 255);
	cache->icon = color->c;
	cache->icon.setAlpha(Ui::kDefaultOutline3Opacity * 255);
}

[[nodiscard]] std::vector<Ui::Text::SpecialColor> HighlightColors(
		not_null<const Ui::ChatStyle*> style) {
	auto result = Ui::SyntaxHighlightColors(style);

	const auto &fg = style->lightButtonFg();
	const auto &bg = style->lightButtonBgOver();
	result.push_back({ &fg->p, &fg->p, &bg->b, &bg->b });

	Ensures(result.size() == Markdown::kNativeIvLinkSpecialColorIndex);
	return result;
}

[[nodiscard]] std::unique_ptr<Ui::ChatTheme> CreateStandaloneChatTheme() {
	const auto palette = style::main_palette::get();
	return std::make_unique<Ui::ChatTheme>(Ui::ChatThemeDescriptor{
		.preparePalette = [=](style::palette &copy) {
			copy = *palette;
		},
		.backgroundData = {
			.colors = { palette->windowBg()->c },
		},
	});
}

[[nodiscard]] const style::margins &EditorBodyPadding() {
	return st::ivEditorBodyPadding;
}

[[nodiscard]] int CompareSelectionPositions(
		Markdown::MarkdownArticleSelectionPosition a,
		Markdown::MarkdownArticleSelectionPosition b) {
	if (a.segment != b.segment) {
		return (a.segment < b.segment) ? -1 : 1;
	}
	if (a.offset != b.offset) {
		return (a.offset < b.offset) ? -1 : 1;
	}
	return 0;
}

[[nodiscard]] Markdown::MarkdownArticleSelection NormalizeSelection(
		Markdown::MarkdownArticleSelection selection) {
	if (selection.empty()) {
		return {};
	}
	if (CompareSelectionPositions(selection.from, selection.to) > 0) {
		std::swap(selection.from, selection.to);
	}
	return selection;
}

[[nodiscard]] Markdown::MarkdownArticleSelectionEndpoint MakeSelectionEndpoint(
		const Markdown::MarkdownArticleHitTestResult &hit) {
	return {
		.segment = hit.segmentIndex,
		.direct = hit.direct,
	};
}

[[nodiscard]] int FieldNaturalHeight(not_null<Ui::InputField*> field) {
	const auto margins = field->fullTextMargins();
	return std::max(
		int(std::ceil(field->document()->size().height()))
			+ margins.top()
			+ margins.bottom(),
		1);
}

} // namespace

Widget::Widget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	std::shared_ptr<State> state)
: Ui::RpWidget(parent)
, _controller(controller)
, _peer(peer)
, _state(std::move(state))
, _article(std::make_shared<Markdown::MarkdownArticle>(st::defaultMarkdown))
, _field(base::make_unique_q<Ui::InputField>(
	this,
	st::ivEditorInputField,
	Ui::InputField::Mode::MultiLine,
	rpl::single(QString())))
, _theme(CreateStandaloneChatTheme())
, _style(std::make_unique<Ui::ChatStyle>(style::main_palette::get())) {
	_style->apply(_theme.get());
	_highlightColors = HighlightColors(_style.get());

	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);

	const auto weak = QPointer<Widget>(this);
	_article->setTextRepaintCallbacks(
		[=] {
			if (weak) {
				weak->update();
			}
		},
		[=](QRect rect) {
			if (!weak) {
				return;
			} else if (rect.isEmpty()) {
				weak->update();
			} else {
				weak->update(rect.translated(weak->articleTopLeft()));
			}
		});

	const auto allowPremiumEmoji = [peer](
			not_null<DocumentData*> emoji) {
		return Data::AllowEmojiWithoutPremium(peer, emoji);
	};
	InitMessageFieldHandlers({
		.session = &_controller->session(),
		.show = _controller->uiShow(),
		.field = _field.get(),
		.customEmojiPaused = [=] {
			return _controller->isGifPausedAtLeastFor(
				Window::GifPauseReason::Layer);
		},
		.allowPremiumEmoji = allowPremiumEmoji,
	});
	_field->setMimeDataHook(WrappedMessageFieldMimeHook(
		Ui::InputField::MimeDataHook(),
		_field.get()));
	_field->setSubmitSettings(Ui::InputField::SubmitSettings::None);
	_field->setMaxHeight(std::numeric_limits<int>::max());

	_field->heightChanges(
	) | rpl::on_next([=] {
		updateInlineFieldHeightOverride();
	}, _field->lifetime());
	_field->focusedChanges(
	) | rpl::on_next([=](bool focused) {
		if (!focused) {
			commitInlineField();
			refreshPreparedContent();
		}
	}, _field->lifetime());

	_field->hide();
	refreshPreparedContent();
}

void Widget::activateInitialNode() {
	const auto ordinal = (_activeOrdinal >= 0)
		? _activeOrdinal
		: _state->activeTextOrdinal();
	if (ordinal < 0) {
		const auto first = _article->firstTextSegmentIndex();
		const auto fallback = textOrdinalForSegment(first);
		if (fallback < 0) {
			return;
		}
		activateTextOrdinal(fallback, 0);
		return;
	}
	activateTextOrdinal(ordinal, 0);
}

void Widget::activateSegment(int segmentIndex, int cursorOffset) {
	const auto ordinal = textOrdinalForSegment(segmentIndex);
	if (ordinal < 0) {
		return;
	}
	activateTextOrdinal(ordinal, cursorOffset);
}

void Widget::activateSegmentSelection(
		int segmentIndex,
		TextSelection selection) {
	const auto ordinal = textOrdinalForSegment(segmentIndex);
	if (ordinal < 0) {
		return;
	}
	activateTextOrdinal(ordinal, selection.from, selection.to);
}

void Widget::commitInlineField() {
	applyFieldTextToState();
}

void Widget::acceptInlineField() {
	if (_field->isHidden()) {
		return;
	}
	commitInlineField();
	_pendingOrdinal = -1;
	_pendingCursorOffset = 0;
	_field->hide();
	_article->clearTextLeafHeightOverride();
	refreshPreparedContent();
}

void Widget::refreshPreparedContent() {
	setDocument(_state->prepared());
	const auto width = std::max(
		widthNoMargins(),
		parentWidget() ? parentWidget()->width() : 0);
	if (width > 0) {
		resizeToWidth(width);
	} else {
		update();
	}
}

void Widget::syncInlineFieldGeometry() {
	syncInlineFieldGeometry(widthNoMargins());
}

void Widget::insertHeading1() {
	commitInlineField();
	_state->insertHeading1AfterActive();
	refreshPreparedContent();
	activateTextOrdinal(_state->activeTextOrdinal(), 0);
}

void Widget::insertBlockquote() {
	commitInlineField();
	_state->insertBlockquoteAfterActive();
	refreshPreparedContent();
	activateTextOrdinal(_state->activeTextOrdinal(), 0);
}

int Widget::resizeGetHeight(int newWidth) {
	if (!_article) {
		return 1;
	}
	const auto width = std::max(newWidth, 1);
	const auto padding = EditorBodyPadding();
	_articleHeight = _article->resizeGetHeight(articleWidth(width));
	ensurePendingActivation();
	syncInlineFieldGeometry(width);
	const auto fieldBottom = (!_field->isHidden())
		? (_field->y() + _field->height())
		: 0;
	return std::max(
		std::max(
			_articleHeight + padding.top() + padding.bottom(),
			fieldBottom),
		st::ivEditorMinHeight);
}

void Widget::focusInEvent(QFocusEvent *e) {
	Ui::RpWidget::focusInEvent(e);
	if (!_field->isHidden()) {
		_field->setFocusFast();
	}
}

void Widget::mouseMoveEvent(QMouseEvent *e) {
	if (!_selectingText) {
		const auto hit = _article->hitTest(
			e->pos() - articleTopLeft(),
			Ui::Text::StateRequest::Flag::LookupSymbol);
		setCursor((hit.valid()
			&& hit.direct
			&& _article->segmentIsText(hit.segmentIndex))
			? style::cur_text
			: style::cur_default);
		Ui::RpWidget::mouseMoveEvent(e);
		return;
	}
	const auto hit = _article->hitTest(
		e->pos() - articleTopLeft(),
		Ui::Text::StateRequest::Flag::LookupSymbol);
	updateTextSelection(hit);
	e->accept();
}

void Widget::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		Ui::RpWidget::mousePressEvent(e);
		return;
	}
	auto articlePoint = e->pos() - articleTopLeft();
	auto hit = _article->hitTest(
		articlePoint,
		Ui::Text::StateRequest::Flag::LookupSymbol);
	if (hit.valid() && hit.direct && _article->segmentIsText(hit.segmentIndex)) {
		if (!_field->isHidden() && hit.segmentIndex != _activeSegmentIndex) {
			acceptInlineField();
			hit = _article->hitTest(
				articlePoint,
				Ui::Text::StateRequest::Flag::LookupSymbol);
		}
		if (hit.valid()
			&& hit.direct
			&& _article->segmentIsText(hit.segmentIndex)
			&& _field->isHidden()) {
			_dragSegment = hit.segmentIndex;
			_dragOffset = _article->selectionOffsetFromHit(
				hit,
				TextSelectType::Letters);
			_selection = {
				{ _dragSegment, _dragOffset },
				{ _dragSegment, _dragOffset },
			};
			_selectionEndpoints = {
				.from = MakeSelectionEndpoint(hit),
				.to = MakeSelectionEndpoint(hit),
			};
			_selectingText = true;
			update();
			e->accept();
			return;
		}
	}
	clearTextSelection();
	e->accept();
}

void Widget::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		Ui::RpWidget::mouseReleaseEvent(e);
		return;
	}
	const auto articlePoint = e->pos() - articleTopLeft();
	const auto hit = _article->hitTest(
		articlePoint,
		Ui::Text::StateRequest::Flag::LookupSymbol);
	if (_selectingText) {
		updateTextSelection(hit);
		const auto selection = _selection;
		const auto segment = _dragSegment;
		clearTextSelection();
		if (segment >= 0 && _article->segmentIsText(segment)) {
			activateSegmentSelection(segment, TextSelection(
				uint16(std::clamp(selection.from.offset, 0, 0xFFFF)),
				uint16(std::clamp(selection.to.offset, 0, 0xFFFF))));
		}
		e->accept();
		return;
	}
	if (hit.valid() && hit.direct && _article->segmentIsText(hit.segmentIndex)) {
		const auto offset = _article->selectionOffsetFromHit(
			hit,
			TextSelectType::Letters);
		if (hit.segmentIndex != _activeSegmentIndex) {
			commitInlineField();
			refreshPreparedContent();
			activateSegment(hit.segmentIndex, offset);
		} else if (!_field->isHidden()) {
			auto cursor = _field->textCursor();
			cursor.setPosition(std::clamp(
				offset,
				0,
				_field->getLastText().size()));
			_field->setTextCursor(cursor);
			_field->setFocusFast();
		}
	} else if (articlePoint.y() >= _articleHeight) {
		activateTrailingParagraph();
	} else {
		if (_field->isHidden()) {
			activateInitialNode();
		} else {
			_field->setFocusFast();
		}
	}
	e->accept();
}

void Widget::paintEvent(QPaintEvent *e) {
	if (!_article) {
		return;
	}
	auto p = Painter(this);
	p.setTextPalette(st::inTextPalette);
	const auto topLeft = articleTopLeft();
	p.translate(topLeft);
	_article->paint(
		p,
		textPaintContext(e->rect().translated(-topLeft.x(), -topLeft.y())));
}

void Widget::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	syncInlineFieldGeometry();
}

void Widget::setDocument(const Markdown::MarkdownArticleContent &prepared) {
	_article->setContent(prepared);
}

void Widget::activateTextOrdinal(int ordinal, int cursorOffset) {
	activateTextOrdinal(ordinal, cursorOffset, cursorOffset);
}

void Widget::activateTextOrdinal(
		int ordinal,
		int selectionFrom,
		int selectionTo) {
	if (!_state->setActiveTextByOrdinal(ordinal)) {
		return;
	}
	_activeOrdinal = ordinal;
	_pendingOrdinal = -1;
	_pendingCursorOffset = 0;

	const auto segmentIndex = segmentIndexForTextOrdinal(ordinal);
	if (segmentIndex < 0) {
		_activeSegmentIndex = -1;
		_pendingOrdinal = ordinal;
		_pendingCursorOffset = selectionTo;
		_field->hide();
		return;
	}

	_activeSegmentIndex = segmentIndex;
	const auto activeText = _state->activeText();
	_settingField = true;
	_field->setTextWithTags(
		{
			activeText.text,
			TextUtilities::ConvertEntitiesToTextTags(activeText.entities),
		},
		Ui::InputField::HistoryAction::Clear);
	auto cursor = _field->textCursor();
	const auto size = _field->getLastText().size();
	const auto from = std::clamp(selectionFrom, 0, size);
	const auto to = std::clamp(selectionTo, 0, size);
	cursor.setPosition(from);
	if (to != from) {
		cursor.setPosition(to, QTextCursor::KeepAnchor);
	}
	_field->setTextCursor(cursor);
	_settingField = false;
	_field->show();
	syncInlineFieldGeometry();
	updateInlineFieldHeightOverride();
	_field->raise();
	_field->setFocusFast();
}

void Widget::activateTrailingParagraph() {
	commitInlineField();
	const auto ordinal = _state->ensureTrailingParagraphActive();
	refreshPreparedContent();
	activateTextOrdinal(ordinal, _state->activeText().text.size());
}

void Widget::applyFieldTextToState() {
	if (_settingField || _field->isHidden()) {
		return;
	}
	const auto text = _field->getTextWithAppliedMarkdown();
	_state->applyActiveText({
		.text = text.text,
		.entities = TextUtilities::ConvertTextTagsToEntities(text.tags),
	});
}

void Widget::ensurePendingActivation() {
	if (_pendingOrdinal < 0) {
		_activeSegmentIndex = (_activeOrdinal >= 0)
			? segmentIndexForTextOrdinal(_activeOrdinal)
			: _article->firstTextSegmentIndex();
		return;
	}
	const auto ordinal = _pendingOrdinal;
	const auto cursorOffset = _pendingCursorOffset;
	_pendingOrdinal = -1;
	_pendingCursorOffset = 0;
	activateTextOrdinal(ordinal, cursorOffset);
}

void Widget::updateInlineFieldHeightOverride() {
	if (_settingField
		|| _field->isHidden()
		|| _activeOrdinal < 0
		|| !_article) {
		return;
	} else if (_syncingInlineFieldGeometry) {
		_pendingHeightOverrideUpdate = true;
		return;
	}
	const auto segmentRect = outerTextSegmentRect(_activeSegmentIndex);
	const auto height = segmentRect.isEmpty()
		? _field->height()
		: std::max(_field->geometry().bottom() + 1 - segmentRect.y(), 1);
	_article->setTextLeafHeightOverride(_activeOrdinal, height);
	resizeToWidth(std::max(widthNoMargins(), 1));
	update();
}

void Widget::syncInlineFieldGeometry(int width) {
	if (_field->isHidden() || width <= 0) {
		return;
	}
	const auto segmentRect = outerTextSegmentRect(_activeSegmentIndex);
	if (segmentRect.isEmpty()) {
		_pendingOrdinal = _activeOrdinal;
		_pendingCursorOffset = _field->textCursor().position();
		_field->hide();
		_article->clearTextLeafHeightOverride();
		return;
	}
	const auto fieldWidth = std::max(
		std::min(segmentRect.width(), width - segmentRect.x()),
		1);
	_syncingInlineFieldGeometry = true;
	_field->resizeToWidth(fieldWidth);
	const auto fieldHeight = FieldNaturalHeight(_field.get());
	const auto left = segmentRect.x();
	const auto top = segmentRect.y();
	_field->setGeometryToLeft(left, top, fieldWidth, fieldHeight, width);
	_field->raise();
	_syncingInlineFieldGeometry = false;
	if (_pendingHeightOverrideUpdate) {
		_pendingHeightOverrideUpdate = false;
		updateInlineFieldHeightOverride();
	}
}

void Widget::clearTextSelection() {
	const auto hadSelection = !_selection.empty() || _selectingText;
	_selection = {};
	_selectionEndpoints = {};
	_dragSegment = -1;
	_dragOffset = 0;
	_selectingText = false;
	if (hadSelection) {
		update();
	}
}

void Widget::updateTextSelection(
		const Markdown::MarkdownArticleHitTestResult &hit) {
	if (!_selectingText || _dragSegment < 0 || !hit.valid()) {
		return;
	}
	if (!hit.direct
		|| hit.segmentIndex != _dragSegment
		|| !_article->segmentIsText(hit.segmentIndex)) {
		return;
	}
	const auto offset = _article->selectionOffsetFromHit(
		hit,
		TextSelectType::Letters);
	const auto adjusted = _article->adjustSelection(
		_dragSegment,
		TextSelection(
			uint16(std::clamp(std::min(_dragOffset, offset), 0, 0xFFFF)),
			uint16(std::clamp(std::max(_dragOffset, offset), 0, 0xFFFF))),
		TextSelectType::Letters);
	const auto selection = NormalizeSelection({
		{ _dragSegment, adjusted.from },
		{ _dragSegment, adjusted.to },
	});
	const auto endpoints = Markdown::MarkdownArticleSelectionEndpoints{
		.from = _selectionEndpoints.from.valid()
			? _selectionEndpoints.from
			: Markdown::MarkdownArticleSelectionEndpoint{ _dragSegment, false },
		.to = MakeSelectionEndpoint(hit),
	};
	const auto endpointsChanged
		= (_selectionEndpoints.from.segment != endpoints.from.segment)
		|| (_selectionEndpoints.from.direct != endpoints.from.direct)
		|| (_selectionEndpoints.to.segment != endpoints.to.segment)
		|| (_selectionEndpoints.to.direct != endpoints.to.direct);
	if (_selection != selection || endpointsChanged) {
		_selection = selection;
		_selectionEndpoints = endpoints;
		update();
	} else {
		_selectionEndpoints = endpoints;
	}
}

int Widget::textOrdinalForSegment(int segmentIndex) const {
	return _article->textLeafIndexForSegment(segmentIndex);
}

int Widget::segmentIndexForTextOrdinal(int ordinal) const {
	return _article->segmentIndexForTextLeafIndex(ordinal);
}

QPoint Widget::articleTopLeft() const {
	const auto padding = EditorBodyPadding();
	return { padding.left(), padding.top() };
}

int Widget::articleWidth(int outerWidth) const {
	const auto padding = EditorBodyPadding();
	return std::max(
		outerWidth - padding.left() - padding.right(),
		1);
}

QRect Widget::outerTextSegmentRect(int segmentIndex) const {
	const auto rect = _article->textSegmentRect(segmentIndex);
	return rect.isEmpty() ? rect : rect.translated(articleTopLeft());
}

Markdown::MarkdownArticlePaintContext Widget::textPaintContext(QRect clip) {
	const auto logicalRect = QRect(QPoint(), QSize(
		articleWidth(std::max(widthNoMargins(), 1)),
		std::max(_articleHeight, 1)));
	auto context = Markdown::MarkdownArticlePaintContext(
		_theme->preparePaintContext(
			_style.get(),
			logicalRect,
			logicalRect,
			clip,
			window() ? !window()->isActiveWindow() : false));
	context.caches = {
		.pre = ensurePrePaintCache(),
		.blockquote = ensureBlockquotePaintCache(),
		.colors = _highlightColors,
		.repaint = [=] {
			crl::on_main(this, [=] {
				update();
			});
		},
		.repaintRect = [=](QRect rect) {
			crl::on_main(this, [=] {
				if (rect.isEmpty()) {
					update();
				} else {
					update(rect.translated(articleTopLeft()));
				}
			});
		},
	};
	context.hiddenTextSegmentIndex = _field->isHidden()
		? -1
		: _activeSegmentIndex;
	context.debugBlockGeometry = true;
	context.selectionState.selection = _selection;
	context.selectionState.endpoints = &_selectionEndpoints;
	return context;
}

Ui::Text::QuotePaintCache *Widget::ensureBlockquotePaintCache() {
	EnsureBlockquotePaintCache(
		_blockquotePaintCache,
		st::defaultMarkdown.quotePaintColors.blockquote);
	return _blockquotePaintCache.get();
}

Ui::Text::QuotePaintCache *Widget::ensurePrePaintCache() {
	EnsurePrePaintCache(_prePaintCache, st::inTextPalette.monoFg);
	return _prePaintCache.get();
}

} // namespace Iv::Editor
