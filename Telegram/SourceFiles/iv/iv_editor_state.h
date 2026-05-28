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
#include <optional>
#include <vector>

namespace Iv::Editor {

class State final {
public:
	enum class FieldMode : uchar {
		Rich,
		Raw,
	};

	enum class InsertBlockType : uchar {
		Heading,
		Blockquote,
		Math,
		Footer,
		Divider,
		Anchor,
		OrderedList,
		BulletList,
		TaskList,
		Pullquote,
		Photo,
		Video,
		Audio,
		Details,
		Table,
		Map,
	};

	struct InsertAction {
		InsertBlockType type;
		int headingLevel = 1;
		double latitude = 0.;
		double longitude = 0.;
	};

	enum class BlockContainerKind : uchar {
		Root,
		BlockChildren,
		ListItemChildren,
	};

	struct BlockContainerStep {
		BlockContainerKind kind = BlockContainerKind::BlockChildren;
		int blockIndex = -1;
		int listItemIndex = -1;

		friend inline bool operator==(
				const BlockContainerStep &a,
				const BlockContainerStep &b) {
			return (a.kind == b.kind)
				&& (a.blockIndex == b.blockIndex)
				&& (a.listItemIndex == b.listItemIndex);
		}
	};

	struct BlockContainerPath {
		std::vector<BlockContainerStep> steps;

		friend inline bool operator==(
				const BlockContainerPath &a,
				const BlockContainerPath &b) {
			return (a.steps == b.steps);
		}
	};

	struct BlockPath {
		BlockContainerPath container;
		int index = -1;

		friend inline bool operator==(
				const BlockPath &a,
				const BlockPath &b) {
			return (a.container == b.container)
				&& (a.index == b.index);
		}
	};

	enum class LeafKind : uchar {
		BlockText,
		BlockCaption,
		ListItemText,
		TableCellText,
		MathFormula,
	};

	struct LeafPath {
		LeafKind kind = LeafKind::BlockText;
		BlockPath block;
		int listItemIndex = -1;
		int tableRowIndex = -1;
		int tableCellIndex = -1;

		friend inline bool operator==(
				const LeafPath &a,
				const LeafPath &b) {
			return (a.kind == b.kind)
				&& (a.block == b.block)
				&& (a.listItemIndex == b.listItemIndex)
				&& (a.tableRowIndex == b.tableRowIndex)
				&& (a.tableCellIndex == b.tableCellIndex);
		}
	};

	struct InsertionAnchor {
		BlockContainerPath container;
		int blockIndex = -1;
	};

	enum class RemovalKind : uchar {
		Block,
		ListItem,
		TableCell,
	};

	struct RemovalTarget {
		RemovalKind kind = RemovalKind::Block;
		BlockPath block;
		int listItemIndex = -1;
		int tableRowIndex = -1;
		int tableCellIndex = -1;

		friend inline bool operator==(
				const RemovalTarget &a,
				const RemovalTarget &b) {
			return (a.kind == b.kind)
				&& (a.block == b.block)
				&& (a.listItemIndex == b.listItemIndex)
				&& (a.tableRowIndex == b.tableRowIndex)
				&& (a.tableCellIndex == b.tableCellIndex);
		}
	};

	struct TextNodeDescriptor {
		LeafPath leaf;
		InsertionAnchor insertionAnchor;
		RemovalTarget removalTarget;
		FieldMode mode = FieldMode::Rich;
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
	[[nodiscard]] FieldMode activeFieldMode() const;
	[[nodiscard]] QString activeRawText() const;
	void applyActiveRawText(QString text);
	[[nodiscard]] int activeTextLength() const;
	[[nodiscard]] std::optional<int> previousEditableOrdinal() const;
	[[nodiscard]] std::optional<int> nextEditableOrdinal() const;
	[[nodiscard]] bool isActiveTopLevelParagraph() const;
	[[nodiscard]] bool activeOwnerIsEmpty() const;
	[[nodiscard]] std::optional<int> removeActiveOwnerAndSelectAdjacent(
		bool forward);
	[[nodiscard]] int ensureTrailingParagraphActive();
	void insertHeading1AfterActive();
	void insertBlockquoteAfterActive();
	void insertBlockAfterActive(InsertAction action);

private:
	[[nodiscard]] std::vector<RichPage::Block> *blockContainer(
		const BlockContainerPath &path);
	[[nodiscard]] const std::vector<RichPage::Block> *blockContainer(
		const BlockContainerPath &path) const;
	[[nodiscard]] RichPage::Block *block(const BlockPath &path);
	[[nodiscard]] const RichPage::Block *block(const BlockPath &path) const;
	[[nodiscard]] RichPage::ListItem *listItem(
		const BlockPath &blockPath,
		int itemIndex);
	[[nodiscard]] const RichPage::ListItem *listItem(
		const BlockPath &blockPath,
		int itemIndex) const;
	[[nodiscard]] RichPage::TableCell *tableCell(
		const BlockPath &blockPath,
		int rowIndex,
		int cellIndex);
	[[nodiscard]] const RichPage::TableCell *tableCell(
		const BlockPath &blockPath,
		int rowIndex,
		int cellIndex) const;
	[[nodiscard]] RichPage::RichText *richText(const LeafPath &path);
	[[nodiscard]] const RichPage::RichText *richText(
		const LeafPath &path) const;
	[[nodiscard]] QString *rawText(const LeafPath &path);
	[[nodiscard]] const QString *rawText(const LeafPath &path) const;
	[[nodiscard]] const TextNodeDescriptor *textNode(int ordinal) const;
	[[nodiscard]] int textNodeOrdinal(const LeafPath &path) const;

