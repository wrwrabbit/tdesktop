/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/editor/iv_editor_widget.h"

#include "base/qt/qt_common_adapters.h"
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
#include "ui/ui_utility.h"
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
#include <QtGui/QTouchEvent>
#include <QtGui/QWheelEvent>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>

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

using PreparedEditBlockContainerPath
	= Markdown::PreparedEditBlockContainerPath;
using PreparedEditBlockContainerStep
	= Markdown::PreparedEditBlockContainerStep;
using PreparedEditBlockContainerKind
	= Markdown::PreparedEditBlockContainerKind;
using PreparedEditBlockPath = Markdown::PreparedEditBlockPath;
using PreparedEditBlockSource = Markdown::PreparedEditBlockSource;
using PreparedEditHit = Markdown::PreparedEditHit;
using PreparedEditHitKind = Markdown::PreparedEditHitKind;
using PreparedEditLeafKind = Markdown::PreparedEditLeafKind;
using PreparedEditLeafSource = Markdown::PreparedEditLeafSource;
using PreparedEditListItemSource = Markdown::PreparedEditListItemSource;
using PreparedEditSelection = Markdown::PreparedEditSelection;
using PreparedEditSelectionKind = Markdown::PreparedEditSelectionKind;
using PreparedEditTableCellSource = Markdown::PreparedEditTableCellSource;
using PreparedEditTableRowSource = Markdown::PreparedEditTableRowSource;

struct NormalizedIntegerRange {
	int from = -1;
	int till = -1;

	[[nodiscard]] bool empty() const {
		return (from < 0) || (till <= from);
	}
};

[[nodiscard]] NormalizedIntegerRange NormalizeIntegerRange(int a, int b) {
	if (a < 0 || b < 0) {
		return {};
	}
	return {
		.from = std::min(a, b),
		.till = std::max(a, b) + 1,
	};
}

[[nodiscard]] PreparedEditSelection BlockSelectionFromIndexes(
		PreparedEditBlockContainerPath container,
		int first,
		int second) {
	const auto range = NormalizeIntegerRange(first, second);
	if (range.empty()) {
		return {};
	}
	return {
		.kind = PreparedEditSelectionKind::Blocks,
		.blocks = {
			.container = std::move(container),
			.from = range.from,
			.till = range.till,
		},
	};
}

[[nodiscard]] int CompareIntegers(int a, int b) {
	return (a < b) ? -1 : (a > b) ? 1 : 0;
}

[[nodiscard]] int ComparePreparedEditBlockContainerSteps(
		const PreparedEditBlockContainerStep &a,
		const PreparedEditBlockContainerStep &b) {
	if (const auto result = CompareIntegers(
			static_cast<int>(a.kind),
			static_cast<int>(b.kind))) {
		return result;
	} else if (const auto result = CompareIntegers(
			a.blockIndex,
			b.blockIndex)) {
		return result;
	}
	return CompareIntegers(a.listItemIndex, b.listItemIndex);
}

[[nodiscard]] int ComparePreparedEditBlockContainerPaths(
		const PreparedEditBlockContainerPath &a,
		const PreparedEditBlockContainerPath &b) {
	const auto common = std::min(a.steps.size(), b.steps.size());
	for (auto i = size_t(); i != common; ++i) {
		if (const auto result = ComparePreparedEditBlockContainerSteps(
				a.steps[i],
				b.steps[i])) {
			return result;
		}
	}
	return CompareIntegers(int(a.steps.size()), int(b.steps.size()));
}

[[nodiscard]] int ComparePreparedEditBlockPaths(
		const PreparedEditBlockPath &a,
		const PreparedEditBlockPath &b) {
	if (const auto result = ComparePreparedEditBlockContainerPaths(
			a.container,
			b.container)) {
		return result;
	}
	return CompareIntegers(a.index, b.index);
}

[[nodiscard]] bool SamePreparedEditBlockPath(
		const PreparedEditBlockPath &a,
		const PreparedEditBlockPath &b) {
	return (ComparePreparedEditBlockPaths(a, b) == 0);
}

[[nodiscard]] bool ValidPreparedEditBlockPath(
		const PreparedEditBlockPath &path) {
	return (path.index >= 0);
}

[[nodiscard]] PreparedEditBlockSource PreparedEditBlockSourceFromPath(
		PreparedEditBlockPath path) {
	return { .path = std::move(path) };
}

enum class StructuralOwnerKind {
	None,
	Block,
	ListItem,
	TableRow,
	TableCell,
};

struct StructuralOwner {
	StructuralOwnerKind kind = StructuralOwnerKind::None;
	std::optional<PreparedEditBlockSource> block;
	std::optional<PreparedEditListItemSource> listItem;
	std::optional<PreparedEditTableRowSource> tableRow;
	std::optional<PreparedEditTableCellSource> tableCell;

	[[nodiscard]] bool valid() const {
		return (kind != StructuralOwnerKind::None);
	}
};

[[nodiscard]] StructuralOwner StructuralOwnerFromBlock(
		const PreparedEditBlockSource &source) {
	if (!ValidPreparedEditBlockPath(source.path)) {
		return {};
	}
	return {
		.kind = StructuralOwnerKind::Block,
		.block = source,
	};
}

[[nodiscard]] StructuralOwner StructuralOwnerFromListItem(
		const PreparedEditListItemSource &source) {
	if (!ValidPreparedEditBlockPath(source.block)
		|| source.listItemIndex < 0) {
		return {};
	}
	return {
		.kind = StructuralOwnerKind::ListItem,
		.block = PreparedEditBlockSourceFromPath(source.block),
		.listItem = source,
	};
}

[[nodiscard]] StructuralOwner StructuralOwnerFromTableRow(
		const PreparedEditTableRowSource &source) {
	if (!ValidPreparedEditBlockPath(source.block)
		|| source.tableRowIndex < 0) {
		return {};
	}
	return {
		.kind = StructuralOwnerKind::TableRow,
		.block = PreparedEditBlockSourceFromPath(source.block),
		.tableRow = source,
	};
}

