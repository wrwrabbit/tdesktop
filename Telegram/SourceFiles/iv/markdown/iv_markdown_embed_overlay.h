/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "iv/markdown/iv_markdown_common.h"
#include "ui/rp_widget.h"

#include <functional>
#include <memory>
#include <string>

#include <QtCore/QPointer>
#include <QtCore/QRect>

class QMouseEvent;
class QPaintEvent;
class QWidget;
struct TextWithEntities;

namespace Ui {
class FlatLabel;
class IconButton;
template <typename Widget>
class PaddingWrap;
class RpWidget;
} // namespace Ui

namespace Webview {
class Window;
} // namespace Webview

namespace Iv::Markdown {

class EmbedOverlay final : public Ui::RpWidget {
public:
	EmbedOverlay(
		QWidget *parent,
		const base::flat_map<QByteArray, QByteArray> *resources,
		std::function<void(QString)> linkActivationCallback,
		std::function<Webview::DataResult(QByteArray, Webview::DataRequest)>
			dataRequestHandler);
	~EmbedOverlay();

	[[nodiscard]] bool showEmbed(const EmbedRequest &request);
	void hide();
	void updateGeometry(QRect geometry);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	void ensureWebview();
	void updateContentGeometry();
	void showWebviewError();
	void showWebviewError(const TextWithEntities &text);
	void clearWebviewError();
	void restoreFocus();
	[[nodiscard]] QRect bodyRect() const;
	[[nodiscard]] QByteArray normalizedRequestId(const std::string &id) const;
	[[nodiscard]] Webview::DataResult handleDataRequest(
		Webview::DataRequest request);

	const base::flat_map<QByteArray, QByteArray> *const _resources = nullptr;
	const std::function<void(QString)> _linkActivationCallback;
	const std::function<Webview::DataResult(QByteArray, Webview::DataRequest)>
		_dataRequestHandler;
	Ui::RpWidget *_content = nullptr;
	Ui::IconButton *_close = nullptr;
	Ui::PaddingWrap<Ui::FlatLabel> *_error = nullptr;
	Ui::FlatLabel *_errorLabel = nullptr;
	std::unique_ptr<Webview::Window> _webview;
	EmbedRequest _request;
	QRect _contentGeometry;
	QPointer<QWidget> _focusRestore;
	bool _pressedOutside = false;

};

} // namespace Iv::Markdown
