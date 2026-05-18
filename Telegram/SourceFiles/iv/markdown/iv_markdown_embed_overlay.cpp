/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_embed_overlay.h"

#include "base/algorithm.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "ui/cached_round_corners.h"
#include "ui/style/style_core_direction.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "webview/webview_data_stream_memory.h"
#include "webview/webview_embed.h"
#include "webview/webview_interface.h"

#include "styles/style_layers.h"
#include "styles/style_iv.h"

#include <QtCore/QEvent>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Iv::Markdown {
namespace {

constexpr auto kReadyRevealDelay = crl::time(1000);

[[nodiscard]] TextWithEntities GenericWebviewErrorText() {
	return { u"Error: Could not initialize WebView."_q };
}

[[nodiscard]] TextWithEntities AvailabilityErrorText(
		const Webview::Available &info) {
	Expects(info.error != Webview::Available::Error::None);

	using Error = Webview::Available::Error;
	switch (info.error) {
	case Error::NoWebview2:
		return tr::lng_payments_webview_install_edge(
			tr::now,
			lt_link,
			tr::link(
				u"Microsoft Edge WebView2 Runtime"_q,
				u"https://go.microsoft.com/fwlink/p/?LinkId=2124703"_q),
			tr::marked);
	case Error::NoWebKitGTK:
		return { tr::lng_payments_webview_install_webkit(tr::now) };
	case Error::OldWindows:
		return { tr::lng_payments_webview_update_windows(tr::now) };
	default:
		return { QString::fromStdString(info.details) };
	}
}

[[nodiscard]] TextWithEntities AddFallbackAction(
		TextWithEntities text,
		const QString &fallbackUrl) {
	if (fallbackUrl.isEmpty()) {
		return text;
	}
	return std::move(text).append(u"\n\n"_q).append(tr::link(
		tr::lng_iv_open_in_browser(tr::now),
		fallbackUrl));
}

[[nodiscard]] int ParsePositiveInt(const QJsonValue &value) {
	auto parsed = 0.;
	if (value.isDouble()) {
		parsed = value.toDouble();
	} else if (value.isString()) {
		auto text = value.toString().trimmed();
		if (text.endsWith(u"px"_q)) {
			text.chop(2);
		}
		auto ok = false;
		parsed = text.toDouble(&ok);
		if (!ok) {
			return 0;
		}
	} else {
		return 0;
	}
	if (parsed <= 0.
		|| parsed > static_cast<double>(std::numeric_limits<int>::max())) {
		return 0;
	}
	return static_cast<int>(std::round(parsed));
}

[[nodiscard]] int ResizeFrameHeight(const QJsonObject &object) {
	if (object.value("eventType").toString() != u"resize_frame"_q) {
		return 0;
	}
	return ParsePositiveInt(
		object.value("eventData").toObject().value("height"));
}

[[nodiscard]] int ResizeFrameHeight(const QJsonDocument &message) {
	if (message.isArray()) {
		const auto array = message.array();
		if (array.size() < 2 || array[0].toString() != u"resize_frame"_q) {
			return 0;
		}
		return ParsePositiveInt(array[1].toObject().value("height"));
	}
	return message.isObject() ? ResizeFrameHeight(message.object()) : 0;
}

[[nodiscard]] QJsonDocument NavigationReadyMessage(
		QByteArray resourceId,
		QString token,
		QString url) {
	auto object = QJsonObject{
		{ u"event"_q, u"navigation_ready"_q },
	};
	if (!resourceId.isEmpty()) {
		object.insert(u"resourceId"_q, QString::fromUtf8(resourceId));
	}
	if (!token.isEmpty()) {
		object.insert(u"token"_q, token);
	}
	if (!url.isEmpty()) {
		object.insert(u"url"_q, url);
	}
	return QJsonDocument(object);
}

[[nodiscard]] QByteArray EmbedInitScript() {
	return QByteArray(
		"(function(){"
		"window.TelegramWebviewProxy={"
		"postEvent:function(eventType,eventData){"
		"if(window.external&&typeof window.external.invoke==='function'){"
		"try{"
		"window.external.invoke(JSON.stringify([eventType,eventData]));"
		"}catch(e){}"
		"}"
		"}"
		"};"
		"if(window!==window.top){"
		"return;"
		"}"
		"function resourceId(){"
		"var path='';"
		"try{path=String(window.location.pathname||'');}catch(e){}"
		"while(path.charAt(0)==='/'){path=path.slice(1);}"
		"return path;"
		"}"
		"function token(){"
		"var hash='';"
		"try{hash=String(window.location.hash||'');}catch(e){}"
		"while(hash.charAt(0)==='#'){hash=hash.slice(1);}"
		"return hash;"
		"}"
		"function url(){"
		"try{return String(window.location.href||'');}catch(e){return '';}"
		"}"
		"function report(){"
		"if(!window.external||typeof window.external.invoke!=='function'){"
		"return;"
		"}"
		"try{"
		"window.external.invoke(JSON.stringify({"
		"event:'navigation_ready',"
		"resourceId:resourceId(),"
		"token:token(),"
		"url:url()"
		"}));"
		"}catch(e){"
		"}"
		"}"
		"if(document.readyState==='complete'){"
		"report();"
		"}else{"
		"var handler=function(){"
		"window.removeEventListener('load',handler,false);"
		"report();"
		"};"
		"window.addEventListener('load',handler,false);"
		"}"
		"})();");
}

} // namespace