[[nodiscard]] PreparedEditTableRowSource PreparedEditTableRowFromCell(
		const PreparedEditTableCellSource &source) {
	return {
		.block = source.block,
		.tableRowIndex = source.tableRowIndex,
	};
}

[[nodiscard]] StructuralOwner StructuralOwnerFromTableCell(
		const PreparedEditTableCellSource &source) {
	if (!ValidPreparedEditBlockPath(source.block)
		|| source.tableRowIndex < 0
		|| source.tableCellIndex < 0) {
		return {};
	}
	return {
		.kind = StructuralOwnerKind::TableCell,
		.block = PreparedEditBlockSourceFromPath(source.block),
		.tableRow = PreparedEditTableRowFromCell(source),
		.tableCell = source,
	};
}

[[nodiscard]] StructuralOwner StructuralOwnerFromLeaf(
		const PreparedEditLeafSource &source) {
	if (!ValidPreparedEditBlockPath(source.block)) {
		return {};
	}
	switch (source.kind) {
	case PreparedEditLeafKind::ListItemText:
		return StructuralOwnerFromListItem({
			.block = source.block,
			.listItemIndex = source.listItemIndex,
		});
	case PreparedEditLeafKind::TableCellText:
		return StructuralOwnerFromTableCell({
			.block = source.block,
			.tableRowIndex = source.tableRowIndex,
			.tableCellIndex = source.tableCellIndex,
		});
	case PreparedEditLeafKind::BlockText:
	case PreparedEditLeafKind::BlockCaption:
	case PreparedEditLeafKind::MathFormula:
		return StructuralOwnerFromBlock(
			PreparedEditBlockSourceFromPath(source.block));
	}
	return {};
}

[[nodiscard]] StructuralOwner StructuralOwnerFromHit(
		const PreparedEditHit &hit) {
	if (!hit.valid()) {
		return {};
	}
	switch (hit.kind) {
	case PreparedEditHitKind::Block:
		if (hit.block) {
			return StructuralOwnerFromBlock(*hit.block);
		}
		break;
	case PreparedEditHitKind::ListItem:
		if (hit.listItem) {
			return StructuralOwnerFromListItem(*hit.listItem);
		}
		break;
	case PreparedEditHitKind::TableRow:
		if (hit.tableRow) {
			return StructuralOwnerFromTableRow(*hit.tableRow);
		}
		break;
	case PreparedEditHitKind::TableCell:
		if (hit.tableCell) {
			return StructuralOwnerFromTableCell(*hit.tableCell);
		}
		break;
	case PreparedEditHitKind::Leaf:
		if (hit.leaf) {
			return StructuralOwnerFromLeaf(*hit.leaf);
		}
		break;
	case PreparedEditHitKind::None:
		break;
	}
	return hit.leaf ? StructuralOwnerFromLeaf(*hit.leaf) : StructuralOwner();
}

[[nodiscard]] std::optional<PreparedEditTableCellSource> TableCellFromOwner(
		const StructuralOwner &owner) {
	return owner.tableCell;
}

[[nodiscard]] std::optional<PreparedEditTableRowSource> TableRowFromOwner(
		const StructuralOwner &owner) {
	return owner.tableRow;
}

[[nodiscard]] std::optional<PreparedEditListItemSource> ListItemFromOwner(
		const StructuralOwner &owner) {
	return owner.listItem;
}

[[nodiscard]] bool IsBlockOwner(const StructuralOwner &owner) {
	return (owner.kind == StructuralOwnerKind::Block);
}

[[nodiscard]] std::optional<PreparedEditBlockPath> BlockPathFromOwner(
		const StructuralOwner &owner) {
	if (owner.kind == StructuralOwnerKind::Block && owner.block) {
		return owner.block->path;
	} else if (owner.kind == StructuralOwnerKind::ListItem
		&& owner.listItem) {
		return owner.listItem->block;
	} else if (owner.kind == StructuralOwnerKind::TableRow
		&& owner.tableRow) {
		return owner.tableRow->block;
	} else if (owner.kind == StructuralOwnerKind::TableCell
		&& owner.tableCell) {
		return owner.tableCell->block;
	}
	return std::nullopt;
}

struct LiftedPreparedEditBlocks {
	PreparedEditBlockContainerPath container;
	int first = -1;
	int second = -1;
};

[[nodiscard]] PreparedEditBlockContainerPath PreparedEditBlockContainerPrefix(
		const PreparedEditBlockContainerPath &path,
		int count) {
	auto result = PreparedEditBlockContainerPath();
	const auto till = std::clamp(count, 0, int(path.steps.size()));
	result.steps.insert(
		result.steps.end(),
		path.steps.begin(),
		path.steps.begin() + till);
	return result;
}

[[nodiscard]] int CommonPreparedEditBlockContainerSize(
		const PreparedEditBlockContainerPath &a,
		const PreparedEditBlockContainerPath &b) {
	const auto common = std::min(a.steps.size(), b.steps.size());
	for (auto i = size_t(); i != common; ++i) {
		if (ComparePreparedEditBlockContainerSteps(
				a.steps[i],
				b.steps[i]) != 0) {
			return int(i);
		}
	}
	return int(common);
}

[[nodiscard]] int LiftedPreparedEditBlockIndex(
		const PreparedEditBlockPath &path,
		int commonContainerSize) {
	if (commonContainerSize == int(path.container.steps.size())) {
		return path.index;
	} else if (commonContainerSize >= 0
		&& commonContainerSize < int(path.container.steps.size())) {
		return path.container.steps[commonContainerSize].blockIndex;
	}
	return -1;
}

