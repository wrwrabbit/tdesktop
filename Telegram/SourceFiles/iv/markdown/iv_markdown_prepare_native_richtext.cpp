#include "iv/markdown/iv_markdown_prepare_native_richtext.h"

#include "iv/markdown/iv_markdown_prepare_links.h"
#include "ui/basic_click_handlers.h"

#include <QtCore/QUrl>

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

[[nodiscard]] const NativeIvPhotoInfo *FindNativeIvPhoto(
		uint64 photoId,
		const NativeIvPrepareState &state) {
	for (const auto &photo : state.photos) {
		if (photo.id == photoId) {
			return &photo;
		}
	}
	return nullptr;
}

void SortPreparedIvRichText(PreparedIvRichText *text) {
	SortEntities(&text->text);
}

[[nodiscard]] QString ExternalLinkDisplayText(const PreparedLink &link) {
	if (link.entityType == EntityType::Email) {
		return link.target;
	}
	const auto original = QUrl(link.target);
	const auto good = QUrl(original.isValid()
		? original.toEncoded()
		: QString());
	return good.isValid() ? good.toDisplayString() : link.target;
}

void FinalizePreparedExternalLink(
		PreparedLink *link,
		QStringView renderedText) {
	if (!link
		|| link->kind != PreparedLinkKind::External
		|| link->entityType != EntityType::Url) {
		return;
	}
	if (renderedText == QStringView(ExternalLinkDisplayText(*link))) {
		return;
	}
	if (UrlClickHandler::EncodeForOpening(renderedText.toString())
		== link->target) {
		link->shown = EntityLinkShown::Partial;
		return;
	}
	link->entityType = EntityType::CustomUrl;
}

[[nodiscard]] bool AddNativeIvPreparedLink(
		TextWithEntities *text,
		std::vector<PreparedLink> *links,
		int from,
		int length,
		QString target) {
	if (!length || target.isEmpty()) {
		return true;
	}
	const auto index = links->size() + 1;
	if (index > std::numeric_limits<uint16>::max()) {
		return true;
	}
	auto prepared = ClassifiedLink(uint16(index), target, nullptr);
	if (prepared.kind == PreparedLinkKind::RejectedRelative
		|| prepared.kind == PreparedLinkKind::LocalFile) {
		return true;
	}
	FinalizePreparedExternalLink(
		&prepared,
		QStringView(text->text).mid(from, length));
	text->entities.push_back(EntityInText(
		EntityType::CustomUrl,
		from,
		length,
		InternalLinkData(uint16(index))));
	links->push_back(std::move(prepared));
	return true;
}

[[nodiscard]] bool AddNativeIvEntity(
		TextWithEntities *text,
		int from,
		EntityType type) {
	const auto length = text->text.size() - from;
	if (length <= 0) {
		return true;
	}
	text->entities.push_back(EntityInText(type, from, length));
	return true;
}

