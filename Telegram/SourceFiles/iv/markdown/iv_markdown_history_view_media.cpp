/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_history_view_media.h"

#include "iv/markdown/iv_markdown_media_block.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtWidgets/QApplication>
#include <array>
#include <unordered_set>

#include "ui/image/image_location.h"
#include "data/data_types.h"
#include "data/data_file_click_handler.h"
#include "data/data_session.h"
#include "history/admin_log/history_admin_log_item.h"
#include "history/history_location_manager.h"
#include "history/view/history_view_cursor_state.h"
#include "history/history_view_top_toast.h"
#include "history/view/history_view_element.h"
#include "history/view/media/history_view_media.h"
#include "ui/basic_click_handlers.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"

#include "rpl/filter.h"
#include "rpl/lifetime.h"

#include <utility>

namespace Iv::Markdown {
namespace {

class IvHistoryViewDelegate final : public HistoryView::SimpleElementDelegate {
public:
	IvHistoryViewDelegate(
		not_null<Window::SessionController*> controller,
		not_null<::Data::Session*> session,
		Fn<void()> update)
	: HistoryView::SimpleElementDelegate(controller, std::move(update))
	, _session(session) {
	}

	HistoryView::Context elementContext() override {
		return HistoryView::Context::TTLViewer;
	}

	HistoryView::ElementChatMode elementChatMode() override {
		return HistoryView::ElementChatMode::Default;
	}

	bool elementAnimationsPaused() override {
		return false;
	}

	void elementOpenPhoto(
			not_null<PhotoData*> photo,
			FullMsgId context) override {
		controller()->openPhoto(photo, { .id = context });
	}

	void elementOpenDocument(
			not_null<DocumentData*> document,
			FullMsgId context,
			bool showInMediaView = false) override {
		controller()->openDocument(
			document,
			showInMediaView,
			{ .id = context });
	}

	void elementCancelUpload(const FullMsgId &context) override {
		if (const auto item = _session->message(context)) {
			controller()->cancelUploadLayer(item);
		}
	}

	void elementShowTooltip(
			const TextWithEntities &text,
			Fn<void()> hiddenCallback) override {
		const auto widget = QApplication::activeWindow();
		if (!widget) {
			return;
		}
		_tooltip.show(
			not_null{ widget },
			&_session->session(),
			text,
			std::move(hiddenCallback));
	}

private:
	const not_null<::Data::Session*> _session;
	HistoryView::InfoTooltip _tooltip;
};

struct IvHistoryViewHit {
	ClickHandlerPtr link;
	MediaActivation activation;
	bool supported = true;
};

[[nodiscard]] MediaActivation ExternalActivation(QString url) {
	auto result = MediaActivation();
	if (!url.isEmpty()) {
		result.kind = MediaActivationKind::ExternalUrl;
		result.url = std::move(url);
	}
	return result;
}

[[nodiscard]] int HistoryViewResizeWidth(int width) {
	return std::max(
		width,
		st::msgMinWidth + st::msgMargin.right() + st::msgMargin.left());
}

class IvHistoryViewBlock final : public MediaBlock {
public:
	IvHistoryViewBlock(
		not_null<Window::SessionController*> controller,
		IvHistoryViewMediaDescriptor descriptor)
	: _stableId(descriptor.stableId)
	, _kind(descriptor.kind)
	, _copyText(std::move(descriptor.copyText))
	, _layoutHint(descriptor.layoutHint)
	, _photoRuntime(std::move(descriptor.photo))
	, _documentRuntime(std::move(descriptor.document))
	, _keepAlive(std::move(descriptor.keepAlive))
	, _session(descriptor.session)
	, _theme(controller->currentChatTheme())
	, _style(controller->chatStyle())
	, _delegate(std::make_unique<IvHistoryViewDelegate>(
		controller,
		_session,
		[=] { requestRepaint(QRect()); }))
	, _item(_delegate.get(), descriptor.item)
	, _view(_item.get())
	, _itemKeepAlive(std::move(descriptor.itemKeepAlive)) {
		auto overridden = false;
		if (_view) {
			if (descriptor.mediaFactory) {
				if (auto media = descriptor.mediaFactory(not_null{ _view })) {
					_view->overrideMedia(std::move(media));
					overridden = true;
				}
			}
			if (overridden) {
				_view->initDimensions();
			}
		}
		_supported = overridden && probeSupport();
		_session->itemRepaintRequest(
		) | rpl::filter([=](not_null<const HistoryItem*> item) {
			return _view && (item == _view->data());
		}) | rpl::on_next([=](not_null<const HistoryItem*>) {
			handleItemRepaint();
		}, _lifetime);
		_session->itemResizeRequest(
		) | rpl::filter([=](not_null<const HistoryItem*> item) {
			return _view && (item == _view->data());
		}) | rpl::on_next([=](not_null<const HistoryItem*>) {
			handleViewResize();
		}, _lifetime);
		_session->viewRepaintRequest(
		) | rpl::filter([=](::Data::RequestViewRepaint data) {
			return (data.view == _view);
		}) | rpl::on_next([=](::Data::RequestViewRepaint data) {
			handleViewRepaint(data.rect);
		}, _lifetime);
		_session->viewResizeRequest(
		) | rpl::filter([=](not_null<HistoryView::Element*> view) {
			return (view == _view);
		}) | rpl::on_next([=](not_null<HistoryView::Element*>) {
			handleViewResize();
		}, _lifetime);
	}