[[nodiscard]] std::optional<LiftedPreparedEditBlocks>
LiftPreparedEditBlocksToCommonContainer(
		const PreparedEditBlockPath &a,
		const PreparedEditBlockPath &b) {
	if (!ValidPreparedEditBlockPath(a) || !ValidPreparedEditBlockPath(b)) {
		return std::nullopt;
	}
	const auto common = CommonPreparedEditBlockContainerSize(
		a.container,
		b.container);
	auto result = LiftedPreparedEditBlocks{
		.container = PreparedEditBlockContainerPrefix(a.container, common),
		.first = LiftedPreparedEditBlockIndex(a, common),
		.second = LiftedPreparedEditBlockIndex(b, common),
	};
	if (result.first < 0 || result.second < 0) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] PreparedEditSelection LiftedBlockSelection(
		const PreparedEditBlockPath &anchor,
		const PreparedEditBlockPath &focus) {
	const auto lifted = LiftPreparedEditBlocksToCommonContainer(anchor, focus);
	if (!lifted) {
		return {};
	}
	return BlockSelectionFromIndexes(
		lifted->container,
		lifted->first,
		lifted->second);
}

[[nodiscard]] auto ListItemSourcesFromBlockPath(
		const PreparedEditBlockPath &path)
-> std::vector<PreparedEditListItemSource> {
	auto result = std::vector<PreparedEditListItemSource>();
	for (auto i = int(path.container.steps.size()); i != 0; --i) {
		const auto stepIndex = i - 1;
		const auto &step = path.container.steps[stepIndex];
		if (step.kind != PreparedEditBlockContainerKind::ListItemChildren
			|| step.blockIndex < 0
			|| step.listItemIndex < 0) {
			continue;
		}
		result.push_back({
			.block = {
				.container = PreparedEditBlockContainerPrefix(
					path.container,
					stepIndex),
				.index = step.blockIndex,
			},
			.listItemIndex = step.listItemIndex,
		});
	}
	return result;
}

[[nodiscard]] auto ListItemSourcesFromOwner(
		const StructuralOwner &owner,
		const std::optional<PreparedEditBlockPath> &block)
-> std::vector<PreparedEditListItemSource> {
	auto result = std::vector<PreparedEditListItemSource>();
	if (const auto listItem = ListItemFromOwner(owner)) {
		result.push_back(*listItem);
	}
	if (!block) {
		return result;
	}
	for (const auto &source : ListItemSourcesFromBlockPath(*block)) {
		if (std::find(result.begin(), result.end(), source) == result.end()) {
			result.push_back(source);
		}
	}
	return result;
}

[[nodiscard]] PreparedEditSelection ListItemSelectionFromSources(
		const std::vector<PreparedEditListItemSource> &anchorSources,
		const std::vector<PreparedEditListItemSource> &focusSources) {
	for (const auto &anchorListItem : anchorSources) {
		for (const auto &focusListItem : focusSources) {
			if (!SamePreparedEditBlockPath(
					anchorListItem.block,
					focusListItem.block)) {
				continue;
			}
			const auto range = NormalizeIntegerRange(
				anchorListItem.listItemIndex,
				focusListItem.listItemIndex);
			if (!range.empty()) {
				return {
					.kind = PreparedEditSelectionKind::ListItems,
					.listItems = {
						.block = anchorListItem.block,
						.from = range.from,
						.till = range.till,
					},
				};
			}
		}
	}
	return {};
}

[[nodiscard]] bool IsMultiListItemSelection(
		const PreparedEditSelection &selection) {
	return !selection.empty()
		&& (selection.kind == PreparedEditSelectionKind::ListItems)
		&& (selection.listItems.till > selection.listItems.from + 1);
}

[[nodiscard]] int FieldNaturalHeight(not_null<Ui::InputField*> field) {
	const auto margins = field->fullTextMargins();
	return std::max(
		int(std::ceil(field->document()->size().height()))
			+ margins.top()
			+ margins.bottom(),
		1);
}

[[nodiscard]] QPoint LocalPosition(QWheelEvent *e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return e->position().toPoint();
#else // Qt >= 6.0
	return e->pos();
#endif // Qt >= 6.0
}

[[nodiscard]] QPoint GlobalPosition(QWheelEvent *e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return e->globalPosition().toPoint();
#else // Qt >= 6.0
	return e->globalPos();
#endif // Qt >= 6.0
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
	setAttribute(Qt::WA_AcceptTouchEvents);
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
	if (_field) {
		const auto raw = _field->rawTextEdit();
		if (object == raw.get() || object == raw->viewport()) {
			const auto type = event->type();
			if (type == QEvent::Wheel) {
				if (_article && _activeSegmentIndex >= 0) {
					const auto wheel = static_cast<QWheelEvent*>(event);
					auto articlePoint = std::optional<QPoint>();
					if (const auto widget = qobject_cast<QWidget*>(object)) {
						articlePoint = widget->mapTo(this, LocalPosition(wheel))
							- articleTopLeft();
					} else {
						articlePoint = mapFromGlobal(GlobalPosition(wheel))
							- articleTopLeft();
					}
					if (!articlePoint) {
						const auto segmentRect = _article->segmentRect(
							_activeSegmentIndex);
						if (!segmentRect.isEmpty()) {
							articlePoint = segmentRect.center();
						}
					}
					if (articlePoint
						&& handleHorizontalScrollWheel(wheel, *articlePoint)) {
						return true;
					}
				}
			} else if (type == QEvent::KeyPress) {
				const auto keyEvent = static_cast<QKeyEvent*>(event);
				if (handleStructuralSelectionKey(keyEvent)
					|| handleFieldKey(keyEvent)) {
					return true;
				}
			} else if ((type == QEvent::MouseButtonPress
				|| type == QEvent::MouseMove
				|| type == QEvent::MouseButtonRelease)
				&& handleFieldMouseEvent(event)) {
				return true;
			}
		}
	}
	return Ui::RpWidget::eventFilter(object, event);
}