[[nodiscard]] bool AppendNativeIvRichText(
		const MTPRichText &text,
		TextWithEntities *result,
		std::vector<PreparedLink> *links,
		QString *blockAnchorId,
		NativeIvPrepareState *state) {
	if (state->blocked()) {
		return false;
	}
	return text.match([&](const MTPDtextEmpty &) {
		return true;
	}, [&](const MTPDtextPlain &data) {
		result->append(qs(data.vtext()));
		return true;
	}, [&](const MTPDtextConcat &data) {
		for (const auto &part : data.vtexts().v) {
			if (!AppendNativeIvRichText(
					part,
					result,
					links,
					blockAnchorId,
					state)) {
				return false;
			}
		}
		return true;
	}, [&](const MTPDtextImage &data) {
		const auto replacementText = u"[image]"_q;
		if (!data.vdocument_id().v || data.vw().v <= 0 || data.vh().v <= 0) {
			result->append(replacementText);
			return true;
		}
		const auto entityData = SerializeInlineTextObjectEntity({
			.kind = InlineTextObjectKind::IvImage,
			.data = InlineTextObjectIvImageData{
				.documentId = uint64(data.vdocument_id().v),
				.width = data.vw().v,
				.height = data.vh().v,
				.replacementText = replacementText,
			},
		});
		if (entityData.isEmpty()) {
			result->append(replacementText);
			return true;
		}
		const auto from = result->text.size();
		result->append(QChar::ObjectReplacementCharacter);
		result->entities.push_back(EntityInText(
			EntityType::CustomEmoji,
			from,
			1,
			entityData));
		return true;
	}, [&](const MTPDtextBold &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Bold);
	}, [&](const MTPDtextItalic &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Italic);
	}, [&](const MTPDtextUnderline &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Underline);
	}, [&](const MTPDtextStrike &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::StrikeOut);
	}, [&](const MTPDtextFixed &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Code);
	}, [&](const MTPDtextUrl &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		if (result->text.size() == from) {
			result->append(qs(data.vurl()));
		}
		return AddNativeIvPreparedLink(
			result,
			links,
			from,
			result->text.size() - from,
			qs(data.vurl()));
	}, [&](const MTPDtextEmail &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		if (result->text.size() == from) {
			result->append(qs(data.vemail()));
		}
		return AddNativeIvPreparedLink(
			result,
			links,
			from,
			result->text.size() - from,
			u"mailto:"_q + qs(data.vemail()));
	}, [&](const MTPDtextSubscript &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Subscript);
	}, [&](const MTPDtextSuperscript &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Superscript);
	}, [&](const MTPDtextMarked &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		return AddNativeIvEntity(result, from, EntityType::Marked);
	}, [&](const MTPDtextPhone &data) {
		const auto from = result->text.size();
		if (!AppendNativeIvRichText(
				data.vtext(),
				result,
				links,
				blockAnchorId,
				state)) {
			return false;
		}
		if (result->text.size() == from) {
			result->append(qs(data.vphone()));
		}
		return AddNativeIvPreparedLink(
			result,
			links,
			from,
			result->text.size() - from,
			u"tel:"_q + qs(data.vphone()));
	}, [&](const MTPDtextAnchor &data) {
		if (blockAnchorId && blockAnchorId->isEmpty()) {
			*blockAnchorId = NormalizeFragmentId(qs(data.vname()));
		}
		return AppendNativeIvRichText(
			data.vtext(),
			result,
			links,
			blockAnchorId,
			state);
	});
}

[[nodiscard]] bool PrepareNativeIvCaption(
		const MTPPageCaption &caption,
		PreparedIvRichText *result,
		QString *blockAnchorId,
		NativeIvPrepareState *state) {
	if (!AppendNativeIvRichText(
			caption.data().vtext(),
			&result->text,
			&result->links,
			blockAnchorId,
			state)) {
		return false;
	}
	auto credit = PreparedIvRichText();
	if (!PrepareNativeIvRichText(
			caption.data().vcredit(),
			&credit,
			blockAnchorId,
			state)) {
		return false;
	}
	if (!credit.text.text.isEmpty()) {
		if (!result->text.text.isEmpty()) {
			result->text.append(QChar('\n'));
		}
		if (!AppendNativeIvRichText(
				caption.data().vcredit(),
				&result->text,
				&result->links,
				blockAnchorId,
				state)) {
			return false;
		}
	}
	return true;
}

} // namespace

void RememberNativeIvPhoto(
		NativeIvPrepareState *state,
		const MTPPhoto &photo) {
	auto info = NativeIvPhotoInfo{
		.id = photo.match([](const auto &data) {
			return data.vid().v;
		}),
	};
	photo.match([](const MTPDphotoEmpty &) {
	}, [&](const MTPDphoto &data) {
		auto width = 0;
		auto height = 0;
		const auto assign = [&](int w, int h) {
			if (w > 0 && h > 0) {
				width = w;
				height = h;
			}
		};
		for (const auto &size : data.vsizes().v) {
			size.match([&](const MTPDphotoSizeEmpty &) {
			}, [&](const MTPDphotoSize &data) {
				if (data.vtype().v == u"y"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"x"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"w"_q) {
					assign(data.vw().v, data.vh().v);
				}
			}, [&](const MTPDphotoCachedSize &data) {
				if (data.vtype().v == u"y"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"x"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"w"_q) {
					assign(data.vw().v, data.vh().v);
				}
			}, [&](const MTPDphotoStrippedSize &) {
			}, [&](const MTPDphotoSizeProgressive &data) {
				if (data.vtype().v == u"y"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"x"_q) {
					assign(data.vw().v, data.vh().v);
				} else if (!width && data.vtype().v == u"w"_q) {
					assign(data.vw().v, data.vh().v);
				}
			}, [&](const MTPDphotoPathSize &) {
			});
		}
		info.width = width;
		info.height = height;
	});
	if (!info.id) {
		return;
	}
	for (auto &existing : state->photos) {
		if (existing.id == info.id) {
			existing = info;
			return;
		}
	}
	state->photos.push_back(info);
}

