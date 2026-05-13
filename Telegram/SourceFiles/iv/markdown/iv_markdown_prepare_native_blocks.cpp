/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_prepare_native_blocks.h"
#include "base/openssl_help.h"
#include "base/unixtime.h"
#include "iv/iv_data.h"
#include "iv/markdown/iv_markdown_prepare_links.h"
#include "lang/lang_keys.h"

#include <algorithm>
#include <array>
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

struct NativeIvHtmlAttribute {
	QByteArray name;
	QByteArray value;
};

using NativeIvHtmlAttributes = std::vector<NativeIvHtmlAttribute>;

[[nodiscard]] QByteArray NativeIvHtmlEscape(QString text) {
	return text.toHtmlEscaped().toUtf8();
}

[[nodiscard]] QByteArray NativeIvHtmlText(QString text) {
	auto result = NativeIvHtmlEscape(std::move(text));
	result.replace("\r\n", "<br>");
	result.replace('\n', "<br>");
	result.replace('\r', "<br>");
	return result;
}

[[nodiscard]] QByteArray NativeIvRichText(QString text) {
	auto result = NativeIvHtmlEscape(std::move(text));
	result.replace("\xE2\x81\xA6", "<span dir=\"ltr\">");
	result.replace("\xE2\x81\xA7", "<span dir=\"rtl\">");
	result.replace("\xE2\x81\xA8", "<span dir=\"auto\">");
	result.replace("\xE2\x81\xA9", "</span>");
	return result;
}

[[nodiscard]] QByteArray NativeIvHtmlTag(
		const QByteArray &name,
		const NativeIvHtmlAttributes &attributes = {},
		const QByteArray &body = {}) {
	auto serialized = QByteArray();
	for (const auto &[attributeName, value] : attributes) {
		if (attributeName.isEmpty()) {
			continue;
		}
		serialized += ' ';
		serialized += attributeName;
		if (!value.isEmpty()) {
			serialized += "=\"";
			serialized += value;
			serialized += '"';
		}
	}
	return QByteArray("<")
		+ name
		+ serialized
		+ '>'
		+ body
		+ "</"
		+ name
		+ '>';
}

[[nodiscard]] QByteArray NativeIvHashBytes(const QByteArray &bytes) {
	auto binary = std::array<uchar, SHA256_DIGEST_LENGTH>{};
	SHA256(
		reinterpret_cast<const unsigned char*>(bytes.constData()),
		bytes.size(),
		binary.data());
	auto result = QByteArray();
	result.reserve(binary.size() * 2);
	for (const auto byte : binary) {
		const auto hex = [](uchar value) -> char {
			return (value >= 10) ? ('a' + (value - 10)) : ('0' + value);
		};
		result.push_back(hex(byte / 16));
		result.push_back(hex(byte % 16));
	}
	return result;
}

[[nodiscard]] QByteArray StoreNativeIvEmbedHtml(
		QByteArray html,
		NativeIvPrepareState *state) {
	const auto resourceId = QByteArray("native-iv-embed/")
		+ NativeIvHashBytes(html)
		+ ".html";
	state->result.embedHtmlResources.emplace(resourceId, std::move(html));
	return resourceId;
}

[[nodiscard]] QByteArray NativeIvResourceUrl(QByteArray resourceId) {
	return QByteArray("/") + resourceId;
}

[[nodiscard]] QByteArray NativeIvPhotoUrl(uint64 photoId) {
	return NativeIvResourceUrl(
		QByteArray("photo/") + QByteArray::number(photoId));
}

[[nodiscard]] QByteArray NativeIvDocumentUrl(uint64 documentId) {
	return NativeIvResourceUrl(
		QByteArray("document/") + QByteArray::number(documentId));
}

[[nodiscard]] QByteArray NativeIvMapUrl(
		const MTPDgeoPoint &geo,
		int width,
		int height,
		int zoom) {
	return NativeIvResourceUrl(QByteArray("map/")
		+ GeoPointId({
			.lat = geo.vlat().v,
			.lon = geo.vlong().v,
			.access = uint64(geo.vaccess_hash().v),
		}) + '&'
		+ QByteArray::number(width) + ','
		+ QByteArray::number(height) + '&'
		+ QByteArray::number(zoom));
}