bool Widget::eventHook(QEvent *e) {
	if (e->type() == QEvent::TouchBegin
		|| e->type() == QEvent::TouchUpdate
		|| e->type() == QEvent::TouchEnd
		|| e->type() == QEvent::TouchCancel) {
		auto *ev = static_cast<QTouchEvent*>(e);
		if (ev->device()->type() == base::TouchDevice::TouchScreen) {
			const auto active = (_horizontalScrollDrag
				== HorizontalScrollDrag::Touch);
			touchEvent(ev);
			if (active
				|| (_horizontalScrollDrag == HorizontalScrollDrag::Touch)) {
				return true;
			}
		}
	}
	return Ui::RpWidget::eventHook(e);
}

void Widget::focusInEvent(QFocusEvent *e) {
	Ui::RpWidget::focusInEvent(e);
	if (!_settingField && !_field->isHidden()) {
		_field->setFocusFast();
	}
}

void Widget::keyPressEvent(QKeyEvent *e) {
	if (handleStructuralSelectionKey(e)) {
		return;
	}
	Ui::RpWidget::keyPressEvent(e);
}

bool Widget::handleHorizontalScrollWheel(
		QWheelEvent *e,
		QPoint articlePoint) {
	const auto phase = e->phase();
	if (phase == Qt::NoScrollPhase) {
		_horizontalScrollLock = std::nullopt;
	} else if (phase == Qt::ScrollBegin) {
		_horizontalScrollLock = std::nullopt;
	}
	if (!_article) {
		return false;
	}
	const auto delta = Ui::ScrollDeltaF(e);
	const auto horizontal = (std::abs(delta.x()) > std::abs(delta.y()));
	if (phase != Qt::NoScrollPhase
		&& phase != Qt::ScrollBegin
		&& !_horizontalScrollLock) {
		_horizontalScrollLock = horizontal ? Qt::Horizontal : Qt::Vertical;
	}
	if (!_article->horizontalScrollHit(articlePoint).scrollable) {
		return false;
	}
	if (horizontal) {
		if (_horizontalScrollLock == Qt::Vertical) {
			return false;
		}
		if (_article->consumeHorizontalScroll(
				articlePoint,
				int(std::round(delta.x())))) {
			syncInlineFieldGeometry();
		}
		e->accept();
		return true;
	}
	if (_horizontalScrollLock == Qt::Horizontal) {
		e->accept();
		return true;
	}
	return false;
}

void Widget::touchEvent(QTouchEvent *e) {
	if (e->type() == QEvent::TouchCancel) {
		_pendingTouchHorizontalScrollPoint = std::nullopt;
		if (_horizontalScrollDrag != HorizontalScrollDrag::Touch) {
			return;
		}
		_horizontalScrollDrag = HorizontalScrollDrag::None;
		if (_article) {
			_article->endHorizontalScroll();
		}
		e->accept();
		return;
	}
	if (!_article || e->touchPoints().isEmpty()) {
		return;
	}
	const auto articlePoint = mapFromGlobal(
		e->touchPoints().cbegin()->screenPos().toPoint()) - articleTopLeft();
	switch (e->type()) {
	case QEvent::TouchBegin: {
		_pendingTouchHorizontalScrollPoint = std::nullopt;
		const auto hit = _article->horizontalScrollHit(articlePoint);
		if (hit.overScrollbar
			&& _article->beginHorizontalScroll(articlePoint, false)) {
			_horizontalScrollDrag = HorizontalScrollDrag::Touch;
			syncInlineFieldGeometry();
			e->accept();
		} else if (hit.overViewport) {
			_pendingTouchHorizontalScrollPoint = articlePoint;
		}
	} break;
	case QEvent::TouchUpdate:
		if (_horizontalScrollDrag == HorizontalScrollDrag::Touch) {
			if (_article->updateHorizontalScroll(articlePoint)) {
				syncInlineFieldGeometry();
			}
			e->accept();
		} else if (_pendingTouchHorizontalScrollPoint) {
			const auto delta = articlePoint - *_pendingTouchHorizontalScrollPoint;
			if (delta.manhattanLength() < QApplication::startDragDistance()) {
				break;
			}
			const auto horizontal = (std::abs(delta.x()) > std::abs(delta.y()));
			if (!horizontal) {
				_pendingTouchHorizontalScrollPoint = std::nullopt;
				break;
			}
			if (_article->beginHorizontalScroll(
					*_pendingTouchHorizontalScrollPoint,
					true)) {
				_horizontalScrollDrag = HorizontalScrollDrag::Touch;
				if (_article->updateHorizontalScroll(articlePoint)) {
					syncInlineFieldGeometry();
				}
				e->accept();
			}
			_pendingTouchHorizontalScrollPoint = std::nullopt;
		}
		break;
	case QEvent::TouchEnd:
		_pendingTouchHorizontalScrollPoint = std::nullopt;
		if (_horizontalScrollDrag == HorizontalScrollDrag::Touch) {
			_horizontalScrollDrag = HorizontalScrollDrag::None;
			_article->endHorizontalScroll();
			e->accept();
		}
		break;
	default:
		break;
	}
}

void Widget::wheelEvent(QWheelEvent *e) {
	if (handleHorizontalScrollWheel(
			e,
			LocalPosition(e) - articleTopLeft())) {
		return;
	}
	e->ignore();
}