EmbedOverlay::EmbedOverlay(
	QWidget *parent,
	const base::flat_map<QByteArray, QByteArray> *resources,
	std::function<void(QString)> linkActivationCallback,
	Webview::StorageId storageId,
	std::function<Webview::DataResult(QByteArray, Webview::DataRequest)>
		dataRequestHandler)
: Ui::RpWidget(parent)
, _webviewParent(parent)
, _resources(resources)
, _linkActivationCallback(std::move(linkActivationCallback))
, _storageId(std::move(storageId))
, _dataRequestHandler(std::move(dataRequestHandler))
, _readyDelayTimer([=] {
	revealReadyEmbed();
})
, _loadingAnimation(
	[=] {
		if (!anim::Disabled()) {
			update(_contentGeometry);
			if (_content) {
				_content->update();
			}
		}
	},
	st::markdownEmbedOverlay.loading) {
	setObjectName(u"nativeIvEmbedOverlay"_q);
	setMouseTracking(true);
	QWidget::hide();

	_content = Ui::CreateChild<Ui::RpWidget>(this);
	_content->setObjectName(u"nativeIvEmbedOverlayShell"_q);
	_content->paintRequest(
	) | rpl::on_next([=] {
		if (!_loading || _contentGeometry.isEmpty()) {
			return;
		}
		auto p = QPainter(_content);
		const auto size = st::markdownEmbedOverlay.loading.size;
		const auto loader = style::centerrect(
			bodyRectInContent(),
			QRect(QPoint(), size));
		_loadingAnimation.draw(p, loader.topLeft(), size, _content->width());
	}, _content->lifetime());
	_content->show();
}

EmbedOverlay::~EmbedOverlay() {
	destroyWebview();
	cancelReadyDelay();
	removeEscapeFilter();
}

bool EmbedOverlay::preloadEmbed(
		const EmbedRequest &request,
		std::function<void()> shownCallback,
		std::function<void()> failedCallback) {
	return startEmbed(
		request,
		false,
		std::move(shownCallback),
		std::move(failedCallback));
}

bool EmbedOverlay::showEmbed(const EmbedRequest &request) {
	return startEmbed(request, true, {}, {});
}

void EmbedOverlay::cancelPreload() {
	closeEmbed();
}

