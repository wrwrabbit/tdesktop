/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/iv_rich_page.h"

#include "base/algorithm.h"
#include "base/flat_map.h"
#include "base/qthelp_url.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "iv/markdown/iv_markdown_prepare.h"
#include "iv/markdown/iv_markdown_prepare_serialize.h"
#include "iv/markdown/iv_markdown_prepare_links.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QSize>

namespace Iv {
namespace {

using Block = RichPage::Block;
using BlockKind = RichPage::BlockKind;
using GroupedMediaIntent = RichPage::GroupedMediaIntent;
using GroupedMediaItem = RichPage::GroupedMediaItem;
using ListItem = RichPage::ListItem;
using ListKind = RichPage::ListKind;
using RelatedArticle = RichPage::RelatedArticle;
using RichText = RichPage::RichText;
using TableAlignment = RichPage::TableAlignment;
using TableCell = RichPage::TableCell;
using TableRow = RichPage::TableRow;
using TableVerticalAlignment = RichPage::TableVerticalAlignment;
using TaskState = RichPage::TaskState;

const auto PhotoLargeLevels = u"ydxcwmbsa"_q;

[[nodiscard]] Block MakeBlock(BlockKind kind) {
	auto result = Block();
	result.kind = kind;
	return result;
}

[[nodiscard]] Block MakeHeadingBlock(int level) {
	auto result = MakeBlock(BlockKind::Heading);
	result.headingLevel = level;
	return result;
}

struct ParseContext {
	not_null<Main::Session*> session;
	base::flat_map<uint64, PhotoData*> photos;
	base::flat_map<uint64, DocumentData*> documents;
	base::flat_map<uint64, QSize> photoSizes;
	struct DocumentInfo {
		int width = 0;
		int height = 0;
		QString fileName;
		QString title;
		QString performer;
		int duration = 0;
		bool isVideoFile = false;
		bool isAnimation = false;
	};
	base::flat_map<uint64, DocumentInfo> documentInfos;
};

[[nodiscard]] QString DateText(TimeId date) {
	return langDateTimeFull(base::unixtime::parse(date));
}

[[nodiscard]] bool AddEntity(
		TextWithEntities *text,
		int from,
		EntityType type,
		const QString &data = QString()) {
	const auto length = text->text.size() - from;
	if (length <= 0) {
		return true;
	}
	text->entities.push_back(EntityInText(type, from, length, data));
	return true;
}

void AddRichAnchor(
		QString *primary,
		std::vector<QString> *extra,
		QString anchorId) {
	if (anchorId.isEmpty()) {
		return;
	}
	if (primary && primary->isEmpty()) {
		*primary = std::move(anchorId);
		return;
	}
	if (primary && *primary == anchorId) {
		return;
	}
	if (extra && !ranges::contains(*extra, anchorId)) {
		extra->push_back(std::move(anchorId));
	}
}

void AppendRich(RichText *to, const RichText &from) {
	to->text.append(from.text);
	AddRichAnchor(&to->anchorId, &to->anchorIds, from.anchorId);
	for (const auto &anchorId : from.anchorIds) {
		AddRichAnchor(&to->anchorId, &to->anchorIds, anchorId);
	}
}

void AppendRich(RichText *to, RichText &&from) {
	to->text.append(std::move(from.text));
	AddRichAnchor(&to->anchorId, &to->anchorIds, std::move(from.anchorId));
	for (auto &anchorId : from.anchorIds) {
		AddRichAnchor(&to->anchorId, &to->anchorIds, std::move(anchorId));
	}
}

[[nodiscard]] QString RichPageLinkEntityData(
		const QString &url,
		uint64 webpageId) {
	return (webpageId && !url.isEmpty())
		? EncodeRichPageLinkUrl(url, webpageId)
		: url;
}

[[nodiscard]] QString MentionNameEntityData(
		not_null<Main::Session*> session,
		uint64 userId) {
	if (userId == 0) {
		return QString();
	}
	const auto loaded = session->data().userLoaded(UserId(userId));
	return TextUtilities::MentionNameDataFromFields({
		.selfId = session->userId().bare,
		.userId = userId,
		.accessHash = loaded ? loaded->accessHash() : 0,
	});
}

[[nodiscard]] TaskState TaskStateFromFlags(
		bool checkbox,
		bool checked) {
	if (!checkbox) {
		return TaskState::None;
	}
	return checked ? TaskState::Checked : TaskState::Unchecked;
}

[[nodiscard]] uint64 PhotoIdFromMtp(const MTPPhoto &photo) {
	return photo.match([](const auto &data) {
		return uint64(data.vid().v);
	});
}

[[nodiscard]] uint64 DocumentIdFromMtp(const MTPDocument &document) {
	return document.match([](const auto &data) {
		return uint64(data.vid().v);
	});
}

[[nodiscard]] QSize PhotoSizeFromMtp(const MTPPhoto &photo) {
	auto result = QSize();
	photo.match([](const MTPDphotoEmpty &) {
	}, [&](const MTPDphoto &data) {
		auto bestLevel = PhotoLargeLevels.size();
		const auto assign = [&](const QString &type, int width, int height) {
			const auto level = type.isEmpty()
				? -1
				: PhotoLargeLevels.indexOf(type.front());
			if (level >= 0 && level < bestLevel) {
				bestLevel = level;
				result = QSize(width, height);
			}
		};
		for (const auto &size : data.vsizes().v) {
			size.match([](const MTPDphotoSizeEmpty &) {
			}, [&](const MTPDphotoSize &row) {
				assign(qs(row.vtype()), row.vw().v, row.vh().v);
			}, [&](const MTPDphotoCachedSize &row) {
				assign(qs(row.vtype()), row.vw().v, row.vh().v);
			}, [&](const MTPDphotoStrippedSize &) {
			}, [&](const MTPDphotoSizeProgressive &row) {
				assign(qs(row.vtype()), row.vw().v, row.vh().v);
			}, [&](const MTPDphotoPathSize &) {
			});
		}
	});
	return result;
}

[[nodiscard]] ParseContext::DocumentInfo DocumentInfoFromMtp(
		const MTPDocument &document) {
	auto result = ParseContext::DocumentInfo();
	document.match([](const MTPDdocumentEmpty &) {
	}, [&](const MTPDdocument &data) {
		for (const auto &attribute : data.vattributes().v) {
			attribute.match([&](const MTPDdocumentAttributeAudio &row) {
				result.duration = row.vduration().v;
				result.title = qs(row.vtitle().value_or_empty());
				result.performer = qs(row.vperformer().value_or_empty());
			}, [&](const MTPDdocumentAttributeFilename &row) {
				result.fileName = qs(row.vfile_name());
			}, [&](const MTPDdocumentAttributeImageSize &row) {
				if (result.width <= 0 || result.height <= 0) {
					result.width = row.vw().v;
					result.height = row.vh().v;
				}
			}, [&](const MTPDdocumentAttributeAnimated &) {
				result.isAnimation = true;
			}, [&](const MTPDdocumentAttributeVideo &row) {
				result.isVideoFile = true;
				result.width = row.vw().v;
				result.height = row.vh().v;
			}, [&](const auto &) {
			});
		}
	});
	return result;
}

void MergeDocumentInfo(
		ParseContext::DocumentInfo *existing,
		ParseContext::DocumentInfo info) {
	if (!existing) {
		return;
	}
	if (existing->width <= 0 && info.width > 0) {
		existing->width = info.width;
	}
	if (existing->height <= 0 && info.height > 0) {
		existing->height = info.height;
	}
	if (existing->fileName.isEmpty() && !info.fileName.isEmpty()) {
		existing->fileName = std::move(info.fileName);
	}
	if (existing->title.isEmpty() && !info.title.isEmpty()) {
		existing->title = std::move(info.title);
	}
	if (existing->performer.isEmpty() && !info.performer.isEmpty()) {
		existing->performer = std::move(info.performer);
	}
	if (existing->duration <= 0 && info.duration > 0) {
		existing->duration = info.duration;
	}
	if (!existing->isVideoFile && info.isVideoFile) {
		existing->isVideoFile = true;
	}
	if (!existing->isAnimation && info.isAnimation) {
		existing->isAnimation = true;
	}
}

void RememberPhoto(ParseContext *context, const MTPPhoto &photo) {
	const auto id = PhotoIdFromMtp(photo);
	if (!id) {
		return;
	}
	context->photos[id] = context->session->data().processPhoto(photo).get();
	const auto size = PhotoSizeFromMtp(photo);
	const auto existing = context->photoSizes.find(id);
	if (!size.isEmpty() || existing == end(context->photoSizes)) {
		context->photoSizes[id] = size;
	}
}

void RememberDocument(ParseContext *context, const MTPDocument &document) {
	const auto id = DocumentIdFromMtp(document);
	if (!id) {
		return;
	}
	const auto processed = context->session->data().processDocument(document).get();
	context->documents[id] = processed;
	auto info = DocumentInfoFromMtp(document);
	const auto existing = context->documentInfos.find(id);
	if (existing != end(context->documentInfos)) {
		MergeDocumentInfo(&existing->second, std::move(info));
	} else {
		context->documentInfos.emplace(id, std::move(info));
	}
}

void RememberWebPageMedia(
		ParseContext *context,
		const MTPDwebPage &webpage) {
	if (const auto photo = webpage.vphoto()) {
		RememberPhoto(context, *photo);
	}
	if (const auto document = webpage.vdocument()) {
		RememberDocument(context, *document);
	}
}

[[nodiscard]] PhotoData *FindPhoto(
		const ParseContext &context,
		uint64 id) {
	const auto i = context.photos.find(id);
	return (i != end(context.photos)) ? i->second : nullptr;
}

[[nodiscard]] DocumentData *FindDocument(
		const ParseContext &context,
		uint64 id) {
	const auto i = context.documents.find(id);
	return (i != end(context.documents)) ? i->second : nullptr;
}

[[nodiscard]] QSize FindPhotoSize(
		const ParseContext &context,
		uint64 id) {
	const auto i = context.photoSizes.find(id);
	return (i != end(context.photoSizes)) ? i->second : QSize();
}

[[nodiscard]] ParseContext::DocumentInfo FindDocumentInfo(
		const ParseContext &context,
		uint64 id) {
	const auto i = context.documentInfos.find(id);
	return (i != end(context.documentInfos))
		? i->second
		: ParseContext::DocumentInfo();
}

[[nodiscard]] TableAlignment TableCellAlignment(
		const MTPDpageTableCell &data) {
	if (data.is_align_right()) {
		return TableAlignment::Right;
	} else if (data.is_align_center()) {
		return TableAlignment::Center;
	}
	return TableAlignment::Left;
}

[[nodiscard]] TableVerticalAlignment TableCellVerticalAlignment(
		const MTPDpageTableCell &data) {
	if (data.is_valign_bottom()) {
		return TableVerticalAlignment::Bottom;
	} else if (data.is_valign_middle()) {
		return TableVerticalAlignment::Middle;
	}
	return TableVerticalAlignment::Top;
}

[[nodiscard]] RichText ParseRichText(
		const MTPRichText &text,
		ParseContext *context);

[[nodiscard]] RichText ParseCaption(
		const MTPPageCaption &caption,
		ParseContext *context);

[[nodiscard]] bool AppendRichText(
		const MTPRichText &text,
		RichText *result,
		ParseContext *context,
		QString *anchorId,
		std::vector<QString> *anchorIds) {
	return text.match([&](const MTPDtextEmpty &) {
		return true;
	}, [&](const MTPDtextPlain &data) {
		result->text.append(qs(data.vtext()));
		return true;
	}, [&](const MTPDtextConcat &data) {
		for (const auto &part : data.vtexts().v) {
			if (!AppendRichText(part, result, context, anchorId, anchorIds)) {
				return false;
			}
		}
		return true;
	}, [&](const MTPDtextImage &data) {
		const auto replacementText = u"[image]"_q;
		if (!data.vdocument_id().v || data.vw().v <= 0 || data.vh().v <= 0) {
			result->text.append(replacementText);
			return true;
		}
		const auto entityData = Markdown::SerializeInlineTextObjectEntity({
			.kind = Markdown::InlineTextObjectKind::IvImage,
			.data = Markdown::InlineTextObjectIvImageData{
				.documentId = uint64(data.vdocument_id().v),
				.width = data.vw().v,
				.height = data.vh().v,
				.replacementText = replacementText,
			},
		});
		if (entityData.isEmpty()) {
			result->text.append(replacementText);
			return true;
		}
		const auto from = result->text.text.size();
		result->text.append(QChar::ObjectReplacementCharacter);
		result->text.entities.push_back(EntityInText(
			EntityType::CustomEmoji,
			from,
			1,
			entityData));
		return true;
	}, [&](const MTPDtextMath &data) {
		const auto source = qs(data.vsource());
		const auto entityData = Markdown::SerializeInlineTextObjectEntity({
			.kind = Markdown::InlineTextObjectKind::Formula,
			.data = Markdown::InlineTextObjectFormulaData{
				.copySource = Markdown::InlineFormulaCopySource(source),
				.trimmedTex = source.trimmed(),
			},
		});
		if (entityData.isEmpty()) {
			result->text.append(source);
			return true;
		}
		const auto from = result->text.text.size();
		result->text.append(QChar::ObjectReplacementCharacter);
		result->text.entities.push_back(EntityInText(
			EntityType::CustomEmoji,
			from,
			1,
			entityData));
		return true;
	}, [&](const MTPDtextCustomEmoji &data) {
		result->text.append(Ui::Text::SingleCustomEmoji(
			::Data::SerializeCustomEmojiId(uint64(data.vdocument_id().v)),
			qs(data.valt())));
		return true;
	}, [&](const MTPDtextBold &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Bold);
	}, [&](const MTPDtextItalic &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Italic);
	}, [&](const MTPDtextUnderline &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Underline);
	}, [&](const MTPDtextStrike &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::StrikeOut);
	}, [&](const MTPDtextFixed &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Code);
	}, [&](const MTPDtextUrl &data) {
		const auto from = result->text.text.size();
		if (!AppendRichText(data.vtext(), result, context, anchorId, anchorIds)) {
			return false;
		}
		const auto target = qs(data.vurl());
		if (result->text.text.size() == from) {
			result->text.append(target);
		}
		return AddEntity(
			&result->text,
			from,
			EntityType::CustomUrl,
			RichPageLinkEntityData(target, uint64(data.vwebpage_id().v)));
	}, [&](const MTPDtextEmail &data) {
		const auto from = result->text.text.size();
		if (!AppendRichText(data.vtext(), result, context, anchorId, anchorIds)) {
			return false;
		}
		const auto email = qs(data.vemail());
		if (result->text.text.size() == from) {
			result->text.append(email);
		}
		const auto target = u"mailto:"_q + email;
		return AddEntity(&result->text, from, EntityType::CustomUrl, target);
	}, [&](const MTPDtextSubscript &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Subscript);
	}, [&](const MTPDtextSuperscript &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Superscript);
	}, [&](const MTPDtextMarked &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Marked);
	}, [&](const MTPDtextSpoiler &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Spoiler);
	}, [&](const MTPDtextMention &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Mention);
	}, [&](const MTPDtextHashtag &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Hashtag);
	}, [&](const MTPDtextBotCommand &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::BotCommand);
	}, [&](const MTPDtextCashtag &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Cashtag);
	}, [&](const MTPDtextAutoUrl &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Url);
	}, [&](const MTPDtextAutoEmail &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Email);
	}, [&](const MTPDtextAutoPhone &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::Phone);
	}, [&](const MTPDtextBankCard &data) {
		const auto from = result->text.text.size();
		return AppendRichText(data.vtext(), result, context, anchorId, anchorIds)
			&& AddEntity(&result->text, from, EntityType::BankCard);
	}, [&](const MTPDtextMentionName &data) {
		const auto from = result->text.text.size();
		if (!AppendRichText(data.vtext(), result, context, anchorId, anchorIds)) {
			return false;
		}
		const auto entityData = MentionNameEntityData(
			context->session,
			uint64(data.vuser_id().v));
		return entityData.isEmpty()
			? true
			: AddEntity(
				&result->text,
				from,
				EntityType::MentionName,
				entityData);
	}, [&](const MTPDtextDate &data) {
		const auto from = result->text.text.size();
		if (!AppendRichText(data.vtext(), result, context, anchorId, anchorIds)) {
			return false;
		}
		auto flags = FormattedDateFlags();
		if (data.is_relative()) {
			flags |= FormattedDateFlag::Relative;
		}
		if (data.is_short_time()) {
			flags |= FormattedDateFlag::ShortTime;
		}
		if (data.is_long_time()) {
			flags |= FormattedDateFlag::LongTime;
		}
		if (data.is_short_date()) {
			flags |= FormattedDateFlag::ShortDate;
		}
		if (data.is_long_date()) {
			flags |= FormattedDateFlag::LongDate;
		}
		if (data.is_day_of_week()) {
			flags |= FormattedDateFlag::DayOfWeek;
		}
		return AddEntity(
			&result->text,
			from,
			EntityType::FormattedDate,
			SerializeFormattedDateData(data.vdate().v, flags));
	}, [&](const MTPDtextPhone &data) {
		const auto from = result->text.text.size();
		if (!AppendRichText(data.vtext(), result, context, anchorId, anchorIds)) {
			return false;
		}
		const auto phone = qs(data.vphone());
		if (result->text.text.size() == from) {
			result->text.append(phone);
		}
		const auto target = u"tel:"_q + phone;
		return AddEntity(&result->text, from, EntityType::CustomUrl, target);
	}, [&](const MTPDtextAnchor &data) {
		AddRichAnchor(
			anchorId,
			anchorIds,
			Markdown::NormalizeFragmentId(qs(data.vname())));
		return AppendRichText(
			data.vtext(),
			result,
			context,
			anchorId,
			anchorIds);
	});
}