void Widget::mouseMoveEvent(QMouseEvent *e) {
	const auto articlePoint = e->pos() - articleTopLeft();
	if (_horizontalScrollDrag == HorizontalScrollDrag::Mouse) {
		if (_article->updateHorizontalScroll(articlePoint)) {
			syncInlineFieldGeometry();
		}
		e->accept();
		return;
	}
	if (!_articleSelectionDrag.active) {
		const auto hit = _article->hitTest(
			articlePoint,
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
		articlePoint,
		Ui::Text::StateRequest::Flag::LookupSymbol);
	const auto editHit = _article->editHitTest(articlePoint);
	updateArticleSelection(articlePoint, hit, editHit);
	e->accept();
}

void Widget::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		Ui::RpWidget::mousePressEvent(e);
		return;
	}
	_trackingPointerPress = true;
	auto articlePoint = e->pos() - articleTopLeft();
	const auto horizontalScrollHit = _article->horizontalScrollHit(
		articlePoint);
	if (horizontalScrollHit.overScrollbar
		&& _article->beginHorizontalScroll(articlePoint, false)) {
		_horizontalScrollDrag = HorizontalScrollDrag::Mouse;
		syncInlineFieldGeometry();
		e->accept();
		return;
	}
	auto hit = _article->hitTest(
		articlePoint,
		Ui::Text::StateRequest::Flag::LookupSymbol);
	const auto editHit = _article->editHitTest(articlePoint);
	const auto startedBelow = (articlePoint.y() >= _articleHeight);
	if (hit.codeHeaderCopy) {
		startArticleSelection(articlePoint, hit, editHit);
		e->accept();
		return;
	}
	if (hit.valid() && hit.direct && _article->segmentIsText(hit.segmentIndex)) {
		startArticleSelection(articlePoint, hit, editHit);
		e->accept();
		return;
	}
	if (startedBelow) {
		if (editHit.valid()) {
			startArticleSelection(
				articlePoint,
				hit,
				editHit,
				false,
				true);
		} else {
			clearSelection();
		}
		e->accept();
		return;
	}
	if (editHit.valid()) {
		startArticleSelection(articlePoint, hit, editHit);
		e->accept();
		return;
	}
	clearSelection();
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
	const auto finishDrag = gsl::finally([&] {
		finishArticleSelection();
	});
	const auto articlePoint = e->pos() - articleTopLeft();
	if (_horizontalScrollDrag == HorizontalScrollDrag::Mouse) {
		const auto changed = _article->updateHorizontalScroll(articlePoint);
		_article->endHorizontalScroll();
		_horizontalScrollDrag = HorizontalScrollDrag::None;
		if (changed) {
			syncInlineFieldGeometry();
		}
		e->accept();
		return;
	}
	const auto hit = _article->hitTest(
		articlePoint,
		Ui::Text::StateRequest::Flag::LookupSymbol);
	const auto editHit = _article->editHitTest(articlePoint);
	const auto formulaOrdinalFromEditHit = [&] {
		return editHit.leaf
			&& (editHit.leaf->kind
				== Markdown::PreparedEditLeafKind::MathFormula)
			? _state->textOrdinalForLeaf(*editHit.leaf)
			: -1;
	};
	const auto directEditableHit = [&] {
		return (hit.valid()
			&& hit.direct
			&& _article->segmentIsEditable(hit.segmentIndex))
			|| (formulaOrdinalFromEditHit() >= 0);
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
	const auto editCodeBlockLanguage = [&] {
		if (!hit.codeHeaderCopy) {
			return false;
		}
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
		return true;
	};
	if (_articleSelectionDrag.active) {
		const auto fromField = _articleSelectionDrag.fromField;
		const auto pendingCodeHeader = _articleSelectionDrag.codeHeader;
		const auto startedBelow = _articleSelectionDrag.startedBelow;
		const auto updateOnRelease
			= (_articleSelectionDrag.mode != DragSelectionMode::None)
			|| (!pendingCodeHeader
				&& (!startedBelow || articlePoint.y() < _articleHeight));
		if (updateOnRelease) {
			updateArticleSelection(articlePoint, hit, editHit);
		}
		if (hasStructuralSelection()) {
			commitVisibleInlineField();
			e->accept();
			return;
		}
		if (_articleSelectionDrag.mode == DragSelectionMode::Text) {
			const auto selection = _selection;
			const auto sameSegmentSelection = !selection.empty()
				&& (selection.from.segment == selection.to.segment)
				&& _article->segmentIsText(selection.from.segment);
			const auto selectionOrdinal = sameSegmentSelection
				? editableOrdinalForSegment(selection.from.segment)
				: -1;
			if (!fromField && selectionOrdinal >= 0) {
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
			} else if (fromField) {
				e->accept();
				return;
			}
		}
		if (pendingCodeHeader
			&& _articleSelectionDrag.mode == DragSelectionMode::None
			&& editCodeBlockLanguage()) {
			e->accept();
			return;
		}
		const auto changed = !_selection.empty()
			|| _selectionEndpoints.from.valid()
			|| _selectionEndpoints.to.valid()
			|| hasStructuralSelection();
		_selection = {};
		_selectionEndpoints = {};
		_structuralSelection = {};
		if (changed) {
			update();
		}
	} else if (hit.codeHeaderCopy && editCodeBlockLanguage()) {
		e->accept();
		return;
	}
	if (directEditableHit()) {
		const auto formulaOrdinal = formulaOrdinalFromEditHit();
		const auto segmentHit = hit.valid()
			&& hit.direct
			&& _article->segmentIsEditable(hit.segmentIndex);
		const auto targetOrdinal = segmentHit
			? editableOrdinalForSegment(hit.segmentIndex)
			: formulaOrdinal;
		const auto offset = segmentHit
			? _article->selectionOffsetFromHit(
				hit,
				TextSelectType::Letters)
			: 0;
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
	const auto raw = _field->rawTextEdit();
	raw->installEventFilter(this);
	raw->viewport()->installEventFilter(this);

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
	if (handleStructuralSelectionKey(e)) {
		return true;
	}
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

void Widget::clearSelection() {
	const auto changed = !_selection.empty()
		|| _selectionEndpoints.from.valid()
		|| _selectionEndpoints.to.valid()
		|| hasStructuralSelection()
		|| _articleSelectionDrag.active;
	_selection = {};
	_selectionEndpoints = {};
	_structuralSelection = {};
	_articleSelectionDrag = {};
	if (changed) {
		update();
	}
}

void Widget::clearTextSelection() {
	const auto changed = !_selection.empty()
		|| _selectionEndpoints.from.valid()
		|| _selectionEndpoints.to.valid()
		|| (_articleSelectionDrag.active
			&& _articleSelectionDrag.mode == DragSelectionMode::Text);
	_selection = {};
	_selectionEndpoints = {};
	if (_articleSelectionDrag.mode == DragSelectionMode::Text) {
		finishArticleSelection();
	} else {
		_articleSelectionDrag.textSegment = -1;
		_articleSelectionDrag.textOffset = 0;
	}
	if (changed) {
		update();
	}
}

void Widget::clearStructuralSelection() {
	const auto changed = hasStructuralSelection()
		|| (_articleSelectionDrag.active
			&& _articleSelectionDrag.mode == DragSelectionMode::Structural);
	_structuralSelection = {};
	if (_articleSelectionDrag.mode == DragSelectionMode::Structural) {
		finishArticleSelection();
	}
	if (changed) {
		update();
	}
}

bool Widget::hasStructuralSelection() const {
	return !_structuralSelection.empty();
}

void Widget::startArticleSelection(
		QPoint pressPoint,
		const Markdown::MarkdownArticleHitTestResult &hit,
		const PreparedEditHit &editHit,
		bool fromField,
		bool startedBelow) {
	const auto isTextHit = hit.valid()
		&& !hit.codeHeaderCopy
		&& hit.direct
		&& _article->segmentIsText(hit.segmentIndex);
	const auto isDisplayMathHit = hit.valid()
		&& !hit.codeHeaderCopy
		&& hit.direct
		&& _article->segmentIsDisplayMath(hit.segmentIndex);
	const auto displayMathSegment = [&] {
		if (isDisplayMathHit) {
			return hit.segmentIndex;
		}
		if (editHit.leaf
			&& (editHit.leaf->kind
				== Markdown::PreparedEditLeafKind::MathFormula)) {
			const auto ordinal = _state->textOrdinalForLeaf(*editHit.leaf);
			return (ordinal >= 0)
				? segmentIndexForEditableOrdinal(ordinal)
				: -1;
		}
		return -1;
	}();
	if (isTextHit) {
		clearStructuralSelection();
	} else {
		clearTextSelection();
		clearStructuralSelection();
	}
	_articleSelectionDrag = {
		.active = true,
		.fromField = fromField,
		.startedBelow = startedBelow,
		.codeHeader = hit.codeHeaderCopy,
		.pressPoint = pressPoint,
		.anchorHit = editHit,
		.textSegment = -1,
		.textOffset = 0,
		.mode = DragSelectionMode::None,
	};
	if (!isTextHit) {
		if (displayMathSegment >= 0) {
			_articleSelectionDrag.textSegment = displayMathSegment;
			return;
		}
		if (editHit.valid() && !hit.codeHeaderCopy && !startedBelow) {
			_articleSelectionDrag.mode = DragSelectionMode::Structural;
		}
		return;
	}
	const auto offset = _article->selectionOffsetFromHit(
		hit,
		TextSelectType::Letters);
	_articleSelectionDrag.textSegment = hit.segmentIndex;
	_articleSelectionDrag.textOffset = offset;
	_articleSelectionDrag.mode = DragSelectionMode::Text;
	_selection = {
		{ hit.segmentIndex, offset },
		{ hit.segmentIndex, offset },
	};
	_selectionEndpoints = {
		.from = MakeSelectionEndpoint(hit),
		.to = MakeSelectionEndpoint(hit),
	};
	update();
}

void Widget::updateArticleSelection(
		QPoint articlePoint,
		const Markdown::MarkdownArticleHitTestResult &hit,
		const PreparedEditHit &editHit) {
	if (!_articleSelectionDrag.active) {
		return;
	}
	const auto dragSegment = _articleSelectionDrag.textSegment;
	const auto originalMathFormulaHit = [&] {
		return _articleSelectionDrag.anchorHit.leaf
			&& (_articleSelectionDrag.anchorHit.leaf->kind
				== Markdown::PreparedEditLeafKind::MathFormula)
			&& editHit.leaf
			&& (*editHit.leaf == *_articleSelectionDrag.anchorHit.leaf);
	};
	const auto directOriginalTextHit = [&] {
		return (dragSegment >= 0)
			&& hit.valid()
			&& hit.direct
			&& (hit.segmentIndex == dragSegment)
			&& _article->segmentIsText(hit.segmentIndex);
	};
	const auto directOriginalEditableHit = [&] {
		return ((dragSegment >= 0)
			&& hit.valid()
			&& hit.direct
			&& (hit.segmentIndex == dragSegment)
			&& _article->segmentIsEditable(hit.segmentIndex))
			|| originalMathFormulaHit();
	};
	const auto updateTextSelection = [&](bool forceUpdate) {
		const auto offset = _article->selectionOffsetFromHit(
			hit,
			TextSelectType::Letters);
		const auto adjusted = _article->adjustSelection(
			dragSegment,
			TextSelection(
				uint16(std::clamp(
					std::min(_articleSelectionDrag.textOffset, offset),
					0,
					0xFFFF)),
				uint16(std::clamp(
					std::max(_articleSelectionDrag.textOffset, offset),
					0,
					0xFFFF))),
			TextSelectType::Letters);
		const auto selection = NormalizeSelection({
			{ dragSegment, adjusted.from },
			{ dragSegment, adjusted.to },
		});
		const auto endpoints = Markdown::MarkdownArticleSelectionEndpoints{
			.from = _selectionEndpoints.from.valid()
				? _selectionEndpoints.from
				: Markdown::MarkdownArticleSelectionEndpoint{
					dragSegment,
					false },
			.to = MakeSelectionEndpoint(hit),
		};
		const auto endpointsChanged
			= (_selectionEndpoints.from.segment != endpoints.from.segment)
			|| (_selectionEndpoints.from.direct != endpoints.from.direct)
			|| (_selectionEndpoints.to.segment != endpoints.to.segment)
			|| (_selectionEndpoints.to.direct != endpoints.to.direct);
		if (_selection != selection || endpointsChanged || forceUpdate) {
			_selection = selection;
			_selectionEndpoints = endpoints;
			update();
		} else {
			_selectionEndpoints = endpoints;
		}
	};
	const auto clearFieldSelection = [&] {
		if (!_articleSelectionDrag.fromField) {
			return;
		}
		auto cursor = _field->textCursor();
		if (!cursor.hasSelection()) {
			return;
		}
		cursor.clearSelection();
		_field->setTextCursor(cursor);
	};
	if (_articleSelectionDrag.mode == DragSelectionMode::Structural) {
		if (directOriginalTextHit()) {
			const auto forceUpdate = !_structuralSelection.empty();
			_structuralSelection = {};
			_articleSelectionDrag.mode = DragSelectionMode::Text;
			updateTextSelection(forceUpdate);
			return;
		}
		if (directOriginalEditableHit()) {
			const auto changed = !_structuralSelection.empty();
			_structuralSelection = {};
			_articleSelectionDrag.mode = DragSelectionMode::None;
			if (changed) {
				update();
			}
			return;
		}
		const auto selection = structuralSelectionFromHits(
			_articleSelectionDrag.anchorHit,
			editHit);
		if (_structuralSelection != selection) {
			_structuralSelection = selection;
			update();
		}
		return;
	} else if (_articleSelectionDrag.mode == DragSelectionMode::None) {
		if (directOriginalEditableHit()) {
			return;
		}
		if (!editHit.valid()
			|| (_articleSelectionDrag.startedBelow
				&& articlePoint.y() >= _articleHeight)) {
			return;
		}
		_articleSelectionDrag.mode = DragSelectionMode::Structural;
		const auto selection = structuralSelectionFromHits(
			_articleSelectionDrag.anchorHit,
			editHit);
		if (_structuralSelection != selection) {
			_structuralSelection = selection;
			update();
		}
		return;
	}
	if (_articleSelectionDrag.mode != DragSelectionMode::Text) {
		return;
	}
	if (directOriginalTextHit()) {
		updateTextSelection(false);
		return;
	}
	const auto selection = structuralSelectionFromHits(
		_articleSelectionDrag.anchorHit,
		editHit);
	const auto changed = !_selection.empty()
		|| _selectionEndpoints.from.valid()
		|| _selectionEndpoints.to.valid()
		|| (_structuralSelection != selection);
	_selection = {};
	_selectionEndpoints = {};
	_structuralSelection = selection;
	_articleSelectionDrag.mode = DragSelectionMode::Structural;
	clearFieldSelection();
	if (changed) {
		update();
	}
}

void Widget::finishArticleSelection() {
	_articleSelectionDrag = {};
}

bool Widget::handleStructuralSelectionKey(QKeyEvent *e) {
	if (!hasStructuralSelection()) {
		return false;
	}
	const auto key = e->key();
	if (key == Qt::Key_Escape) {
		clearStructuralSelection();
		e->accept();
		return true;
	}
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	if (modifiers != Qt::NoModifier) {
		return false;
	}
	const auto forward = (key == Qt::Key_Delete);
	if (!forward && key != Qt::Key_Backspace) {
		return false;
	}
	const auto selection = _structuralSelection;
	commitInlineField();
	_pendingOrdinal = -1;
	_pendingCursorOffset = 0;
	hideInlineField();
	_article->clearTextLeafHeightOverride();
	const auto target = _state->removeStructuralSelection(
		selection,
		forward);
	clearSelection();
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
	e->accept();
	return true;
}

bool Widget::handleFieldMouseEvent(QEvent *event) {
	if (!_article || _field->isHidden() || _activeSegmentIndex < 0) {
		return false;
	}
	const auto type = event->type();
	const auto mouse = static_cast<QMouseEvent*>(event);
	if (type == QEvent::MouseButtonPress) {
		if (mouse->button() != Qt::LeftButton) {
			return false;
		}
		const auto segmentRect = _article->segmentRect(_activeSegmentIndex);
		if (segmentRect.isEmpty()) {
			return false;
		}
		auto anchorHit = _article->editHitTest(segmentRect.center());
		if (!anchorHit.valid()) {
			anchorHit = _article->editHitTest(segmentRect.topLeft());
		}
		if (!anchorHit.valid()) {
			return false;
		}
		clearTextSelection();
		clearStructuralSelection();
		const auto globalPoint = mouse->globalPos();
		const auto articlePoint = mapFromGlobal(globalPoint)
			- articleTopLeft();
		const auto cursor = _field->textCursor();
		_trackingPointerPress = true;
		_articleSelectionDrag = {
			.active = true,
			.fromField = true,
			.startedBelow = false,
			.codeHeader = false,
			.pressPoint = articlePoint,
			.anchorHit = anchorHit,
			.textSegment = _activeSegmentIndex,
			.textOffset = std::clamp(
				cursor.position(),
				0,
				int(_field->getLastText().size())),
			.mode = DragSelectionMode::Text,
		};
		return false;
	} else if (!_articleSelectionDrag.active
		|| !_articleSelectionDrag.fromField) {
		return false;
	} else if (type == QEvent::MouseButtonRelease
		&& mouse->button() != Qt::LeftButton) {
		return false;
	} else if (type == QEvent::MouseMove
		&& !(mouse->buttons() & Qt::LeftButton)) {
		finishArticleSelection();
		_trackingPointerPress = false;
		return false;
	}

	const auto globalPoint = mouse->globalPos();
	const auto articlePoint = mapFromGlobal(globalPoint) - articleTopLeft();
	const auto hit = _article->hitTest(
		articlePoint,
		Ui::Text::StateRequest::Flag::LookupSymbol);
	const auto editHit = _article->editHitTest(articlePoint);
	const auto insideActiveField = _field->rect().contains(
		_field->mapFromGlobal(globalPoint));
	const auto originalSegmentHit = hit.valid()
		&& hit.direct
		&& (hit.segmentIndex == _articleSelectionDrag.textSegment)
		&& _article->segmentIsEditable(hit.segmentIndex);
	const auto originalMathFormulaHit = _articleSelectionDrag.anchorHit.leaf
		&& (_articleSelectionDrag.anchorHit.leaf->kind
			== Markdown::PreparedEditLeafKind::MathFormula)
		&& editHit.leaf
		&& (*editHit.leaf == *_articleSelectionDrag.anchorHit.leaf);
	const auto clearArticleSelection = [&] {
		const auto changed = !_selection.empty()
			|| _selectionEndpoints.from.valid()
			|| _selectionEndpoints.to.valid()
			|| hasStructuralSelection();
		_selection = {};
		_selectionEndpoints = {};
		_structuralSelection = {};
		if (changed) {
			update();
		}
	};
	if (insideActiveField || originalSegmentHit || originalMathFormulaHit) {
		if (_articleSelectionDrag.mode == DragSelectionMode::Structural) {
			clearArticleSelection();
			_articleSelectionDrag.mode = DragSelectionMode::Text;
		}
		if (type == QEvent::MouseButtonRelease) {
			finishArticleSelection();
			_trackingPointerPress = false;
		}
		return false;
	}

	updateArticleSelection(articlePoint, hit, editHit);
	if (type == QEvent::MouseButtonRelease) {
		if (hasStructuralSelection()) {
			commitInlineField();
			_pendingOrdinal = -1;
			_pendingCursorOffset = 0;
			hideInlineField();
			_article->clearTextLeafHeightOverride();
			refreshPreparedContent();
			finishArticleSelection();
			_trackingPointerPress = false;
			mouse->accept();
			return true;
		}
		finishArticleSelection();
		_trackingPointerPress = false;
		return false;
	}
	if (_articleSelectionDrag.mode == DragSelectionMode::Structural) {
		mouse->accept();
		return true;
	}
	return false;
}

PreparedEditSelection Widget::structuralSelectionFromHits(
		const PreparedEditHit &anchor,
		const PreparedEditHit &focus) const {
	const auto anchorOwner = StructuralOwnerFromHit(anchor);
	const auto focusOwner = StructuralOwnerFromHit(focus);
	if (!anchorOwner.valid() || !focusOwner.valid()) {
		return {};
	}
	const auto anchorCell = TableCellFromOwner(anchorOwner);
	const auto focusCell = TableCellFromOwner(focusOwner);
	if (anchorCell
		&& focusCell
		&& SamePreparedEditBlockPath(anchorCell->block, focusCell->block)
		&& anchorCell->tableRowIndex == focusCell->tableRowIndex) {
		const auto range = NormalizeIntegerRange(
			anchorCell->tableCellIndex,
			focusCell->tableCellIndex);
		if (!range.empty()) {
			return {
				.kind = PreparedEditSelectionKind::TableCells,
				.tableCells = {
					.block = anchorCell->block,
					.tableRowIndex = anchorCell->tableRowIndex,
					.from = range.from,
					.till = range.till,
				},
			};
		}
	}
	const auto anchorRow = TableRowFromOwner(anchorOwner);
	const auto focusRow = TableRowFromOwner(focusOwner);
	if (anchorRow
		&& focusRow
		&& SamePreparedEditBlockPath(anchorRow->block, focusRow->block)) {
		const auto range = NormalizeIntegerRange(
			anchorRow->tableRowIndex,
			focusRow->tableRowIndex);
		if (!range.empty()) {
			return {
				.kind = PreparedEditSelectionKind::TableRows,
				.tableRows = {
					.block = anchorRow->block,
					.from = range.from,
					.till = range.till,
				},
			};
		}
	}
	const auto anchorListItem = ListItemFromOwner(anchorOwner);
	const auto focusListItem = ListItemFromOwner(focusOwner);
	if (anchorListItem
		&& focusListItem
		&& SamePreparedEditBlockPath(
			anchorListItem->block,
			focusListItem->block)) {
		const auto range = NormalizeIntegerRange(
			anchorListItem->listItemIndex,
			focusListItem->listItemIndex);
		if (!range.empty()) {
			return {
				.kind = PreparedEditSelectionKind::ListItems,
				.listItems = {
					.block = anchorListItem->block,
					.from = range.from,
					.till = range.till,
				},
			};
		}
	}
	const auto anchorBlock = BlockPathFromOwner(anchorOwner);
	const auto focusBlock = BlockPathFromOwner(focusOwner);
	if (!anchorBlock || !focusBlock) {
		return {};
	}
	if (ComparePreparedEditBlockContainerPaths(
			anchorBlock->container,
			focusBlock->container) == 0) {
		const auto blockSelection = BlockSelectionFromIndexes(
			anchorBlock->container,
			anchorBlock->index,
			focusBlock->index);
		if (!blockSelection.empty()) {
			return blockSelection;
		}
	}
	const auto listItemsFromChildren = ListItemSelectionFromSources(
		ListItemSourcesFromOwner(anchorOwner, anchorBlock),
		ListItemSourcesFromOwner(focusOwner, focusBlock));
	const auto liftedBlockSelection = LiftedBlockSelection(
		*anchorBlock,
		*focusBlock);
	if (IsBlockOwner(anchorOwner)
		&& IsBlockOwner(focusOwner)
		&& !liftedBlockSelection.empty()
		&& !IsMultiListItemSelection(listItemsFromChildren)) {
		return liftedBlockSelection;
	}
	if (!listItemsFromChildren.empty()) {
		return listItemsFromChildren;
	}
	if (!liftedBlockSelection.empty()) {
		return liftedBlockSelection;
	}
	return {};
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
	if (!_structuralSelection.empty()) {
		context.selectionState.structuralSelection = &_structuralSelection;
	}
	return context;
}

} // namespace Iv::Editor