bool EmbedOverlay::startEmbed(
		const EmbedRequest &request,
		bool showErrorOnFailure,
		std::function<void()> shownCallback,
		std::function<void()> failedCallback) {
	if (!request) {
		return false;
	}
	closeEmbed();
	if (isHidden()) {
		_focusRestore = QApplication::focusWidget();
	}
	_request = request;
	_preferredBodySize = QSize();
	_pendingPreferredBodySize = QSize();
	_readyNavigationToken = QString();
	_cssToQtScale = 1.;
	_readyFromResource = false;
	_ready = false;
	_loading = true;
	_showErrorOnFailure = showErrorOnFailure;
	_shownCallback = std::move(shownCallback);
	_failedCallback = std::move(failedCallback);
	clearWebviewError();
	QWidget::hide();
	removeEscapeFilter();
	updateContentGeometry();
	_loadingAnimation.start();
	_content->update();
	if (!_webview) {
		const auto available = Webview::Availability();
		if (available.error != Webview::Available::Error::None) {
			showWebviewError(AvailabilityErrorText(available));
			return true;
		}
	}
	ensureWebview();
	if (_webview && _webview->widget()) {
		const auto hasResource = _resources
			&& (_resources->find(request.resourceId) != _resources->end());
		_readyFromResource = hasResource;
		_readyNavigationToken = hasResource
			? QString::number(++_navigationGeneration)
			: QString();
		updateWebviewGeometry();
		if (hasResource) {
			_webview->navigateToData(
				QString::fromUtf8(request.resourceId)
					+ u"#"_q
					+ _readyNavigationToken);
		} else if (!request.fallbackUrl.isEmpty()) {
			_webview->navigate(request.fallbackUrl);
		} else {
			showWebviewError(GenericWebviewErrorText());
		}
		if (_error && !_error->isHidden()) {
			_error->raise();
		}
	} else {
		showWebviewError();
	}
	return true;
}

void EmbedOverlay::closeEmbed() {
	cancelReadyDelay();
	_loading = false;
	_loadingAnimation.stop(anim::type::instant);
	if (_content) {
		_content->update();
	}
	destroyWebview();
	removeEscapeFilter();
	QWidget::hide();
	clearWebviewError();
	_request = EmbedRequest();
	_preferredBodySize = QSize();
	_pendingPreferredBodySize = QSize();
	_readyNavigationToken = QString();
	_cssToQtScale = 1.;
	_readyFromResource = false;
	_ready = false;
	_showErrorOnFailure = false;
	_shownCallback = nullptr;
	_failedCallback = nullptr;
	_pressedOutside = false;
	restoreFocus();
}

void EmbedOverlay::updateGeometry(QRect geometry, int contentWidth) {
	setGeometry(geometry);
	_contentWidth = contentWidth;
	updateContentGeometry();
	if (!isHidden()) {
		raiseSurfaces();
	}
}

void EmbedOverlay::testHandleWebviewMessage(const QJsonDocument &message) {
	handleWebviewMessage(message);
}

void EmbedOverlay::testHandleNavigationDone(bool success) {
	if (success && _readyFromResource) {
		handleWebviewMessage(NavigationReadyMessage(
			_request.resourceId,
			_readyNavigationToken,
			QString()));
		return;
	}
	handleNavigationDone(success);
}

bool EmbedOverlay::testLoadingCoverVisible() const {
	return _loading;
}

bool EmbedOverlay::testReadyDelayScheduled() const {
	return _readyDelayTimer.isActive();
}

void EmbedOverlay::testFireReadyDelay() {
	cancelReadyDelay();
	revealReadyEmbed();
}

const Webview::StorageId &EmbedOverlay::testEffectiveStorageId() const {
	return _storageId;
}

bool EmbedOverlay::eventFilter(QObject *object, QEvent *event) {
	const auto type = event->type();
	if (isHidden()
		|| (type != QEvent::ShortcutOverride && type != QEvent::KeyPress)
		|| !eventFromOverlayWindow(object)) {
		return QObject::eventFilter(object, event);
	}
	const auto keyEvent = static_cast<QKeyEvent*>(event);
	if (keyEvent->key() != Qt::Key_Escape) {
		return QObject::eventFilter(object, event);
	}
	keyEvent->accept();
	if (type == QEvent::KeyPress) {
		closeEmbed();
	}
	return true;
}