[[nodiscard]] RichText ParseRichText(
		const MTPRichText &text,
		ParseContext *context) {
	auto result = RichText();
	auto anchorId = QString();
	auto anchorIds = std::vector<QString>();
	(void)AppendRichText(text, &result, context, &anchorId, &anchorIds);
	result.anchorId = std::move(anchorId);
	result.anchorIds = std::move(anchorIds);
	return result;
}

[[nodiscard]] RichText ParseCaption(
		const MTPPageCaption &caption,
		ParseContext *context) {
	auto result = RichText();
	auto anchorId = QString();
	auto anchorIds = std::vector<QString>();
	(void)AppendRichText(
		caption.data().vtext(),
		&result,
		context,
		&anchorId,
		&anchorIds);
	auto credit = RichText();
	(void)AppendRichText(
		caption.data().vcredit(),
		&credit,
		context,
		&anchorId,
		&anchorIds);
	if (!credit.text.empty()) {
		if (!result.text.empty()) {
			result.text.append(QChar('\n'));
		}
		AppendRich(&result, std::move(credit));
	}
	result.anchorId = std::move(anchorId);
	result.anchorIds = std::move(anchorIds);
	return result;
}

void AdoptAnchor(QString *anchorId, RichText *text) {
	if (anchorId->isEmpty() && !text->anchorId.isEmpty()) {
		*anchorId = std::move(text->anchorId);
		text->anchorId.clear();
	}
}

