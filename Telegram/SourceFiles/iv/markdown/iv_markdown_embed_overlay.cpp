/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_embed_overlay.h"

#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "ui/cached_round_corners.h"
#include "ui/style/style_core_direction.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/padding_wrap.h"
#include "webview/webview_data_stream_memory.h"
#include "webview/webview_embed.h"
#include "webview/webview_interface.h"

#include "styles/style_layers.h"
#include "styles/style_iv.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <array>

namespace Iv::Markdown {
namespace {

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

} // namespace

EmbedOverlay::EmbedOverlay(
	QWidget *parent,
	const base::flat_map<QByteArray, QByteArray> *resources,
	std::function<void(QString)> linkActivationCallback,
	std::function<Webview::DataResult(QByteArray, Webview::DataRequest)>
		dataRequestHandler)
: Ui::RpWidget(parent)
, _resources(resources)
, _linkActivationCallback(std::move(linkActivationCallback))
, _dataRequestHandler(std::move(dataRequestHandler)) {
	setObjectName(u"nativeIvEmbedOverlay"_q);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMouseTracking(true);
	hide();

	_content = Ui::CreateChild<Ui::RpWidget>(this);
	_content->setObjectName(u"nativeIvEmbedOverlayShell"_q);
	_content->show();

	_close = Ui::CreateChild<Ui::IconButton>(
		_content,
		st::markdownEmbedOverlay.close);
	_close->setObjectName(u"nativeIvEmbedOverlayClose"_q);
	_close->setClickedCallback([=] {
		hide();
	});
	_close->show();
}

EmbedOverlay::~EmbedOverlay() = default;

bool EmbedOverlay::showEmbed(const EmbedRequest &request) {
	if (!request) {
		return false;
	}
	if (isHidden()) {
		_focusRestore = QApplication::focusWidget();
	}
	_request = request;
	clearWebviewError();
	QWidget::show();
	raise();
	updateContentGeometry();
	if (!_webview) {
		const auto available = Webview::Availability();
		if (available.error != Webview::Available::Error::None) {
			showWebviewError(AvailabilityErrorText(available));
			return true;
		}
	}
	ensureWebview();
	if (_webview && _webview->widget()) {
		_webview->widget()->show();
		if (_resources
			&& (_resources->find(request.resourceId) != _resources->end())) {
			_webview->navigateToData(QString::fromUtf8(request.resourceId));
		} else if (!request.fallbackUrl.isEmpty()) {
			_webview->navigate(request.fallbackUrl);
		} else {
			showWebviewError(GenericWebviewErrorText());
		}
		_close->raise();
		if (_error && !_error->isHidden()) {
			_error->raise();
		}
		_webview->focus();
	} else {
		showWebviewError();
	}
	return true;
}

void EmbedOverlay::hide() {
	if (isHidden()) {
		return;
	}
	QWidget::hide();
	restoreFocus();
}

void EmbedOverlay::updateGeometry(QRect geometry) {
	setGeometry(geometry);
	updateContentGeometry();
	if (!isHidden()) {
		raise();
	}
}

void EmbedOverlay::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::markdownEmbedOverlay.scrimBg);
	if (_contentGeometry.isEmpty()) {
		return;
	}
	Ui::Shadow::paint(p, _contentGeometry, width(), st::boxRoundShadow);
	Ui::FillRoundRect(
		p,
		_contentGeometry,
		st::markdownEmbedOverlay.bg,
		Ui::BoxCorners);
}

void EmbedOverlay::mousePressEvent(QMouseEvent *e) {
	_pressedOutside = !_contentGeometry.contains(e->pos());
	e->accept();
}

void EmbedOverlay::mouseReleaseEvent(QMouseEvent *e) {
	const auto outside = !_contentGeometry.contains(e->pos());
	if (_pressedOutside && outside && e->button() == Qt::LeftButton) {
		hide();
	}
	_pressedOutside = false;
	e->accept();
}

void EmbedOverlay::ensureWebview() {
	if (_webview && _webview->widget()) {
		updateContentGeometry();
		return;
	}
	_webview = std::make_unique<Webview::Window>(
		_content,
		Webview::WindowConfig{
			.opaqueBg = st::markdownEmbedOverlay.bg->c,
			.safe = true,
		});
	const auto raw = _webview.get();
	const auto widget = raw->widget();
	if (!widget) {
		_webview = nullptr;
		return;
	}
	widget->show();
	QObject::connect(widget, &QObject::destroyed, this, [=] {
		if (!_webview || _webview.get() != raw) {
			return;
		}
		crl::on_main(this, [=] {
			if (_webview && _webview.get() == raw) {
				_webview = nullptr;
				showWebviewError({ u"Error: WebView has crashed."_q });
			}
		});
	});
	raw->setDataRequestHandler([=](Webview::DataRequest request) {
		return handleDataRequest(std::move(request));
	});
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
	updateContentGeometry();
}

void EmbedOverlay::updateContentGeometry() {
	if (!_content) {
		return;
	}
	const auto available = rect().marginsRemoved(
		st::markdownEmbedOverlay.margin + st::boxRoundShadow.extend);
	if (available.isEmpty()) {
		_contentGeometry = QRect();
		_content->setGeometry(_contentGeometry);
		update();
		return;
	}
	const auto padding = st::markdownEmbedOverlay.padding;
	const auto chromeHeight = padding.top()
		+ st::markdownEmbedOverlay.close.height
		+ padding.bottom();
	const auto desiredWidth = _request.fullWidth
		? available.width()
		: (_request.width > 0
			? _request.width
			: st::markdownEmbedOverlay.size.width());
	const auto desiredHeight = (_request.height > 0
		? _request.height
		: st::markdownEmbedOverlay.size.height()) + chromeHeight;
	const auto width = std::max(
		1,
		std::min(available.width(), desiredWidth));
	const auto height = std::max(
		chromeHeight,
		std::min(available.height(), desiredHeight));
	_contentGeometry = style::centerrect(
		available,
		QRect(0, 0, width, height));
	_content->setGeometry(_contentGeometry);
	_close->moveToRight(
		st::markdownEmbedOverlay.closePosition.x(),
		st::markdownEmbedOverlay.closePosition.y(),
		_content->width());
	const auto body = bodyRect();
	if (_webview && _webview->widget()) {
		_webview->widget()->setGeometry(body);
	}
	if (_error && _errorLabel) {
		_errorLabel->setContextCopyText(_request.fallbackUrl);
		_error->resizeToWidth(std::max(body.width(), 1));
		_error->moveToLeft(
			body.x(),
			body.y() + std::max((body.height() - _error->height()) / 2, 0),
			_content->width());
		_error->raise();
	}
	_close->raise();
	update();
}

void EmbedOverlay::showWebviewError() {
	const auto available = Webview::Availability();
	showWebviewError((available.error != Webview::Available::Error::None)
		? AvailabilityErrorText(available)
		: GenericWebviewErrorText());
}

void EmbedOverlay::showWebviewError(const TextWithEntities &text) {
	if (_webview && _webview->widget()) {
		_webview->widget()->hide();
	}
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

QRect EmbedOverlay::bodyRect() const {
	const auto padding = st::markdownEmbedOverlay.padding;
	const auto top = padding.top()
		+ st::markdownEmbedOverlay.close.height
		+ padding.bottom();
	return QRect(
		padding.left(),
		top,
		std::max(_content->width() - padding.left() - padding.right(), 1),
		std::max(_content->height() - top - padding.bottom(), 1));
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