void EmbedOverlay::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);

	auto p = QPainter(this);
	const auto full = rect();
	p.fillRect(full, st::markdownEmbedOverlay.scrimBg);
	if (_contentGeometry.isEmpty()) {
		return;
	}
	if (_request.fullWidth) {
		p.fillRect(_contentGeometry, st::markdownEmbedOverlay.bg);
	} else {
		Ui::FillRoundRect(
			p,
			_contentGeometry,
			st::markdownEmbedOverlay.bg,
			Ui::PrepareCornerPixmaps(
				st::roundRadiusSmall,
				st::markdownEmbedOverlay.bg));
	}
}

void EmbedOverlay::mousePressEvent(QMouseEvent *e) {
	_pressedOutside = !_contentGeometry.contains(e->pos());
	e->accept();
}

void EmbedOverlay::mouseReleaseEvent(QMouseEvent *e) {
	const auto outside = !_contentGeometry.contains(e->pos());
	if (_pressedOutside && outside && e->button() == Qt::LeftButton) {
		closeEmbed();
	}
	_pressedOutside = false;
	e->accept();
}

void EmbedOverlay::installEscapeFilter() {
	if (!_escapeFilterInstalled) {
		qApp->installEventFilter(this);
		_escapeFilterInstalled = true;
	}
}

void EmbedOverlay::removeEscapeFilter() {
	if (_escapeFilterInstalled) {
		qApp->removeEventFilter(this);
		_escapeFilterInstalled = false;
	}
}

bool EmbedOverlay::eventFromOverlayWindow(QObject *object) const {
	const auto overlayWindow = window();
	const auto activeWindow = QApplication::activeWindow();
	if (!overlayWindow) {
		return true;
	} else if (const auto widget = qobject_cast<QWidget*>(object)) {
		return widget->window() == overlayWindow;
	} else if (const auto qwindow = qobject_cast<QWindow*>(object)) {
		return (qwindow == overlayWindow->windowHandle())
			|| !activeWindow
			|| (activeWindow == overlayWindow);
	}
	return !activeWindow || (activeWindow == overlayWindow);
}

Webview::WindowConfig EmbedOverlay::makeWindowConfig() const {
	return {
		.opaqueBg = st::markdownEmbedOverlay.bg->c,
		.storageId = _storageId,
		.safe = true,
	};
}

void EmbedOverlay::ensureWebview() {
	if (_webview && _webview->widget()) {
		updateContentGeometry();
		return;
	}
	const auto generation = ++_webviewGeneration;
	_webview = std::make_unique<Webview::Window>(
		_webviewParent ? _webviewParent.data() : this,
		makeWindowConfig());
	const auto raw = _webview.get();
	const auto widget = raw->widget();
	if (!widget) {
		_webview = nullptr;
		showWebviewError();
		return;
	}
	widget->hide();
	QObject::connect(widget, &QObject::destroyed, this, [=] {
		if (_webviewGeneration != generation
			|| !_webview
			|| _webview.get() != raw) {
			return;
		}
		crl::on_main(this, [=] {
			if (_webviewGeneration == generation
				&& _webview
				&& _webview.get() == raw) {
				_webview = nullptr;
				showWebviewError({ u"Error: WebView has crashed."_q });
			}
		});
	});
	raw->setDataRequestHandler([=](Webview::DataRequest request) {
		return handleDataRequest(std::move(request));
	});
	raw->init(EmbedInitScript());
	raw->setNavigationStartHandler([=](const QString &uri, bool newWindow) {
		Q_UNUSED(newWindow);

		if (uri == u"about:blank"_q
			|| uri.startsWith(u"http://desktop-app-resource/"_q)) {
			return true;
		}
		if (_linkActivationCallback && !uri.isEmpty()) {
			_linkActivationCallback(uri);
		}
		return false;
	});
	raw->setNavigationDoneHandler([=](bool success) {
		crl::on_main(this, [=] {
			if (_webviewGeneration == generation
				&& _webview
				&& (_webview.get() == raw)) {
				handleNavigationDone(success);
			}
		});
	});
	raw->setMessageHandler([=](const QJsonDocument &message) {
		crl::on_main(this, [=] {
			if (_webviewGeneration == generation
				&& _webview
				&& (_webview.get() == raw)) {
				handleWebviewMessage(message);
			}
		});
	});
	updateContentGeometry();
}

