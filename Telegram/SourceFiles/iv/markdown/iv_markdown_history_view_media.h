/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "iv/markdown/iv_markdown_common.h"
#include "base/weak_ptr.h"

#include <functional>
#include <memory>
#include <vector>

namespace Window {
class SessionController;
} // namespace Window

class HistoryItem;

namespace HistoryView {
class Element;
class Media;
} // namespace HistoryView

namespace Data {
class Session;
} // namespace Data

namespace Iv::Markdown {

enum class IvHistoryViewMediaKind {
	Photo,
	Document,
	Map,
	Audio,
};

struct IvHistoryViewMediaDescriptor {
	using MediaFactory = std::function<std::unique_ptr<HistoryView::Media>(
		not_null<HistoryView::Element*> view)>;

	uint64 stableId = 0;
	IvHistoryViewMediaKind kind = IvHistoryViewMediaKind::Map;
	QString copyText;
	QSize layoutHint;
	::Data::Session *session = nullptr;
	HistoryItem *item = nullptr;
	MediaFactory mediaFactory;
	std::vector<std::shared_ptr<void>> keepAlive;
	std::vector<std::shared_ptr<void>> itemKeepAlive;
	std::shared_ptr<PhotoRuntime> photo;
	std::shared_ptr<DocumentRuntime> document;
};

class IvHistoryViewMediaBlockFactory final : public HostedMediaBlockFactory {
public:
	using PhotoFactory = std::function<std::shared_ptr<MediaBlock>(
		Window::SessionController *controller,
		const PreparedPhotoBlockData &prepared)>;
	using VideoFactory = std::function<std::shared_ptr<MediaBlock>(
		Window::SessionController *controller,
		const PreparedVideoBlockData &prepared)>;
	using AudioFactory = std::function<std::shared_ptr<MediaBlock>(
		Window::SessionController *controller,
		const PreparedAudioBlockData &prepared)>;
	using MapFactory = std::function<std::shared_ptr<MediaBlock>(
		Window::SessionController *controller,
		const PreparedMapBlockData &prepared)>;

	IvHistoryViewMediaBlockFactory(
		base::weak_ptr<Window::SessionController> controller,
		PhotoFactory createPhoto = {},
		VideoFactory createVideo = {},
		AudioFactory createAudio = {},
		MapFactory createMap = {});

	[[nodiscard]] std::shared_ptr<MediaBlock> createPhoto(
		const PreparedPhotoBlockData &prepared) const override;
	[[nodiscard]] std::shared_ptr<MediaBlock> createVideo(
		const PreparedVideoBlockData &prepared) const override;
	[[nodiscard]] std::shared_ptr<MediaBlock> createAudio(
		const PreparedAudioBlockData &prepared) const override;
	[[nodiscard]] std::shared_ptr<MediaBlock> createMap(
		const PreparedMapBlockData &prepared) const override;

private:
	template <typename Prepared, typename Factory>
	[[nodiscard]] std::shared_ptr<MediaBlock> create(
			const Prepared &prepared,
			const Factory &factory) const {
		if (!factory) {
			return nullptr;
		}
		const auto controller = _controller.get();
		return controller ? factory(controller, prepared) : nullptr;
	}

	const base::weak_ptr<Window::SessionController> _controller;
	const PhotoFactory _createPhoto;
	const VideoFactory _createVideo;
	const AudioFactory _createAudio;
	const MapFactory _createMap;
};

[[nodiscard]] std::shared_ptr<MediaBlock> CreateIvHistoryViewMediaBlock(
	Window::SessionController *controller,
	IvHistoryViewMediaDescriptor descriptor);

} // namespace Iv::Markdown
