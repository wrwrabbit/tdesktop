/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <memory>
#include <optional>

class DocumentData;
class PhotoData;

namespace Iv {
struct RichPage;

struct Source {
	MTPPage page;
	std::optional<MTPPhoto> webpagePhoto;
	std::optional<MTPDocument> webpageDocument;
};

class Data final {
public:
	Data(
		const MTPDwebPage &webpage,
		const MTPPage &page,
		std::shared_ptr<const RichPage> richPage);
	~Data();

	[[nodiscard]] QString id() const;
	[[nodiscard]] QString name() const;
	[[nodiscard]] uint64 pageId() const;
	[[nodiscard]] int32 hash() const;
	[[nodiscard]] bool partial() const;
	[[nodiscard]] const std::shared_ptr<const RichPage> &richPage() const;
	[[nodiscard]] std::optional<Source> sourceFallback() const;

private:
	const uint64 _pageId = 0;
	const int32 _hash = 0;
	const QString _url;
	const QString _name;
	const bool _partial = false;
	const std::shared_ptr<const RichPage> _richPage;
	const std::optional<MTPPage> _pageFallback;
	const std::optional<MTPPhoto> _webpagePhotoFallback;
	const std::optional<MTPDocument> _webpageDocumentFallback;

};

[[nodiscard]] QString SiteNameFromUrl(const QString &url);

} // namespace Iv
