/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/editor/iv_editor_widget.h"

#include "data/data_msg_id.h"
#include "ui/image/image_location.h"
#include "data/data_types.h"
#include "chat_helpers/message_field.h"
#include "data/stickers/data_custom_emoji.h"
#include "iv/markdown/iv_markdown_prepare_native_richtext.h"
#include "spellcheck/spellcheck_highlight_syntax.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/painter.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/fields/input_field.h"
#include "styles/palette.h"
#include "styles/style_chat.h"
#include "styles/style_iv.h"

#include <QtCore/QDate>
#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtGui/QFocusEvent>
#include <QtGui/QKeyEvent>
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

void EnableQTextEditLineMetrics(style::TextStyle &style) {
	style.qtextEditLineMetrics = true;
}

void EnableQTextEditLineMetrics(style::Markdown &style) {
	EnableQTextEditLineMetrics(style.body);
	EnableQTextEditLineMetrics(style.heading1);
	EnableQTextEditLineMetrics(style.heading2);
	EnableQTextEditLineMetrics(style.heading3);
	EnableQTextEditLineMetrics(style.heading4);
	EnableQTextEditLineMetrics(style.heading5);
	EnableQTextEditLineMetrics(style.heading6);
	EnableQTextEditLineMetrics(style.code);
	EnableQTextEditLineMetrics(style.displayMath.fallbackStyle);
	EnableQTextEditLineMetrics(style.table.headerStyle);
	EnableQTextEditLineMetrics(style.table.bodyStyle);
	EnableQTextEditLineMetrics(style.details.summaryStyle);
	EnableQTextEditLineMetrics(style.embedPost.authorStyle);
	EnableQTextEditLineMetrics(style.embedPost.dateStyle);
	EnableQTextEditLineMetrics(style.placeholder.labelStyle);
	EnableQTextEditLineMetrics(style.audio.titleStyle);
	EnableQTextEditLineMetrics(style.audio.subtitleStyle);
	EnableQTextEditLineMetrics(style.channel.titleStyle);
	EnableQTextEditLineMetrics(style.channel.subtitleStyle);
	EnableQTextEditLineMetrics(style.channel.button.textStyle);
	EnableQTextEditLineMetrics(style.relatedArticle.titleStyle);
	EnableQTextEditLineMetrics(style.relatedArticle.subtitleStyle);
	EnableQTextEditLineMetrics(style.relatedArticle.footerStyle);
}

[[nodiscard]] style::Markdown CreateEditorMarkdownStyle() {
	auto result = st::messageMarkdown;
	EnableQTextEditLineMetrics(result);
	return result;
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
, _articleStyle(std::make_shared<style::Markdown>(
	CreateEditorMarkdownStyle()))
, _article(std::make_shared<Markdown::MarkdownArticle>(*_articleStyle))
, _theme(CreateStandaloneChatTheme())
, _style(std::make_unique<Ui::ChatStyle>(style::main_palette::get())) {
	_style->apply(_theme.get());
	_highlightColors = HighlightColors(_style.get());

	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);

	Spellchecker::HighlightReady(
	) | rpl::on_next([=](Spellchecker::HighlightProcessId processId) {
		if (_article && _article->highlightProcessDone(processId)) {
			update();
		}
	}, _highlightReadyLifetime);

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

	const auto &fieldStyle = inlineFieldStyleFor(
		Markdown::MarkdownArticleTextLeafStyle());
	_activeFieldStyleKey = fieldStyle.key;
	_field = base::make_unique_q<Ui::InputField>(
		this,
		*fieldStyle.style,
		Ui::InputField::Mode::MultiLine,
		rpl::single(QString()));
	setupInlineField();
	refreshPreparedContent();
}

void Widget::activateInitialNode() {
	const auto ordinal = (_activeOrdinal >= 0)
		? _activeOrdinal
		: _state->activeTextOrdinal();
	if (ordinal < 0) {
		const auto first = _article->firstEditableSegmentIndex();
		const auto fallback = editableOrdinalForSegment(first);
		if (fallback < 0) {
			return;
		}
		activateTextOrdinal(fallback, 0);
		return;
	}
	activateTextOrdinal(ordinal, 0);
}

