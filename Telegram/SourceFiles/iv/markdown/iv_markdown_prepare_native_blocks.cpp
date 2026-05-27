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
#include "iv/iv_rich_page.h"
#include "iv/markdown/iv_markdown_prepare_links.h"
#include "lang/lang_keys.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Iv::Markdown {
namespace {

using RichPage = Iv::RichPage;
using RichPageBlock = Iv::RichPage::Block;
using RichPageBlockKind = Iv::RichPage::BlockKind;
using RichPageListItem = Iv::RichPage::ListItem;
using RichPageRelatedArticle = Iv::RichPage::RelatedArticle;
using RichPageTableCell = Iv::RichPage::TableCell;
using RichPageTableRow = Iv::RichPage::TableRow;

[[nodiscard]] QString NativeIvDateText(TimeId date) {
	return langDateTimeFull(base::unixtime::parse(date));
}

[[nodiscard]] int ScaleNativeIvFormulaCap(
		int cap,
		int textSize,
		int baseTextSize) {
	if (cap <= 0) {
		return 0;
	}
	const auto numerator = int64(cap) * std::max(textSize, 1);
	const auto denominator = std::max(baseTextSize, 1);
	return std::max(int((numerator + denominator - 1) / denominator), 1);
}

[[nodiscard]] NativeIvRichTextContext NativeIvRichTextContextForTextSize(
		int textSize,
		const MarkdownPrepareDimensions &dimensions) {
	return {
		.textSize = textSize,
		.renderWidthCap = ScaleNativeIvFormulaCap(
			dimensions.displayMathMaxRenderWidth,
			textSize,
			dimensions.displayMathTextSize),
		.renderHeightCap = ScaleNativeIvFormulaCap(
			dimensions.displayMathMaxRenderHeight,
			textSize,
			dimensions.displayMathTextSize),
	};
}

[[nodiscard]] int NativeIvFlowTextSize(
		PreparedBlockKind kind,
		int headingLevel,
		const MarkdownPrepareDimensions &dimensions) {
	if (kind == PreparedBlockKind::Heading
		&& headingLevel >= 1
		&& headingLevel <= int(dimensions.headingTextSizes.size())) {
		return dimensions.headingTextSizes[headingLevel - 1];
	}
	return dimensions.bodyTextSize;
}

[[nodiscard]] QString NativeIvRelatedArticleFooterText(
		const MTPDpageRelatedArticle &data) {
	const auto author = qs(data.vauthor().value_or_empty()).trimmed();
	const auto published = data.vpublished_date();
	if (published && !author.isEmpty()) {
		return author + u", "_q + NativeIvDateText(published->v);
	} else if (published) {
		return NativeIvDateText(published->v);
	}
	return author;
}

[[nodiscard]] PreparedLink PrepareNativeIvRelatedArticleLink(
		QString url,
		uint64 webpageId,
		QStringView renderedText) {
	auto result = PreparedLink();
	if (webpageId) {
		result.kind = PreparedLinkKind::InstantViewPage;
		result.webpageId = webpageId;
		NormalizePreparedUrlLink(&result, url);
	} else {
		result = ClassifiedLink(0, url, nullptr);
		if (result.kind == PreparedLinkKind::RejectedRelative
			|| result.kind == PreparedLinkKind::LocalFile) {
			result.kind = PreparedLinkKind::External;
			NormalizePreparedUrlLink(&result, url);
		}
	}
	FinalizePreparedUrlLink(&result, renderedText);
	return result;
}

[[nodiscard]] QString NativeIvDetailsAnchorId(NativeIvPrepareState *state) {
	return u"details-"_q + QString::number(++state->nextGeneratedId);
}

void SortPreparedIvRichText(PreparedIvRichText *text) {
	SortEntities(&text->text);
}

[[nodiscard]] uint16 InternalPreparedLinkIndex(const QString &data) {
	const auto prefix = u"internal:index"_q;
	return (data.size() == prefix.size() + 1 && data.startsWith(prefix))
		? uint16(data.back().unicode())
		: uint16(0);
}

[[nodiscard]] uint16 RemappedPreparedLinkIndex(
		const std::vector<std::pair<uint16, uint16>> &remapped,
		uint16 index) {
	for (const auto &[from, to] : remapped) {
		if (from == index) {
			return to;
		}
	}
	return 0;
}

void AppendPreparedIvRichText(
		PreparedIvRichText *result,
		PreparedIvRichText prepared) {
	auto remapped = std::vector<std::pair<uint16, uint16>>();
	remapped.reserve(prepared.links.size());
	result->links.reserve(result->links.size() + prepared.links.size());
	for (const auto &link : prepared.links) {
		const auto index = result->links.size() + 1;
		if (index > std::numeric_limits<uint16>::max()) {
			continue;
		}
		auto copy = link;
		copy.index = uint16(index);
		remapped.push_back({ link.index, copy.index });
		result->links.push_back(std::move(copy));
	}

	const auto shift = result->text.text.size();
	result->text.text.append(prepared.text.text);
	result->text.entities.reserve(
		result->text.entities.size() + prepared.text.entities.size());
	for (const auto &entity : prepared.text.entities) {
		auto data = entity.data();
		if (entity.type() == EntityType::CustomUrl) {
			if (const auto from = InternalPreparedLinkIndex(data)) {
				if (const auto to = RemappedPreparedLinkIndex(remapped, from)) {
					data = InternalLinkData(to);
				}
			}
		}
		result->text.entities.push_back(EntityInText(
			entity.type(),
			entity.offset() + shift,
			entity.length(),
			data));
	}
	result->anchorIds.reserve(result->anchorIds.size() + prepared.anchorIds.size());
	for (auto &anchorId : prepared.anchorIds) {
		result->anchorIds.push_back(std::move(anchorId));
	}
}

[[nodiscard]] bool PrepareNativeIvCaption(
		const MTPPageCaption &caption,
		PreparedIvRichText *result,
		QString *blockAnchorId,
		NativeIvPrepareState *state) {
	auto text = PreparedIvRichText();
	if (!PrepareNativeIvRichText(
			caption.data().vtext(),
			&text,
			blockAnchorId,
			state)) {
		return false;
	}
	AppendPreparedIvRichText(result, std::move(text));

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
		AppendPreparedIvRichText(result, std::move(credit));
	}
	return true;
}

struct NativeIvHtmlAttribute {
	QByteArray name;
	QByteArray value;
};

using NativeIvHtmlAttributes = std::vector<NativeIvHtmlAttribute>;