bool PrepareNativeIvRichText(
		const MTPRichText &text,
		PreparedIvRichText *result,
		QString *blockAnchorId,
		NativeIvPrepareState *state) {
	return AppendNativeIvRichText(
		text,
		&result->text,
		&result->links,
		blockAnchorId,
		state);
}

bool AppendPreparedIvRichBlock(
		std::vector<PreparedBlock> *result,
		PreparedBlockKind kind,
		int headingLevel,
		PreparedIvRichText prepared,
		QString anchorId,
		bool allowEmpty) {
	SortPreparedIvRichText(&prepared);
	if (prepared.text.text.isEmpty() && !allowEmpty) {
		return true;
	}
	auto block = PreparedBlock();
	block.kind = kind;
	block.headingLevel = headingLevel;
	block.text = std::move(prepared.text);
	block.links = std::move(prepared.links);
	block.anchorId = std::move(anchorId);
	result->push_back(std::move(block));
	return true;
}

bool PrepareNativeIvPlainPlaceholderBlock(
		QString label,
		std::vector<PreparedBlock> *result) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Placeholder;
	block.placeholder.label = std::move(label);
	block.placeholder.copyText = block.placeholder.label;
	result->push_back(std::move(block));
	return true;
}

bool PrepareNativeIvPhotoBlock(
		const MTPDpageBlockPhoto &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	const auto info = FindNativeIvPhoto(uint64(data.vphoto_id().v), *state);
	if (!info || info->width <= 0 || info->height <= 0) {
		return PrepareNativeIvPlaceholderBlock(
			u"Photo Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}
	auto caption = PreparedIvRichText();
	auto anchorId = QString();
	if (!PrepareNativeIvCaption(data.vcaption(), &caption, &anchorId, state)) {
		return false;
	}
	SortPreparedIvRichText(&caption);
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Photo;
	block.text = std::move(caption.text);
	block.links = std::move(caption.links);
	block.anchorId = std::move(anchorId);
	block.photo.photoId = info->id;
	block.photo.width = info->width;
	block.photo.height = info->height;
	block.photo.urlOverride = data.vurl() ? qs(*data.vurl()) : QString();
	block.photo.viewerOpen = true;
	result->push_back(std::move(block));
	return true;
}

namespace {

[[nodiscard]] QString NativeIvPlaceholderCopyText(
		const QString &label,
		const TextWithEntities &caption) {
	return caption.text.isEmpty()
		? label
		: (label + u"\n"_q + caption.text);
}

} // namespace

bool PrepareNativeIvPlaceholderBlock(
		QString label,
		const MTPPageCaption &caption,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto prepared = PreparedIvRichText();
	auto anchorId = QString();
	if (!PrepareNativeIvCaption(caption, &prepared, &anchorId, state)) {
		return state->result.failure.failed()
			? false
			: PrepareNativeIvPlainPlaceholderBlock(std::move(label), result);
	}
	SortPreparedIvRichText(&prepared);
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Placeholder;
	block.text = std::move(prepared.text);
	block.links = std::move(prepared.links);
	block.anchorId = std::move(anchorId);
	block.placeholder.label = label;
	block.placeholder.copyText = NativeIvPlaceholderCopyText(
		block.placeholder.label,
		block.text);
	result->push_back(std::move(block));
	return true;
}

} // namespace Iv::Markdown
