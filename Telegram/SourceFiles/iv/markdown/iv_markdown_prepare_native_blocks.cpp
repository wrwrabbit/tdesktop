/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_prepare_native_blocks.h"
#include "base/unixtime.h"
#include "iv/markdown/iv_markdown_prepare_links.h"
#include "lang/lang_keys.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Iv::Markdown {
namespace {

void ShiftEntities(EntitiesInText *entities, int delta) {
	if (!delta) {
		return;
	}
	for (auto &entity : *entities) {
		entity = EntityInText(
			entity.type(),
			entity.offset() + delta,
			entity.length(),
			entity.data());
	}
}

void PrependText(TextWithEntities *text, QString prefix) {
	if (prefix.isEmpty()) {
		return;
	}
	text->text.prepend(prefix);
	ShiftEntities(&text->entities, prefix.size());
}

[[nodiscard]] QString NativeIvDateText(TimeId date) {
	return langDateTimeFull(base::unixtime::parse(date));
}

[[nodiscard]] QString NativeIvDetailsAnchorId(NativeIvPrepareState *state) {
	return u"details-"_q + QString::number(++state->nextGeneratedId);
}

void SortPreparedIvRichText(PreparedIvRichText *text) {
	SortEntities(&text->text);
}

[[nodiscard]] QString StripOneTrailingNewline(QString text) {
	if (text.endsWith(u"\r\n"_q)) {
		text.chop(2);
	} else if (!text.isEmpty()) {
		const auto last = text.back();
		if ((last == QChar(u'\n')) || (last == QChar(u'\r'))) {
			text.chop(1);
		}
	}
	return text;
}

[[nodiscard]] PreparedBlock EmptyParagraphBlock() {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Paragraph;
	return block;
}

[[nodiscard]] PreparedBlock PrepareRuleBlock() {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Rule;
	return block;
}

[[nodiscard]] bool AppendNativeIvFlowBlock(
		std::vector<PreparedBlock> *result,
		PreparedBlockKind kind,
		int headingLevel,
		const MTPRichText &text,
		NativeIvPrepareState *state,
		bool allowEmpty = false) {
	auto prepared = PreparedIvRichText();
	auto anchorId = QString();
	if (!PrepareNativeIvRichText(text, &prepared, &anchorId, state)) {
		return false;
	}
	return AppendPreparedIvRichBlock(
		result,
		kind,
		headingLevel,
		std::move(prepared),
		std::move(anchorId),
		allowEmpty);
}

[[nodiscard]] bool PrepareNativeIvQuoteBlock(
		const MTPRichText &text,
		const MTPRichText &caption,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Quote;
	auto body = PreparedIvRichText();
	if (!PrepareNativeIvRichText(text, &body, &block.anchorId, state)) {
		return false;
	}
	if (!AppendPreparedIvRichBlock(
		&block.children,
		PreparedBlockKind::Paragraph,
		0,
		std::move(body))) {
		return false;
	}
	auto cite = PreparedIvRichText();
	if (!PrepareNativeIvRichText(caption, &cite, &block.anchorId, state)) {
		return false;
	}
	if (!AppendPreparedIvRichBlock(
		&block.children,
		PreparedBlockKind::Paragraph,
		0,
		std::move(cite))) {
		return false;
	}
	if (block.children.empty()) {
		block.children.push_back(EmptyParagraphBlock());
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] bool PrepareNativeIvList(
		const QVector<MTPPageListItem> &items,
		PreparedBlock *result,
		NativeIvPrepareState *state) {
	for (const auto &item : items) {
		auto block = PreparedBlock();
		block.kind = PreparedBlockKind::ListItem;
		block.listKind = result->listKind;
		block.listDelimiter = result->listDelimiter;
		const auto ok = item.match([&](const MTPDpageListItemText &data) {
			auto prepared = PreparedIvRichText();
			if (!PrepareNativeIvRichText(
					data.vtext(),
					&prepared,
					&block.anchorId,
					state)) {
				return false;
			}
			return AppendPreparedIvRichBlock(
				&block.children,
				PreparedBlockKind::Paragraph,
				0,
				std::move(prepared));
		}, [&](const MTPDpageListItemBlocks &data) {
			return PrepareNativeIvBlocks(data.vblocks().v, &block.children, state);
		});
		if (!ok) {
			return false;
		}
		if (block.children.empty()) {
			block.children.push_back(EmptyParagraphBlock());
		}
		result->children.push_back(std::move(block));
	}
	return true;
}

[[nodiscard]] bool ParseOrderedNumber(
		const QString &value,
		int *result) {
	auto ok = false;
	const auto parsed = value.toInt(&ok);
	if (!ok) {
		return false;
	}
	*result = parsed;
	return true;
}

[[nodiscard]] int NextNativeIvOrderedNumber(const PreparedBlock &result) {
	return result.children.empty()
		? result.startNumber
		: (result.children.back().orderedNumber + 1);
}

[[nodiscard]] bool PrepareNativeIvOrderedList(
		const QVector<MTPPageListOrderedItem> &items,
		PreparedBlock *result,
		NativeIvPrepareState *state) {
	auto firstNumber = true;
	for (const auto &item : items) {
		auto block = PreparedBlock();
		block.kind = PreparedBlockKind::ListItem;
		block.listKind = result->listKind;
		block.listDelimiter = result->listDelimiter;
		const auto ok = item.match([&](const MTPDpageListOrderedItemText &data) {
			auto orderedNumber = 0;
			if (!ParseOrderedNumber(qs(data.vnum()), &orderedNumber)) {
				orderedNumber = NextNativeIvOrderedNumber(*result);
			}
			block.orderedNumber = orderedNumber;
			auto prepared = PreparedIvRichText();
			if (!PrepareNativeIvRichText(
					data.vtext(),
					&prepared,
					&block.anchorId,
					state)) {
				return false;
			}
			return AppendPreparedIvRichBlock(
				&block.children,
				PreparedBlockKind::Paragraph,
				0,
				std::move(prepared));
		}, [&](const MTPDpageListOrderedItemBlocks &data) {
			auto orderedNumber = 0;
			if (!ParseOrderedNumber(qs(data.vnum()), &orderedNumber)) {
				orderedNumber = NextNativeIvOrderedNumber(*result);
			}
			block.orderedNumber = orderedNumber;
			return PrepareNativeIvBlocks(data.vblocks().v, &block.children, state);
		});
		if (!ok) {
			return false;
		}
		if (firstNumber) {
			result->startNumber = block.orderedNumber;
			firstNumber = false;
		}
		if (block.children.empty()) {
			block.children.push_back(EmptyParagraphBlock());
		}
		result->children.push_back(std::move(block));
	}
	return true;
}

[[nodiscard]] TableAlignment NativeIvTableAlignment(
		const MTPDpageTableCell &cell) {
	if (cell.is_align_right()) {
		return TableAlignment::Right;
	} else if (cell.is_align_center()) {
		return TableAlignment::Center;
	}
	return TableAlignment::Left;
}

[[nodiscard]] PreparedTableCellVerticalAlignment NativeIvTableVerticalAlignment(
		const MTPDpageTableCell &cell) {
	if (cell.is_valign_bottom()) {
		return PreparedTableCellVerticalAlignment::Bottom;
	} else if (cell.is_valign_middle()) {
		return PreparedTableCellVerticalAlignment::Middle;
	}
	return PreparedTableCellVerticalAlignment::Top;
}

using NativeIvTableOccupancyRow = std::vector<char>;
using NativeIvTableOccupancyGrid = std::vector<NativeIvTableOccupancyRow>;

[[nodiscard]] int NormalizeNativeIvTableSpan(int span) {
	return std::max(span, 1);
}

[[nodiscard]] int ClampNativeIvTableRowspan(
		int rawRowspan,
		int row,
		int rowCount) {
	if ((row < 0) || (row >= rowCount) || (rowCount <= 0)) {
		return 0;
	}
	const auto remainingRows = int64(rowCount) - row;
	return int(std::min<int64>(
		NormalizeNativeIvTableSpan(rawRowspan),
		remainingRows));
}

[[nodiscard]] int ClampNativeIvTableColspan(
		int rawColspan,
		int column,
		int maxColumns) {
	if ((column < 0) || (column >= maxColumns) || (maxColumns <= 0)) {
		return 0;
	}
	const auto remainingColumns = int64(maxColumns) - column;
	return int(std::min<int64>(
		NormalizeNativeIvTableSpan(rawColspan),
		remainingColumns));
}

[[nodiscard]] bool CanOccupyNativeIvTableSlots(
		const NativeIvTableOccupancyGrid &occupancy,
		int row,
		int column,
		int rowspan,
		int colspan) {
	if ((row < 0)
		|| (column < 0)
		|| (rowspan <= 0)
		|| (colspan <= 0)
		|| (row >= int(occupancy.size()))) {
		return false;
	}
	const auto rowLimit = int(std::min<int64>(
		int64(row) + rowspan,
		occupancy.size()));
	const auto columnLimit64 = int64(column) + colspan;
	if (columnLimit64 <= column) {
		return false;
	}
	const auto columnLimit = int(std::min<int64>(
		columnLimit64,
		std::numeric_limits<int>::max()));
	for (auto currentRow = row; currentRow < rowLimit; ++currentRow) {
		const auto &occupied = occupancy[currentRow];
		const auto occupiedLimit = std::min(columnLimit, int(occupied.size()));
		for (auto currentColumn = column;
			currentColumn < occupiedLimit;
			++currentColumn) {
			if (occupied[currentColumn]) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] int FirstAvailableNativeIvTableColumn(
		const NativeIvTableOccupancyGrid &occupancy,
		int row,
		int rowspan,
		int colspan,
		int maxColumns) {
	if ((row < 0)
		|| (row >= int(occupancy.size()))
		|| (rowspan <= 0)
		|| (colspan <= 0)
		|| (maxColumns <= 0)) {
		return -1;
	}
	for (auto column = 0; column < maxColumns; ++column) {
		const auto effectiveColspan = ClampNativeIvTableColspan(
			colspan,
			column,
			maxColumns);
		if (effectiveColspan <= 0) {
			continue;
		}
		if (CanOccupyNativeIvTableSlots(
				occupancy,
				row,
				column,
				rowspan,
				effectiveColspan)) {
			return column;
		}
	}
	return -1;
}

void MarkNativeIvTableSlots(
		NativeIvTableOccupancyGrid *occupancy,
		int row,
		int column,
		int rowspan,
		int colspan) {
	if ((row < 0)
		|| (column < 0)
		|| (rowspan <= 0)
		|| (colspan <= 0)
		|| (row >= int(occupancy->size()))) {
		return;
	}
	const auto rowLimit = int(std::min<int64>(
		int64(row) + rowspan,
		occupancy->size()));
	const auto columnLimit64 = int64(column) + colspan;
	if (columnLimit64 <= column) {
		return;
	}
	const auto columnLimit = int(std::min<int64>(
		columnLimit64,
		std::numeric_limits<int>::max()));
	for (auto currentRow = row; currentRow < rowLimit; ++currentRow) {
		auto &occupied = (*occupancy)[currentRow];
		if (columnLimit > int(occupied.size())) {
			occupied.resize(columnLimit, false);
		}
		for (auto currentColumn = column;
			currentColumn < columnLimit;
			++currentColumn) {
			occupied[currentColumn] = true;
		}
	}
}

[[nodiscard]] int NativeIvTableColumnCount(
		const NativeIvTableOccupancyGrid &occupancy) {
	auto result = 0;
	for (const auto &row : occupancy) {
		result = std::max(result, int(row.size()));
	}
	return result;
}

[[nodiscard]] int NativeIvTableOccupiedSlotCount(
		const NativeIvTableOccupancyGrid &occupancy) {
	auto result = 0;
	for (const auto &row : occupancy) {
		result += int(std::count(row.begin(), row.end(), char(true)));
	}
	return result;
}

[[nodiscard]] bool PrepareNativeIvTableBlock(
		const MTPDpageBlockTable &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Table;
	block.tableBordered = data.is_bordered();
	block.tableStriped = data.is_striped();

	auto title = PreparedIvRichText();
	if (!PrepareNativeIvRichText(
			data.vtitle(),
			&title,
			&block.anchorId,
			state)) {
		return false;
	}
	SortPreparedIvRichText(&title);
	block.text = std::move(title.text);
	block.links = std::move(title.links);

	const auto &limits = PrepareTableRenderLimitsForIv();
	const auto rowCount = int(data.vrows().v.size());

	const auto placeholder = [&] {
		if (state->result.failure.failed()) {
			return false;
		}
		if (!block.text.text.isEmpty() || !block.anchorId.isEmpty()) {
			auto titleBlock = EmptyParagraphBlock();
			titleBlock.text = std::move(block.text);
			titleBlock.links = std::move(block.links);
			titleBlock.anchorId = std::move(block.anchorId);
			result->push_back(std::move(titleBlock));
		}
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Table Placeholder"_q,
			result);
	};

	if (rowCount > limits.maxRows) {
		return placeholder();
	}

	auto occupancy = NativeIvTableOccupancyGrid(rowCount);
	block.tableRows.reserve(rowCount);
	auto occupiedSlotCountSoFar = int64(0);

	auto rowIndex = 0;
	for (const auto &row : data.vrows().v) {
		auto preparedRow = PreparedTableRow();
		const auto ok = row.match([&](const MTPDpageTableRow &rowData) {
			preparedRow.cells.reserve(std::min(
				int(rowData.vcells().v.size()),
				limits.maxColumns));
			for (const auto &cell : rowData.vcells().v) {
				auto preparedCell = PreparedTableCell();
				const auto cellOk = cell.match([&](const MTPDpageTableCell &cellData) {
					const auto rawColspan = cellData.vcolspan()
						? cellData.vcolspan()->v
						: 1;
					const auto rawRowspan = cellData.vrowspan()
						? cellData.vrowspan()->v
						: 1;
					const auto normalizedColspan = NormalizeNativeIvTableSpan(
						rawColspan);
					const auto rowspan = ClampNativeIvTableRowspan(
						rawRowspan,
						rowIndex,
						rowCount);
					if (rowspan <= 0) {
						return true;
					}
					const auto column = FirstAvailableNativeIvTableColumn(
						occupancy,
						rowIndex,
						rowspan,
						normalizedColspan,
						limits.maxColumns);
					if (column < 0) {
						return true;
					}
					const auto colspan = ClampNativeIvTableColspan(
						normalizedColspan,
						column,
						limits.maxColumns);
					if (colspan <= 0) {
						return true;
					}
					const auto occupiedSlotGrowth = int64(rowspan) * colspan;
					if (occupiedSlotGrowth > limits.maxCells
						|| (occupiedSlotCountSoFar + occupiedSlotGrowth)
							> limits.maxCells) {
						return true;
					}
					preparedCell.column = column;
					preparedCell.alignment = NativeIvTableAlignment(cellData);
					preparedCell.header = cellData.is_header();
					preparedCell.verticalAlignment
						= NativeIvTableVerticalAlignment(cellData);
					preparedCell.colspan = colspan;
					preparedCell.rowspan = rowspan;
					if (cellData.vtext()) {
						auto rich = PreparedIvRichText();
						if (!PrepareNativeIvRichText(
								*cellData.vtext(),
								&rich,
								nullptr,
								state)) {
							return false;
						}
						SortPreparedIvRichText(&rich);
						preparedCell.text = std::move(rich.text);
						preparedCell.links = std::move(rich.links);
					}
					MarkNativeIvTableSlots(
						&occupancy,
						rowIndex,
						column,
						rowspan,
						colspan);
					occupiedSlotCountSoFar += occupiedSlotGrowth;
					preparedRow.cells.push_back(std::move(preparedCell));
					return true;
				});
				if (!cellOk) {
					return false;
				}
			}
			preparedRow.header = !preparedRow.cells.empty()
				&& std::all_of(
					preparedRow.cells.begin(),
					preparedRow.cells.end(),
					[](const PreparedTableCell &cell) {
						return cell.header;
					});
			return true;
		});
		if (!ok) {
			return placeholder();
		}
		block.tableRows.push_back(std::move(preparedRow));
		++rowIndex;
	}

	block.tableColumnCount = NativeIvTableColumnCount(occupancy);
	const auto occupiedSlotCount = NativeIvTableOccupiedSlotCount(occupancy);
	if (rowCount > limits.maxRows
		|| block.tableColumnCount > limits.maxColumns
		|| occupiedSlotCount > limits.maxCells
		|| (int64(rowCount) * block.tableColumnCount) > limits.maxCells) {
		return placeholder();
	}

	block.tableAlignments.resize(block.tableColumnCount, TableAlignment::Left);
	for (const auto &preparedRow : block.tableRows) {
		for (const auto &preparedCell : preparedRow.cells) {
			const auto from = std::max(preparedCell.column, 0);
			const auto to = std::min(
				preparedCell.column + preparedCell.colspan,
				block.tableColumnCount);
			for (auto column = from; column != to; ++column) {
				block.tableAlignments[column] = preparedCell.alignment;
			}
		}
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] bool PrepareNativeIvDetailsBlock(
		const MTPDpageBlockDetails &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto summary = PreparedIvRichText();
	auto anchorId = NativeIvDetailsAnchorId(state);
	if (!PrepareNativeIvRichText(
			data.vtitle(),
			&summary,
			&anchorId,
			state)) {
		return false;
	}
	if (!summary.links.empty()) {
		const auto isLink = [](const EntityInText &entity) {
			return entity.type() == EntityType::CustomUrl;
		};
		const auto from = std::remove_if(
			summary.text.entities.begin(),
			summary.text.entities.end(),
			isLink);
		summary.text.entities.erase(from, summary.text.entities.end());
		summary.links.clear();
	}
	SortPreparedIvRichText(&summary);
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Details;
	block.anchorId = std::move(anchorId);
	block.collapsed = !data.is_open();
	block.text = std::move(summary.text);
	block.links = std::move(summary.links);
	if (!PrepareNativeIvBlocks(data.vblocks().v, &block.children, state)) {
		return false;
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] bool PrepareNativeIvBlock(
		const MTPPageBlock &block,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state);
[[nodiscard]] bool PrepareNativeIvBlock(
		const MTPPageBlock &block,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	if (state->blocked()) {
		return false;
	}
	return block.match([&](const MTPDpageBlockUnsupported &) {
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Unsupported Content"_q,
			result);
	}, [&](const MTPDpageBlockTitle &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			1,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockSubtitle &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			2,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockAuthorDate &data) {
		auto prepared = PreparedIvRichText();
		auto anchorId = QString();
		if (!PrepareNativeIvRichText(
				data.vauthor(),
				&prepared,
				&anchorId,
				state)) {
			return false;
		}
		if (const auto date = data.vpublished_date().v) {
			if (!prepared.text.text.isEmpty()) {
				prepared.text.append(u" \u2022 "_q);
			}
			prepared.text.append(NativeIvDateText(date));
		}
		return AppendPreparedIvRichBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			std::move(prepared),
			std::move(anchorId));
	}, [&](const MTPDpageBlockHeader &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			3,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockSubheader &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			4,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockParagraph &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockPreformatted &data) {
		auto prepared = PreparedIvRichText();
		auto anchorId = QString();
		if (!PrepareNativeIvRichText(
				data.vtext(),
				&prepared,
				&anchorId,
				state)) {
			return false;
		}
		auto block = PreparedBlock();
		block.kind = PreparedBlockKind::CodeBlock;
		block.anchorId = std::move(anchorId);
		block.codeLanguage = qs(data.vlanguage()).trimmed();
		block.text.text = StripOneTrailingNewline(prepared.text.text);
		result->push_back(std::move(block));
		return true;
	}, [&](const MTPDpageBlockFooter &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockDivider &) {
		result->push_back(PrepareRuleBlock());
		return true;
	}, [&](const MTPDpageBlockAnchor &data) {
		const auto anchorId = NormalizeFragmentId(qs(data.vname()));
		if (anchorId.isEmpty()) {
			return true;
		}
		auto prepared = PreparedIvRichText();
		return AppendPreparedIvRichBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			std::move(prepared),
			anchorId,
			true);
	}, [&](const MTPDpageBlockList &data) {
		auto prepared = PreparedBlock();
		prepared.kind = PreparedBlockKind::List;
		prepared.listKind = ListKind::Bullet;
		return PrepareNativeIvList(data.vitems().v, &prepared, state)
			? (result->push_back(std::move(prepared)), true)
			: false;
	}, [&](const MTPDpageBlockBlockquote &data) {
		return PrepareNativeIvQuoteBlock(
			data.vtext(),
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockPullquote &data) {
		return PrepareNativeIvQuoteBlock(
			data.vtext(),
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockPhoto &data) {
		return PrepareNativeIvPhotoBlock(data, result, state);
	}, [&](const MTPDpageBlockVideo &data) {
		return PrepareNativeIvVideoBlock(data, result, state);
	}, [&](const MTPDpageBlockCover &data) {
		return PrepareNativeIvBlock(data.vcover(), result, state);
	}, [&](const MTPDpageBlockEmbed &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Embed Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockEmbedPost &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Embed Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockCollage &data) {
		return PrepareNativeIvGroupedMediaBlock(
			data.vitems().v,
			data.vcaption(),
			PreparedGroupedMediaIntent::Collage,
			u"Collage placeholder"_q,
			result,
			state);
	}, [&](const MTPDpageBlockSlideshow &data) {
		return PrepareNativeIvGroupedMediaBlock(
			data.vitems().v,
			data.vcaption(),
			PreparedGroupedMediaIntent::Slideshow,
			u"Grouped Media Placeholder"_q,
			result,
			state);
	}, [&](const MTPDpageBlockChannel &data) {
		return PrepareNativeIvChannelBlock(data, result, state);
	}, [&](const MTPDpageBlockAudio &data) {
		return PrepareNativeIvAudioBlock(data, result, state);
	}, [&](const MTPDpageBlockKicker &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			5,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockTable &data) {
		return PrepareNativeIvTableBlock(data, result, state);
	}, [&](const MTPDpageBlockOrderedList &data) {
		auto prepared = PreparedBlock();
		prepared.kind = PreparedBlockKind::List;
		prepared.listKind = ListKind::Ordered;
		prepared.listDelimiter = ListDelimiter::Period;
		prepared.startNumber = 1;
		return PrepareNativeIvOrderedList(data.vitems().v, &prepared, state)
			? (result->push_back(std::move(prepared)), true)
			: false;
	}, [&](const MTPDpageBlockDetails &data) {
		return PrepareNativeIvDetailsBlock(data, result, state);
	}, [&](const MTPDpageBlockRelatedArticles &) {
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Related Articles Placeholder"_q,
			result);
	}, [&](const MTPDpageBlockMap &data) {
		return PrepareNativeIvMapBlock(data, result, state);
		});
}

} // namespace

bool PrepareNativeIvBlocks(
		const QVector<MTPPageBlock> &blocks,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	for (const auto &block : blocks) {
		if (!PrepareNativeIvBlock(block, result, state)) {
			if (state->result.failure.failed()) {
				return false;
			}
			(void)PrepareNativeIvPlainPlaceholderBlock(
				u"Unsupported Block"_q,
				result);
		}
	}
	return !state->result.failure.failed();
}

} // namespace Iv::Markdown
