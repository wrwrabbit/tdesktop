/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "iv/editor/iv_editor_state.h"
#include "iv/markdown/iv_markdown_article.h"
#include "ui/style/style_core_types.h"
#include "ui/rp_widget.h"
#include "rpl/lifetime.h"

#include <memory>
#include <optional>
#include <vector>

class QEvent;
class QKeyEvent;
class QObject;
class QTouchEvent;
class QWheelEvent;

namespace Ui {
class ChatStyle;
class ChatTheme;
class InputField;
} // namespace Ui

namespace style {
struct InputField;
struct Markdown;
} // namespace style

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
	void insertBlock(State::InsertAction action);
	void insertPreparedBlock(RichPage::Block block);
	void insertPreparedBlocks(std::vector<RichPage::Block> blocks);
	void insertHeading1();
	void insertBlockquote();

	int resizeGetHeight(int newWidth) override;

protected:
	bool eventFilter(QObject *object, QEvent *event) override;
	bool eventHook(QEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;

private:
	struct InlineFieldStyleData {
		const style::TextStyle *textStyle = nullptr;
		int lineHeight = 0;
		style::color textFg;
		style::align align = style::al_left;
		bool italic = false;
	};

	struct InlineFieldStyleKey {
		style::font font;
		int lineHeight = 0;
		style::color textFg;
		style::align align = style::al_left;

		friend inline bool operator==(
				const InlineFieldStyleKey &a,
				const InlineFieldStyleKey &b) {
			return (a.font == b.font)
				&& (a.lineHeight == b.lineHeight)
				&& (a.textFg == b.textFg)
				&& (a.align == b.align);
		}

		friend inline bool operator!=(
				const InlineFieldStyleKey &a,
				const InlineFieldStyleKey &b) {
			return !(a == b);
		}
	};

	struct CachedInlineFieldStyle {
		InlineFieldStyleKey key;
		std::shared_ptr<style::InputField> style;
	};

	enum class DragSelectionMode {
		None,
		Text,
		Structural,
	};

	struct ArticleSelectionDrag {
		bool active = false;
		bool fromField = false;
		bool startedBelow = false;
		bool codeHeader = false;
		QPoint pressPoint;
		Markdown::PreparedEditHit anchorHit;
		int textSegment = -1;
		int textOffset = 0;
		DragSelectionMode mode = DragSelectionMode::None;
	};

	enum class HorizontalScrollDrag {
		None,
		Mouse,
		Touch,
	};

	void setDocument(const Markdown::MarkdownArticleContent &prepared);
	void activateTextOrdinal(int ordinal, int cursorOffset);
	void activateTextOrdinal(int ordinal, int selectionFrom, int selectionTo);
	[[nodiscard]] Markdown::MarkdownArticleTextLeafStyle
	inlineFieldStyleForSegment(int segmentIndex) const;
	[[nodiscard]] const CachedInlineFieldStyle &inlineFieldStyleFor(
		const Markdown::MarkdownArticleTextLeafStyle &style);
	[[nodiscard]] const CachedInlineFieldStyle &inlineFieldStyleFor(
		const InlineFieldStyleData &data);
	[[nodiscard]] InlineFieldStyleData normalizedInlineFieldStyle(
		const Markdown::MarkdownArticleTextLeafStyle &style) const;
	[[nodiscard]] InlineFieldStyleKey inlineFieldStyleKey(
		const InlineFieldStyleData &data) const;
	void ensureInlineFieldForSegment(int segmentIndex);
	void setupInlineField();
	void recreateInlineField(const style::InputField &st);
	void activateTrailingParagraph();
	void applyFieldTextToState();
	void hideInlineField();
	void acceptInlineField();
	void hideInlineFieldAndRefresh();
	void activateTextOrdinalAtEnd(int ordinal);
	[[nodiscard]] bool handleFieldKey(QKeyEvent *e);
	[[nodiscard]] bool moveBoundary(bool forward, bool allowTrailing);
	[[nodiscard]] bool moveBoundaryAfterCommit(
		bool forward,
		bool allowTrailing);
	[[nodiscard]] bool removeBoundaryOwner(bool forward);
	void ensurePendingActivation();
	void updateInlineFieldHeightOverride();
	void syncInlineFieldGeometry(int width);
	void clearSelection();
	void clearTextSelection();
	void clearStructuralSelection();
	[[nodiscard]] bool hasStructuralSelection() const;
	void startArticleSelection(
		QPoint pressPoint,
		const Markdown::MarkdownArticleHitTestResult &hit,
		const Markdown::PreparedEditHit &editHit,
		bool fromField = false,
		bool startedBelow = false);
	void updateArticleSelection(
		QPoint articlePoint,
		const Markdown::MarkdownArticleHitTestResult &hit,
		const Markdown::PreparedEditHit &editHit);
	void finishArticleSelection();
	[[nodiscard]] bool handleStructuralSelectionKey(QKeyEvent *e);
	[[nodiscard]] bool handleFieldMouseEvent(QEvent *event);
	[[nodiscard]] bool handleHorizontalScrollWheel(
		QWheelEvent *e,
		QPoint articlePoint);
	[[nodiscard]] Markdown::PreparedEditSelection structuralSelectionFromHits(
		const Markdown::PreparedEditHit &anchor,
		const Markdown::PreparedEditHit &focus) const;
	[[nodiscard]] int editableOrdinalForSegment(int segmentIndex) const;
	[[nodiscard]] int segmentIndexForEditableOrdinal(int ordinal) const;
	[[nodiscard]] QPoint articleTopLeft() const;
	[[nodiscard]] int articleWidth(int outerWidth) const;
	void touchEvent(QTouchEvent *e);
	[[nodiscard]] QRect outerEditableSegmentRect(int segmentIndex) const;
	[[nodiscard]] Markdown::MarkdownArticlePaintContext textPaintContext(
		QRect clip);

	const not_null<Window::SessionController*> _controller;
	const not_null<PeerData*> _peer;
	const std::shared_ptr<State> _state;
	std::shared_ptr<style::Markdown> _articleStyle;
	std::shared_ptr<Markdown::MarkdownArticle> _article;
	base::unique_qptr<Ui::InputField> _field;
	std::unique_ptr<Ui::ChatTheme> _theme;
	std::unique_ptr<Ui::ChatStyle> _style;
	std::vector<Ui::Text::SpecialColor> _highlightColors;
	rpl::lifetime _highlightReadyLifetime;
	std::vector<CachedInlineFieldStyle> _fieldStyles;
	std::optional<InlineFieldStyleKey> _activeFieldStyleKey;
	State::FieldMode _fieldMode = State::FieldMode::Rich;
	int _articleHeight = 0;
	int _activeOrdinal = -1;
	int _activeSegmentIndex = -1;
	int _pendingOrdinal = -1;
	int _pendingCursorOffset = 0;
	Markdown::MarkdownArticleSelection _selection;
	Markdown::MarkdownArticleSelectionEndpoints _selectionEndpoints;
	Markdown::PreparedEditSelection _structuralSelection;
	ArticleSelectionDrag _articleSelectionDrag;
	std::optional<Qt::Orientation> _horizontalScrollLock;
	bool _settingField = false;
	bool _trackingPointerPress = false;
	Markdown::MarkdownArticleEditControlHit _pressedControl;
	std::optional<QPoint> _pressedControlPoint;
	HorizontalScrollDrag _horizontalScrollDrag = HorizontalScrollDrag::None;
	std::optional<QPoint> _pendingTouchHorizontalScrollPoint;
	bool _syncingInlineFieldGeometry = false;
	bool _pendingHeightOverrideUpdate = false;
};

} // namespace Iv::Editor
