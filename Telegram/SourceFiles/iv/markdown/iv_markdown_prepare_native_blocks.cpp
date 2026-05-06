#include "iv/markdown/iv_markdown_prepare_native_blocks.h"

#include "iv/markdown/iv_markdown_prepare_links.h"

#include "base/unixtime.h"
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

[[nodiscard]] bool PrepareNativeIvTableBlock(
		const MTPDpageBlockTable &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto title = PreparedIvRichText();
	auto anchorId = QString();
	if (!PrepareNativeIvRichText(data.vtitle(), &title, &anchorId, state)) {
		return false;
	}
	if (!AppendPreparedIvRichBlock(
		result,
		PreparedBlockKind::Paragraph,
		0,
		std::move(title),
		std::move(anchorId))) {
		return false;
	}
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Table;
	for (const auto &row : data.vrows().v) {
		auto preparedRow = PreparedTableRow();
		auto header = std::optional<bool>();
		auto column = 0;
		const auto ok = row.match([&](const MTPDpageTableRow &data) {
			for (const auto &cell : data.vcells().v) {
				auto preparedCell = PreparedTableCell();
				const auto cellOk = cell.match([&](const MTPDpageTableCell &data) {
					if ((data.vcolspan() && data.vcolspan()->v > 1)
						|| (data.vrowspan() && data.vrowspan()->v > 1)) {
						return false;
					}
					const auto cellHeader = data.is_header();
					if (header && *header != cellHeader) {
						return false;
					}
					header = cellHeader;
					preparedCell.column = column++;
					preparedCell.alignment = NativeIvTableAlignment(data);
					if (data.vtext()) {
						auto rich = PreparedIvRichText();
						if (!PrepareNativeIvRichText(
								*data.vtext(),
								&rich,
								nullptr,
								state)) {
							return false;
						}
						SortPreparedIvRichText(&rich);
						preparedCell.text = std::move(rich.text);
						preparedCell.links = std::move(rich.links);
					}
					return true;
				});
				if (!cellOk) {
					return false;
				}
				preparedRow.cells.push_back(std::move(preparedCell));
			}
			return true;
		});
		if (!ok) {
			return state->result.failure.failed()
				? false
				: PrepareNativeIvPlainPlaceholderBlock(
					u"Table Placeholder"_q,
					result);
		}
		preparedRow.header = header.value_or(false);
		if (!block.tableColumnCount) {
			block.tableColumnCount = column;
			block.tableAlignments.resize(column, TableAlignment::Left);
		} else if (block.tableColumnCount != column) {
			return PrepareNativeIvPlainPlaceholderBlock(
				u"Table Placeholder"_q,
				result);
		}
		for (const auto &cell : preparedRow.cells) {
			if (cell.column >= 0 && cell.column < int(block.tableAlignments.size())) {
				block.tableAlignments[cell.column] = cell.alignment;
			}
		}
		block.tableRows.push_back(std::move(preparedRow));
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
		return PrepareNativeIvPlaceholderBlock(
			u"Video Placeholder"_q,
			data.vcaption(),
			result,
			state);
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
		return PrepareNativeIvPlaceholderBlock(
			u"Collage placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockSlideshow &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Grouped Media Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}, [&](const MTPDpageBlockChannel &) {
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Channel Placeholder"_q,
			result);
	}, [&](const MTPDpageBlockAudio &data) {
		return PrepareNativeIvPlaceholderBlock(
			u"Audio File Placeholder"_q,
			data.vcaption(),
			result,
			state);
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
		return PrepareNativeIvPlaceholderBlock(
			u"Map Placeholder"_q,
			data.vcaption(),
			result,
			state);
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