void EmbedOverlay::handleWebviewMessage(const QJsonDocument &message) {
	if (!_request) {
		return;
	}
	if (const auto height = ResizeFrameHeight(message)) {
		applyPreferredBodyHeight(cssPixelsToQt(height));
		return;
	}
	const auto object = message.object();
	const auto event = object.value("event").toString();
	if (event == u"navigation_ready"_q) {
		if (!_webview || !_webview->widget()) {
			showWebviewError();
			return;
		}
		if (!_readyFromResource) {
			return;
		}
		if (object.value("token").toString() != _readyNavigationToken) {
			return;
		}
		if (normalizedRequestId(
			object.value("resourceId").toString().toStdString())
				!= _request.resourceId) {
			return;
		}
		setReady();
		return;
	}
	if (event != u"preferred_size"_q) {
		return;
	}
	const auto width = ParsePositiveInt(object.value("width"));
	const auto height = ParsePositiveInt(object.value("height"));
	if (!width || !height) {
		return;
	}
	auto viewportWidth = ParsePositiveInt(object.value("viewportWidth"));
	if (!viewportWidth) {
		viewportWidth = ParsePositiveInt(object.value("viewport_width"));
	}
	if (!viewportWidth) {
		viewportWidth = ParsePositiveInt(object.value("innerWidth"));
	}
	updateCssToQtScale(viewportWidth);
	auto resourceId = object.value("resourceId").toString();
	if (resourceId.isEmpty()) {
		resourceId = object.value("resource_id").toString();
	}
	if (!resourceId.isEmpty()
		&& normalizedRequestId(resourceId.toStdString()) != _request.resourceId) {
		return;
	}
	applyPreferredBodySize(QSize(cssPixelsToQt(width), cssPixelsToQt(height)));
}

void EmbedOverlay::handleNavigationDone(bool success) {
	if (!_request || _ready) {
		return;
	}
	if (success) {
		if (!_readyFromResource) {
			setReady();
		}
		return;
	}
	showWebviewError();
}

void EmbedOverlay::cancelReadyDelay() {
	_readyDelayTimer.cancel();
}

void EmbedOverlay::setReady() {
	if (_ready || !_webview || !_webview->widget()) {
		return;
	}
	_ready = true;
	if (_pendingPreferredBodySize.isValid()
		&& (_pendingPreferredBodySize.width() > 0
			|| _pendingPreferredBodySize.height() > 0)) {
		_preferredBodySize = _pendingPreferredBodySize;
		_pendingPreferredBodySize = QSize();
	}
	updateContentGeometry();
	cancelReadyDelay();
	_readyDelayTimer.callOnce(kReadyRevealDelay);
}

void EmbedOverlay::revealReadyEmbed() {
	if (!_ready || !_request || !_webview || !_webview->widget()) {
		return;
	}
	cancelReadyDelay();
	clearWebviewError();
	QWidget::show();
	installEscapeFilter();
	raiseSurfaces();
	updateContentGeometry();
	_loading = false;
	_loadingAnimation.stop(anim::type::normal);
	_content->update();
	_webview->widget()->show();
	_webview->widget()->raise();
	_webview->focus();
	update();
	_content->update();
	if (const auto shownCallback = base::take(_shownCallback)) {
		shownCallback();
	}
	_failedCallback = nullptr;
}

void EmbedOverlay::applyPreferredBodySize(QSize size) {
	if (!size.isValid()) {
		return;
	}
	if (!_ready) {
		_pendingPreferredBodySize = size;
		return;
	}
	if (_preferredBodySize == size) {
		return;
	}
	_preferredBodySize = size;
	updateContentGeometry();
}

void EmbedOverlay::applyPreferredBodyHeight(int height) {
	if (height <= 0) {
		return;
	}
	applyPreferredBodySize(QSize(_preferredBodySize.width(), height));
}

