/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/iv_editor_state.h"

#include <algorithm>

namespace Iv::Editor {
namespace {

using Block = RichPage::Block;
using RichText = RichPage::RichText;
using BlockKind = RichPage::BlockKind;
using TextSlot = State::TextSlot;
using TextNodeDescriptor = State::TextNodeDescriptor;

} // namespace

State::State()
: _richPage(std::make_shared<RichPage>()) {
	_richPage->blocks.push_back(makeParagraphBlock());
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
	const auto current = descriptor ? richText(*descriptor) : nullptr;
	return current
		? stripEditModeWrapperEntities(current->text)
		: TextWithEntities();
}

void State::applyActiveText(TextWithEntities text) {
	const auto descriptor = textNode(_activeTextOrdinal);
	const auto current = descriptor ? richText(*descriptor) : nullptr;
	if (!current) {
		return;
	}
	current->text = std::move(text);
	rebuild();
}

int State::ensureTrailingParagraphActive() {
	if (_richPage->blocks.empty()
		|| _richPage->blocks.back().kind != BlockKind::Paragraph) {
		_richPage->blocks.push_back(makeParagraphBlock());
	}
	const auto path = std::vector<int>{ int(_richPage->blocks.size()) - 1 };
	rebuild();
	if (!setActiveTextByOrdinal(textNodeOrdinal(path, TextSlot::Text))) {
		ensureActiveTextOrdinal();
	}
	return _activeTextOrdinal;
}

void State::insertHeading1AfterActive() {
	insertBlockAfterActive(makeHeading1Block(), TextSlot::Text);
}

void State::insertBlockquoteAfterActive() {
	insertBlockAfterActive(makeBlockquoteBlock(), TextSlot::Text);
}

Block *State::blockFromPath(const std::vector<int> &path) {
	auto *blocks = &_richPage->blocks;
	auto *result = static_cast<Block*>(nullptr);
	for (const auto index : path) {
		if (index < 0 || index >= int(blocks->size())) {
			return nullptr;
		}
		result = &(*blocks)[index];
		blocks = &result->blocks;
	}
	return result;
}

const Block *State::blockFromPath(const std::vector<int> &path) const {
	const auto *blocks = &_richPage->blocks;
	auto *result = static_cast<const Block*>(nullptr);
	for (const auto index : path) {
		if (index < 0 || index >= int(blocks->size())) {
			return nullptr;
		}
		result = &(*blocks)[index];
		blocks = &result->blocks;
	}
	return result;
}

std::vector<Block> *State::blockContainerFromPath(const std::vector<int> &path) {
	if (path.empty()) {
		return &_richPage->blocks;
	}
	const auto parent = blockFromPath(path);
	return parent ? &parent->blocks : nullptr;
}

RichText *State::richText(const TextNodeDescriptor &descriptor) {
	const auto block = blockFromPath(descriptor.blockPath);
	if (!block) {
		return nullptr;
	}
	return (descriptor.slot == TextSlot::Caption) ? &block->caption : &block->text;
}

const RichText *State::richText(const TextNodeDescriptor &descriptor) const {
	const auto block = blockFromPath(descriptor.blockPath);
	if (!block) {
		return nullptr;
	}
	return (descriptor.slot == TextSlot::Caption) ? &block->caption : &block->text;
}

const TextNodeDescriptor *State::textNode(int ordinal) const {
	return (ordinal >= 0 && ordinal < textNodeCount())
		? &_textNodes[ordinal]
		: nullptr;
}

int State::textNodeOrdinal(
		const std::vector<int> &blockPath,
		TextSlot slot) const {
	for (auto i = 0, count = textNodeCount(); i != count; ++i) {
		const auto &descriptor = _textNodes[i];
		if (descriptor.slot == slot && descriptor.blockPath == blockPath) {
			return i;
		}
	}
	return -1;
}

void State::rebuild() {
	rebuildTextNodes();
	ensureActiveTextOrdinal();
	_prepared = Markdown::TryPrepareNativeInstantView({
		.richPage = _richPage,
		.mediaRuntime = nullptr,
		.editMode = true,
	}).content;
}

void State::rebuildTextNodes() {
	_textNodes.clear();
	_textNodes.reserve(_richPage->blocks.size() * 2);
	for (auto i = 0, count = int(_richPage->blocks.size()); i != count; ++i) {
		const auto path = std::vector<int>{ i };
		const auto &block = _richPage->blocks[i];
		switch (block.kind) {
		case BlockKind::Heading:
		case BlockKind::Paragraph:
			_textNodes.push_back({
				.blockPath = path,
				.slot = TextSlot::Text,
			});
			break;
		case BlockKind::Quote:
			_textNodes.push_back({
				.blockPath = path,
				.slot = TextSlot::Text,
			});
			_textNodes.push_back({
				.blockPath = path,
				.slot = TextSlot::Caption,
			});
			break;
		default:
			break;
		}
	}
}

void State::ensureActiveTextOrdinal() {
	if (_textNodes.empty()) {
		_activeTextOrdinal = -1;
	} else if (_activeTextOrdinal < 0 || _activeTextOrdinal >= textNodeCount()) {
		_activeTextOrdinal = 0;
	}
}

void State::insertBlockAfterActive(Block block, TextSlot slotToFocus) {
	auto parentPath = std::vector<int>();
	auto insertionIndex = int(_richPage->blocks.size());
	if (const auto descriptor = textNode(_activeTextOrdinal)) {
		parentPath = descriptor->blockPath;
		if (!parentPath.empty()) {
			insertionIndex = parentPath.back() + 1;
			parentPath.pop_back();
		}
	}
	auto *blocks = blockContainerFromPath(parentPath);
	if (!blocks) {
		parentPath.clear();
		blocks = &_richPage->blocks;
		insertionIndex = int(blocks->size());
	} else {
		insertionIndex = std::clamp(insertionIndex, 0, int(blocks->size()));
	}
	blocks->insert(blocks->begin() + insertionIndex, std::move(block));
	auto insertedPath = parentPath;
	insertedPath.push_back(insertionIndex);
	rebuild();
	if (!setActiveTextByOrdinal(textNodeOrdinal(insertedPath, slotToFocus))) {
		ensureActiveTextOrdinal();
	}
}

Block State::makeParagraphBlock() {
	auto block = Block();
	block.kind = BlockKind::Paragraph;
	return block;
}

Block State::makeHeading1Block() {
	auto block = Block();
	block.kind = BlockKind::Heading;
	block.headingLevel = 1;
	return block;
}

Block State::makeBlockquoteBlock() {
	auto block = Block();
	block.kind = BlockKind::Quote;
	block.pullquote = false;
	return block;
}

bool State::stripWrapperEntityInEditMode(EntityType type) {
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

TextWithEntities State::stripEditModeWrapperEntities(TextWithEntities text) {
	auto filtered = EntitiesInText();
	filtered.reserve(text.entities.size());
	for (const auto &entity : text.entities) {
		if (!stripWrapperEntityInEditMode(entity.type())) {
			filtered.push_back(entity);
		}
	}
	text.entities = std::move(filtered);
	return text;
}

} // namespace Iv::Editor
