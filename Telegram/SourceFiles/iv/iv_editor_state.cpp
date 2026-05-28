/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/iv_editor_state.h"

#include <algorithm>
#include <utility>

namespace Iv::Editor {
namespace {

using Block = RichPage::Block;
using BlockContainerKind = State::BlockContainerKind;
using BlockContainerPath = State::BlockContainerPath;
using BlockKind = RichPage::BlockKind;
using BlockPath = State::BlockPath;
using FieldMode = State::FieldMode;
using InsertBlockType = State::InsertBlockType;
using InsertionAnchor = State::InsertionAnchor;
using LeafKind = State::LeafKind;
using LeafPath = State::LeafPath;
using ListItem = RichPage::ListItem;
using ListKind = RichPage::ListKind;
using RemovalKind = State::RemovalKind;
using RemovalTarget = State::RemovalTarget;
using RichText = RichPage::RichText;
using TableCell = RichPage::TableCell;
using TaskState = RichPage::TaskState;
using TextNodeDescriptor = State::TextNodeDescriptor;

[[nodiscard]] BlockContainerPath BlockChildrenContainer(BlockPath path) {
	auto result = std::move(path.container);
	result.steps.push_back({
		.kind = BlockContainerKind::BlockChildren,
		.blockIndex = path.index,
	});
	return result;
}

[[nodiscard]] BlockContainerPath ListItemChildrenContainer(
		BlockPath path,
		int itemIndex) {
	auto result = std::move(path.container);
	result.steps.push_back({
		.kind = BlockContainerKind::ListItemChildren,
		.blockIndex = path.index,
		.listItemIndex = itemIndex,
	});
	return result;
}

[[nodiscard]] bool ContainerHasPrefix(
		const BlockContainerPath &path,
		const BlockContainerPath &prefix) {
	if (path.steps.size() < prefix.steps.size()) {
		return false;
	}
	return std::equal(
		prefix.steps.begin(),
		prefix.steps.end(),
		path.steps.begin());
}

[[nodiscard]] bool StringIsEmpty(const QString &text) {
	return text.trimmed().isEmpty();
}

} // namespace

State::State()
: _richPage(std::make_shared<RichPage>()) {
	_richPage->blocks.push_back(MakeParagraphBlock());
	rebuild();
}

const RichPage &State::richPage() const {
	return *_richPage;
}

const Markdown::MarkdownArticleContent &State::prepared() const {
	return _prepared;
}

const std::vector<TextNodeDescriptor> &State::textNodes() const {
	return _textNodes;
}

int State::textNodeCount() const {
	return int(_textNodes.size());
}

int State::activeTextOrdinal() const {
	return _activeTextOrdinal;
}

bool State::setActiveTextByOrdinal(int ordinal) {
	if (ordinal < 0 || ordinal >= textNodeCount()) {
		return false;
	}
	_activeTextOrdinal = ordinal;
	return true;
}

TextWithEntities State::activeText() const {
	const auto descriptor = textNode(_activeTextOrdinal);
	if (!descriptor) {
		return TextWithEntities();
	}
	if (const auto current = richText(descriptor->leaf)) {
		return StripEditModeWrapperEntities(current->text);
	}
	if (const auto current = rawText(descriptor->leaf)) {
		return MakeText(*current);
	}
	return TextWithEntities();
}

void State::applyActiveText(TextWithEntities text) {
	const auto descriptor = textNode(_activeTextOrdinal);
	if (!descriptor) {
		return;
	}
	if (auto current = richText(descriptor->leaf)) {
		current->text = std::move(text);
		rebuild();
		return;
	}
	if (auto current = rawText(descriptor->leaf)) {
		*current = std::move(text.text);
		rebuild();
	}
}

FieldMode State::activeFieldMode() const {
	const auto descriptor = textNode(_activeTextOrdinal);
	return descriptor ? descriptor->mode : FieldMode::Rich;
}

QString State::activeRawText() const {
	const auto descriptor = textNode(_activeTextOrdinal);
	if (!descriptor) {
		return QString();
	}
	if (const auto current = rawText(descriptor->leaf)) {
		return *current;
	}
	if (const auto current = richText(descriptor->leaf)) {
		return StripEditModeWrapperEntities(current->text).text;
	}
	return QString();
}

void State::applyActiveRawText(QString text) {
	const auto descriptor = textNode(_activeTextOrdinal);
	if (!descriptor) {
		return;
	}
	if (auto current = rawText(descriptor->leaf)) {
		*current = std::move(text);
		rebuild();
		return;
	}
	if (auto current = richText(descriptor->leaf)) {
		current->text = MakeText(std::move(text));
		rebuild();
	}
}

int State::activeTextLength() const {
	return activeRawText().size();
}

std::optional<int> State::previousEditableOrdinal() const {
	return adjacentEditableOrdinal(false);
}

std::optional<int> State::nextEditableOrdinal() const {
	return adjacentEditableOrdinal(true);
}

bool State::isActiveTopLevelParagraph() const {
	const auto descriptor = textNode(_activeTextOrdinal);
	if (!descriptor || !descriptor->leaf.block.container.steps.empty()) {
		return false;
	}
	const auto owner = block(descriptor->leaf.block);
	return owner && owner->kind == BlockKind::Paragraph;
}

bool State::activeOwnerIsEmpty() const {
	const auto descriptor = textNode(_activeTextOrdinal);
	return descriptor && removalTargetIsEmpty(descriptor->removalTarget);
}

std::optional<int> State::removeActiveOwnerAndSelectAdjacent(bool forward) {
	const auto descriptor = textNode(_activeTextOrdinal);
	if (!descriptor || !removalTargetIsEmpty(descriptor->removalTarget)) {
		return std::nullopt;
	}
	const auto target = descriptor->removalTarget;
	auto first = _activeTextOrdinal;
	auto last = _activeTextOrdinal;
	while (first > 0 && _textNodes[first - 1].removalTarget == target) {
		--first;
	}
	while (last + 1 < textNodeCount()
		&& _textNodes[last + 1].removalTarget == target) {
		++last;
	}
	auto adjacent = std::optional<LeafPath>();
	if (forward) {
		for (auto i = last + 1, count = textNodeCount(); i != count; ++i) {
			if (!(_textNodes[i].removalTarget == target)) {
				adjacent = _textNodes[i].leaf;
				break;
			}
		}
	} else {
		for (auto i = first; i != 0; --i) {
			if (!(_textNodes[i - 1].removalTarget == target)) {
				adjacent = _textNodes[i - 1].leaf;
				break;
			}
		}
	}
	if (!removeTarget(target)) {
		return std::nullopt;
	}
	rebuild();
	if (adjacent && (!forward || target.kind == RemovalKind::TableCell)) {
		const auto ordinal = textNodeOrdinal(*adjacent);
		if (setActiveTextByOrdinal(ordinal)) {
			return _activeTextOrdinal;
		}
	}
	if (!textNodeCount()) {
		return std::nullopt;
	}
	const auto fallback = forward
		? std::min(first, textNodeCount() - 1)
		: std::max(std::min(first - 1, textNodeCount() - 1), 0);
	if (!setActiveTextByOrdinal(fallback)) {
		ensureActiveTextOrdinal();
	}
	return _activeTextOrdinal;
}

int State::ensureTrailingParagraphActive() {
	if (_richPage->blocks.empty()
		|| _richPage->blocks.back().kind != BlockKind::Paragraph) {
		_richPage->blocks.push_back(MakeParagraphBlock());
	}
	const auto path = BlockPath{
		.container = BlockContainerPath(),
		.index = int(_richPage->blocks.size()) - 1,
	};
	rebuild();
	const auto ordinal = textNodeOrdinal({
			.kind = LeafKind::BlockText,
			.block = path,
		});
	if (!setActiveTextByOrdinal(ordinal)) {
		ensureActiveTextOrdinal();
	}
	return _activeTextOrdinal;
}

void State::insertHeading1AfterActive() {
	insertBlockAfterActive({
		.type = InsertBlockType::Heading,
		.headingLevel = 1,
	});
}

void State::insertBlockquoteAfterActive() {
	insertBlockAfterActive({
		.type = InsertBlockType::Blockquote,
	});
}

void State::insertBlockAfterActive(InsertAction action) {
	auto anchor = InsertionAnchor{
		.container = BlockContainerPath(),
		.blockIndex = int(_richPage->blocks.size()) - 1,
	};
	const auto descriptor = textNode(_activeTextOrdinal);
	if (descriptor) {
		anchor = descriptor->insertionAnchor;
	}
	auto *blocks = blockContainer(anchor.container);
	if (!blocks) {
		anchor = {
			.container = BlockContainerPath(),
			.blockIndex = int(_richPage->blocks.size()) - 1,
		};
		blocks = &_richPage->blocks;
	}
	const auto insertAt = std::clamp(
		anchor.blockIndex + 1,
		0,
		int(blocks->size()));
	blocks->insert(blocks->begin() + insertAt, makeBlock(action));
	rebuild();
	focusInsertedBlock({
		.container = anchor.container,
		.index = insertAt,
	});
}

std::vector<Block> *State::blockContainer(const BlockContainerPath &path) {
	auto *blocks = &_richPage->blocks;
	for (const auto &step : path.steps) {
		if (step.blockIndex < 0 || step.blockIndex >= int(blocks->size())) {
			return nullptr;
		}
		auto &parent = (*blocks)[step.blockIndex];
		if (step.kind == BlockContainerKind::BlockChildren) {
			blocks = &parent.blocks;
		} else if (step.kind == BlockContainerKind::ListItemChildren) {
			if (step.listItemIndex < 0
				|| step.listItemIndex >= int(parent.listItems.size())) {
				return nullptr;
			}
			blocks = &parent.listItems[step.listItemIndex].blocks;
		} else {
			return nullptr;
		}
	}
	return blocks;
}

const std::vector<Block> *State::blockContainer(
		const BlockContainerPath &path) const {
	const auto *blocks = &_richPage->blocks;
	for (const auto &step : path.steps) {
		if (step.blockIndex < 0 || step.blockIndex >= int(blocks->size())) {
			return nullptr;
		}
		const auto &parent = (*blocks)[step.blockIndex];
		if (step.kind == BlockContainerKind::BlockChildren) {
			blocks = &parent.blocks;
		} else if (step.kind == BlockContainerKind::ListItemChildren) {
			if (step.listItemIndex < 0
				|| step.listItemIndex >= int(parent.listItems.size())) {
				return nullptr;
			}
			blocks = &parent.listItems[step.listItemIndex].blocks;
		} else {
			return nullptr;
		}
	}
	return blocks;
}

Block *State::block(const BlockPath &path) {
	const auto blocks = blockContainer(path.container);
	if (!blocks || path.index < 0 || path.index >= int(blocks->size())) {
		return nullptr;
	}
	return &(*blocks)[path.index];
}

const Block *State::block(const BlockPath &path) const {
	const auto blocks = blockContainer(path.container);
	if (!blocks || path.index < 0 || path.index >= int(blocks->size())) {
		return nullptr;
	}
	return &(*blocks)[path.index];
}

ListItem *State::listItem(const BlockPath &blockPath, int itemIndex) {
	const auto list = block(blockPath);
	if (!list
		|| itemIndex < 0
		|| itemIndex >= int(list->listItems.size())) {
		return nullptr;
	}
	return &list->listItems[itemIndex];
}

const ListItem *State::listItem(
		const BlockPath &blockPath,
		int itemIndex) const {
	const auto list = block(blockPath);
	if (!list
		|| itemIndex < 0
		|| itemIndex >= int(list->listItems.size())) {
		return nullptr;
	}
	return &list->listItems[itemIndex];
}

TableCell *State::tableCell(
		const BlockPath &blockPath,
		int rowIndex,
		int cellIndex) {
	const auto table = block(blockPath);
	if (!table
		|| rowIndex < 0
		|| rowIndex >= int(table->tableRows.size())) {
		return nullptr;
	}
	auto &row = table->tableRows[rowIndex];
	if (cellIndex < 0 || cellIndex >= int(row.cells.size())) {
		return nullptr;
	}
	return &row.cells[cellIndex];
}

const TableCell *State::tableCell(
		const BlockPath &blockPath,
		int rowIndex,
		int cellIndex) const {
	const auto table = block(blockPath);
	if (!table
		|| rowIndex < 0
		|| rowIndex >= int(table->tableRows.size())) {
		return nullptr;
	}
	const auto &row = table->tableRows[rowIndex];
	if (cellIndex < 0 || cellIndex >= int(row.cells.size())) {
		return nullptr;
	}
	return &row.cells[cellIndex];
}

RichText *State::richText(const LeafPath &path) {
	switch (path.kind) {
	case LeafKind::BlockText:
		if (const auto owner = block(path.block)) {
			return &owner->text;
		}
		return nullptr;
	case LeafKind::BlockCaption:
		if (const auto owner = block(path.block)) {
			return &owner->caption;
		}
		return nullptr;
	case LeafKind::ListItemText:
		if (const auto item = listItem(path.block, path.listItemIndex)) {
			return &item->text;
		}
		return nullptr;
	case LeafKind::TableCellText:
		if (const auto cell = tableCell(
				path.block,
				path.tableRowIndex,
				path.tableCellIndex)) {
			return &cell->text;
		}
		return nullptr;
	case LeafKind::MathFormula:
		return nullptr;
	}
	return nullptr;
}

const RichText *State::richText(const LeafPath &path) const {
	switch (path.kind) {
	case LeafKind::BlockText:
		if (const auto owner = block(path.block)) {
			return &owner->text;
		}
		return nullptr;
	case LeafKind::BlockCaption:
		if (const auto owner = block(path.block)) {
			return &owner->caption;
		}
		return nullptr;
	case LeafKind::ListItemText:
		if (const auto item = listItem(path.block, path.listItemIndex)) {
			return &item->text;
		}
		return nullptr;
	case LeafKind::TableCellText:
		if (const auto cell = tableCell(
				path.block,
				path.tableRowIndex,
				path.tableCellIndex)) {
			return &cell->text;
		}
		return nullptr;
	case LeafKind::MathFormula:
		return nullptr;
	}
	return nullptr;
}

QString *State::rawText(const LeafPath &path) {
	if (path.kind != LeafKind::MathFormula) {
		return nullptr;
	}
	const auto owner = block(path.block);
	return owner ? &owner->formula : nullptr;
}

const QString *State::rawText(const LeafPath &path) const {
	if (path.kind != LeafKind::MathFormula) {
		return nullptr;
	}
	const auto owner = block(path.block);
	return owner ? &owner->formula : nullptr;
}

const TextNodeDescriptor *State::textNode(int ordinal) const {
	return (ordinal >= 0 && ordinal < textNodeCount())
		? &_textNodes[ordinal]
		: nullptr;
}

int State::textNodeOrdinal(const LeafPath &path) const {
	for (auto i = 0, count = textNodeCount(); i != count; ++i) {
		if (_textNodes[i].leaf == path) {
			return i;
		}
	}
	return -1;
}

void State::rebuild() {
	rebuildTextNodes();
	ensureEditableNodes();
	ensureActiveTextOrdinal();
	_prepared = Markdown::TryPrepareNativeInstantView({
		.richPage = _richPage,
		.mediaRuntime = nullptr,
		.editMode = true,
	}).content;
}

void State::rebuildTextNodes() {
	_textNodes.clear();
	_textNodes.reserve(_richPage->blocks.size() * 3);
	rebuildTextNodes(_richPage->blocks, BlockContainerPath());
}

void State::rebuildTextNodes(
		const std::vector<Block> &blocks,
		const BlockContainerPath &container) {
	for (auto i = 0, count = int(blocks.size()); i != count; ++i) {
		const auto path = BlockPath{
			.container = container,
			.index = i,
		};
		const auto &block = blocks[i];
		switch (block.kind) {
		case BlockKind::Heading:
		case BlockKind::Paragraph:
			appendBlockTextNode(path, LeafKind::BlockText);
			break;
		case BlockKind::Quote:
			appendBlockTextNode(
				path,
				LeafKind::BlockText,
				FieldMode::Rich,
				InsertionAnchor{
					.container = BlockChildrenContainer(path),
					.blockIndex = -1,
				});
			appendBlockTextNode(
				path,
				LeafKind::BlockCaption,
				FieldMode::Rich,
				InsertionAnchor{
					.container = BlockChildrenContainer(path),
					.blockIndex = -1,
				});
			rebuildTextNodes(block.blocks, BlockChildrenContainer(path));
			break;
		case BlockKind::List:
			for (auto j = 0, itemCount = int(block.listItems.size());
					j != itemCount;
					++j) {
				const auto &item = block.listItems[j];
				if (!RichTextIsEmpty(item.text) || item.blocks.empty()) {
					appendListItemTextNode(path, j);
				}
				if (!item.blocks.empty()) {
					rebuildTextNodes(
						item.blocks,
						ListItemChildrenContainer(path, j));
				}
			}
			break;
		case BlockKind::Photo:
		case BlockKind::Video:
		case BlockKind::Audio:
		case BlockKind::Map:
			appendBlockTextNode(path, LeafKind::BlockCaption);
			break;
		case BlockKind::Math:
			appendBlockTextNode(path, LeafKind::MathFormula, FieldMode::Raw);
			break;
		case BlockKind::Table:
			if (!block.text.text.text.isEmpty()) {
				appendBlockTextNode(path, LeafKind::BlockText);
			}
			for (auto j = 0, rowCount = int(block.tableRows.size());
					j != rowCount;
					++j) {
				const auto &row = block.tableRows[j];
				for (auto k = 0, cellCount = int(row.cells.size());
						k != cellCount;
						++k) {
					appendTableCellTextNode(path, j, k);
				}
			}
			break;
		case BlockKind::Details:
			appendBlockTextNode(
				path,
				LeafKind::BlockText,
				FieldMode::Rich,
				InsertionAnchor{
					.container = BlockChildrenContainer(path),
					.blockIndex = -1,
				});
			rebuildTextNodes(block.blocks, BlockChildrenContainer(path));
			break;
		default:
			rebuildTextNodes(block.blocks, BlockChildrenContainer(path));
			break;
		}
	}
}

void State::appendBlockTextNode(
		const BlockPath &path,
		LeafKind kind,
		FieldMode mode,
		std::optional<InsertionAnchor> insertionAnchor) {
	_textNodes.push_back({
		.leaf = {
			.kind = kind,
			.block = path,
		},
		.insertionAnchor = insertionAnchor.value_or(InsertionAnchor{
			.container = path.container,
			.blockIndex = path.index,
		}),
		.removalTarget = {
			.kind = RemovalKind::Block,
			.block = path,
		},
		.mode = mode,
	});
}

void State::appendListItemTextNode(const BlockPath &path, int itemIndex) {
	_textNodes.push_back({
		.leaf = {
			.kind = LeafKind::ListItemText,
			.block = path,
			.listItemIndex = itemIndex,
		},
		.insertionAnchor = {
			.container = ListItemChildrenContainer(path, itemIndex),
			.blockIndex = -1,
		},
		.removalTarget = {
			.kind = RemovalKind::ListItem,
			.block = path,
			.listItemIndex = itemIndex,
		},
		.mode = FieldMode::Rich,
	});
}

void State::appendTableCellTextNode(
		const BlockPath &path,
		int rowIndex,
		int cellIndex) {
	_textNodes.push_back({
		.leaf = {
			.kind = LeafKind::TableCellText,
			.block = path,
			.tableRowIndex = rowIndex,
			.tableCellIndex = cellIndex,
		},
		.insertionAnchor = {
			.container = path.container,
			.blockIndex = path.index,
		},
		.removalTarget = {
			.kind = RemovalKind::TableCell,
			.block = path,
			.tableRowIndex = rowIndex,
			.tableCellIndex = cellIndex,
		},
		.mode = FieldMode::Rich,
	});
}

void State::ensureActiveTextOrdinal() {
	if (_textNodes.empty()) {
		_activeTextOrdinal = -1;
	} else if (_activeTextOrdinal < 0 || _activeTextOrdinal >= textNodeCount()) {
		_activeTextOrdinal = 0;
	}
}

void State::ensureEditableNodes() {
	if (!_textNodes.empty()) {
		return;
	}
	_richPage->blocks.push_back(MakeParagraphBlock());
	rebuildTextNodes();
}

void State::focusInsertedBlock(const BlockPath &path) {
	for (auto i = 0, count = textNodeCount(); i != count; ++i) {
		if (descriptorBelongsToBlock(_textNodes[i], path)) {
			if (setActiveTextByOrdinal(i)) {
				return;
			}
		}
	}
	ensureActiveTextOrdinal();
}

std::optional<int> State::adjacentEditableOrdinal(bool forward) const {
	if (_activeTextOrdinal < 0) {
		return std::nullopt;
	}
	const auto ordinal = _activeTextOrdinal + (forward ? 1 : -1);
	return (ordinal >= 0 && ordinal < textNodeCount())
		? std::make_optional(ordinal)
		: std::nullopt;
}

bool State::descriptorBelongsToBlock(
		const TextNodeDescriptor &descriptor,
		const BlockPath &path) const {
	const auto &owner = descriptor.leaf.block;
	if (owner == path) {
		return true;
	}
	if (!ContainerHasPrefix(owner.container, path.container)) {
		return false;
	}
	if (owner.container.steps.size() <= path.container.steps.size()) {
		return false;
	}
	const auto &step = owner.container.steps[path.container.steps.size()];
	return step.blockIndex == path.index
		&& (step.kind == BlockContainerKind::BlockChildren
			|| step.kind == BlockContainerKind::ListItemChildren);
}

bool State::removalTargetIsEmpty(const RemovalTarget &target) const {
	switch (target.kind) {
	case RemovalKind::Block:
		if (const auto owner = block(target.block)) {
			return BlockIsEmpty(*owner);
		}
		return false;
	case RemovalKind::ListItem:
		if (const auto item = listItem(target.block, target.listItemIndex)) {
			return ListItemIsEmpty(*item);
		}
		return false;
	case RemovalKind::TableCell:
		if (const auto cell = tableCell(
				target.block,
				target.tableRowIndex,
				target.tableCellIndex)) {
			return RichTextIsEmpty(cell->text);
		}
		return false;
	}
	return false;
}

bool State::removeTarget(const RemovalTarget &target) {
	switch (target.kind) {
	case RemovalKind::Block: {
		const auto blocks = blockContainer(target.block.container);
		if (!blocks
			|| target.block.index < 0
			|| target.block.index >= int(blocks->size())) {
			return false;
		}
		blocks->erase(blocks->begin() + target.block.index);
		return true;
	}
	case RemovalKind::ListItem: {
		const auto owner = block(target.block);
		if (!owner
			|| target.listItemIndex < 0
			|| target.listItemIndex >= int(owner->listItems.size())) {
			return false;
		}
		owner->listItems.erase(owner->listItems.begin() + target.listItemIndex);
		return true;
	}
	case RemovalKind::TableCell: {
		const auto cell = tableCell(
			target.block,
			target.tableRowIndex,
			target.tableCellIndex);
		if (!cell) {
			return false;
		}
		cell->text = RichText();
		return true;
	}
	}
	return false;
}

bool State::anchorIdExists(const QString &id) const {
	return anchorIdExists(_richPage->blocks, id);
}

bool State::anchorIdExists(
		const std::vector<Block> &blocks,
		const QString &id) const {
	for (const auto &block : blocks) {
		if (block.anchorId == id
			|| block.text.anchorId == id
			|| block.caption.anchorId == id) {
			return true;
		}
		if (anchorIdExists(block.blocks, id)) {
			return true;
		}
		for (const auto &item : block.listItems) {
			if (item.anchorId == id || item.text.anchorId == id) {
				return true;
			}
			if (anchorIdExists(item.blocks, id)) {
				return true;
			}
		}
		for (const auto &row : block.tableRows) {
			for (const auto &cell : row.cells) {
				if (cell.text.anchorId == id) {
					return true;
				}
			}
		}
	}
	return false;
}

QString State::nextAnchorId() const {
	for (auto i = 1;; ++i) {
		auto result = u"anchor-%1"_q.arg(i);
		if (!anchorIdExists(result)) {
			return result;
		}
	}
}

Block State::makeBlock(InsertAction action) const {
	switch (action.type) {
	case InsertBlockType::Heading:
		return MakeHeadingBlock(action.headingLevel);
	case InsertBlockType::Blockquote:
		return MakeQuoteBlock(false);
	case InsertBlockType::Math:
		return MakeMathBlock();
	case InsertBlockType::Footer:
		return MakeParagraphBlock();
	case InsertBlockType::Divider:
		return MakeDividerBlock();
	case InsertBlockType::Anchor:
		return MakeAnchorBlock(nextAnchorId());
	case InsertBlockType::OrderedList:
		return MakeListBlock(ListKind::Ordered);
	case InsertBlockType::BulletList:
		return MakeListBlock(ListKind::Bullet);
	case InsertBlockType::TaskList:
		return MakeListBlock(ListKind::Bullet, TaskState::Unchecked);
	case InsertBlockType::Pullquote:
		return MakeQuoteBlock(true);
	case InsertBlockType::Photo:
		return MakeMediaBlock(BlockKind::Photo);
	case InsertBlockType::Video:
		return MakeMediaBlock(BlockKind::Video);
	case InsertBlockType::Audio:
		return MakeMediaBlock(BlockKind::Audio);
	case InsertBlockType::Details:
		return MakeDetailsBlock();
	case InsertBlockType::Table:
		return MakeTableBlock();
	case InsertBlockType::Map:
		return MakeMapBlock(action.latitude, action.longitude);
	}
	return MakeParagraphBlock();
}

TextWithEntities State::MakeText(QString text) {
	auto result = TextWithEntities();
	result.text = std::move(text);
	return result;
}

Block State::MakeParagraphBlock() {
	auto block = Block();
	block.kind = BlockKind::Paragraph;
	block.text.text = MakeText(u"Text"_q);
	return block;
}

Block State::MakeHeadingBlock(int level) {
	auto block = Block();
	block.kind = BlockKind::Heading;
	block.headingLevel = std::clamp(level, 1, 6);
	block.text.text = MakeText(u"Header"_q);
	return block;
}

Block State::MakeQuoteBlock(bool pullquote) {
	auto block = Block();
	block.kind = BlockKind::Quote;
	block.pullquote = pullquote;
	block.text.text = MakeText(u"Text"_q);
	block.caption.text = MakeText(u"Caption"_q);
	return block;
}

Block State::MakeMathBlock() {
	auto block = Block();
	block.kind = BlockKind::Math;
	block.formula = u"x^2 + y^2"_q;
	return block;
}

Block State::MakeDividerBlock() {
	auto block = Block();
	block.kind = BlockKind::Divider;
	return block;
}

Block State::MakeAnchorBlock(QString anchorId) {
	auto block = Block();
	block.kind = BlockKind::Anchor;
	block.anchorId = std::move(anchorId);
	return block;
}

Block State::MakeListBlock(ListKind kind, TaskState taskState) {
	auto block = Block();
	block.kind = BlockKind::List;
	block.listKind = kind;
	block.listItems.reserve(3);
	for (auto i = 0; i != 3; ++i) {
		auto item = ListItem();
		item.taskState = taskState;
		item.text.text = MakeText(u"Text"_q);
		block.listItems.push_back(std::move(item));
	}
	return block;
}

Block State::MakeDetailsBlock() {
	auto block = Block();
	block.kind = BlockKind::Details;
	block.open = true;
	block.text.text = MakeText(u"Header"_q);
	block.blocks.push_back(MakeParagraphBlock());
	return block;
}

Block State::MakeTableBlock() {
	auto block = Block();
	block.kind = BlockKind::Table;
	block.tableRows.reserve(3);
	for (auto rowIndex = 0; rowIndex != 3; ++rowIndex) {
		auto row = RichPage::TableRow();
		row.cells.reserve(3);
		for (auto cellIndex = 0; cellIndex != 3; ++cellIndex) {
			auto cell = TableCell();
			cell.header = (rowIndex == 0);
			cell.text.text = MakeText(u"Cell"_q);
			row.cells.push_back(std::move(cell));
		}
		block.tableRows.push_back(std::move(row));
	}
	return block;
}

Block State::MakeMediaBlock(BlockKind kind) {
	auto block = Block();
	block.kind = kind;
	block.caption.text = MakeText(u"Caption"_q);
	return block;
}

Block State::MakeMapBlock(double latitude, double longitude) {
	auto block = Block();
	block.kind = BlockKind::Map;
	block.latitude = latitude;
	block.longitude = longitude;
	block.caption.text = MakeText(u"Caption"_q);
	return block;
}

bool State::RichTextIsEmpty(const RichText &text) {
	return StringIsEmpty(text.text.text);
}

bool State::ListItemIsEmpty(const ListItem &item) {
	if (!RichTextIsEmpty(item.text) || !item.anchorId.isEmpty()) {
		return false;
	}
	for (const auto &block : item.blocks) {
		if (!BlockIsEmpty(block)) {
			return false;
		}
	}
	return true;
}

bool State::BlockIsEmpty(const Block &block) {
	if (!RichTextIsEmpty(block.text)
		|| !RichTextIsEmpty(block.caption)
		|| !StringIsEmpty(block.formula)
		|| !block.anchorId.isEmpty()
		|| !block.url.isEmpty()
		|| !block.html.isEmpty()
		|| !block.author.isEmpty()
		|| !block.username.isEmpty()
		|| !block.channelTitle.isEmpty()
		|| !block.audioTitle.isEmpty()
		|| !block.audioPerformer.isEmpty()
		|| !block.audioFileName.isEmpty()
		|| block.photo
		|| block.document
		|| block.peer
		|| block.photoId
		|| block.documentId
		|| block.channelId
		|| block.accessHash
		|| block.latitude != 0.
		|| block.longitude != 0.) {
		return false;
	}
	for (const auto &child : block.blocks) {
		if (!BlockIsEmpty(child)) {
			return false;
		}
	}
	for (const auto &item : block.listItems) {
		if (!ListItemIsEmpty(item)) {
			return false;
		}
	}
	for (const auto &row : block.tableRows) {
		for (const auto &cell : row.cells) {
			if (!RichTextIsEmpty(cell.text)) {
				return false;
			}
		}
	}
	return true;
}

bool State::StripWrapperEntityInEditMode(EntityType type) {
	switch (type) {
	case EntityType::Url:
	case EntityType::Email:
	case EntityType::Hashtag:
	case EntityType::Cashtag:
	case EntityType::Mention:
	case EntityType::BotCommand:
	case EntityType::Phone:
	case EntityType::BankCard:
		return true;
	default:
		return false;
	}
}

TextWithEntities State::StripEditModeWrapperEntities(TextWithEntities text) {
	auto filtered = EntitiesInText();
	filtered.reserve(text.entities.size());
	for (const auto &entity : text.entities) {
		if (!StripWrapperEntityInEditMode(entity.type())) {
			filtered.push_back(entity);
		}
	}
	text.entities = std::move(filtered);
	return text;
}

} // namespace Iv::Editor