	[[nodiscard]] uint64 stableId() const override {
		return _stableId;
	}

	[[nodiscard]] bool supported() const {
		return _supported;
	}

	[[nodiscard]] int resizeGetHeight(int width) override {
		if (!_view) {
			return 0;
		}
		_requestedWidth = HistoryViewResizeWidth(width);
		return _view->resizeGetHeight(_requestedWidth);
	}

	void setGeometry(QRect geometry) override {
		if (!_view) {
			_geometry = geometry;
			return;
		}
		const auto width = HistoryViewResizeWidth(geometry.width());
		if (_requestedWidth != width) {
			_requestedWidth = width;
			_view->resizeGetHeight(_requestedWidth);
		}
		_geometry = QRect(geometry.topLeft(), _view->currentSize());
	}

	[[nodiscard]] QRect geometry() const override {
		return _geometry;
	}

	[[nodiscard]] int firstLineBaseline() const override {
		return _geometry.y();
	}

	void paint(
			Painter &p,
			QRect clip,
			const MarkdownArticlePaintCaches &caches) const override {
		Q_UNUSED(caches);
		if (!_view || _geometry.isEmpty()) {
			return;
		}
		const auto visible = clip.intersected(_geometry);
		if (visible.isEmpty()) {
			return;
		}
		p.save();
		p.translate(_geometry.topLeft());
		const auto localClip = visible.translated(-_geometry.topLeft());
		const auto rect = QRect(QPoint(), _view->currentSize());
		auto context = _theme->preparePaintContext(
			_style,
			rect,
			rect,
			localClip,
			!QApplication::activeWindow());
		context.outbg = _view->hasOutLayout();
		_view->draw(p, context);
		p.restore();
	}

	[[nodiscard]] ClickHandlerPtr linkAt(QPoint point) const override {
		return resolveHit(point).link;
	}

	[[nodiscard]] MediaActivation activationAt(QPoint point) const override {
		return resolveHit(point).activation;
	}

	[[nodiscard]] MediaBlockSelectionData selectionData() const override {
		return {
			.copyText = _copyText,
		};
	}

private:
	[[nodiscard]] IvHistoryViewHit resolveHit(QPoint point) const {
		auto result = IvHistoryViewHit();
		if (!_supported || !_view || !_geometry.contains(point)) {
			return result;
		}
		return resolveLocalHit(point - _geometry.topLeft());
	}

	[[nodiscard]] IvHistoryViewHit resolveLocalHit(QPoint point) const {
		auto result = IvHistoryViewHit();
		if (!_view) {
			return result;
		}
		const auto state = _view->textState(
			point,
			HistoryView::StateRequest{
				.flags = Ui::Text::StateRequest::Flag::LookupLink
					| Ui::Text::StateRequest::Flag::LookupCustomTooltip,
			});
		return classifyState(state);
	}

	[[nodiscard]] IvHistoryViewHit classifyState(
			const HistoryView::TextState &state) const {
		return classifyHandler(state.link);
	}

	[[nodiscard]] IvHistoryViewHit classifyHandler(
			const ClickHandlerPtr &handler) const {
		auto result = IvHistoryViewHit();
		if (!handler) {
			return result;
		}
		if (_kind == IvHistoryViewMediaKind::Photo) {
			if (std::dynamic_pointer_cast<PhotoSaveClickHandler>(handler)
				|| std::dynamic_pointer_cast<PhotoCancelClickHandler>(handler)) {
				result.link = handler;
				return result;
			}
			if (std::dynamic_pointer_cast<PhotoOpenClickHandler>(handler)
				&& _photoRuntime) {
				result.activation.kind = MediaActivationKind::Photo;
				result.activation.photo = _photoRuntime;
				return result;
			}
			result.supported = false;
			return result;
		}
		if (std::dynamic_pointer_cast<VoiceSeekClickHandler>(handler)
			|| std::dynamic_pointer_cast<PhotoSaveClickHandler>(handler)
			|| std::dynamic_pointer_cast<PhotoCancelClickHandler>(handler)
			|| std::dynamic_pointer_cast<DocumentSaveClickHandler>(handler)
			|| std::dynamic_pointer_cast<DocumentCancelClickHandler>(handler)
			|| std::dynamic_pointer_cast<DocumentOpenWithClickHandler>(handler)) {
			result.link = handler;
			return result;
		}
		if (std::dynamic_pointer_cast<PhotoOpenClickHandler>(handler)
			&& _photoRuntime) {
			result.activation.kind = MediaActivationKind::Photo;
			result.activation.photo = _photoRuntime;
			return result;
		}
		if (std::dynamic_pointer_cast<DocumentOpenClickHandler>(handler)
			&& _documentRuntime) {
			result.activation.kind = MediaActivationKind::Document;
			result.activation.document = _documentRuntime;
			return result;
		}
		if (std::dynamic_pointer_cast<LocationClickHandler>(handler)) {
			result.activation = ExternalActivation(handler->url());
			return result;
		}
		if (std::dynamic_pointer_cast<UrlClickHandler>(handler)
			|| !handler->url().isEmpty()) {
			result.activation = ExternalActivation(handler->url());
			return result;
		}
		result.supported = false;
		return result;
	}