[[nodiscard]] QByteArray WrapNativeIvEmbedHtml(QByteArray body) {
	return QByteArray(
		"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
		"<style>"
		"html,body{margin:0;padding:0;background:#fff;color:#000;}"
		"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}"
		"figure{display:block;margin:0;max-width:100%;}"
		"img,video{display:block;max-width:100%;height:auto;}"
		".iframe-wrap{display:block;margin:0 auto;max-width:100%;overflow:hidden;}"
		"iframe{display:block;width:100%;max-width:100%;border:0;}"
		".embed-post{box-sizing:border-box;margin:0;padding:16px;"
		"border:1px solid rgba(0,0,0,.08);border-radius:12px;}"
		".embed-post address{display:flex;align-items:center;gap:10px;"
		"margin:0 0 12px;font-style:normal;}"
		".embed-post address figure{width:40px;height:40px;flex:none;"
		"border-radius:20px;background:center/cover no-repeat #f1f1f1;}"
		".embed-post address .meta{display:flex;flex-direction:column;gap:2px;}"
		".embed-post blockquote{margin:12px 0;padding-left:12px;"
		"border-left:3px solid rgba(0,0,0,.12);}"
		".embed-post ul,.embed-post ol{padding-left:20px;}"
		".embed-post p,.embed-post pre,.embed-post h1,.embed-post h2,"
		".embed-post h3,.embed-post h4,.embed-post h5{margin:0 0 12px;}"
		".embed-post figure+figure{margin-top:12px;}"
		".embed-post small{display:block;margin-top:8px;}"
		"</style></head><body>")
		+ body
		+ QByteArray(
		"<script>"
		"(function(){"
		"var iframeWindows=new Map();"
		"var lastPreferredWidth=0;"
		"var lastPreferredHeight=0;"
		"var measureScheduled=false;"
		"function clamp(value,max){"
		"value=Number(value);"
		"if(!isFinite(value)){return 0;}"
		"value=Math.round(value);"
		"if(value<1){return 0;}"
		"return Math.min(value,max||100000);"
		"}"
		"function findWrap(iframe){"
		"var node=iframe;"
		"while(node&&node!==document.body){"
		"if(node.classList&&node.classList.contains('iframe-wrap')){"
		"return node;"
		"}"
		"node=node.parentNode;"
		"}"
		"return null;"
		"}"
		"function rememberIframe(iframe){"
		"try{"
		"if(iframe.contentWindow){"
		"iframeWindows.set(iframe.contentWindow,iframe);"
		"}"
		"}catch(e){"
		"}"
		"}"
		"function seedIframeHeight(iframe){"
		"var wrap=findWrap(iframe);"
		"if(!wrap){return;}"
		"var fixed=clamp(wrap.getAttribute('data-height'),100000);"
		"if(fixed){"
		"iframe.style.height=fixed+'px';"
		"return;"
		"}"
		"if(iframe.getAttribute('data-native-iv-resized')==='1'){"
		"return;"
		"}"
		"var ratio=Number(wrap.getAttribute('data-aspect-ratio'));"
		"if(!isFinite(ratio)||ratio<=0){"
		"return;"
		"}"
		"var width=clamp(Math.ceil("
		"wrap.getBoundingClientRect().width"
		"||iframe.getBoundingClientRect().width"
		"||0),100000);"
		"if(!width){"
		"return;"
		"}"
		"var height=clamp(width*ratio,100000);"
		"if(height){"
		"iframe.style.height=height+'px';"
		"}"
		"}"
		"function syncIframe(iframe){"
		"rememberIframe(iframe);"
		"seedIframeHeight(iframe);"
		"if(iframe.getAttribute('data-native-iv-registered')==='1'){"
		"return;"
		"}"
		"iframe.setAttribute('data-native-iv-registered','1');"
		"iframe.addEventListener('load',function(){"
		"rememberIframe(iframe);"
		"seedIframeHeight(iframe);"
		"scheduleMeasure();"
		"},false);"
		"}"
		"function syncIframes(){"
		"var iframes=document.getElementsByTagName('iframe');"
		"for(var i=0;i<iframes.length;i++){"
		"syncIframe(iframes[i]);"
		"}"
		"}"
		"function measurePreferredSize(){"
		"var width=0;"
		"var height=0;"
		"var body=document.body;"
		"if(body){"
		"var bodyRect=body.getBoundingClientRect();"
		"height=Math.max(height,body.scrollHeight,body.offsetHeight,bodyRect.height);"
		"var children=body.children;"
		"for(var i=0;i<children.length;i++){"
		"var child=children[i];"
		"var rect=child.getBoundingClientRect();"
		"width=Math.max(width,child.scrollWidth,child.offsetWidth,rect.width);"
		"height=Math.max(height,child.scrollHeight,child.offsetHeight,rect.bottom-bodyRect.top);"
		"}"
		"if(!width){"
		"width=Math.max(body.scrollWidth,body.offsetWidth,bodyRect.width);"
		"}"
		"}"
		"var doc=document.documentElement;"
		"if(doc&&(!width||!height)){"
		"var docRect=doc.getBoundingClientRect();"
		"if(!width){"
		"width=Math.max(doc.scrollWidth,doc.offsetWidth,doc.clientWidth,docRect.width);"
		"}"
		"if(!height){"
		"height=Math.max(doc.scrollHeight,doc.offsetHeight,doc.clientHeight,docRect.height);"
		"}"
		"}"
		"return{"
		"width:clamp(width,100000),"
		"height:clamp(height,100000)"
		"};"
		"}"
		"function reportPreferredSize(){"
		"var size=measurePreferredSize();"
		"if(!size.width||!size.height){"
		"return;"
		"}"
		"if(size.width===lastPreferredWidth&&size.height===lastPreferredHeight){"
		"return;"
		"}"
		"lastPreferredWidth=size.width;"
		"lastPreferredHeight=size.height;"
		"if(window.external&&typeof window.external.invoke==='function'){"
		"try{"
		"window.external.invoke(JSON.stringify({"
		"event:'preferred_size',"
		"width:size.width,"
		"height:size.height"
		"}));"
		"}catch(e){"
		"}"
		"}"
		"}"
		"function scheduleMeasure(){"
		"if(measureScheduled){"
		"return;"
		"}"
		"measureScheduled=true;"
		"var callback=function(){"
		"measureScheduled=false;"
		"reportPreferredSize();"
		"};"
		"if(window.requestAnimationFrame){"
		"window.requestAnimationFrame(callback);"
		"}else{"
		"setTimeout(callback,0);"
		"}"
		"}"
		"window.addEventListener('message',function(event){"
		"var iframe=iframeWindows.get(event.source);"
		"if(!iframe){"
		"return;"
		"}"
		"var data=event.data;"
		"if(typeof data==='string'){"
		"try{"
		"data=JSON.parse(data);"
		"}catch(e){"
		"return;"
		"}"
		"}"
		"if(!data||data.eventType!=='resize_frame'||!data.eventData){"
		"return;"
		"}"
		"var height=clamp(data.eventData.height,100000);"
		"if(!height){"
		"return;"
		"}"
		"iframe.setAttribute('data-native-iv-resized','1');"
		"iframe.style.height=height+'px';"
		"scheduleMeasure();"
		"},false);"
		"document.addEventListener('DOMContentLoaded',function(){"
		"syncIframes();"
		"scheduleMeasure();"
		"},false);"
		"window.addEventListener('load',function(){"
		"syncIframes();"
		"scheduleMeasure();"
		"},false);"
		"window.addEventListener('resize',function(){"
		"syncIframes();"
		"scheduleMeasure();"
		"},false);"
		"if(window.ResizeObserver){"
		"var observer=new ResizeObserver(function(){"
		"scheduleMeasure();"
		"});"
		"if(document.documentElement){"
		"observer.observe(document.documentElement);"
		"}"
		"if(document.body){"
		"observer.observe(document.body);"
		"}"
		"}else{"
		"setInterval(scheduleMeasure,250);"
		"}"
		"syncIframes();"
		"scheduleMeasure();"
		"})();"
		"</script></body></html>");
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

[[nodiscard]] const NativeIvDocumentInfo *FindNativeIvDocument(
		uint64 documentId,
		const NativeIvPrepareState &state) {
	for (const auto &document : state.documents) {
		if (document.id == documentId) {
			return &document;
		}
	}
	return nullptr;
}

[[nodiscard]] QByteArray RenderNativeIvRichTextHtml(
		const MTPRichText &text,
		NativeIvPrepareState *state) {
	return text.match([&](const MTPDtextEmpty &) {
		return QByteArray();
	}, [&](const MTPDtextPlain &data) {
		return NativeIvRichText(qs(data.vtext()));
	}, [&](const MTPDtextConcat &data) {
		auto result = QByteArray();
		for (const auto &part : data.vtexts().v) {
			result += RenderNativeIvRichTextHtml(part, state);
		}
		return result;
	}, [&](const MTPDtextImage &data) {
		const auto documentId = uint64(data.vdocument_id().v);
		if (!FindNativeIvDocument(documentId, *state)) {
			return NativeIvRichText(u"Image not found."_q);
		}
		auto attributes = NativeIvHtmlAttributes{
			{ "class", "pic" },
			{ "src", NativeIvHtmlEscape(
				QString::fromUtf8(NativeIvDocumentUrl(documentId))) },
		};
		if (const auto width = data.vw().v) {
			attributes.push_back({ "width", QByteArray::number(width) });
		}
		if (const auto height = data.vh().v) {
			attributes.push_back({ "height", QByteArray::number(height) });
		}
		return NativeIvHtmlTag("img", attributes);
	}, [&](const MTPDtextBold &data) {
		return NativeIvHtmlTag("b", {}, RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextItalic &data) {
		return NativeIvHtmlTag("i", {}, RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextUnderline &data) {
		return NativeIvHtmlTag("u", {}, RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextStrike &data) {
		return NativeIvHtmlTag("s", {}, RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextFixed &data) {
		return NativeIvHtmlTag("code", {}, RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextUrl &data) {
		auto attributes = NativeIvHtmlAttributes{
			{ "href", NativeIvHtmlEscape(qs(data.vurl())) },
		};
		return NativeIvHtmlTag(
			"a",
			attributes,
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextEmail &data) {
		return NativeIvHtmlTag(
			"a",
			{ { "href", "mailto:" + NativeIvHtmlEscape(qs(data.vemail())) } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextSubscript &data) {
		return NativeIvHtmlTag("sub", {}, RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextSuperscript &data) {
		return NativeIvHtmlTag("sup", {}, RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextMarked &data) {
		return NativeIvHtmlTag("mark", {}, RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextPhone &data) {
		return NativeIvHtmlTag(
			"a",
			{ { "href", "tel:" + NativeIvHtmlEscape(qs(data.vphone())) } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextAnchor &data) {
		const auto inner = RenderNativeIvRichTextHtml(data.vtext(), state);
		const auto name = NativeIvHtmlEscape(qs(data.vname()));
		return inner.isEmpty()
			? NativeIvHtmlTag("a", { { "name", name } })
			: NativeIvHtmlTag(
				"span",
				{ { "class", "reference" } },
				NativeIvHtmlTag("a", { { "name", name } }) + inner);
	});
}

[[nodiscard]] QByteArray NativeIvCaptionHtml(
		const MTPPageCaption &caption,
		NativeIvPrepareState *state) {
	auto text = RenderNativeIvRichTextHtml(caption.data().vtext(), state);
	const auto credit = RenderNativeIvRichTextHtml(caption.data().vcredit(), state);
	if (!credit.isEmpty()) {
		text += NativeIvHtmlTag("cite", { { "dir", "auto" } }, credit);
	} else if (text.isEmpty()) {
		return QByteArray();
	}
	return NativeIvHtmlTag("figcaption", { { "dir", "auto" } }, text);
}

[[nodiscard]] std::optional<QByteArray> RenderNativeIvEmbedPostHtml(
		const MTPDpageBlockEmbedPost &data,
		NativeIvPrepareState *state,
		bool includeCaption);
[[nodiscard]] QString StripOneTrailingNewline(QString text);

[[nodiscard]] std::optional<QByteArray> RenderNativeIvBlockHtml(
	const MTPPageBlock &block,
	NativeIvPrepareState *state);

[[nodiscard]] std::optional<QByteArray> RenderNativeIvBlocksHtml(
		const QVector<MTPPageBlock> &blocks,
		NativeIvPrepareState *state) {
	auto result = QByteArray();
	for (const auto &block : blocks) {
		const auto rendered = RenderNativeIvBlockHtml(block, state);
		if (!rendered) {
			return std::nullopt;
		}
		result += *rendered;
	}
	return result;
}

[[nodiscard]] std::optional<QByteArray> RenderNativeIvListItemHtml(
		const MTPPageListItem &item,
		NativeIvPrepareState *state) {
	return item.match([&](const MTPDpageListItemText &data)
			-> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"li",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDpageListItemBlocks &data) -> std::optional<QByteArray> {
		const auto blocks = RenderNativeIvBlocksHtml(data.vblocks().v, state);
		return blocks ? std::make_optional(NativeIvHtmlTag("li", {}, *blocks)) : std::nullopt;
	});
}

[[nodiscard]] std::optional<QByteArray> RenderNativeIvOrderedListItemHtml(
		const MTPPageListOrderedItem &item,
		NativeIvPrepareState *state) {
	return item.match([&](const MTPDpageListOrderedItemText &data)
			-> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"li",
			{
				{ "value", NativeIvHtmlEscape(qs(data.vnum())) },
				{ "dir", "auto" },
			},
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDpageListOrderedItemBlocks &data)
			-> std::optional<QByteArray> {
		const auto blocks = RenderNativeIvBlocksHtml(data.vblocks().v, state);
		if (!blocks) {
			return std::nullopt;
		}
		return NativeIvHtmlTag(
			"li",
			{ { "value", NativeIvHtmlEscape(qs(data.vnum())) } },
			*blocks);
	});
}

[[nodiscard]] QByteArray RenderNativeIvEmbedHtml(
		const MTPDpageBlockEmbed &data,
		NativeIvPrepareState *state,
		bool includeCaption) {
	auto iframeWidth = QByteArray();
	auto iframeHeight = QByteArray();
	auto width = QByteArray();
	auto height = QByteArray();
	auto aspectRatio = QByteArray();
	auto iframeAttributes = NativeIvHtmlAttributes();
	const auto autosize = !data.vw();
	if (autosize) {
		iframeWidth = "100%";
	} else if (data.is_full_width() || !data.vw()->v) {
		width = "100%";
		height = QByteArray::number(data.vh()->v) + "px";
		iframeWidth = "100%";
		iframeHeight = height;
	} else {
		width = QByteArray::number(data.vw()->v) + "px";
		aspectRatio = QByteArray::number(
			double(data.vh()->v) / std::max(data.vw()->v, 1),
			'g',
			16);
	}
	if (!iframeWidth.isEmpty()) {
		iframeAttributes.push_back({ "width", iframeWidth });
	}
	if (!iframeHeight.isEmpty()) {
		iframeAttributes.push_back({ "height", iframeHeight });
	}
	if (const auto url = data.vurl()) {
		if (autosize) {
			iframeAttributes.push_back({
				"srcdoc",
				NativeIvHtmlEscape(qs(*url)),
			});
		} else {
			iframeAttributes.push_back({
				"src",
				NativeIvHtmlEscape(qs(*url)),
			});
		}
	} else if (const auto html = data.vhtml()) {
		const auto resourceId = StoreNativeIvEmbedHtml(html->v, state);
		iframeAttributes.push_back({
			"src",
			NativeIvHtmlEscape(QString::fromUtf8(NativeIvResourceUrl(resourceId))),
		});
	} else {
		return QByteArray();
	}
	if (!data.is_allow_scrolling()) {
		iframeAttributes.push_back({ "scrolling", "no" });
	}
	iframeAttributes.push_back({ "frameborder", "0" });
	iframeAttributes.push_back({ "allowtransparency", "true" });
	iframeAttributes.push_back({ "allowfullscreen", "true" });
	auto content = NativeIvHtmlTag("iframe", iframeAttributes);
	if (!autosize) {
		auto wrapAttributes = NativeIvHtmlAttributes{
			{ "class", "iframe-wrap" },
		};
		auto style = QByteArray();
		if (!width.isEmpty()) {
			style += QByteArray("width:") + width + ';';
		}
		if (!style.isEmpty()) {
			wrapAttributes.push_back({ "style", style });
		}
		if (!height.isEmpty()) {
			wrapAttributes.push_back({
				"data-height",
				QByteArray::number(data.vh()->v),
			});
		} else if (!aspectRatio.isEmpty()) {
			wrapAttributes.push_back({
				"data-aspect-ratio",
				aspectRatio,
			});
		}
		content = NativeIvHtmlTag("div", wrapAttributes, content);
	}
	if (includeCaption) {
		content += NativeIvCaptionHtml(data.vcaption(), state);
	}
	auto figureAttributes = NativeIvHtmlAttributes();
	if (!data.is_full_width()) {
		figureAttributes.push_back({ "class", "nowide" });
	}
	return NativeIvHtmlTag("figure", figureAttributes, content);
}

[[nodiscard]] std::optional<QByteArray> RenderNativeIvEmbedPostHtml(
		const MTPDpageBlockEmbedPost &data,
		NativeIvPrepareState *state,
		bool includeCaption) {
	auto content = QByteArray();
	if (!data.vblocks().v.isEmpty()) {
		auto address = QByteArray();
		if (const auto photoId = uint64(data.vauthor_photo_id().v)
			; FindNativeIvPhoto(photoId, *state)) {
			address += NativeIvHtmlTag(
				"figure",
				{
					{ "style", QByteArray("background-image:url('")
						+ NativeIvPhotoUrl(photoId)
						+ "')" },
				});
		}
		auto meta = NativeIvHtmlTag(
			"a",
			{
				{ "rel", "author" },
				{ "onclick", "return false;" },
			},
			NativeIvHtmlText(qs(data.vauthor())));
		if (const auto date = data.vdate().v) {
			meta += NativeIvHtmlTag(
				"time",
				{},
				NativeIvHtmlText(NativeIvDateText(date)));
		}
		address += NativeIvHtmlTag("div", { { "class", "meta" } }, meta);
		const auto blocks = RenderNativeIvBlocksHtml(data.vblocks().v, state);
		if (!blocks) {
			return std::nullopt;
		}
		content = NativeIvHtmlTag(
			"blockquote",
			{ { "class", "embed-post" } },
			NativeIvHtmlTag("address", {}, address) + *blocks);
	} else {
		const auto url = qs(data.vurl());
		content = NativeIvHtmlTag(
			"section",
			{ { "class", "embed-post" } },
			NativeIvHtmlTag(
				"strong",
				{},
				NativeIvHtmlText(qs(data.vauthor())))
			+ NativeIvHtmlTag(
				"small",
				{},
				NativeIvHtmlTag(
					"a",
					{ { "href", NativeIvHtmlEscape(url) } },
					NativeIvHtmlText(url))));
	}
	if (includeCaption) {
		content += NativeIvCaptionHtml(data.vcaption(), state);
	}
	return NativeIvHtmlTag("figure", {}, content);
}

[[nodiscard]] std::optional<QByteArray> RenderNativeIvBlockHtml(
		const MTPPageBlock &block,
		NativeIvPrepareState *state) {
	return block.match([&](const MTPDpageBlockTitle &data)
			-> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"h1",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDpageBlockSubtitle &data) -> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"h2",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDpageBlockAuthorDate &data) -> std::optional<QByteArray> {
		auto inner = RenderNativeIvRichTextHtml(data.vauthor(), state);
		if (const auto date = data.vpublished_date().v) {
			if (!inner.isEmpty()) {
				inner += " \xE2\x80\xA2 ";
			}
			inner += NativeIvHtmlTag(
				"time",
				{},
				NativeIvHtmlEscape(NativeIvDateText(date)));
		}
		return NativeIvHtmlTag("address", { { "dir", "auto" } }, inner);
	}, [&](const MTPDpageBlockHeader &data) -> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"h3",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDpageBlockSubheader &data) -> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"h4",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDpageBlockParagraph &data) -> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"p",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDpageBlockPreformatted &data) -> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"pre",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDpageBlockFooter &data) -> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"footer",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDpageBlockDivider &) -> std::optional<QByteArray> {
		return NativeIvHtmlTag("hr");
	}, [&](const MTPDpageBlockAnchor &data) -> std::optional<QByteArray> {
		const auto name = NormalizeFragmentId(qs(data.vname()));
		return name.isEmpty()
			? std::make_optional(QByteArray())
			: std::make_optional(NativeIvHtmlTag(
				"a",
				{ { "name", NativeIvHtmlEscape(name) } }));
	}, [&](const MTPDpageBlockList &data) -> std::optional<QByteArray> {
		auto inner = QByteArray();
		for (const auto &item : data.vitems().v) {
			const auto rendered = RenderNativeIvListItemHtml(item, state);
			if (!rendered) {
				return std::nullopt;
			}
			inner += *rendered;
		}
		return NativeIvHtmlTag("ul", {}, inner);
	}, [&](const MTPDpageBlockOrderedList &data) -> std::optional<QByteArray> {
		auto inner = QByteArray();
		for (const auto &item : data.vitems().v) {
			const auto rendered = RenderNativeIvOrderedListItemHtml(item, state);
			if (!rendered) {
				return std::nullopt;
			}
			inner += *rendered;
		}
		return NativeIvHtmlTag("ol", {}, inner);
	}, [&](const MTPDpageBlockBlockquote &data) -> std::optional<QByteArray> {
		auto inner = NativeIvHtmlTag(
			"p",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
		const auto caption = RenderNativeIvRichTextHtml(data.vcaption(), state);
		if (!caption.isEmpty()) {
			inner += NativeIvHtmlTag("cite", { { "dir", "auto" } }, caption);
		}
		return NativeIvHtmlTag("blockquote", {}, inner);
	}, [&](const MTPDpageBlockPullquote &data) -> std::optional<QByteArray> {
		auto inner = NativeIvHtmlTag(
			"p",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
		const auto caption = RenderNativeIvRichTextHtml(data.vcaption(), state);
		if (!caption.isEmpty()) {
			inner += NativeIvHtmlTag("cite", { { "dir", "auto" } }, caption);
		}
		return NativeIvHtmlTag("blockquote", {}, inner);
	}, [&](const MTPDpageBlockPhoto &data) -> std::optional<QByteArray> {
		const auto photoId = uint64(data.vphoto_id().v);
		if (!FindNativeIvPhoto(photoId, *state)) {
			return std::nullopt;
		}
		auto content = NativeIvHtmlTag(
			"img",
			{
				{ "src", NativeIvHtmlEscape(QString::fromUtf8(NativeIvPhotoUrl(photoId))) },
			});
		content += NativeIvCaptionHtml(data.vcaption(), state);
		return NativeIvHtmlTag("figure", {}, content);
	}, [&](const MTPDpageBlockVideo &data) -> std::optional<QByteArray> {
		const auto documentId = uint64(data.vvideo_id().v);
		if (!FindNativeIvDocument(documentId, *state)) {
			return std::nullopt;
		}
		auto content = NativeIvHtmlTag(
			"video",
			{
				{ "controls", "controls" },
				{ "src", NativeIvHtmlEscape(QString::fromUtf8(NativeIvDocumentUrl(documentId))) },
			});
		content += NativeIvCaptionHtml(data.vcaption(), state);
		return NativeIvHtmlTag("figure", {}, content);
	}, [&](const MTPDpageBlockCover &data) -> std::optional<QByteArray> {
		return RenderNativeIvBlockHtml(data.vcover(), state);
	}, [&](const MTPDpageBlockEmbed &data) -> std::optional<QByteArray> {
		const auto html = RenderNativeIvEmbedHtml(data, state, true);
		return html.isEmpty() ? std::nullopt : std::make_optional(std::move(html));
	}, [&](const MTPDpageBlockEmbedPost &data) -> std::optional<QByteArray> {
		return RenderNativeIvEmbedPostHtml(data, state, true);
	}, [&](const MTPDpageBlockAudio &data) -> std::optional<QByteArray> {
		const auto documentId = uint64(data.vaudio_id().v);
		if (!FindNativeIvDocument(documentId, *state)) {
			return std::nullopt;
		}
		auto content = NativeIvHtmlTag(
			"audio",
			{
				{ "controls", "controls" },
				{ "src", NativeIvHtmlEscape(QString::fromUtf8(NativeIvDocumentUrl(documentId))) },
			});
		content += NativeIvCaptionHtml(data.vcaption(), state);
		return NativeIvHtmlTag("figure", {}, content);
	}, [&](const MTPDpageBlockMap &data) -> std::optional<QByteArray> {
		return data.vgeo().match([&](const MTPDgeoPoint &geo)
				-> std::optional<QByteArray> {
			if (!geo.vaccess_hash().v || data.vw().v <= 0 || data.vh().v <= 0) {
				return std::nullopt;
			}
			auto content = NativeIvHtmlTag(
				"img",
				{
					{ "src", NativeIvHtmlEscape(QString::fromUtf8(NativeIvMapUrl(
						geo,
						data.vw().v,
						data.vh().v,
						data.vzoom().v))) },
				});
			content += NativeIvCaptionHtml(data.vcaption(), state);
			return NativeIvHtmlTag("figure", {}, content);
		}, [&](const auto &) -> std::optional<QByteArray> {
			return std::nullopt;
		});
	}, [&](const MTPDpageBlockKicker &data) -> std::optional<QByteArray> {
		return NativeIvHtmlTag(
			"h5",
			{ { "dir", "auto" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const auto &) -> std::optional<QByteArray> {
		return std::nullopt;
	});
}

[[nodiscard]] bool PrepareNativeIvEmbedBlock(
		const MTPDpageBlockEmbed &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	const auto html = RenderNativeIvEmbedHtml(data, state, false);
	if (html.isEmpty()) {
		return PrepareNativeIvPlaceholderBlock(
			u"Embed Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}
	auto request = EmbedRequest{
		.resourceId = StoreNativeIvEmbedHtml(
			WrapNativeIvEmbedHtml(html),
			state),
		.fallbackUrl = (!data.vw() || !data.vurl())
			? QString()
			: qs(*data.vurl()),
		.width = data.vw() ? data.vw()->v : 0,
		.height = data.vh() ? data.vh()->v : 0,
		.fullWidth = data.is_full_width(),
		.allowScrolling = data.is_allow_scrolling(),
	};
	return PrepareNativeIvPlaceholderBlock(
		u"Embed Placeholder"_q,
		data.vcaption(),
		result,
		state,
		std::move(request));
}

[[nodiscard]] bool PrepareNativeIvEmbedPostBlock(
		const MTPDpageBlockEmbedPost &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	const auto html = RenderNativeIvEmbedPostHtml(data, state, false);
	if (!html) {
		return PrepareNativeIvPlaceholderBlock(
			u"Embed Placeholder"_q,
			data.vcaption(),
			result,
			state);
	}
	auto request = EmbedRequest{
		.resourceId = StoreNativeIvEmbedHtml(
			WrapNativeIvEmbedHtml(*html),
			state),
		.fallbackUrl = qs(data.vurl()),
		.allowScrolling = true,
	};
	return PrepareNativeIvPlaceholderBlock(
		u"Embed Placeholder"_q,
		data.vcaption(),
		result,
		state,
		std::move(request));
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
		return PrepareNativeIvEmbedBlock(data, result, state);
	}, [&](const MTPDpageBlockEmbedPost &data) {
		return PrepareNativeIvEmbedPostBlock(data, result, state);
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