[[nodiscard]] QByteArray NativeIvHtmlEscape(QString text) {
	return text.toHtmlEscaped().toUtf8();
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
		QByteArray name,
		NativeIvHtmlAttributes attributes = {},
		QByteArray body = {}) {
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

[[nodiscard]] QByteArray NativeIvDocumentUrl(uint64 documentId) {
	return NativeIvResourceUrl(
		QByteArray("document/") + QByteArray::number(documentId));
}

[[nodiscard]] QByteArray WrapNativeIvEmbedHtml(QByteArray body) {
	return QByteArray(R"NativeIvHtml(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<script>
window.TelegramWebviewProxy = {
	postEvent: function(eventType, eventData) {
		if (window.external && typeof window.external.invoke === 'function') {
			try {
				window.external.invoke(JSON.stringify([eventType, eventData]));
			} catch (e) {
			}
		}
	}
};
</script>
<style>
html,
body {
	margin: 0;
	padding: 0;
	background: #fff;
	color: #000;
}

body {
	font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
}

figure {
	display: block;
	margin: 0;
	max-width: 100%;
}

img,
video {
	display: block;
	max-width: 100%;
	height: auto;
}

.iframe-wrap {
	display: block;
	margin: 0 auto;
	max-width: 100%;
	overflow: hidden;
}

iframe {
	display: block;
	width: 100%;
	max-width: 100%;
	border: 0;
}
</style>
</head>
<body>)NativeIvHtml")
		+ body
		+ QByteArray(R"NativeIvHtml(<script>
(function() {
	var iframeWindows = new Map();
	var lastPreferredWidth = 0;
	var lastPreferredHeight = 0;
	var measureScheduled = false;

	function clamp(value, max) {
		value = Number(value);
		if (!isFinite(value)) {
			return 0;
		}
		value = Math.round(value);
		if (value < 1) {
			return 0;
		}
		return Math.min(value, max || 100000);
	}

	function resourceId() {
		var path = '';
		try {
			path = String(window.location.pathname || '');
		} catch (e) {
		}
		while (path.charAt(0) === '/') {
			path = path.slice(1);
		}
		return path;
	}

	function documentHeight(doc) {
		if (!doc) {
			return 0;
		}
		var body = doc.body;
		var root = doc.documentElement;
		var height = 0;
		if (body) {
			var bodyRect = body.getBoundingClientRect();
			height = Math.max(
				height,
				body.scrollHeight,
				body.offsetHeight,
				body.clientHeight,
				bodyRect.height);
		}
		if (root) {
			var rootRect = root.getBoundingClientRect();
			height = Math.max(
				height,
				root.scrollHeight,
				root.offsetHeight,
				root.clientHeight,
				rootRect.height);
		}
		return clamp(height, 100000);
	}

	function iframeDocumentHeight(iframe) {
		try {
			return documentHeight(
				iframe.contentDocument
				|| (iframe.contentWindow && iframe.contentWindow.document));
		} catch (e) {
			return 0;
		}
	}

	function findWrap(iframe) {
		var node = iframe;
		while (node && node !== document.body) {
			if (node.classList && node.classList.contains('iframe-wrap')) {
				return node;
			}
			node = node.parentNode;
		}
		return null;
	}

	function rememberIframe(iframe) {
		try {
			if (iframe.contentWindow) {
				iframeWindows.set(iframe.contentWindow, iframe);
			}
		} catch (e) {
		}
	}

	function seedIframeHeight(iframe) {
		if (iframe.getAttribute('data-native-iv-resized') === '1') {
			return;
		}
		var measured = iframeDocumentHeight(iframe);
		if (measured) {
			iframe.setAttribute('data-native-iv-measured', '1');
			iframe.style.height = measured + 'px';
			return;
		}
		var wrap = findWrap(iframe);
		if (!wrap) {
			return;
		}
		var fixed = clamp(wrap.getAttribute('data-height'), 100000);
		if (fixed) {
			iframe.style.height = fixed + 'px';
			return;
		}
		var ratio = Number(wrap.getAttribute('data-aspect-ratio'));
		if (!isFinite(ratio) || ratio <= 0) {
			return;
		}
		var width = clamp(Math.ceil(
			wrap.getBoundingClientRect().width
			|| iframe.getBoundingClientRect().width
			|| 0), 100000);
		if (!width) {
			return;
		}
		var height = clamp(width * ratio, 100000);
		if (height) {
			iframe.style.height = height + 'px';
		}
	}

	function syncIframe(iframe) {
		rememberIframe(iframe);
		seedIframeHeight(iframe);
		if (iframe.getAttribute('data-native-iv-registered') === '1') {
			return;
		}
		iframe.setAttribute('data-native-iv-registered', '1');
		iframe.addEventListener('load', function() {
			rememberIframe(iframe);
			seedIframeHeight(iframe);
			scheduleMeasure();
		}, false);
	}

	function syncIframes() {
		var iframes = document.getElementsByTagName('iframe');
		for (var i = 0; i < iframes.length; i++) {
			syncIframe(iframes[i]);
		}
	}

	function measurePreferredSize() {
		var width = 0;
		var height = 0;
		var viewportWidth = clamp(
			Math.max(
				window.innerWidth || 0,
				document.documentElement
					? document.documentElement.clientWidth
					: 0),
			100000);
		var body = document.body;
		if (body) {
			var bodyRect = body.getBoundingClientRect();
			height = Math.max(
				height,
				body.scrollHeight,
				body.offsetHeight,
				bodyRect.height);
			var children = body.children;
			for (var i = 0; i < children.length; i++) {
				var child = children[i];
				var rect = child.getBoundingClientRect();
				width = Math.max(
					width,
					child.scrollWidth,
					child.offsetWidth,
					rect.width);
				height = Math.max(
					height,
					child.scrollHeight,
					child.offsetHeight,
					rect.bottom - bodyRect.top);
			}
			if (!width) {
				width = Math.max(
					body.scrollWidth,
					body.offsetWidth,
					bodyRect.width);
			}
		}
		var doc = document.documentElement;
		if (doc && (!width || !height)) {
			var docRect = doc.getBoundingClientRect();
			if (!width) {
				width = Math.max(
					doc.scrollWidth,
					doc.offsetWidth,
					doc.clientWidth,
					docRect.width);
			}
			if (!height) {
				height = Math.max(
					doc.scrollHeight,
					doc.offsetHeight,
					doc.clientHeight,
					docRect.height);
			}
		}
		return {
			width: clamp(width, 100000),
			height: clamp(height, 100000),
			viewportWidth: viewportWidth
		};
	}

	function reportPreferredSize() {
		var size = measurePreferredSize();
		if (!size.width || !size.height) {
			return;
		}
		if (size.width === lastPreferredWidth
			&& size.height === lastPreferredHeight) {
			return;
		}
		lastPreferredWidth = size.width;
		lastPreferredHeight = size.height;
		if (window.external && typeof window.external.invoke === 'function') {
			try {
				window.external.invoke(JSON.stringify({
					event: 'preferred_size',
					resourceId: resourceId(),
					width: size.width,
					height: size.height,
					viewportWidth: size.viewportWidth,
					devicePixelRatio: window.devicePixelRatio || 1
				}));
			} catch (e) {
			}
		}
	}

	function scheduleMeasure() {
		if (measureScheduled) {
			return;
		}
		measureScheduled = true;
		var callback = function() {
			measureScheduled = false;
			reportPreferredSize();
		};
		if (window.requestAnimationFrame) {
			window.requestAnimationFrame(callback);
		} else {
			setTimeout(callback, 0);
		}
	}

	window.addEventListener('message', function(event) {
		var iframe = iframeWindows.get(event.source);
		if (!iframe) {
			return;
		}
		var data = event.data;
		if (typeof data === 'string') {
			try {
				data = JSON.parse(data);
			} catch (e) {
				return;
			}
		}
		if (!data || data.eventType !== 'resize_frame' || !data.eventData) {
			return;
		}
		var height = clamp(data.eventData.height, 100000);
		if (!height) {
			return;
		}
		iframe.setAttribute('data-native-iv-resized', '1');
		iframe.style.height = height + 'px';
		scheduleMeasure();
	}, false);

	document.addEventListener('DOMContentLoaded', function() {
		syncIframes();
		scheduleMeasure();
	}, false);

	window.addEventListener('load', function() {
		syncIframes();
		scheduleMeasure();
	}, false);

	window.addEventListener('resize', function() {
		syncIframes();
		scheduleMeasure();
	}, false);

	if (window.ResizeObserver) {
		var observer = new ResizeObserver(function() {
			scheduleMeasure();
		});
		if (document.documentElement) {
			observer.observe(document.documentElement);
		}
		if (document.body) {
			observer.observe(document.body);
		}
	}

	setInterval(function() {
		syncIframes();
		scheduleMeasure();
	}, 250);

	syncIframes();
	scheduleMeasure();
})();
</script>
</body>
</html>)NativeIvHtml");
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
	const auto renderAutoLink = [&](const MTPRichText &child, QString prefix) {
		auto inner = RenderNativeIvRichTextHtml(child, state);
		const auto target = child.match([&](const MTPDtextPlain &data) {
			return prefix + qs(data.vtext());
		}, [](const auto &) {
			return QString();
		});
		return target.isEmpty()
			? inner
			: NativeIvHtmlTag(
				"a",
				{ { "href", NativeIvHtmlEscape(target) } },
				inner);
	};
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
	}, [&](const MTPDtextMath &data) {
		return NativeIvRichText(InlineFormulaCopySource(qs(data.vsource())));
	}, [&](const MTPDtextCustomEmoji &data) {
		return NativeIvRichText(qs(data.valt()));
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
	}, [&](const MTPDtextSpoiler &data) {
		return NativeIvHtmlTag(
			"span",
			{ { "class", "spoiler" } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextMention &data) {
		return RenderNativeIvRichTextHtml(data.vtext(), state);
	}, [&](const MTPDtextHashtag &data) {
		return RenderNativeIvRichTextHtml(data.vtext(), state);
	}, [&](const MTPDtextBotCommand &data) {
		return RenderNativeIvRichTextHtml(data.vtext(), state);
	}, [&](const MTPDtextCashtag &data) {
		return RenderNativeIvRichTextHtml(data.vtext(), state);
	}, [&](const MTPDtextAutoUrl &data) {
		return renderAutoLink(data.vtext(), QString());
	}, [&](const MTPDtextAutoEmail &data) {
		return renderAutoLink(data.vtext(), u"mailto:"_q);
	}, [&](const MTPDtextAutoPhone &data) {
		return renderAutoLink(data.vtext(), u"tel:"_q);
	}, [&](const MTPDtextBankCard &data) {
		return RenderNativeIvRichTextHtml(data.vtext(), state);
	}, [&](const MTPDtextMentionName &data) {
		return RenderNativeIvRichTextHtml(data.vtext(), state);
	}, [&](const MTPDtextDate &data) {
		return RenderNativeIvRichTextHtml(data.vtext(), state);
	}, [&](const MTPDtextPhone &data) {
		return NativeIvHtmlTag(
			"a",
			{ { "href", "tel:" + NativeIvHtmlEscape(qs(data.vphone())) } },
			RenderNativeIvRichTextHtml(data.vtext(), state));
	}, [&](const MTPDtextAnchor &data) {
		auto inner = RenderNativeIvRichTextHtml(data.vtext(), state);
		const auto name = NativeIvHtmlEscape(qs(data.vname()));
		auto anchor = NativeIvHtmlTag("a", { { "name", name } });
		if (inner.isEmpty()) {
			return anchor;
		}
		anchor += inner;
		return NativeIvHtmlTag("span", { { "class", "reference" } }, anchor);
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

[[nodiscard]] QString StripOneTrailingNewline(QString text);
[[nodiscard]] PreparedBlock PrepareNativeIvEmbedPostFallbackParagraph(
	QString url);

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
	const auto fullWidth = data.is_full_width() || (data.vw() && !data.vw()->v);
	if (autosize) {
		iframeWidth = "100%";
	} else if (fullWidth) {
		width = "100%";
		if (data.vh()) {
			if (data.vw()->v) {
				aspectRatio = QByteArray::number(
					double(data.vh()->v) / std::max(data.vw()->v, 1),
					'g',
					16);
			} else {
				height = QByteArray::number(data.vh()->v) + "px";
			}
		}
		iframeWidth = "100%";
		iframeHeight = height;
	} else {
		width = QByteArray::number(data.vw()->v) + "px";
		if (data.vh()) {
			aspectRatio = QByteArray::number(
				double(data.vh()->v) / std::max(data.vw()->v, 1),
				'g',
				16);
		}
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
	if (!fullWidth) {
		figureAttributes.push_back({ "class", "nowide" });
	}
	return NativeIvHtmlTag("figure", figureAttributes, content);
}

[[nodiscard]] bool PrepareNativeIvEmbedBlock(
		const MTPDpageBlockEmbed &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	const auto label = tr::lng_iv_click_to_view(tr::now);
	const auto html = RenderNativeIvEmbedHtml(data, state, false);
	if (html.isEmpty()) {
		return PrepareNativeIvPlaceholderBlock(
			label,
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
		.fullWidth = data.is_full_width() || (data.vw() && !data.vw()->v),
		.fixedHeight = (data.vh() != nullptr),
		.allowScrolling = data.is_allow_scrolling(),
	};
	return PrepareNativeIvPlaceholderBlock(
		label,
		data.vcaption(),
		result,
		state,
		std::move(request));
}

[[nodiscard]] bool PrepareNativeIvEmbedPostBlock(
		const MTPDpageBlockEmbedPost &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto caption = PreparedIvRichText();
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::EmbedPost;
	block.embedPost.url = qs(data.vurl()).trimmed();
	block.embedPost.authorPhotoId = uint64(data.vauthor_photo_id().v);
	block.embedPost.author = qs(data.vauthor()).trimmed();
	if (const auto date = data.vdate().v) {
		block.embedPost.dateText = NativeIvDateText(date);
	}
	if (!PrepareNativeIvCaption(
			data.vcaption(),
			&caption,
			&block.anchorId,
			state)) {
		return false;
	}
	SortPreparedIvRichText(&caption);
	block.text = std::move(caption.text);
	block.links = std::move(caption.links);
	block.anchorIds = std::move(caption.anchorIds);
	block.supplementary = true;
	if (data.vblocks().v.isEmpty()) {
		block.children.push_back(
			PrepareNativeIvEmbedPostFallbackParagraph(block.embedPost.url));
	} else if (!PrepareNativeIvBlocks(
			data.vblocks().v,
			&block.children,
			state)) {
		return false;
	}
	result->push_back(std::move(block));
	return true;
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

[[nodiscard]] PreparedBlock PrepareNativeIvEmbedPostFallbackParagraph(
		QString url) {
	auto block = EmptyParagraphBlock();
	if (url.isEmpty()) {
		return block;
	}
	auto link = PreparedLink();
	link.index = 1;
	link.kind = PreparedLinkKind::External;
	NormalizePreparedUrlLink(&link, url);
	FinalizePreparedUrlLink(&link, url);
	block.text.text = std::move(url);
	block.text.entities.push_back(EntityInText(
		EntityType::CustomUrl,
		0,
		block.text.text.size(),
		InternalLinkData(link.index)));
	block.links.push_back(std::move(link));
	return block;
}

[[nodiscard]] PreparedBlock PrepareRuleBlock() {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Rule;
	return block;
}

[[nodiscard]] bool PrepareNativeIvMathBlock(
		const MTPDpageBlockMath &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	const auto source = qs(data.vsource());
	if (source.trimmed().isEmpty()) {
		return true;
	}
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::DisplayMath;
	block.formulaTex = source;
	block.mathKind = MathKind::Display;
	block.formulaIndex = state->rememberFormula(block);
	result->push_back(std::move(block));
	return true;
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
	const auto context = NativeIvRichTextContextForTextSize(
		NativeIvFlowTextSize(kind, headingLevel, state->dimensions),
		state->dimensions);
	if (!PrepareNativeIvRichText(text, &prepared, &anchorId, state, context)) {
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

void WrapPreparedIvRichTextItalic(PreparedIvRichText *prepared) {
	if (!prepared || prepared->text.text.isEmpty()) {
		return;
	}
	prepared->text.entities.push_back(EntityInText(
		EntityType::Italic,
		0,
		prepared->text.text.size()));
}

bool AppendPreparedQuoteParagraph(
		std::vector<PreparedBlock> *result,
		PreparedIvRichText prepared,
		bool pullquote,
		bool supplementary = false) {
	if (pullquote) {
		WrapPreparedIvRichTextItalic(&prepared);
	}
	const auto count = result->size();
	if (!AppendPreparedIvRichBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			std::move(prepared),
			QString(),
			false,
			supplementary)) {
		return false;
	}
	if (pullquote && (result->size() > count)) {
		result->back().flowAlignment = TableAlignment::Center;
	}
	return true;
}

[[nodiscard]] bool PrepareNativeIvQuoteBlock(
		const MTPRichText &text,
		const MTPRichText &caption,
		bool pullquote,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Quote;
	block.pullquote = pullquote;
	auto body = PreparedIvRichText();
	if (!PrepareNativeIvRichText(text, &body, &block.anchorId, state)) {
		return false;
	}
	if (!AppendPreparedQuoteParagraph(
			&block.children,
			std::move(body),
			pullquote)) {
		return false;
	}
	auto cite = PreparedIvRichText();
	if (!PrepareNativeIvRichText(caption, &cite, &block.anchorId, state)) {
		return false;
	}
	if (!AppendPreparedQuoteParagraph(
			&block.children,
			std::move(cite),
			pullquote,
			true)) {
		return false;
	}
	if (block.children.empty()) {
		block.children.push_back(EmptyParagraphBlock());
		if (pullquote) {
			block.children.back().flowAlignment = TableAlignment::Center;
		}
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] TaskState NativeIvTaskState(bool checkbox, bool checked) {
	if (!checkbox) {
		return TaskState::None;
	}
	return checked ? TaskState::Checked : TaskState::Unchecked;
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
			block.taskState = NativeIvTaskState(
				data.is_checkbox(),
				data.is_checked());
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
			block.taskState = NativeIvTaskState(
				data.is_checkbox(),
				data.is_checked());
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
			block.taskState = NativeIvTaskState(
				data.is_checkbox(),
				data.is_checked());
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
			block.taskState = NativeIvTaskState(
				data.is_checkbox(),
				data.is_checked());
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
	block.anchorIds = std::move(title.anchorIds);

	const auto &limits = PrepareTableRenderLimitsForIv();
	const auto rowCount = std::min(int(data.vrows().v.size()), limits.maxRows);

	auto occupancy = NativeIvTableOccupancyGrid(rowCount);
	block.tableRows.reserve(rowCount);
	auto occupiedSlotCountSoFar = int64(0);

	auto rowIndex = 0;
	for (const auto &row : data.vrows().v) {
		if (rowIndex >= rowCount) {
			break;
		}
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
						const auto context = NativeIvRichTextContextForTextSize(
							cellData.is_header()
								? state->dimensions.tableHeaderTextSize
								: state->dimensions.tableBodyTextSize,
							state->dimensions);
						if (!PrepareNativeIvRichText(
								*cellData.vtext(),
								&rich,
								nullptr,
								state,
								context)) {
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
			return false;
		}
		block.tableRows.push_back(std::move(preparedRow));
		++rowIndex;
	}

	block.tableColumnCount = NativeIvTableColumnCount(occupancy);

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

[[nodiscard]] bool PrepareNativeIvRelatedArticlesBlock(
		const MTPDpageBlockRelatedArticles &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto related = std::vector<PreparedBlock>();
	related.reserve(data.varticles().v.size());
	for (const auto &article : data.varticles().v) {
		auto prepared = PreparedBlock();
		prepared.kind = PreparedBlockKind::RelatedArticle;
		const auto &row = article.data();
		prepared.relatedArticle.title = qs(
			row.vtitle().value_or_empty()).trimmed();
		prepared.relatedArticle.description = qs(
			row.vdescription().value_or_empty()).trimmed();
		prepared.relatedArticle.footer = NativeIvRelatedArticleFooterText(row);
		prepared.relatedArticle.photoId = row.vphoto_id().value_or_empty();
		if (prepared.relatedArticle.title.isEmpty()
			&& prepared.relatedArticle.description.isEmpty()
			&& prepared.relatedArticle.footer.isEmpty()) {
			prepared.relatedArticle.title = qs(row.vurl()).trimmed();
		}
		const auto linkText = !prepared.relatedArticle.title.isEmpty()
			? prepared.relatedArticle.title
			: !prepared.relatedArticle.description.isEmpty()
			? prepared.relatedArticle.description
			: prepared.relatedArticle.footer;
		prepared.relatedArticle.link = PrepareNativeIvRelatedArticleLink(
			qs(row.vurl()),
			uint64(row.vwebpage_id().v),
			linkText);
		const auto appendLine = [&](QString *copyText, const QString &line) {
			if (line.isEmpty()) {
				return;
			} else if (!copyText->isEmpty()) {
				copyText->append(QChar('\n'));
			}
			copyText->append(line);
		};
		appendLine(&prepared.relatedArticle.copyText, prepared.relatedArticle.title);
		appendLine(
			&prepared.relatedArticle.copyText,
			prepared.relatedArticle.description);
		appendLine(&prepared.relatedArticle.copyText, prepared.relatedArticle.footer);
		if (prepared.relatedArticle.copyText.isEmpty()) {
			prepared.relatedArticle.copyText = prepared.relatedArticle.link.target;
		}
		related.push_back(std::move(prepared));
	}
	if (related.empty()) {
		return true;
	}

	auto title = PreparedIvRichText();
	auto anchorId = QString();
	if (!PrepareNativeIvRichText(
			data.vtitle(),
			&title,
			&anchorId,
			state)) {
		return false;
	}
	SortPreparedIvRichText(&title);
	if (!title.text.text.isEmpty()) {
		if (!AppendPreparedIvRichBlock(
				result,
				PreparedBlockKind::Heading,
				4,
				std::move(title),
				std::move(anchorId))) {
			return false;
		}
	} else if (!anchorId.isEmpty() && related.front().anchorId.isEmpty()) {
		related.front().anchorId = std::move(anchorId);
	}
	result->insert(
		result->end(),
		std::make_move_iterator(related.begin()),
		std::make_move_iterator(related.end()));
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
			std::move(anchorId),
			false,
			true);
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
	}, [&](const MTPDpageBlockThinking &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Thinking,
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
			false,
			result,
			state);
	}, [&](const MTPDpageBlockPullquote &data) {
		return PrepareNativeIvQuoteBlock(
			data.vtext(),
			data.vcaption(),
			true,
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
	}, [&](const MTPDpageBlockHeading1 &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			1,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockHeading2 &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			2,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockHeading3 &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			3,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockHeading4 &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			4,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockHeading5 &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			5,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockHeading6 &data) {
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			6,
			data.vtext(),
			state);
	}, [&](const MTPDpageBlockMath &data) {
		return PrepareNativeIvMathBlock(data, result, state);
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
	}, [&](const MTPDpageBlockRelatedArticles &data) {
		return PrepareNativeIvRelatedArticlesBlock(data, result, state);
	}, [&](const MTPDpageBlockMap &data) {
		return PrepareNativeIvMapBlock(data, result, state);
	}, [&](const MTPDinputPageBlockMap &) {
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Unsupported Content"_q,
			result);
	}, [&](const MTPDinputPageBlockOrderedList &) {
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Unsupported Content"_q,
			result);
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

namespace {

[[nodiscard]] bool PrepareNativeIvRichPlaceholderBlock(
		QString label,
		const RichPage::RichText &caption,
		QString anchorId,
		std::optional<EmbedRequest> embed,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto prepared = PreparedIvRichText();
	if (!PrepareNativeIvRichText(
			caption,
			&prepared,
			&anchorId,
			state)) {
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
	block.anchorIds = std::move(prepared.anchorIds);
	block.supplementary = true;
	block.placeholder.label = std::move(label);
	block.placeholder.copyText = block.text.text.isEmpty()
		? block.placeholder.label
		: (block.placeholder.label + u"\n"_q + block.text.text);
	block.placeholder.embed = std::move(embed);
	if (block.placeholder.embed) {
		block.placeholder.id = { .value = uint64(++state->nextGeneratedId) };
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] QByteArray RenderCanonicalNativeIvEmbedHtml(
		const RichPageBlock &block,
		NativeIvPrepareState *state) {
	auto attributes = NativeIvHtmlAttributes();
	const auto fullWidth = block.fullWidth || (block.width <= 0);
	if (fullWidth) {
		attributes.push_back({ "width", "100%" });
	} else {
		attributes.push_back({
			"width",
			QByteArray::number(block.width),
		});
	}
	if (block.height > 0) {
		attributes.push_back({
			"height",
			QByteArray::number(block.height),
		});
	}
	if (!block.url.isEmpty()) {
		attributes.push_back({ "src", NativeIvHtmlEscape(block.url) });
	} else if (!block.html.isEmpty()) {
		const auto resourceId = StoreNativeIvEmbedHtml(
			block.html.toUtf8(),
			state);
		attributes.push_back({
			"src",
			NativeIvHtmlEscape(
				QString::fromUtf8(NativeIvResourceUrl(resourceId))),
		});
	} else {
		return QByteArray();
	}
	if (!block.allowScrolling) {
		attributes.push_back({ "scrolling", "no" });
	}
	attributes.push_back({ "frameborder", "0" });
	attributes.push_back({ "allowtransparency", "true" });
	attributes.push_back({ "allowfullscreen", "true" });
	auto content = NativeIvHtmlTag("iframe", attributes);
	if (!fullWidth) {
		auto wrapAttributes = NativeIvHtmlAttributes{
			{ "class", "iframe-wrap" },
		};
		if (block.height > 0) {
			wrapAttributes.push_back({
				"style",
				QByteArray("height:")
					+ QByteArray::number(block.height)
					+ "px",
			});
		}
		content = NativeIvHtmlTag(
			"div",
			std::move(wrapAttributes),
			content);
	}
	return NativeIvHtmlTag("figure", {}, content);
}

[[nodiscard]] auto EmbedRequestFromCanonicalBlock(
		const RichPageBlock &block,
		NativeIvPrepareState *state) -> std::optional<EmbedRequest> {
	const auto html = RenderCanonicalNativeIvEmbedHtml(block, state);
	if (html.isEmpty()) {
		return std::nullopt;
	}
	return EmbedRequest{
		.resourceId = StoreNativeIvEmbedHtml(
			WrapNativeIvEmbedHtml(html),
			state),
		.fallbackUrl = block.url.isEmpty() ? QString() : block.url,
		.width = block.width,
		.height = block.height,
		.fullWidth = block.fullWidth || !block.width,
		.fixedHeight = (block.height > 0),
		.allowScrolling = block.allowScrolling,
	};
}

[[nodiscard]] bool AppendNativeIvFlowBlock(
		std::vector<PreparedBlock> *result,
		PreparedBlockKind kind,
		int headingLevel,
		const RichPage::RichText &text,
		QString anchorId,
		NativeIvPrepareState *state,
		bool allowEmpty = false) {
	auto prepared = PreparedIvRichText();
	const auto context = NativeIvRichTextContextForTextSize(
		NativeIvFlowTextSize(kind, headingLevel, state->dimensions),
		state->dimensions);
	if (!PrepareNativeIvRichText(
			text,
			&prepared,
			&anchorId,
			state,
			context)) {
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

[[nodiscard]] bool PrepareCanonicalNativeIvQuoteBlock(
		const RichPageBlock &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Quote;
	block.anchorId = data.anchorId;
	block.pullquote = data.pullquote;
	auto body = PreparedIvRichText();
	if (!PrepareNativeIvRichText(data.text, &body, &block.anchorId, state)) {
		return false;
	}
	if (!AppendPreparedQuoteParagraph(
			&block.children,
			std::move(body),
			data.pullquote)) {
		return false;
	}
	auto cite = PreparedIvRichText();
	if (!PrepareNativeIvRichText(data.caption, &cite, &block.anchorId, state)) {
		return false;
	}
	if (!AppendPreparedQuoteParagraph(
			&block.children,
			std::move(cite),
			data.pullquote,
			true)) {
		return false;
	}
	if (block.children.empty()) {
		block.children.push_back(EmptyParagraphBlock());
		if (data.pullquote) {
			block.children.back().flowAlignment = TableAlignment::Center;
		}
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] TaskState NativeIvTaskStateFromRichPage(
		RichPage::TaskState state) {
	switch (state) {
	case RichPage::TaskState::Unchecked:
		return TaskState::Unchecked;
	case RichPage::TaskState::Checked:
		return TaskState::Checked;
	case RichPage::TaskState::None:
		break;
	}
	return TaskState::None;
}

[[nodiscard]] bool PrepareCanonicalNativeIvList(
		const std::vector<RichPageListItem> &items,
		PreparedBlock *result,
		NativeIvPrepareState *state) {
	auto firstNumber = true;
	for (const auto &item : items) {
		auto block = PreparedBlock();
		block.kind = PreparedBlockKind::ListItem;
		block.listKind = result->listKind;
		block.listDelimiter = result->listDelimiter;
		block.taskState = NativeIvTaskStateFromRichPage(item.taskState);
		if (result->listKind == ListKind::Ordered) {
			auto orderedNumber = 0;
			if (!ParseOrderedNumber(item.number, &orderedNumber)) {
				orderedNumber = NextNativeIvOrderedNumber(*result);
			}
			block.orderedNumber = orderedNumber;
			if (firstNumber) {
				result->startNumber = block.orderedNumber;
				firstNumber = false;
			}
		}
		if (!item.text.text.empty()) {
			auto prepared = PreparedIvRichText();
			auto anchorId = item.anchorId;
			if (!PrepareNativeIvRichText(
					item.text,
					&prepared,
					&anchorId,
					state)) {
				return false;
			}
			block.anchorId = std::move(anchorId);
			if (!AppendPreparedIvRichBlock(
					&block.children,
					PreparedBlockKind::Paragraph,
					0,
					std::move(prepared))) {
				return false;
			}
		} else if (!PrepareNativeIvBlocks(
				RichPage{
					.blocks = item.blocks,
				},
				&block.children,
				state)) {
			return false;
		}
		if (block.children.empty()) {
			block.children.push_back(EmptyParagraphBlock());
		}
		result->children.push_back(std::move(block));
	}
	return true;
}

[[nodiscard]] TableAlignment NativeIvTableAlignment(
		const RichPageTableCell &cell) {
	switch (cell.alignment) {
	case RichPage::TableAlignment::Center:
		return TableAlignment::Center;
	case RichPage::TableAlignment::Right:
		return TableAlignment::Right;
	case RichPage::TableAlignment::Left:
		break;
	}
	return TableAlignment::Left;
}

[[nodiscard]] PreparedTableCellVerticalAlignment NativeIvTableVerticalAlignment(
		const RichPageTableCell &cell) {
	switch (cell.verticalAlignment) {
	case RichPage::TableVerticalAlignment::Middle:
		return PreparedTableCellVerticalAlignment::Middle;
	case RichPage::TableVerticalAlignment::Bottom:
		return PreparedTableCellVerticalAlignment::Bottom;
	case RichPage::TableVerticalAlignment::Top:
		break;
	}
	return PreparedTableCellVerticalAlignment::Top;
}

[[nodiscard]] bool PrepareCanonicalNativeIvTableBlock(
		const RichPageBlock &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Table;
	block.tableBordered = data.bordered;
	block.tableStriped = data.striped;
	auto title = PreparedIvRichText();
	block.anchorId = data.anchorId;
	if (!PrepareNativeIvRichText(data.text, &title, &block.anchorId, state)) {
		return false;
	}
	SortPreparedIvRichText(&title);
	block.text = std::move(title.text);
	block.links = std::move(title.links);
	block.anchorIds = std::move(title.anchorIds);

	const auto &limits = PrepareTableRenderLimitsForIv();
	const auto rowCount = std::min(int(data.tableRows.size()), limits.maxRows);
	auto occupancy = NativeIvTableOccupancyGrid(rowCount);
	block.tableRows.reserve(rowCount);
	auto occupiedSlotCountSoFar = int64(0);

	for (auto rowIndex = 0; rowIndex != rowCount; ++rowIndex) {
		const auto &row = data.tableRows[rowIndex];
		auto preparedRow = PreparedTableRow();
		preparedRow.cells.reserve(std::min(int(row.cells.size()), limits.maxColumns));
		for (const auto &cell : row.cells) {
			auto preparedCell = PreparedTableCell();
			const auto normalizedColspan = NormalizeNativeIvTableSpan(cell.colspan);
			const auto rowspan = ClampNativeIvTableRowspan(
				cell.rowspan,
				rowIndex,
				rowCount);
			if (rowspan <= 0) {
				continue;
			}
			const auto column = FirstAvailableNativeIvTableColumn(
				occupancy,
				rowIndex,
				rowspan,
				normalizedColspan,
				limits.maxColumns);
			if (column < 0) {
				continue;
			}
			const auto colspan = ClampNativeIvTableColspan(
				normalizedColspan,
				column,
				limits.maxColumns);
			if (colspan <= 0) {
				continue;
			}
			const auto occupiedSlotGrowth = int64(rowspan) * colspan;
			if (occupiedSlotGrowth > limits.maxCells
				|| (occupiedSlotCountSoFar + occupiedSlotGrowth) > limits.maxCells) {
				continue;
			}
			preparedCell.column = column;
			preparedCell.alignment = NativeIvTableAlignment(cell);
			preparedCell.header = cell.header;
			preparedCell.verticalAlignment = NativeIvTableVerticalAlignment(cell);
			preparedCell.colspan = colspan;
			preparedCell.rowspan = rowspan;
			if (!cell.text.text.empty()) {
				auto rich = PreparedIvRichText();
				const auto context = NativeIvRichTextContextForTextSize(
					cell.header
						? state->dimensions.tableHeaderTextSize
						: state->dimensions.tableBodyTextSize,
					state->dimensions);
				if (!PrepareNativeIvRichText(
						cell.text,
						&rich,
						nullptr,
						state,
						context)) {
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
		}
		preparedRow.header = !preparedRow.cells.empty()
			&& std::all_of(
				preparedRow.cells.begin(),
				preparedRow.cells.end(),
				[](const PreparedTableCell &cell) {
					return cell.header;
				});
		block.tableRows.push_back(std::move(preparedRow));
	}

	block.tableColumnCount = NativeIvTableColumnCount(occupancy);
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

[[nodiscard]] bool PrepareCanonicalNativeIvDetailsBlock(
		const RichPageBlock &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto summary = PreparedIvRichText();
	auto anchorId = NativeIvDetailsAnchorId(state);
	if (!PrepareNativeIvRichText(
			data.text,
			&summary,
			&anchorId,
			state)) {
		return false;
	}
	if (!summary.links.empty()) {
		const auto from = std::remove_if(
			summary.text.entities.begin(),
			summary.text.entities.end(),
			[](const EntityInText &entity) {
				return entity.type() == EntityType::CustomUrl;
			});
		summary.text.entities.erase(from, summary.text.entities.end());
		summary.links.clear();
	}
	SortPreparedIvRichText(&summary);
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::Details;
	block.anchorId = std::move(anchorId);
	block.collapsed = !data.open;
	block.text = std::move(summary.text);
	block.links = std::move(summary.links);
	return PrepareNativeIvBlocks(
		RichPage{
			.blocks = data.blocks,
		},
		&block.children,
		state)
		? (result->push_back(std::move(block)), true)
		: false;
}

[[nodiscard]] QString NativeIvRelatedArticleFooterText(
		const RichPageRelatedArticle &article) {
	if (article.publishedDate && !article.author.isEmpty()) {
		return article.author + u", "_q + NativeIvDateText(article.publishedDate);
	} else if (article.publishedDate) {
		return NativeIvDateText(article.publishedDate);
	}
	return article.author;
}

[[nodiscard]] bool PrepareCanonicalNativeIvRelatedArticlesBlock(
		const RichPageBlock &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto related = std::vector<PreparedBlock>();
	related.reserve(data.relatedArticles.size());
	for (const auto &article : data.relatedArticles) {
		auto prepared = PreparedBlock();
		prepared.kind = PreparedBlockKind::RelatedArticle;
		prepared.relatedArticle.title = article.title.trimmed();
		prepared.relatedArticle.description = article.description.trimmed();
		prepared.relatedArticle.footer = NativeIvRelatedArticleFooterText(article);
		prepared.relatedArticle.photoId = article.photoId;
		if (prepared.relatedArticle.title.isEmpty()
			&& prepared.relatedArticle.description.isEmpty()
			&& prepared.relatedArticle.footer.isEmpty()) {
			prepared.relatedArticle.title = article.url.trimmed();
		}
		const auto linkText = !prepared.relatedArticle.title.isEmpty()
			? prepared.relatedArticle.title
			: !prepared.relatedArticle.description.isEmpty()
			? prepared.relatedArticle.description
			: prepared.relatedArticle.footer;
		prepared.relatedArticle.link = PrepareNativeIvRelatedArticleLink(
			article.url,
			article.webpageId,
			linkText);
		const auto appendLine = [&](QString *copyText, const QString &line) {
			if (line.isEmpty()) {
				return;
			} else if (!copyText->isEmpty()) {
				copyText->append(QChar('\n'));
			}
			copyText->append(line);
		};
		appendLine(&prepared.relatedArticle.copyText, prepared.relatedArticle.title);
		appendLine(
			&prepared.relatedArticle.copyText,
			prepared.relatedArticle.description);
		appendLine(&prepared.relatedArticle.copyText, prepared.relatedArticle.footer);
		if (prepared.relatedArticle.copyText.isEmpty()) {
			prepared.relatedArticle.copyText = prepared.relatedArticle.link.target;
		}
		related.push_back(std::move(prepared));
	}
	if (related.empty()) {
		return true;
	}

	auto title = PreparedIvRichText();
	auto anchorId = data.anchorId;
	if (!PrepareNativeIvRichText(data.text, &title, &anchorId, state)) {
		return false;
	}
	SortPreparedIvRichText(&title);
	if (!title.text.text.isEmpty()) {
		if (!AppendPreparedIvRichBlock(
				result,
				PreparedBlockKind::Heading,
				4,
				std::move(title),
				std::move(anchorId))) {
			return false;
		}
	} else if (!anchorId.isEmpty() && related.front().anchorId.isEmpty()) {
		related.front().anchorId = std::move(anchorId);
	}
	result->insert(
		result->end(),
		std::make_move_iterator(related.begin()),
		std::make_move_iterator(related.end()));
	return true;
}

[[nodiscard]] bool PrepareCanonicalNativeIvEmbedPostBlock(
		const RichPageBlock &data,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	auto caption = PreparedIvRichText();
	auto block = PreparedBlock();
	block.kind = PreparedBlockKind::EmbedPost;
	block.embedPost.url = data.url.trimmed();
	block.embedPost.authorPhotoId = data.photoId;
	block.embedPost.author = data.author.trimmed();
	if (data.date) {
		block.embedPost.dateText = NativeIvDateText(data.date);
	}
	auto anchorId = data.anchorId;
	if (!PrepareNativeIvRichText(data.caption, &caption, &anchorId, state)) {
		return false;
	}
	SortPreparedIvRichText(&caption);
	block.text = std::move(caption.text);
	block.links = std::move(caption.links);
	block.anchorId = std::move(anchorId);
	block.anchorIds = std::move(caption.anchorIds);
	block.supplementary = true;
	if (data.blocks.empty()) {
		block.children.push_back(
			PrepareNativeIvEmbedPostFallbackParagraph(block.embedPost.url));
	} else if (!PrepareNativeIvBlocks(
			RichPage{
				.blocks = data.blocks,
			},
			&block.children,
			state)) {
		return false;
	}
	result->push_back(std::move(block));
	return true;
}

[[nodiscard]] bool PrepareCanonicalNativeIvBlock(
		const RichPageBlock &block,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	if (state->blocked()) {
		return false;
	}
	switch (block.kind) {
	case RichPageBlockKind::Unsupported:
		return PrepareNativeIvPlainPlaceholderBlock(
			u"Unsupported Content"_q,
			result);
	case RichPageBlockKind::Heading:
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Heading,
			block.headingLevel,
			block.text,
			block.anchorId,
			state);
	case RichPageBlockKind::Paragraph:
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			block.text,
			block.anchorId,
			state);
	case RichPageBlockKind::Thinking:
		return AppendNativeIvFlowBlock(
			result,
			PreparedBlockKind::Thinking,
			0,
			block.text,
			block.anchorId,
			state);
	case RichPageBlockKind::AuthorDate: {
		auto prepared = PreparedIvRichText();
		auto anchorId = block.anchorId;
		if (!PrepareNativeIvRichText(
				block.text,
				&prepared,
				&anchorId,
				state)) {
			return false;
		}
		if (block.date) {
			if (!prepared.text.text.isEmpty()) {
				prepared.text.append(u" \u2022 "_q);
			}
			prepared.text.append(NativeIvDateText(block.date));
		}
		return AppendPreparedIvRichBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			std::move(prepared),
			std::move(anchorId),
			false,
			true);
	}
	case RichPageBlockKind::Code: {
		auto prepared = PreparedIvRichText();
		auto anchorId = block.anchorId;
		if (!PrepareNativeIvRichText(
				block.text,
				&prepared,
				&anchorId,
				state)) {
			return false;
		}
		auto code = PreparedBlock();
		code.kind = PreparedBlockKind::CodeBlock;
		code.anchorId = std::move(anchorId);
		code.codeLanguage = block.language;
		code.text.text = StripOneTrailingNewline(prepared.text.text);
		result->push_back(std::move(code));
		return true;
	}
	case RichPageBlockKind::Divider:
		result->push_back(PrepareRuleBlock());
		return true;
	case RichPageBlockKind::Anchor: {
		auto prepared = PreparedIvRichText();
		return AppendPreparedIvRichBlock(
			result,
			PreparedBlockKind::Paragraph,
			0,
			std::move(prepared),
			block.anchorId,
			true);
	}
	case RichPageBlockKind::List: {
		auto prepared = PreparedBlock();
		prepared.kind = PreparedBlockKind::List;
		prepared.listKind = (block.listKind == RichPage::ListKind::Ordered)
			? ListKind::Ordered
			: ListKind::Bullet;
		if (prepared.listKind == ListKind::Ordered) {
			prepared.listDelimiter = ListDelimiter::Period;
			prepared.startNumber = 1;
		}
		return PrepareCanonicalNativeIvList(
			block.listItems,
			&prepared,
			state)
			? (result->push_back(std::move(prepared)), true)
			: false;
	}
	case RichPageBlockKind::Quote:
		return PrepareCanonicalNativeIvQuoteBlock(block, result, state);
	case RichPageBlockKind::Photo:
		return PrepareNativeIvPhotoBlock(block, result, state);
	case RichPageBlockKind::Video:
		return PrepareNativeIvVideoBlock(block, result, state);
	case RichPageBlockKind::Embed:
		return PrepareNativeIvRichPlaceholderBlock(
			tr::lng_iv_click_to_view(tr::now),
			block.caption,
			block.anchorId,
			EmbedRequestFromCanonicalBlock(block, state),
			result,
			state);
	case RichPageBlockKind::EmbedPost:
		return PrepareCanonicalNativeIvEmbedPostBlock(block, result, state);
	case RichPageBlockKind::GroupedMedia:
		return PrepareNativeIvGroupedMediaBlock(block, result, state);
	case RichPageBlockKind::Channel:
		return PrepareNativeIvChannelBlock(block, result, state);
	case RichPageBlockKind::Audio:
		return PrepareNativeIvAudioBlock(block, result, state);
	case RichPageBlockKind::Math:
		if (block.formula.trimmed().isEmpty()) {
			return true;
		} else {
			auto prepared = PreparedBlock();
			prepared.kind = PreparedBlockKind::DisplayMath;
			prepared.formulaTex = block.formula;
			prepared.mathKind = MathKind::Display;
			prepared.formulaIndex = state->rememberFormula(prepared);
			result->push_back(std::move(prepared));
			return true;
		}
	case RichPageBlockKind::Table:
		return PrepareCanonicalNativeIvTableBlock(block, result, state);
	case RichPageBlockKind::Details:
		return PrepareCanonicalNativeIvDetailsBlock(block, result, state);
	case RichPageBlockKind::RelatedArticles:
		return PrepareCanonicalNativeIvRelatedArticlesBlock(block, result, state);
	case RichPageBlockKind::Map:
		return PrepareNativeIvMapBlock(block, result, state);
	}
	return PrepareNativeIvPlainPlaceholderBlock(
		u"Unsupported Content"_q,
		result);
}

[[nodiscard]] bool PrepareCanonicalNativeIvBlocks(
		const std::vector<RichPageBlock> &blocks,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	for (const auto &block : blocks) {
		if (!PrepareCanonicalNativeIvBlock(block, result, state)) {
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

} // namespace

bool PrepareNativeIvBlocks(
		const Iv::RichPage &page,
		std::vector<PreparedBlock> *result,
		NativeIvPrepareState *state) {
	return PrepareCanonicalNativeIvBlocks(page.blocks, result, state);
}

} // namespace Iv::Markdown
