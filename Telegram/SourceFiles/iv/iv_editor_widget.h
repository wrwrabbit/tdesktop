/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "iv/iv_editor_state.h"
#include "iv/markdown/iv_markdown_article.h"
#include "ui/rp_widget.h"

#include <memory>
#include <vector>

namespace Ui {
class ChatStyle;
class ChatTheme;
class InputField;
namespace Text {
struct QuotePaintCache;
} // namespace Text
} // namespace Ui

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace Iv::Editor {

class Widget final : public Ui::RpWidget {
public:
	Widget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		std::shared_ptr<State> state);

	void activateInitialNode();
	void activateSegment(int segmentIndex, int cursorOffset);
	void commitInlineField();
	void refreshPreparedContent();
	void syncInlineFieldGeometry();
	void insertHeading1();
	void insertBlockquote();

	int resizeGetHeight(int newWidth) override;

protected:
	void focusInEvent(QFocusEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void setDocument(const Markdown::MarkdownArticleContent &prepared);
	void activateTextOrdinal(int ordinal, int cursorOffset);
	void activateTextOrdinal(int ordinal, int selectionFrom, int selectionTo);
	void activateSegmentSelection(int segmentIndex, TextSelection selection);
	void activateTrailingParagraph();
	void applyFieldTextToState();
	void acceptInlineField();
	void ensurePendingActivation();
	void updateInlineFieldHeightOverride();
	void syncInlineFieldGeometry(int width);
	void clearTextSelection();
	void updateTextSelection(const Markdown::MarkdownArticleHitTestResult &hit);
	[[nodiscard]] int textOrdinalForSegment(int segmentIndex) const;
	[[nodiscard]] int segmentIndexForTextOrdinal(int ordinal) const;
	[[nodiscard]] QPoint articleTopLeft() const;
	[[nodiscard]] int articleWidth(int outerWidth) const;
	[[nodiscard]] QRect outerTextSegmentRect(int segmentIndex) const;
	[[nodiscard]] Markdown::MarkdownArticlePaintContext textPaintContext(
		QRect clip);
	[[nodiscard]] Ui::Text::QuotePaintCache *ensureBlockquotePaintCache();
	[[nodiscard]] Ui::Text::QuotePaintCache *ensurePrePaintCache();

	const not_null<Window::SessionController*> _controller;
	const not_null<PeerData*> _peer;
	const std::shared_ptr<State> _state;
	std::shared_ptr<Markdown::MarkdownArticle> _article;
	base::unique_qptr<Ui::InputField> _field;
	std::unique_ptr<Ui::ChatTheme> _theme;
	std::unique_ptr<Ui::ChatStyle> _style;
	std::vector<Ui::Text::SpecialColor> _highlightColors;
	std::unique_ptr<Ui::Text::QuotePaintCache> _prePaintCache;
	std::unique_ptr<Ui::Text::QuotePaintCache> _blockquotePaintCache;
	int _articleHeight = 0;
	int _activeOrdinal = -1;
	int _activeSegmentIndex = -1;
	int _pendingOrdinal = -1;
	int _pendingCursorOffset = 0;
	Markdown::MarkdownArticleSelection _selection;
	Markdown::MarkdownArticleSelectionEndpoints _selectionEndpoints;
	int _dragSegment = -1;
	int _dragOffset = 0;
	bool _settingField = false;
	bool _syncingInlineFieldGeometry = false;
	bool _pendingHeightOverrideUpdate = false;
	bool _selectingText = false;
};

} // namespace Iv::Editor