void AdoptLeadingParagraphListItemText(ListItem *item) {
	if (item->blocks.empty()
		|| item->blocks.front().kind != BlockKind::Paragraph) {
		return;
	}
	item->text = std::move(item->blocks.front().text);
	item->anchorId = std::move(item->blocks.front().anchorId);
	item->blocks.erase(item->blocks.begin());
}

void AppendBlocks(
		const QVector<MTPPageBlock> &blocks,
		std::vector<Block> *result,
		ParseContext *context);

void AppendBlock(
		const MTPPageBlock &block,
		std::vector<Block> *result,
	ParseContext *context) {
	block.match([&](const MTPDpageBlockUnsupported &) {
		result->push_back(MakeBlock(BlockKind::Unsupported));
	}, [&](const MTPDpageBlockTitle &data) {
		auto parsed = MakeHeadingBlock(1);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockSubtitle &data) {
		auto parsed = MakeHeadingBlock(2);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockAuthorDate &data) {
		auto parsed = MakeBlock(BlockKind::AuthorDate);
		parsed.text = ParseRichText(data.vauthor(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		parsed.date = data.vpublished_date().v;
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockHeader &data) {
		auto parsed = MakeHeadingBlock(3);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockSubheader &data) {
		auto parsed = MakeHeadingBlock(4);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockParagraph &data) {
		auto parsed = MakeBlock(BlockKind::Paragraph);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockThinking &data) {
		auto parsed = MakeBlock(BlockKind::Thinking);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockPreformatted &data) {
		auto parsed = MakeBlock(BlockKind::Code);
		parsed.language = qs(data.vlanguage()).trimmed();
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockFooter &data) {
		auto parsed = MakeBlock(BlockKind::Footer);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockDivider &) {
		result->push_back(MakeBlock(BlockKind::Divider));
	}, [&](const MTPDpageBlockAnchor &data) {
		const auto anchorId = Markdown::NormalizeFragmentId(qs(data.vname()));
		if (!anchorId.isEmpty()) {
			auto parsed = MakeBlock(BlockKind::Anchor);
			parsed.anchorId = anchorId;
			result->push_back(std::move(parsed));
		}
	}, [&](const MTPDpageBlockList &data) {
		auto parsed = MakeBlock(BlockKind::List);
		parsed.listKind = ListKind::Bullet;
		parsed.listItems.reserve(data.vitems().v.size());
		for (const auto &item : data.vitems().v) {
			auto listItem = ListItem();
			item.match([&](const MTPDpageListItemText &row) {
				listItem.taskState = TaskStateFromFlags(
					row.is_checkbox(),
					row.is_checked());
				listItem.text = ParseRichText(row.vtext(), context);
				AdoptAnchor(&listItem.anchorId, &listItem.text);
			}, [&](const MTPDpageListItemBlocks &row) {
				listItem.taskState = TaskStateFromFlags(
					row.is_checkbox(),
					row.is_checked());
				AppendBlocks(row.vblocks().v, &listItem.blocks, context);
				AdoptLeadingParagraphListItemText(&listItem);
			});
			parsed.listItems.push_back(std::move(listItem));
		}
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockBlockquote &data) {
		auto parsed = MakeBlock(BlockKind::Quote);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		parsed.caption = ParseRichText(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockBlockquoteBlocks &data) {
		auto parsed = MakeBlock(BlockKind::Quote);
		AppendBlocks(data.vblocks().v, &parsed.blocks, context);
		parsed.caption = ParseRichText(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockPullquote &data) {
		auto parsed = MakeBlock(BlockKind::Quote);
		parsed.pullquote = true;
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		parsed.caption = ParseRichText(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockPhoto &data) {
		const auto photoId = uint64(data.vphoto_id().v);
		const auto size = FindPhotoSize(*context, photoId);
		auto parsed = MakeBlock(BlockKind::Photo);
		parsed.url = qs(data.vurl().value_or_empty());
		parsed.width = size.width();
		parsed.height = size.height();
		parsed.photoId = photoId;
		parsed.spoiler = data.is_spoiler();
		parsed.photo = FindPhoto(*context, photoId);
		parsed.caption = ParseCaption(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockVideo &data) {
		const auto documentId = uint64(data.vvideo_id().v);
		const auto info = FindDocumentInfo(*context, documentId);
		auto parsed = MakeBlock(BlockKind::Video);
		parsed.width = info.width;
		parsed.height = info.height;
		parsed.documentId = documentId;
		parsed.autoplay = data.is_autoplay();
		parsed.loop = data.is_loop();
		parsed.spoiler = data.is_spoiler();
		parsed.document = FindDocument(*context, documentId);
		parsed.caption = ParseCaption(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockCover &data) {
		AppendBlock(data.vcover(), result, context);
	}, [&](const MTPDpageBlockEmbed &data) {
		auto parsed = MakeBlock(BlockKind::Embed);
		parsed.url = qs(data.vurl().value_or_empty());
		parsed.html = data.vhtml() ? qba(*data.vhtml()) : QByteArray();
		parsed.width = data.vw().value_or_empty();
		parsed.height = data.vh().value_or_empty();
		parsed.fullWidth = data.is_full_width() || (data.vw() && !data.vw()->v);
		parsed.fixedHeight = (data.vh() != nullptr);
		parsed.allowScrolling = data.is_allow_scrolling();
		parsed.caption = ParseCaption(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockEmbedPost &data) {
		auto parsed = MakeBlock(BlockKind::EmbedPost);
		parsed.url = qs(data.vurl());
		parsed.author = qs(data.vauthor());
		parsed.date = data.vdate().v;
		parsed.photoId = uint64(data.vauthor_photo_id().v);
		parsed.photo = FindPhoto(*context, parsed.photoId);
		AppendBlocks(data.vblocks().v, &parsed.blocks, context);
		parsed.caption = ParseCaption(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockCollage &data) {
		auto parsed = MakeBlock(BlockKind::GroupedMedia);
		parsed.mediaIntent = GroupedMediaIntent::Collage;
		parsed.mediaItems.reserve(data.vitems().v.size());
		for (const auto &item : data.vitems().v) {
			item.match([&](const MTPDpageBlockPhoto &row) {
				const auto photoId = uint64(row.vphoto_id().v);
				const auto size = FindPhotoSize(*context, photoId);
				parsed.mediaItems.push_back({
					.kind = BlockKind::Photo,
					.photo = FindPhoto(*context, photoId),
					.photoId = photoId,
					.width = size.width(),
					.height = size.height(),
					.spoiler = row.is_spoiler(),
				});
			}, [&](const MTPDpageBlockVideo &row) {
				const auto documentId = uint64(row.vvideo_id().v);
				const auto info = FindDocumentInfo(*context, documentId);
				parsed.mediaItems.push_back({
					.kind = BlockKind::Video,
					.document = FindDocument(*context, documentId),
					.documentId = documentId,
					.width = info.width,
					.height = info.height,
					.autoplay = row.is_autoplay(),
					.loop = row.is_loop(),
					.spoiler = row.is_spoiler(),
				});
			}, [](const auto &) {
			});
		}
		parsed.caption = ParseCaption(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockSlideshow &data) {
		auto parsed = MakeBlock(BlockKind::GroupedMedia);
		parsed.mediaIntent = GroupedMediaIntent::Slideshow;
		parsed.mediaItems.reserve(data.vitems().v.size());
		for (const auto &item : data.vitems().v) {
			item.match([&](const MTPDpageBlockPhoto &row) {
				const auto photoId = uint64(row.vphoto_id().v);
				const auto size = FindPhotoSize(*context, photoId);
				parsed.mediaItems.push_back({
					.kind = BlockKind::Photo,
					.photo = FindPhoto(*context, photoId),
					.photoId = photoId,
					.width = size.width(),
					.height = size.height(),
					.spoiler = row.is_spoiler(),
				});
			}, [&](const MTPDpageBlockVideo &row) {
				const auto documentId = uint64(row.vvideo_id().v);
				const auto info = FindDocumentInfo(*context, documentId);
				parsed.mediaItems.push_back({
					.kind = BlockKind::Video,
					.document = FindDocument(*context, documentId),
					.documentId = documentId,
					.width = info.width,
					.height = info.height,
					.autoplay = row.is_autoplay(),
					.loop = row.is_loop(),
					.spoiler = row.is_spoiler(),
				});
			}, [](const auto &) {
			});
		}
		parsed.caption = ParseCaption(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockChannel &data) {
		auto parsed = MakeBlock(BlockKind::Channel);
		parsed.peer = context->session->data().processChat(data.vchannel()).get();
		data.vchannel().match([&](const MTPDchannel &row) {
			parsed.channelId = row.vid().v;
			parsed.channelTitle = qs(row.vtitle());
			if (const auto username = row.vusername()) {
				parsed.username = qs(*username);
			}
		}, [&](const MTPDchannelForbidden &row) {
			parsed.channelId = row.vid().v;
			parsed.channelTitle = qs(row.vtitle());
		}, [&](const MTPDchat &row) {
			parsed.channelId = row.vid().v;
			parsed.channelTitle = qs(row.vtitle());
		}, [&](const MTPDchatForbidden &row) {
			parsed.channelId = row.vid().v;
			parsed.channelTitle = qs(row.vtitle());
		}, [](const auto &) {
		});
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockAudio &data) {
		const auto documentId = uint64(data.vaudio_id().v);
		const auto info = FindDocumentInfo(*context, documentId);
		auto parsed = MakeBlock(BlockKind::Audio);
		parsed.audioTitle = info.title;
		parsed.audioPerformer = info.performer;
		parsed.audioFileName = info.fileName;
		parsed.documentId = documentId;
		parsed.document = FindDocument(*context, documentId);
		parsed.audioDuration = info.duration;
		parsed.caption = ParseCaption(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockKicker &data) {
		auto parsed = MakeHeadingBlock(5);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockHeading1 &data) {
		auto parsed = MakeHeadingBlock(1);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockHeading2 &data) {
		auto parsed = MakeHeadingBlock(2);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockHeading3 &data) {
		auto parsed = MakeHeadingBlock(3);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockHeading4 &data) {
		auto parsed = MakeHeadingBlock(4);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockHeading5 &data) {
		auto parsed = MakeHeadingBlock(5);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockHeading6 &data) {
		auto parsed = MakeHeadingBlock(6);
		parsed.text = ParseRichText(data.vtext(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockMath &data) {
		auto parsed = MakeBlock(BlockKind::Math);
		parsed.formula = qs(data.vsource());
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockTable &data) {
		auto parsed = MakeBlock(BlockKind::Table);
		parsed.bordered = data.is_bordered();
		parsed.striped = data.is_striped();
		parsed.text = ParseRichText(data.vtitle(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		parsed.tableRows.reserve(data.vrows().v.size());
		for (const auto &row : data.vrows().v) {
			row.match([&](const MTPDpageTableRow &rowData) {
				auto parsedRow = TableRow();
				parsedRow.cells.reserve(rowData.vcells().v.size());
				for (const auto &cell : rowData.vcells().v) {
					cell.match([&](const MTPDpageTableCell &cellData) {
						auto parsedCell = TableCell{
							.colspan = cellData.vcolspan().value_or(1),
							.rowspan = cellData.vrowspan().value_or(1),
							.header = cellData.is_header(),
							.alignment = TableCellAlignment(cellData),
							.verticalAlignment = TableCellVerticalAlignment(cellData),
						};
						if (const auto text = cellData.vtext()) {
							parsedCell.text = ParseRichText(*text, context);
							parsedCell.text.anchorId.clear();
							parsedCell.text.anchorIds.clear();
						}
						parsedRow.cells.push_back(std::move(parsedCell));
					});
				}
				parsed.tableRows.push_back(std::move(parsedRow));
			});
		}
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockOrderedList &data) {
		auto parsed = MakeBlock(BlockKind::List);
		parsed.listKind = ListKind::Ordered;
		parsed.listItems.reserve(data.vitems().v.size());
		const auto step = data.is_reversed() ? -1 : 1;
		auto nextNumber = data.vstart().value_or(
			data.is_reversed() ? int(data.vitems().v.size()) : 1);
		for (const auto &item : data.vitems().v) {
			auto listItem = ListItem();
			const auto fillNumber = [&](const auto &row) {
				if (const auto num = row.vnum()) {
					listItem.number = qs(*num);
				} else if (const auto value = row.vvalue()) {
					listItem.number = QString::number(value->v);
				} else {
					listItem.number = QString::number(nextNumber);
				}
			};
			item.match([&](const MTPDpageListOrderedItemText &row) {
				listItem.taskState = TaskStateFromFlags(
					row.is_checkbox(),
					row.is_checked());
				fillNumber(row);
				listItem.text = ParseRichText(row.vtext(), context);
				AdoptAnchor(&listItem.anchorId, &listItem.text);
			}, [&](const MTPDpageListOrderedItemBlocks &row) {
				listItem.taskState = TaskStateFromFlags(
					row.is_checkbox(),
					row.is_checked());
				fillNumber(row);
				AppendBlocks(row.vblocks().v, &listItem.blocks, context);
				AdoptLeadingParagraphListItemText(&listItem);
			});
			parsed.listItems.push_back(std::move(listItem));
			nextNumber += step;
		}
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockDetails &data) {
		auto parsed = MakeBlock(BlockKind::Details);
		parsed.open = data.is_open();
		parsed.text = ParseRichText(data.vtitle(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		AppendBlocks(data.vblocks().v, &parsed.blocks, context);
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockRelatedArticles &data) {
		auto parsed = MakeBlock(BlockKind::RelatedArticles);
		parsed.text = ParseRichText(data.vtitle(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.text);
		parsed.relatedArticles.reserve(data.varticles().v.size());
		for (const auto &article : data.varticles().v) {
			const auto &row = article.data();
			parsed.relatedArticles.push_back({
				.url = qs(row.vurl()),
				.webpageId = uint64(row.vwebpage_id().v),
				.photo = FindPhoto(
					*context,
					uint64(row.vphoto_id().value_or_empty())),
				.photoId = uint64(row.vphoto_id().value_or_empty()),
				.title = qs(row.vtitle().value_or_empty()).trimmed(),
				.description = qs(row.vdescription().value_or_empty()).trimmed(),
				.author = qs(row.vauthor().value_or_empty()).trimmed(),
				.publishedDate = row.vpublished_date().value_or_empty(),
			});
		}
		result->push_back(std::move(parsed));
	}, [&](const MTPDpageBlockMap &data) {
		auto parsed = MakeBlock(BlockKind::Map);
		parsed.width = data.vw().v;
		parsed.height = data.vh().v;
		parsed.zoom = data.vzoom().v;
		data.vgeo().match([&](const MTPDgeoPoint &row) {
			parsed.latitude = row.vlat().v;
			parsed.longitude = row.vlong().v;
			parsed.accessHash = row.vaccess_hash().v;
		}, [](const auto &) {
		});
		parsed.caption = ParseCaption(data.vcaption(), context);
		AdoptAnchor(&parsed.anchorId, &parsed.caption);
		result->push_back(std::move(parsed));
	}, [&](const MTPDinputPageBlockMap &) {
		result->push_back(MakeBlock(BlockKind::Unsupported));
	});
}

void AppendBlocks(
		const QVector<MTPPageBlock> &blocks,
		std::vector<Block> *result,
		ParseContext *context) {
	result->reserve(result->size() + blocks.size());
	for (const auto &block : blocks) {
		AppendBlock(block, result, context);
	}
}

void AppendSummaryLine(
		TextWithEntities *result,
		TextWithEntities &&line) {
	TextUtilities::Trim(line);
	if (line.empty()) {
		return;
	}
	if (!result->empty()) {
		result->append(QChar('\n'));
	}
	result->append(std::move(line));
}

void AppendSummaryLine(
		TextWithEntities *result,
		const TextWithEntities &line,
		const QString &prefix = QString()) {
	auto prepared = TextWithEntities();
	if (!prefix.isEmpty()) {
		prepared.append(prefix);
	}
	prepared.append(line);
	AppendSummaryLine(result, std::move(prepared));
}

void AppendSummaryLine(
		TextWithEntities *result,
		const RichText &line,
		const QString &prefix = QString()) {
	AppendSummaryLine(result, line.text, prefix);
}

[[nodiscard]] QString FooterText(const RelatedArticle &article) {
	if (article.publishedDate && !article.author.isEmpty()) {
		return article.author + u", "_q + DateText(article.publishedDate);
	} else if (article.publishedDate) {
		return DateText(article.publishedDate);
	}
	return article.author;
}

void AppendSummaryBlock(TextWithEntities *result, const Block &block);

void AppendSummaryBlocks(
		TextWithEntities *result,
		const std::vector<Block> &blocks) {
	for (const auto &block : blocks) {
		AppendSummaryBlock(result, block);
	}
}

[[nodiscard]] TextWithEntities FlattenSummaryBlocks(
		const std::vector<Block> &blocks) {
	auto result = TextWithEntities();
	AppendSummaryBlocks(&result, blocks);
	TextUtilities::Trim(result);
	return result;
}

void AppendSummaryBlock(TextWithEntities *result, const Block &block) {
	switch (block.kind) {
	case BlockKind::Unsupported:
	case BlockKind::Divider:
	case BlockKind::Anchor:
	case BlockKind::Thinking:
		return;
	case BlockKind::Heading:
	case BlockKind::Paragraph:
	case BlockKind::Footer:
	case BlockKind::Code:
		AppendSummaryLine(result, block.text);
		return;
	case BlockKind::AuthorDate: {
		auto line = block.text.text;
		if (block.date) {
			if (!line.empty()) {
				line.append(u" - "_q);
			}
			line.append(DateText(block.date));
		}
		AppendSummaryLine(result, std::move(line));
		return;
	}
	case BlockKind::List: {
		auto ordered = 1;
		for (const auto &item : block.listItems) {
			auto prefix = QString();
			if (item.taskState == TaskState::Unchecked) {
				prefix = u"[ ] "_q;
			} else if (item.taskState == TaskState::Checked) {
				prefix = u"[x] "_q;
			} else if (block.listKind == ListKind::Ordered) {
				const auto number = item.number.isEmpty()
					? QString::number(ordered)
					: item.number;
				prefix = number + u". "_q;
			} else {
				prefix = u"- "_q;
			}
			if (!item.text.text.empty()) {
				AppendSummaryLine(result, item.text, prefix);
			} else {
				auto nested = FlattenSummaryBlocks(item.blocks);
				AppendSummaryLine(result, std::move(nested), prefix);
			}
			++ordered;
		}
		return;
	}
	case BlockKind::Quote:
		AppendSummaryLine(result, block.text);
		AppendSummaryBlocks(result, block.blocks);
		AppendSummaryLine(result, block.caption);
		return;
	case BlockKind::Photo:
	case BlockKind::Video:
	case BlockKind::Audio:
	case BlockKind::GroupedMedia:
	case BlockKind::Map:
		AppendSummaryLine(result, block.caption);
		return;
	case BlockKind::Embed:
		if (!block.caption.text.empty()) {
			AppendSummaryLine(result, block.caption);
		} else if (!block.url.isEmpty()) {
			AppendSummaryLine(result, TextWithEntities::Simple(block.url));
		}
		return;
	case BlockKind::EmbedPost: {
		auto line = QString();
		if (!block.author.isEmpty()) {
			line = block.author;
		}
		if (block.date) {
			if (!line.isEmpty()) {
				line += u", "_q;
			}
			line += DateText(block.date);
		}
		if (!line.isEmpty()) {
			AppendSummaryLine(result, TextWithEntities::Simple(line));
		}
		AppendSummaryBlocks(result, block.blocks);
		AppendSummaryLine(result, block.caption);
		return;
	}
	case BlockKind::Channel:
		if (block.peer) {
			AppendSummaryLine(
				result,
				TextWithEntities::Simple(block.peer->name()));
		}
		return;
	case BlockKind::Math:
		AppendSummaryLine(result, TextWithEntities::Simple(block.formula));
		return;
	case BlockKind::Table: {
		AppendSummaryLine(result, block.text);
		for (const auto &row : block.tableRows) {
			auto line = TextWithEntities();
			auto first = true;
			for (const auto &cell : row.cells) {
				auto cellText = TextUtilities::SingleLine(cell.text.text);
				TextUtilities::Trim(cellText);
				if (cellText.empty()) {
					continue;
				}
				if (!first) {
					line.append(u" | "_q);
				}
				line.append(cellText);
				first = false;
			}
			AppendSummaryLine(result, std::move(line));
		}
		return;
	}
	case BlockKind::Details:
		AppendSummaryLine(result, block.text);
		AppendSummaryBlocks(result, block.blocks);
		return;
	case BlockKind::RelatedArticles:
		AppendSummaryLine(result, block.text);
		for (const auto &article : block.relatedArticles) {
			if (!article.title.isEmpty()) {
				AppendSummaryLine(
					result,
					TextWithEntities::Simple(article.title));
			}
			if (!article.description.isEmpty()) {
				AppendSummaryLine(
					result,
					TextWithEntities::Simple(article.description));
			}
			const auto footer = FooterText(article);
			if (!footer.isEmpty()) {
				AppendSummaryLine(
					result,
					TextWithEntities::Simple(footer));
			}
		}
		return;
	}
}

std::shared_ptr<const RichPage> ParsePage(
		not_null<Main::Session*> session,
		const MTPPage &page,
		const MTPDwebPage *webpage) {
	return page.match([&](const MTPDpage &data) {
		auto result = std::make_shared<RichPage>();
		auto context = ParseContext{ session };
		result->url = qs(data.vurl());
		result->rtl = data.is_rtl();
		result->part = data.is_part();
		result->views = data.vviews().value_or_empty();
		for (const auto &photo : data.vphotos().v) {
			RememberPhoto(&context, photo);
		}
		for (const auto &document : data.vdocuments().v) {
			RememberDocument(&context, document);
		}
		if (webpage) {
			RememberWebPageMedia(&context, *webpage);
		}
		AppendBlocks(data.vblocks().v, &result->blocks, &context);
		return std::shared_ptr<const RichPage>(std::move(result));
	}, [](const auto &) {
		return std::shared_ptr<const RichPage>();
	});
}

} // namespace

QString EncodeRichPageLinkUrl(
		const QString &url,
		uint64 webpageId) {
	return u"internal:wrapped?url="_q
		+ qthelp::url_encode(url)
		+ u"&context=iv&webpage_id="_q
		+ QString::number(webpageId);
}

std::optional<RichPageLinkUrl> DecodeRichPageLinkUrl(const QString &data) {
	const auto wrappedPrefix = u"internal:wrapped?"_q;
	if (!data.startsWith(wrappedPrefix)) {
		return std::nullopt;
	}
	const auto params = qthelp::url_parse_params(data.mid(wrappedPrefix.size()));
	if (params.value(u"context"_q) != u"iv"_q) {
		return std::nullopt;
	}
	const auto url = params.value(u"url"_q);
	if (url.isEmpty()) {
		return std::nullopt;
	}
	const auto webpageIdValue = params.value(u"webpage_id"_q);
	auto webpageId = uint64();
	if (!webpageIdValue.isEmpty()) {
		auto ok = false;
		webpageId = webpageIdValue.toULongLong(&ok);
		if (!ok) {
			return std::nullopt;
		}
	}
	return RichPageLinkUrl{
		.url = url,
		.webpageId = webpageId,
	};
}

std::shared_ptr<const RichPage> ParseRichPage(
		not_null<Main::Session*> session,
		const MTPRichMessage &message) {
	auto result = std::make_shared<RichPage>();
	auto context = ParseContext{ session };
	const auto &data = message.data();
	result->rtl = data.is_rtl();
	result->part = data.is_part();
	for (const auto &photo : data.vphotos().v) {
		RememberPhoto(&context, photo);
	}
	for (const auto &document : data.vdocuments().v) {
		RememberDocument(&context, document);
	}
	AppendBlocks(data.vblocks().v, &result->blocks, &context);
	return result;
}

std::shared_ptr<const RichPage> ParseRichPage(
		not_null<Main::Session*> session,
		const MTPPage &page) {
	return ParsePage(session, page, nullptr);
}

std::shared_ptr<const RichPage> ParseRichPage(
		not_null<Main::Session*> session,
		const MTPDwebPage &webpage) {
	const auto cachedPage = webpage.vcached_page();
	if (!cachedPage) {
		return std::shared_ptr<const RichPage>();
	}
	return ParsePage(session, *cachedPage, &webpage);
}

TextWithEntities FlattenRichPageSummary(const RichPage &page) {
	auto result = FlattenSummaryBlocks(page.blocks);
	TextUtilities::Trim(result);
	return result;
}

TextWithEntities FlattenRichPageSummary(
		const std::shared_ptr<const RichPage> &page) {
	return page ? FlattenRichPageSummary(*page) : TextWithEntities();
}

} // namespace Iv