	void rebuild();
	void rebuildTextNodes();
	void rebuildTextNodes(
		const std::vector<RichPage::Block> &blocks,
		const BlockContainerPath &container);
	void appendBlockTextNode(
		const BlockPath &path,
		LeafKind kind,
		FieldMode mode = FieldMode::Rich,
		std::optional<InsertionAnchor> insertionAnchor = std::nullopt);
	void appendListItemTextNode(const BlockPath &path, int itemIndex);
	void appendTableCellTextNode(
		const BlockPath &path,
		int rowIndex,
		int cellIndex);
	void ensureActiveTextOrdinal();
	void ensureEditableNodes();
	void focusInsertedBlock(const BlockPath &path);
	[[nodiscard]] std::optional<int> adjacentEditableOrdinal(
		bool forward) const;
	[[nodiscard]] bool descriptorBelongsToBlock(
		const TextNodeDescriptor &descriptor,
		const BlockPath &path) const;
	[[nodiscard]] bool removalTargetIsEmpty(
		const RemovalTarget &target) const;
	[[nodiscard]] bool removeTarget(const RemovalTarget &target);
	[[nodiscard]] bool anchorIdExists(const QString &id) const;
	[[nodiscard]] bool anchorIdExists(
		const std::vector<RichPage::Block> &blocks,
		const QString &id) const;
	[[nodiscard]] QString nextAnchorId() const;
	[[nodiscard]] RichPage::Block makeBlock(InsertAction action) const;

	[[nodiscard]] static TextWithEntities MakeText(QString text);
	[[nodiscard]] static RichPage::Block MakeParagraphBlock();
	[[nodiscard]] static RichPage::Block MakeHeadingBlock(int level);
	[[nodiscard]] static RichPage::Block MakeQuoteBlock(bool pullquote);
	[[nodiscard]] static RichPage::Block MakeMathBlock();
	[[nodiscard]] static RichPage::Block MakeDividerBlock();
	[[nodiscard]] static RichPage::Block MakeAnchorBlock(QString anchorId);
	[[nodiscard]] static RichPage::Block MakeListBlock(
		RichPage::ListKind kind,
		RichPage::TaskState taskState = RichPage::TaskState::None);
	[[nodiscard]] static RichPage::Block MakeDetailsBlock();
	[[nodiscard]] static RichPage::Block MakeTableBlock();
	[[nodiscard]] static RichPage::Block MakeMediaBlock(
		RichPage::BlockKind kind);
	[[nodiscard]] static RichPage::Block MakeMapBlock(
		double latitude,
		double longitude);
	[[nodiscard]] static bool RichTextIsEmpty(const RichPage::RichText &text);
	[[nodiscard]] static bool ListItemIsEmpty(
		const RichPage::ListItem &item);
	[[nodiscard]] static bool BlockIsEmpty(const RichPage::Block &block);
	[[nodiscard]] static bool StripWrapperEntityInEditMode(EntityType type);
	[[nodiscard]] static TextWithEntities StripEditModeWrapperEntities(
		TextWithEntities text);

	std::shared_ptr<RichPage> _richPage;
	Markdown::MarkdownArticleContent _prepared;
	std::vector<TextNodeDescriptor> _textNodes;
	int _activeTextOrdinal = -1;

};

} // namespace Iv::Editor