	[[nodiscard]] bool probeSupport() {
		if (!_view) {
			return false;
		}
		switch (_kind) {
		case IvHistoryViewMediaKind::Photo:
			return supportsHitClassification();
		case IvHistoryViewMediaKind::Document:
			return supportsHitClassification();
		case IvHistoryViewMediaKind::Map:
		case IvHistoryViewMediaKind::Audio:
			return true;
		}
		return false;
	}

	[[nodiscard]] bool supportsHitClassification() {
		const auto width = HistoryViewResizeWidth(_layoutHint.width());
		_view->resizeGetHeight(width);
		const auto size = _view->currentSize();
		if (size.isEmpty()) {
			return false;
		}
		const auto right = std::max(size.width() - 1, 0);
		const auto bottom = std::max(size.height() - 1, 0);
		const auto points = std::array{
			QPoint(size.width() / 2, size.height() / 2),
			QPoint(0, 0),
			QPoint(right, 0),
			QPoint(0, bottom),
			QPoint(right, bottom),
		};
		for (const auto &point : points) {
			if (!resolveLocalHit(point).supported) {
				return false;
			}
		}
		return true;
	}

	void handleViewRepaint(QRect rect) {
		Q_UNUSED(rect);
		requestRepaint(QRect());
	}

	void handleItemRepaint() {
		requestRepaint(QRect());
	}

	void handleViewResize() {
		if (!_view) {
			return;
		}
		const auto previous = _view->currentSize();
		if (_requestedWidth > 0) {
			_view->resizeGetHeight(_requestedWidth);
		}
		if (_geometry.isEmpty()) {
			return;
		}
		if (_view->currentSize() != previous) {
			requestRelayout(_geometry);
		} else {
			requestRepaint(QRect());
		}
	}

	const uint64 _stableId = 0;
	const IvHistoryViewMediaKind _kind = IvHistoryViewMediaKind::Map;
	const QString _copyText;
	const QSize _layoutHint;
	const std::shared_ptr<PhotoRuntime> _photoRuntime;
	const std::shared_ptr<DocumentRuntime> _documentRuntime;
	const std::vector<std::shared_ptr<void>> _keepAlive;
	const not_null<::Data::Session*> _session;
	const not_null<Ui::ChatTheme*> _theme;
	const not_null<const Ui::ChatStyle*> _style;
	const std::unique_ptr<IvHistoryViewDelegate> _delegate;
	AdminLog::OwnedItem _item;
	HistoryView::Element *_view = nullptr;
	const std::vector<std::shared_ptr<void>> _itemKeepAlive;
	rpl::lifetime _lifetime;
	QRect _geometry;
	int _requestedWidth = 0;
	bool _supported = false;
};

} // namespace

IvHistoryViewMediaBlockFactory::IvHistoryViewMediaBlockFactory(
	base::weak_ptr<Window::SessionController> controller,
	PhotoFactory createPhoto,
	VideoFactory createVideo,
	AudioFactory createAudio,
	MapFactory createMap)
: _controller(std::move(controller))
, _createPhoto(std::move(createPhoto))
, _createVideo(std::move(createVideo))
, _createAudio(std::move(createAudio))
, _createMap(std::move(createMap)) {
}

std::shared_ptr<MediaBlock> IvHistoryViewMediaBlockFactory::createPhoto(
		const PreparedPhotoBlockData &prepared) const {
	return create(prepared, _createPhoto);
}

std::shared_ptr<MediaBlock> IvHistoryViewMediaBlockFactory::createVideo(
		const PreparedVideoBlockData &prepared) const {
	return create(prepared, _createVideo);
}

std::shared_ptr<MediaBlock> IvHistoryViewMediaBlockFactory::createAudio(
		const PreparedAudioBlockData &prepared) const {
	return create(prepared, _createAudio);
}

std::shared_ptr<MediaBlock> IvHistoryViewMediaBlockFactory::createMap(
		const PreparedMapBlockData &prepared) const {
	return create(prepared, _createMap);
}

std::shared_ptr<MediaBlock> CreateIvHistoryViewMediaBlock(
		Window::SessionController *controller,
		IvHistoryViewMediaDescriptor descriptor) {
	if (!controller || !descriptor.item) {
		return nullptr;
	}
	if (!descriptor.session || !descriptor.mediaFactory) {
		return nullptr;
	}
	const auto block = std::make_shared<IvHistoryViewBlock>(
		controller,
		std::move(descriptor));
	return block->supported() ? block : nullptr;
}

} // namespace Iv::Markdown