void EmbedOverlay::updateCssToQtScale(int viewportWidth) {
	const auto body = bodyGeometry();
	if (viewportWidth <= 0 || body.width() <= 0) {
		return;
	}
	_cssToQtScale = std::clamp(body.width() / double(viewportWidth), 0.25, 4.);
}

void EmbedOverlay::updateContentGeometry() {
	if (!_content) {
		return;
	}
	const auto margin = st::markdownEmbedOverlay.margin;
	const auto available = _request.fullWidth
		? rect().marginsRemoved({ 0, margin.top(), 0, margin.bottom() })
		: rect().marginsRemoved(margin);
	if (available.isEmpty()) {
		_contentGeometry = QRect();
		_content->setGeometry(_contentGeometry);
		updateWebviewGeometry();
		if (_error) {
			_error->setGeometry(QRect());
		}
		update();
		return;
	}
	const auto padding = contentPadding();
	const auto horizontalPadding = padding.left() + padding.right();
	const auto verticalPadding = padding.top() + padding.bottom();
	const auto availableWidth = std::max(available.width(), 1);
	const auto availableHeight = std::max(available.height(), 1);
	const auto availableBodyHeight = std::max(
		availableHeight - verticalPadding,
		1);
	const auto width = _request.fullWidth
		? availableWidth
		: std::clamp(
			(_contentWidth > 0)
				? _contentWidth
				: st::markdownEmbedOverlay.size.width(),
			1,
			availableWidth);
	const auto bodyWidth = std::max(width - horizontalPadding, 1);
	const auto desiredBodyHeight = [&] {
		if (_preferredBodySize.height() > 0) {
			return _preferredBodySize.height();
		}
		if (_request.height > 0) {
			if (_request.width > 0) {
				return std::max(
					static_cast<int>(std::round(
						bodyWidth
							* (double(_request.height) / _request.width))),
					1);
			}
			return _request.height;
		}
		return st::markdownEmbedOverlay.size.height();
	}();
	const auto height = std::min(
		std::clamp(desiredBodyHeight, 1, availableBodyHeight)
			+ verticalPadding,
		availableHeight);
	_contentGeometry = style::centerrect(
		available,
		QRect(0, 0, width, height));
	_content->setGeometry(_contentGeometry);
	updateWebviewGeometry();
	if (_error && _errorLabel) {
		_errorLabel->setContextCopyText(_request.fallbackUrl);
		const auto body = bodyRectInContent();
		_error->resizeToWidth(std::max(body.width(), 1));
		_error->moveToLeft(
			body.x(),
			body.y() + std::max((body.height() - _error->height()) / 2, 0),
			_content->width());
		_error->raise();
	}
	update();
	_content->update();
}

void EmbedOverlay::updateWebviewGeometry() {
	if (_webview && _webview->widget()) {
		_webview->widget()->setGeometry(bodyGeometry());
		if (!_webview->widget()->isHidden()) {
			_webview->widget()->raise();
		}
	}
}

void EmbedOverlay::raiseSurfaces() {
	raise();
	if (_webview && _webview->widget() && !_webview->widget()->isHidden()) {
		_webview->widget()->raise();
	}
}

void EmbedOverlay::hideWebview() {
	if (_webview && _webview->widget()) {
		_webview->widget()->hide();
	}
}

void EmbedOverlay::destroyWebview() {
	++_webviewGeneration;
	if (_webview) {
		if (const auto widget = _webview->widget()) {
			widget->hide();
		}
		_webview = nullptr;
	}
}

void EmbedOverlay::showWebviewError() {
	const auto available = Webview::Availability();
	showWebviewError((available.error != Webview::Available::Error::None)
		? AvailabilityErrorText(available)
		: GenericWebviewErrorText());
}

