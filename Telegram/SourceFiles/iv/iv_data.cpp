/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/iv_data.h"

#include "iv/iv_prepare.h"
#include "iv/iv_rich_page.h"
#include "core/cached_webview_availability.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace Iv {
namespace {

bool FailureRecorded/* = false*/;

} // namespace

QByteArray GeoPointId(Geo point) {
	const auto lat = int(point.lat * 1000000);
	const auto lon = int(point.lon * 1000000);
	const auto combined = (std::uint64_t(std::uint32_t(lat)) << 32)
		| std::uint64_t(std::uint32_t(lon));
	return QByteArray::number(quint64(combined))
		+ ','
		+ QByteArray::number(point.access);
}

Geo GeoPointFromId(QByteArray data) {
	const auto parts = data.split(',');
	if (parts.size() != 2) {
		return {};
	}
	const auto combined = parts[0].toULongLong();
	const auto lat = int(std::uint32_t(combined >> 32));
	const auto lon = int(std::uint32_t(combined & 0xFFFFFFFFULL));
	return {
		.lat = lat / 1000000.,
		.lon = lon / 1000000.,
		.access = parts[1].toULongLong(),
	};
}

Data::Data(
	const MTPDwebPage &webpage,
	const MTPPage &page,
	std::shared_ptr<const RichPage> richPage,
	const PhotoData *webpagePhoto,
	const DocumentData *webpageDocument)
: _pageId(webpage.vid().v)
, _url(qs(webpage.vurl()))
, _name(webpage.vsite_name()
	? qs(*webpage.vsite_name())
	: SiteNameFromUrl(_url))
, _partial(page.match([](const MTPDpage &data) {
		return data.is_part();
	}, [](const auto &) {
		return false;
	}))
, _richPage(std::move(richPage))
, _webpagePhoto(webpagePhoto)
, _webpageDocument(webpageDocument)
, _pageFallback(page)
, _webpagePhotoFallback(webpage.vphoto()
	? std::optional<MTPPhoto>(*webpage.vphoto())
	: std::optional<MTPPhoto>())
, _webpageDocumentFallback(webpage.vdocument()
	? std::optional<MTPDocument>(*webpage.vdocument())
	: std::optional<MTPDocument>()) {
}

QString Data::id() const {
	return _url;
}

bool Data::partial() const {
	return _partial;
}

Data::~Data() = default;

auto Data::richPage() const -> const std::shared_ptr<const RichPage> & {
	return _richPage;
}

auto Data::sourceFallback() const -> std::optional<Source> {
	if (!_pageFallback) {
		return std::nullopt;
	}
	return Source{
		.pageId = _pageId,
		.richPage = _richPage,
		.hasRawPage = true,
		.page = *_pageFallback,
		.webpagePhoto = _webpagePhotoFallback,
		.webpageDocument = _webpageDocumentFallback,
		.name = _name,
		.updatedCachedViews = _updatedCachedViews,
	};
}

void Data::updateCachedViews(int cachedViews) {
	_updatedCachedViews = std::max(_updatedCachedViews, cachedViews);
}

void Data::prepare(const Options &options, Fn<void(Prepared)> done) const {
	crl::async([source = Source{
			.pageId = _pageId,
			.richPage = _richPage,
			.hasRawPage = _pageFallback.has_value(),
			.page = _pageFallback.value_or(MTPPage()),
			.webpagePhoto = _webpagePhotoFallback,
			.webpageDocument = _webpageDocumentFallback,
			.name = _name,
			.updatedCachedViews = _updatedCachedViews,
		}, options, done = std::move(done)]() mutable {
		done(Prepare(source, options));
	});
}

QString SiteNameFromUrl(const QString &url) {
	const auto u = QUrl(url);
	QString pretty = u.isValid() ? u.toDisplayString() : url;
	const auto m = QRegularExpression(u"^[a-zA-Z0-9]+://"_q).match(pretty);
	if (m.hasMatch()) pretty = pretty.mid(m.capturedLength());
	int32 slash = pretty.indexOf('/');
	if (slash > 0) pretty = pretty.mid(0, slash);
	QStringList components = pretty.split('.', Qt::SkipEmptyParts);
	if (components.size() >= 2) {
		components = components.mid(components.size() - 2);
		return components.at(0).at(0).toUpper()
			+ components.at(0).mid(1)
			+ '.'
			+ components.at(1);
	}
	return QString();
}

bool ShowButton() {
	const auto &availability = Core::CachedWebviewAvailability();
	return availability.customSchemeRequests
		&& availability.customRangeRequests;
}

void RecordShowFailure() {
	FailureRecorded = true;
}

bool FailedToShow() {
	return FailureRecorded;
}

} // namespace Iv
