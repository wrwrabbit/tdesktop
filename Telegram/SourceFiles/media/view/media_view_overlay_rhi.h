/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/view/media_view_overlay_renderer.h"
#include "ui/rhi/rhi_renderer.h"
#include "ui/rhi/rhi_image.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiSampler;
class QRhiGraphicsPipeline;
class QRhiShaderResourceBindings;
class QRhiResourceUpdateBatch;

namespace Media::View {

class OverlayWidget::RendererRhi final
	: public OverlayWidget::Renderer
	, public Ui::Rhi::Renderer {
public:
	explicit RendererRhi(not_null<OverlayWidget*> owner);

	void initialize(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) override;
	void render(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) override;
	void releaseResources() override;

	QColor rhiClearColor() override;
	std::optional<QColor> clearColor() override;

private:
	void paintBackground() override;
	void paintVideoStream() override;
	void paintTransformedVideoFrame(ContentGeometry geometry) override;
	void paintTransformedStaticContent(
		const QImage &image,
		ContentGeometry geometry,
		bool semiTransparent,
		bool fillTransparentBackground,
		int index = 0) override;
	void paintRadialLoading(
		QRect inner,
		bool radial,
		float64 radialOpacity) override;
	void paintThemePreview(QRect outer) override;
	void paintDocumentBubble(QRect outer, QRect icon) override;
	void paintSaveMsg(QRect outer) override;
	void paintChapter(QRect outer) override;
	void paintSpeedBoost(QRect outer) override;
	void paintControlsStart() override;
	void paintControl(
		Over control,
		QRect over,
		float64 overOpacity,
		QRect inner,
		float64 innerOpacity,
		const style::icon &icon) override;
	void paintFooter(QRect outer, float64 opacity) override;
	void paintCaption(QRect outer, float64 opacity) override;
	void paintGroupThumbs(QRect outer, float64 opacity) override;
	void paintRoundedCorners(int radius) override;
	void paintStoriesSiblingPart(
		int index,
		const QImage &image,
		QRect rect,
		float64 opacity = 1.) override;

	void createPipelines();

	void drawTexturedQuad(
		QRhiGraphicsPipeline *pipeline,
		QRhiTexture *texture,
		const float *coords,
		float opacity = 1.f,
		bool blend = false);

	void paintUsingRaster(
		Ui::Rhi::Image &image,
		QRect rect,
		Fn<void(Painter&)> method,
		bool transparent = false);

	[[nodiscard]] Ui::GL::Rect transformRect(const QRect &raster) const;
	[[nodiscard]] Ui::GL::Rect transformRect(const QRectF &raster) const;
	[[nodiscard]] Ui::GL::Rect transformRect(
		const Ui::GL::Rect &raster) const;

	const not_null<OverlayWidget*> _owner;

	QRhi *_rhi = nullptr;
	QRhiRenderTarget *_rt = nullptr;
	QRhiCommandBuffer *_cb = nullptr;
	QRhiResourceUpdateBatch *_rub = nullptr;
	QSize _viewport;
	float _factor = 1.;
	int _ifactor = 1;

	static constexpr int kMaxDraws = 32;
	static constexpr int kVertexSize = 4 * 4 * sizeof(float);
	static constexpr int kUniformSize = 16;

	QRhiBuffer *_vertexBuffer = nullptr;
	QRhiBuffer *_uniformBuffer = nullptr;
	QRhiSampler *_sampler = nullptr;
	QRhiTexture *_placeholderTexture = nullptr;

	QRhiGraphicsPipeline *_imagePipeline = nullptr;
	QRhiGraphicsPipeline *_imageBlendPipeline = nullptr;

	struct DrawCommand {
		QRhiGraphicsPipeline *pipeline = nullptr;
		QRhiShaderResourceBindings *srb = nullptr;
		int vertexIndex = 0;
	};
	std::vector<DrawCommand> _drawCommands;
	std::vector<QRhiShaderResourceBindings*> _perDrawSrbs;
	int _nextVertexSlot = 0;

	QRhiTexture *_rgbaTexture = nullptr;
	QSize _rgbaSize;
	quint64 _cacheKey = 0;

	Ui::Rhi::Image _controlsFadeImage;
	Ui::Rhi::Image _radialImage;
	Ui::Rhi::Image _themePreviewImage;
	Ui::Rhi::Image _documentBubbleImage;
	Ui::Rhi::Image _saveMsgImage;
	Ui::Rhi::Image _footerImage;
	Ui::Rhi::Image _captionImage;
	Ui::Rhi::Image _groupThumbsImage;
	Ui::Rhi::Image _controlsImage;

	bool _initialized = false;

	rpl::lifetime _lifetime;

};

} // namespace Media::View

#endif // Qt >= 6.7
