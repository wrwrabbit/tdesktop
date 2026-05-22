/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "iv/iv_prepare.h"

#include <memory>
#include <optional>

class DocumentData;
class PhotoData;

namespace Iv {
struct RichPage;

struct Options {
};

struct Prepared {
	uint64 pageId = 0;
	QString name;
	QByteArray content;
	QByteArray script;
	QString url;
	QString hash;
	base::flat_map<QByteArray, QByteArray> embeds;
	base::flat_set<QByteArray> channelIds;
	bool rtl = false;
	bool hasCode = false;
	bool hasEmbeds = false;
};

struct Geo {
	float64 lat = 0.;
	float64 lon = 0.;
	uint64 access = 0;
};

[[nodiscard]] QByteArray GeoPointId(Geo point);
[[nodiscard]] Geo GeoPointFromId(QByteArray data);

class Data final {
public:
	Data(
		const MTPDwebPage &webpage,
		const MTPPage &page,
		std::shared_ptr<const RichPage> richPage);
	~Data();

	[[nodiscard]] QString id() const;
	[[nodiscard]] bool partial() const;
	[[nodiscard]] const std::shared_ptr<const RichPage> &richPage() const;
	[[nodiscard]] std::optional<Source> sourceFallback() const;

	void updateCachedViews(int cachedViews);

	void prepare(const Options &options, Fn<void(Prepared)> done) const;

private:
	const uint64 _pageId = 0;
	const QString _url;
	const QString _name;
	const bool _partial = false;
	const std::shared_ptr<const RichPage> _richPage;
	const std::optional<MTPPage> _pageFallback;
	const std::optional<MTPPhoto> _webpagePhotoFallback;
	const std::optional<MTPDocument> _webpageDocumentFallback;
	int _updatedCachedViews = 0;

};

[[nodiscard]] QString SiteNameFromUrl(const QString &url);

[[nodiscard]] bool ShowButton();

void RecordShowFailure();
[[nodiscard]] bool FailedToShow();

} // namespace Iv