void Widget::activateSegment(int segmentIndex, int cursorOffset) {
	const auto ordinal = editableOrdinalForSegment(segmentIndex);
	if (ordinal < 0) {
		return;
	}
	activateTextOrdinal(ordinal, cursorOffset);
}

void Widget::commitInlineField() {
	applyFieldTextToState();
}

void Widget::hideInlineFieldAndRefresh() {
	if (_field->isHidden()) {
		return;
	}
	commitInlineField();
	_pendingOrdinal = -1;
	_pendingCursorOffset = 0;
	hideInlineField();
	_article->clearTextLeafHeightOverride();
	refreshPreparedContent();
}

void Widget::acceptInlineField() {
	hideInlineFieldAndRefresh();
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

void Widget::insertBlock(State::InsertAction action) {
	commitInlineField();
	_state->insertBlockAfterActive(action);
	refreshPreparedContent();
	activateTextOrdinal(_state->activeTextOrdinal(), 0);
}

void Widget::insertPreparedBlock(RichPage::Block block) {
	auto blocks = std::vector<RichPage::Block>();
	blocks.push_back(std::move(block));
	insertPreparedBlocks(std::move(blocks));
}

void Widget::insertPreparedBlocks(std::vector<RichPage::Block> blocks) {
	if (blocks.empty()) {
		return;
	}
	commitInlineField();
	_state->insertPreparedBlocksAfterActive(std::move(blocks));
	refreshPreparedContent();
	activateTextOrdinal(_state->activeTextOrdinal(), 0);
}

void Widget::insertHeading1() {
	insertBlock({
		.type = State::InsertBlockType::Heading,
		.headingLevel = 1,
	});
}

void Widget::insertBlockquote() {
	insertBlock({ .type = State::InsertBlockType::Blockquote });
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

bool Widget::eventFilter(QObject *object, QEvent *event) {
	if (_field
		&& object == _field->rawTextEdit().get()
		&& event->type() == QEvent::KeyPress
		&& handleFieldKey(static_cast<QKeyEvent*>(event))) {
		return true;
	}
	return Ui::RpWidget::eventFilter(object, event);
}

void Widget::focusInEvent(QFocusEvent *e) {
	Ui::RpWidget::focusInEvent(e);
	if (!_settingField && !_field->isHidden()) {
		_field->setFocusFast();
	}
}

void Widget::mouseMoveEvent(QMouseEvent *e) {
	if (!_selectingText) {
		const auto hit = _article->hitTest(
			e->pos() - articleTopLeft(),
			Ui::Text::StateRequest::Flag::LookupSymbol);
		auto cursor = style::cur_default;
		if (hit.valid() && hit.codeHeaderCopy) {
			cursor = style::cur_pointer;
		} else if (hit.valid()
			&& hit.direct
			&& _article->segmentIsText(hit.segmentIndex)) {
			cursor = style::cur_text;
		}
		setCursor(cursor);
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
	_trackingPointerPress = true;
	auto articlePoint = e->pos() - articleTopLeft();
	auto hit = _article->hitTest(
		articlePoint,
		Ui::Text::StateRequest::Flag::LookupSymbol);
	if (hit.codeHeaderCopy) {
		clearTextSelection();
		_selectingText = false;
		e->accept();
		return;
	}
	if (hit.valid() && hit.direct && _article->segmentIsText(hit.segmentIndex)) {
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
	clearTextSelection();
	e->accept();
}

void Widget::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		Ui::RpWidget::mouseReleaseEvent(e);
		return;
	}
	const auto guard = gsl::finally([&] {
		_trackingPointerPress = false;
	});
	const auto articlePoint = e->pos() - articleTopLeft();
	const auto hit = _article->hitTest(
		articlePoint,
		Ui::Text::StateRequest::Flag::LookupSymbol);
	const auto directTextHit = [&] {
		return hit.valid()
			&& hit.direct
			&& _article->segmentIsText(hit.segmentIndex);
	};
	const auto directEditableHit = [&] {
		return hit.valid()
			&& hit.direct
			&& _article->segmentIsEditable(hit.segmentIndex);
	};
	const auto commitVisibleInlineField = [&] {
		if (_field->isHidden()) {
			return false;
		}
		commitInlineField();
		_pendingOrdinal = -1;
		_pendingCursorOffset = 0;
		hideInlineField();
		_article->clearTextLeafHeightOverride();
		refreshPreparedContent();
		return true;
	};
	const auto focusOrActivateInitial = [&] {
		if (_field->isHidden()) {
			activateInitialNode();
		} else {
			_field->setFocusFast();
		}
	};
	if (hit.codeHeaderCopy) {
		clearTextSelection();
		auto languageHit = hit;
		if (commitVisibleInlineField()) {
			languageHit = _article->hitTest(
				articlePoint,
				Ui::Text::StateRequest::Flag::LookupSymbol);
		}
		const auto ordinal = languageHit.codeHeaderCopy
			? editableOrdinalForSegment(languageHit.segmentIndex)
			: -1;
		if (const auto now = _state->codeBlockLanguage(ordinal)) {
			const auto weak = QPointer<Widget>(this);
			DefaultEditLanguageCallback(_controller->uiShow())(
				*now,
				[=](QString language) {
					if (!weak) {
						return;
					}
					if (_state->setCodeBlockLanguage(ordinal, language)) {
						refreshPreparedContent();
						update();
					}
				});
		}
		e->accept();
		return;
	}
	if (_selectingText) {
		updateTextSelection(hit);
		const auto selection = _selection;
		const auto sameSegmentSelection = !selection.empty()
			&& (selection.from.segment == selection.to.segment)
			&& _article->segmentIsText(selection.from.segment);
		const auto selectionOrdinal = sameSegmentSelection
			? editableOrdinalForSegment(selection.from.segment)
			: -1;
		if (selectionOrdinal >= 0) {
			const auto selectionFrom = selection.from.offset;
			const auto selectionTo = selection.to.offset;
			clearTextSelection();
			commitVisibleInlineField();
			activateTextOrdinal(
				selectionOrdinal,
				selectionFrom,
				selectionTo);
			e->accept();
			return;
		} else if (selection.empty() && directTextHit()) {
			const auto targetOrdinal = editableOrdinalForSegment(
				hit.segmentIndex);
			const auto targetOffset = _article->selectionOffsetFromHit(
				hit,
				TextSelectType::Letters);
			if (targetOrdinal >= 0) {
				clearTextSelection();
				commitVisibleInlineField();
				activateTextOrdinal(targetOrdinal, targetOffset);
				e->accept();
				return;
			}
		}
		clearTextSelection();
		if (articlePoint.y() >= _articleHeight) {
			activateTrailingParagraph();
		} else {
			focusOrActivateInitial();
		}
		e->accept();
		return;
	}
	if (directEditableHit()) {
		const auto targetOrdinal = editableOrdinalForSegment(hit.segmentIndex);
		const auto offset = _article->selectionOffsetFromHit(
			hit,
			TextSelectType::Letters);
		if (targetOrdinal >= 0
			&& !_field->isHidden()
			&& hit.segmentIndex == _activeSegmentIndex) {
			auto cursor = _field->textCursor();
			cursor.setPosition(std::clamp(
				offset,
				0,
				_field->getLastText().size()));
			_field->setTextCursor(cursor);
			_field->setFocusFast();
		} else if (targetOrdinal >= 0) {
			commitVisibleInlineField();
			activateTextOrdinal(targetOrdinal, offset);
		}
	} else if (articlePoint.y() >= _articleHeight) {
		activateTrailingParagraph();
	} else {
		focusOrActivateInitial();
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

Markdown::MarkdownArticleTextLeafStyle Widget::inlineFieldStyleForSegment(
		int segmentIndex) const {
	return _article
		? _article->editableStyleForSegment(segmentIndex)
		: Markdown::MarkdownArticleTextLeafStyle();
}

const Widget::CachedInlineFieldStyle &Widget::inlineFieldStyleFor(
		const Markdown::MarkdownArticleTextLeafStyle &leafStyle) {
	return inlineFieldStyleFor(normalizedInlineFieldStyle(leafStyle));
}

const Widget::CachedInlineFieldStyle &Widget::inlineFieldStyleFor(
		const InlineFieldStyleData &data) {
	const auto key = inlineFieldStyleKey(data);
	for (const auto &cached : _fieldStyles) {
		if (cached.key == key) {
			return cached;
		}
	}
	auto fieldStyle = std::make_shared<style::InputField>(
		st::ivEditorInputField);
	fieldStyle->style = *data.textStyle;
	fieldStyle->style.font = data.italic
		? data.textStyle->font->italic()
		: data.textStyle->font;
	fieldStyle->style.lineHeight = data.lineHeight;
	fieldStyle->textFg = data.textFg;
	fieldStyle->textAlign = data.align;
	_fieldStyles.push_back({
		.key = key,
		.style = std::move(fieldStyle),
	});
	return _fieldStyles.back();
}

Widget::InlineFieldStyleData Widget::normalizedInlineFieldStyle(
		const Markdown::MarkdownArticleTextLeafStyle &leafStyle) const {
	const auto valid = leafStyle.valid();
	const auto textStyle = valid
		? leafStyle.textStyle
		: &_articleStyle->body;
	const auto lineHeight = (valid && leafStyle.lineHeight > 0)
		? leafStyle.lineHeight
		: std::max(textStyle->lineHeight, textStyle->font->height);
	return {
		.textStyle = textStyle,
		.lineHeight = lineHeight,
		.textFg = valid ? leafStyle.textColor : _articleStyle->textColor,
		.align = valid ? leafStyle.align : style::al_left,
		.italic = valid ? leafStyle.italic : false,
	};
}

Widget::InlineFieldStyleKey Widget::inlineFieldStyleKey(
		const InlineFieldStyleData &data) const {
	const auto textStyle = data.textStyle
		? data.textStyle
		: &_articleStyle->body;
	return {
		.font = data.italic
			? textStyle->font->italic()
			: textStyle->font,
		.lineHeight = data.lineHeight,
		.textFg = data.textFg,
		.align = data.align,
	};
}

void Widget::ensureInlineFieldForSegment(int segmentIndex) {
	const auto data = normalizedInlineFieldStyle(
		inlineFieldStyleForSegment(segmentIndex));
	const auto key = inlineFieldStyleKey(data);
	const auto mode = _state->activeFieldMode();
	if (_activeFieldStyleKey
		&& *_activeFieldStyleKey == key
		&& _fieldMode == mode) {
		return;
	}
	const auto &cached = inlineFieldStyleFor(data);
	_activeFieldStyleKey = key;
	_fieldMode = mode;
	recreateInlineField(*cached.style);
}

void Widget::setupInlineField() {
	if (_fieldMode == State::FieldMode::Rich) {
		const auto allowPremiumEmoji = [peer = _peer](
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
			.fieldStyle = &_field->st(),
		});
		_field->setMimeDataHook(WrappedMessageFieldMimeHook(
			Ui::InputField::MimeDataHook(),
			_field.get()));
	} else {
		_field->setInstantReplacesEnabled(
			rpl::single(false),
			rpl::single(false));
		_field->setMarkdownReplacesEnabled(
			rpl::single(Ui::MarkdownEnabledState{
				Ui::MarkdownDisabled()
			}));
	}
	_field->setDocumentMargin(0.);
	_field->setAdditionalMargins({});
	_field->setSubmitSettings(Ui::InputField::SubmitSettings::None);
	_field->setMaxHeight(std::numeric_limits<int>::max());
	_field->rawTextEdit()->installEventFilter(this);

	_field->heightChanges(
	) | rpl::on_next([=] {
		updateInlineFieldHeightOverride();
	}, _field->lifetime());
	_field->focusedChanges(
	) | rpl::on_next([=](bool focused) {
		if (!focused && !_settingField && !_trackingPointerPress) {
			commitInlineField();
			refreshPreparedContent();
		}
	}, _field->lifetime());

	hideInlineField();
}

void Widget::recreateInlineField(const style::InputField &st) {
	const auto text = _field->getTextWithTags();
	const auto cursor = _field->textCursor();
	const auto position = cursor.position();
	const auto anchor = cursor.anchor();
	const auto wasHidden = _field->isHidden();
	const auto hadFocus = _field->hasFocus();

	_settingField = true;
	_field = base::make_unique_q<Ui::InputField>(
		this,
		st,
		Ui::InputField::Mode::MultiLine,
		rpl::single(QString()));
	setupInlineField();
	_field->setTextWithTags(text, Ui::InputField::HistoryAction::Clear);
	auto restored = _field->textCursor();
	const auto size = _field->getLastText().size();
	const auto restoredAnchor = std::clamp(anchor, 0, size);
	const auto restoredPosition = std::clamp(position, 0, size);
	restored.setPosition(restoredAnchor);
	if (restoredPosition != restoredAnchor) {
		restored.setPosition(restoredPosition, QTextCursor::KeepAnchor);
	}
	_field->setTextCursor(restored);
	if (!wasHidden) {
		_field->show();
		_field->raise();
		if (hadFocus) {
			_field->setFocusFast();
		}
	}
	_settingField = false;
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

	const auto segmentIndex = segmentIndexForEditableOrdinal(ordinal);
	if (segmentIndex < 0) {
		_activeSegmentIndex = -1;
		_pendingOrdinal = ordinal;
		_pendingCursorOffset = selectionTo;
		hideInlineField();
		return;
	}

	_activeSegmentIndex = segmentIndex;
	ensureInlineFieldForSegment(segmentIndex);
	_settingField = true;
	if (_state->activeFieldMode() == State::FieldMode::Raw) {
		_field->setTextWithTags(
			{ _state->activeRawText(), {} },
			Ui::InputField::HistoryAction::Clear);
		_article->clearTextLeafHeightOverride();
	} else {
		const auto activeText = _state->activeText();
		_field->setTextWithTags(
			{
				activeText.text,
				TextUtilities::ConvertEntitiesToTextTags(activeText.entities),
			},
			Ui::InputField::HistoryAction::Clear);
	}
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
	if (_state->activeFieldMode() == State::FieldMode::Raw) {
		_state->applyActiveRawText(_field->getLastText());
		return;
	}
	const auto text = _field->getTextWithAppliedMarkdown();
	_state->applyActiveText({
		.text = text.text,
		.entities = TextUtilities::ConvertTextTagsToEntities(text.tags),
	});
}

void Widget::hideInlineField() {
	if (_field->isHidden()) {
		return;
	}
	const auto wasSettingField = _settingField;
	_settingField = true;
	const auto guard = gsl::finally([&] {
		_settingField = wasSettingField;
	});
	_field->hide();
}

void Widget::activateTextOrdinalAtEnd(int ordinal) {
	if (!_state->setActiveTextByOrdinal(ordinal)) {
		return;
	}
	activateTextOrdinal(ordinal, _state->activeTextLength());
}

bool Widget::handleFieldKey(QKeyEvent *e) {
	if (_field->isHidden()) {
		return false;
	}
	const auto key = e->key();
	if (key == Qt::Key_Escape) {
		hideInlineFieldAndRefresh();
		e->accept();
		return true;
	}
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	if (modifiers != Qt::NoModifier) {
		return false;
	}
	const auto cursor = _field->textCursor();
	if (cursor.hasSelection()) {
		return false;
	}
	const auto position = cursor.position();
	const auto length = _field->getLastText().size();
	auto handled = false;
	if (position <= 0
		&& (key == Qt::Key_Left
			|| key == Qt::Key_Up
			|| key == Qt::Key_PageUp)) {
		handled = moveBoundary(false, false);
	} else if (position >= length
		&& (key == Qt::Key_Right
			|| key == Qt::Key_Down
			|| key == Qt::Key_PageDown)) {
		handled = moveBoundary(true, true);
	} else if (position <= 0 && key == Qt::Key_Backspace) {
		handled = removeBoundaryOwner(false);
	} else if (position >= length && key == Qt::Key_Delete) {
		handled = removeBoundaryOwner(true);
	}
	if (handled) {
		e->accept();
	}
	return handled;
}

bool Widget::moveBoundary(bool forward, bool allowTrailing) {
	const auto target = forward
		? _state->nextEditableOrdinal()
		: _state->previousEditableOrdinal();
	const auto addTrailing = forward
		&& allowTrailing
		&& !target
		&& !_state->isActiveTopLevelParagraph();
	if (!target && !addTrailing) {
		return false;
	}
	commitInlineField();
	if (target) {
		refreshPreparedContent();
		if (forward) {
			activateTextOrdinal(*target, 0);
		} else {
			activateTextOrdinalAtEnd(*target);
		}
		return true;
	}
	const auto ordinal = _state->ensureTrailingParagraphActive();
	refreshPreparedContent();
	activateTextOrdinal(ordinal, 0);
	return true;
}

bool Widget::moveBoundaryAfterCommit(bool forward, bool allowTrailing) {
	const auto target = forward
		? _state->nextEditableOrdinal()
		: _state->previousEditableOrdinal();
	if (target) {
		refreshPreparedContent();
		if (forward) {
			activateTextOrdinal(*target, 0);
		} else {
			activateTextOrdinalAtEnd(*target);
		}
		return true;
	}
	if (forward && allowTrailing && !_state->isActiveTopLevelParagraph()) {
		const auto ordinal = _state->ensureTrailingParagraphActive();
		refreshPreparedContent();
		activateTextOrdinal(ordinal, 0);
		return true;
	}
	return false;
}

bool Widget::removeBoundaryOwner(bool forward) {
	commitInlineField();
	if (_state->activeOwnerIsEmpty()) {
		const auto target = _state->removeActiveOwnerAndSelectAdjacent(
			forward);
		hideInlineField();
		_article->clearTextLeafHeightOverride();
		refreshPreparedContent();
		if (target) {
			if (forward) {
				activateTextOrdinal(*target, 0);
			} else {
				activateTextOrdinalAtEnd(*target);
			}
		} else {
			activateInitialNode();
		}
		return true;
	}
	return moveBoundaryAfterCommit(forward, forward);
}

void Widget::ensurePendingActivation() {
	if (_pendingOrdinal < 0) {
		_activeSegmentIndex = (_activeOrdinal >= 0)
			? segmentIndexForEditableOrdinal(_activeOrdinal)
			: _article->firstEditableSegmentIndex();
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
	const auto textLeafIndex = _article->textLeafIndexForSegment(
		_activeSegmentIndex);
	if (textLeafIndex < 0) {
		_article->clearTextLeafHeightOverride();
		return;
	}
	const auto segmentRect = outerEditableSegmentRect(_activeSegmentIndex);
	const auto height = segmentRect.isEmpty()
		? _field->height()
		: std::max(_field->geometry().bottom() + 1 - segmentRect.y(), 1);
	_article->setTextLeafHeightOverride(textLeafIndex, height);
	resizeToWidth(std::max(widthNoMargins(), 1));
	update();
}

void Widget::syncInlineFieldGeometry(int width) {
	if (_field->isHidden() || width <= 0) {
		return;
	}
	if (_activeSegmentIndex >= 0) {
		ensureInlineFieldForSegment(_activeSegmentIndex);
	}
	const auto segmentRect = outerEditableSegmentRect(_activeSegmentIndex);
	if (segmentRect.isEmpty()) {
		_pendingOrdinal = _activeOrdinal;
		_pendingCursorOffset = _field->textCursor().position();
		hideInlineField();
		_article->clearTextLeafHeightOverride();
		return;
	}
	const auto margins = _field->fullTextMargins();
	const auto left = segmentRect.x() - margins.left();
	const auto top = segmentRect.y() - margins.top();
	const auto fieldWidth = std::max(
		std::min(
			segmentRect.width() + margins.left() + margins.right(),
			width - left),
		1);
	_syncingInlineFieldGeometry = true;
	_field->resizeToWidth(fieldWidth);
	const auto fieldHeight = FieldNaturalHeight(_field.get());
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

int Widget::editableOrdinalForSegment(int segmentIndex) const {
	return _article->editableIndexForSegment(segmentIndex);
}

int Widget::segmentIndexForEditableOrdinal(int ordinal) const {
	return _article->segmentIndexForEditableIndex(ordinal);
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

QRect Widget::outerEditableSegmentRect(int segmentIndex) const {
	const auto rect = _article->segmentRect(segmentIndex);
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
	const auto messageStyle = context.messageStyle();
	context.caches = {
		.pre = messageStyle->preCache.get(),
		.blockquote = context.quoteCache({}, 0),
		.colors = _highlightColors,
		.st = &messageStyle->richPageStyle,
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
	const auto hiddenSegmentIndex = _field->isHidden()
		? -1
		: _activeSegmentIndex;
	context.hiddenTextSegmentIndex = hiddenSegmentIndex;
	context.hiddenSegmentIndex = hiddenSegmentIndex;
	context.selectionState.selection = _selection;
	context.selectionState.endpoints = &_selectionEndpoints;
	return context;
}

} // namespace Iv::Editor