void EmbedOverlay::showWebviewError(const TextWithEntities &text) {
	cancelReadyDelay();
	_ready = false;
	_loading = false;
	_loadingAnimation.stop(anim::type::normal);
	_content->update();
	hideWebview();
	destroyWebview();
	_shownCallback = nullptr;
	const auto failedCallback = base::take(_failedCallback);
	const auto showError = !isHidden() || _showErrorOnFailure;
	_showErrorOnFailure = false;
	_readyNavigationToken = QString();
	_readyFromResource = false;
	if (!showError) {
		clearWebviewError();
		_request = EmbedRequest();
		_preferredBodySize = QSize();
		_pendingPreferredBodySize = QSize();
		_cssToQtScale = 1.;
		restoreFocus();
		if (failedCallback) {
			failedCallback();
		}
		return;
	}
	QWidget::show();
	installEscapeFilter();
	raiseSurfaces();
	if (!_error) {
		_error = Ui::CreateChild<Ui::PaddingWrap<Ui::FlatLabel>>(
			_content,
			object_ptr<Ui::FlatLabel>(
				_content,
				QString(),
				st::markdownEmbedOverlay.errorLabel),
			st::markdownEmbedOverlay.errorPadding);
		_error->setObjectName(u"nativeIvEmbedOverlayErrorWrap"_q);
		_errorLabel = _error->entity();
		_errorLabel->setObjectName(u"nativeIvEmbedOverlayError"_q);
		_errorLabel->setClickHandlerFilter([=](
				const ClickHandlerPtr &handler,
				Qt::MouseButton) {
			const auto entity = handler->getTextEntity();
			if (entity.type != EntityType::CustomUrl) {
				return true;
			}
			File::OpenUrl(entity.data);
			return false;
		});
	}
	_errorLabel->setMarkedText(AddFallbackAction(text, _request.fallbackUrl));
	_error->show();
	updateContentGeometry();
	if (failedCallback) {
		failedCallback();
	}
}

void EmbedOverlay::clearWebviewError() {
	if (_error) {
		_error->hide();
	}
}

void EmbedOverlay::restoreFocus() {
	if (_focusRestore && _focusRestore->isVisible()) {
		_focusRestore->setFocus(Qt::OtherFocusReason);
	}
	_focusRestore = nullptr;
}

QRect EmbedOverlay::bodyGeometry() const {
	return _contentGeometry.isEmpty()
		? QRect()
		: _contentGeometry.marginsRemoved(contentPadding());
}

QRect EmbedOverlay::bodyRectInContent() const {
	const auto body = bodyGeometry();
	return QRect(
		body.topLeft() - _contentGeometry.topLeft(),
		body.size());
}

QMargins EmbedOverlay::contentPadding() const {
	return _request.fullWidth ? QMargins() : st::markdownEmbedOverlay.padding;
}

int EmbedOverlay::cssPixelsToQt(int value) const {
	return (value > 0)
		? std::max(static_cast<int>(std::round(value * _cssToQtScale)), 1)
		: 0;
}

QByteArray EmbedOverlay::normalizedRequestId(const std::string &id) const {
	auto normalized = QByteArray::fromStdString(id);
	if (const auto pos = normalized.indexOf('#'); pos >= 0) {
		normalized = normalized.left(pos);
	}
	while (normalized.startsWith('/')) {
		normalized.remove(0, 1);
	}
	return normalized;
}

Webview::DataResult EmbedOverlay::handleDataRequest(
		Webview::DataRequest request) {
	const auto id = normalizedRequestId(request.id);
	if (_resources) {
		if (const auto i = _resources->find(id); i != _resources->end()) {
			request.done({
				.stream = std::make_unique<Webview::DataStreamFromMemory>(
					i->second,
					"text/html; charset=utf-8"),
			});
			return Webview::DataResult::Done;
		}
	}
	const auto existing = std::array<const char*, 4>{
		"photo/",
		"document/",
		"map/",
		"html/",
	};
	for (const auto prefix : existing) {
		if (id.startsWith(prefix)) {
			return _dataRequestHandler
				? _dataRequestHandler(id, std::move(request))
				: Webview::DataResult::Failed;
		}
	}
	return Webview::DataResult::Failed;
}

} // namespace Iv::Markdown
