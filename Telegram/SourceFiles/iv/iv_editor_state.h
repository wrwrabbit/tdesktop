/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "iv/iv_rich_page.h"
#include "iv/markdown/iv_markdown_prepare.h"

#include <memory>
#include <vector>

namespace Iv::Editor {

class State final {
public:
	enum class TextSlot : uchar {
		Text,
		Caption,
	};

	struct TextNodeDescriptor {
		std::vector<int> blockPath;
		TextSlot slot = TextSlot::Text;
	};

	State();

	[[nodiscard]] const RichPage &richPage() const;
	[[nodiscard]] const Markdown::MarkdownArticleContent &prepared() const;
	[[nodiscard]] const std::vector<TextNodeDescriptor> &textNodes() const;
	[[nodiscard]] int textNodeCount() const;
	[[nodiscard]] int activeTextOrdinal() const;
	[[nodiscard]] bool setActiveTextByOrdinal(int ordinal);
	[[nodiscard]] TextWithEntities activeText() const;
	void applyActiveText(TextWithEntities text);
	[[nodiscard]] int ensureTrailingParagraphActive();
	void insertHeading1AfterActive();
	void insertBlockquoteAfterActive();

private:
	[[nodiscard]] RichPage::Block *blockFromPath(const std::vector<int> &path);
	[[nodiscard]] const RichPage::Block *blockFromPath(
		const std::vector<int> &path) const;
	[[nodiscard]] std::vector<RichPage::Block> *blockContainerFromPath(
		const std::vector<int> &path);
	[[nodiscard]] RichPage::RichText *richText(
		const TextNodeDescriptor &descriptor);
	[[nodiscard]] const RichPage::RichText *richText(
		const TextNodeDescriptor &descriptor) const;
	[[nodiscard]] const TextNodeDescriptor *textNode(int ordinal) const;
	[[nodiscard]] int textNodeOrdinal(
		const std::vector<int> &blockPath,
		TextSlot slot) const;

	void rebuild();
	void rebuildTextNodes();
	void ensureActiveTextOrdinal();
	void insertBlockAfterActive(RichPage::Block block, TextSlot slotToFocus);

	[[nodiscard]] static RichPage::Block makeParagraphBlock();
	[[nodiscard]] static RichPage::Block makeHeading1Block();
	[[nodiscard]] static RichPage::Block makeBlockquoteBlock();
	[[nodiscard]] static bool stripWrapperEntityInEditMode(EntityType type);
	[[nodiscard]] static TextWithEntities stripEditModeWrapperEntities(
		TextWithEntities text);

	std::shared_ptr<RichPage> _richPage;
	Markdown::MarkdownArticleContent _prepared;
	std::vector<TextNodeDescriptor> _textNodes;
	int _activeTextOrdinal = -1;
};

} // namespace Iv::Editor
