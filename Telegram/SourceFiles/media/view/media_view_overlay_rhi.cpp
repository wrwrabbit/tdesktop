/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/view/media_view_overlay_rhi.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

#include "ui/rhi/rhi_shader.h"
#include "ui/painter.h"
#include "media/streaming/media_streaming_common.h"
#include "base/debug_log.h"
#include "styles/style_media_view.h"

#include <rhi/qrhi.h>

namespace Media::View {
namespace {

using namespace Ui::GL;

struct ImageUniforms {
	float viewport[2];
	float g_opacity;
	float _pad0;
};
static_assert(sizeof(ImageUniforms) % 16 == 0);

[[nodiscard]] QShader LoadShader(const QString &name) {
	return Ui::Rhi::ShaderFromFile(
		u":/shaders/"_q + name + u".qsb"_q);
}

} // namespace

OverlayWidget::RendererRhi::RendererRhi(not_null<OverlayWidget*> owner)
: _owner(owner) {
}

void OverlayWidget::RendererRhi::initialize(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) {
	if (_initialized && _rhi == rhi) {
		return;
	}
	releaseResources();

	_rhi = rhi;
	_rt = rt;
	_cb = cb;

	_vertexBuffer = rhi->newBuffer(
		QRhiBuffer::Dynamic,
		QRhiBuffer::VertexBuffer,
		kMaxDraws * kVertexSize);
	_vertexBuffer->create();

	_uniformBuffer = rhi->newBuffer(
		QRhiBuffer::Dynamic,
		QRhiBuffer::UniformBuffer,
		kMaxDraws * 256);
	_uniformBuffer->create();

	_sampler = rhi->newSampler(
		QRhiSampler::Linear,
		QRhiSampler::Linear,
		QRhiSampler::None,
		QRhiSampler::ClampToEdge,
		QRhiSampler::ClampToEdge);
	_sampler->create();

	_placeholderTexture = rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
	_placeholderTexture->create();

	createPipelines();
	_initialized = true;

	LOG(("[RENDERER_TEST] component=overlay backend=%1 device=%2 status=OK")
		.arg(rhi->backendName())
		.arg(rhi->driverInfo().deviceName));
}

void OverlayWidget::RendererRhi::createPipelines() {
	const auto rpDesc = _rt->renderPassDescriptor();

	const auto argb32Vert = LoadShader(u"argb32.vert"_q);
	const auto argb32Frag = LoadShader(u"argb32.frag"_q);
	const auto controlsFrag = LoadShader(u"controls.frag"_q);

	auto *sampleSrb = _rhi->newShaderResourceBindings();
	sampleSrb->setBindings({
		QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
			0,
			QRhiShaderResourceBinding::VertexStage
				| QRhiShaderResourceBinding::FragmentStage,
			_uniformBuffer,
			sizeof(ImageUniforms)),
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_sampler),
	});
	sampleSrb->create();
	_perDrawSrbs.push_back(sampleSrb);

	auto *imagePipeline = _rhi->newGraphicsPipeline();
	imagePipeline->setShaderStages({
		{ QRhiShaderStage::Vertex, argb32Vert },
		{ QRhiShaderStage::Fragment, argb32Frag },
	});
	QRhiVertexInputLayout inputLayout;
	inputLayout.setBindings({ { 4 * sizeof(float) } });
	inputLayout.setAttributes({
		{ 0, 0, QRhiVertexInputAttribute::Float2, 0 },
		{ 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
	});
	imagePipeline->setVertexInputLayout(inputLayout);
	imagePipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
	imagePipeline->setShaderResourceBindings(sampleSrb);
	imagePipeline->setRenderPassDescriptor(rpDesc);
	imagePipeline->create();
	_imagePipeline = imagePipeline;

	auto *imageBlendPipeline = _rhi->newGraphicsPipeline();
	imageBlendPipeline->setShaderStages({
		{ QRhiShaderStage::Vertex, argb32Vert },
		{ QRhiShaderStage::Fragment, controlsFrag },
	});
	imageBlendPipeline->setVertexInputLayout(inputLayout);
	QRhiGraphicsPipeline::TargetBlend blend;
	blend.enable = true;
	blend.srcColor = QRhiGraphicsPipeline::One;
	blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
	blend.srcAlpha = QRhiGraphicsPipeline::One;
	blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
	imageBlendPipeline->setTargetBlends({ blend });
	imageBlendPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
	imageBlendPipeline->setShaderResourceBindings(sampleSrb);
	imageBlendPipeline->setRenderPassDescriptor(rpDesc);
	imageBlendPipeline->create();
	_imageBlendPipeline = imageBlendPipeline;
}

void OverlayWidget::RendererRhi::render(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) {
	_rhi = rhi;
	_rt = rt;
	_cb = cb;
	_nextVertexSlot = 0;
	_nextPoolIndex = 0;
	for (auto *srb : _perDrawSrbs) {
		delete srb;
	}
	_perDrawSrbs.clear();
	_drawCommands.clear();

	const auto size = rt->pixelSize();
	_factor = _owner->widget()->devicePixelRatioF();
	_ifactor = int(std::ceil(_factor));
	_viewport = QSize(
		int(size.width() / _factor),
		int(size.height() / _factor));

	_rub = rhi->nextResourceUpdateBatch();

	_owner->paint(this);

	cb->beginPass(rt, QColor(0, 0, 0, 0), { 1.0f, 0 }, _rub);
	_rub = nullptr;

	for (const auto &cmd : _drawCommands) {
		cb->setGraphicsPipeline(cmd.pipeline);
		cb->setShaderResources(cmd.srb);
		cb->setViewport({
			0, 0,
			float(rt->pixelSize().width()),
			float(rt->pixelSize().height()) });
		const QRhiCommandBuffer::VertexInput vbufBinding(
			_vertexBuffer, cmd.vertexIndex * kVertexSize);
		cb->setVertexInput(0, 1, &vbufBinding);
		cb->draw(4);
	}
	_drawCommands.clear();

	cb->endPass();
}

QRhiTexture *OverlayWidget::RendererRhi::acquirePoolTexture(QSize size) {
	if (_nextPoolIndex < int(_texturePool.size())) {
		auto &entry = _texturePool[_nextPoolIndex++];
		if (entry.size == size) {
			return entry.texture;
		}
		delete entry.texture;
		entry.texture = _rhi->newTexture(QRhiTexture::BGRA8, size);
		entry.texture->create();
		entry.size = size;
		return entry.texture;
	}
	auto *tex = _rhi->newTexture(QRhiTexture::BGRA8, size);
	tex->create();
	_texturePool.push_back({ tex, size });
	_nextPoolIndex++;
	return tex;
}

void OverlayWidget::RendererRhi::releaseResources() {
	_drawCommands.clear();

	for (auto &entry : _texturePool) {
		delete entry.texture;
	}
	_texturePool.clear();

	delete _imagePipeline;
	_imagePipeline = nullptr;
	delete _imageBlendPipeline;
	_imageBlendPipeline = nullptr;

	for (auto *srb : _perDrawSrbs) {
		delete srb;
	}
	_perDrawSrbs.clear();

	delete _rgbaTexture;
	_rgbaTexture = nullptr;
	delete _placeholderTexture;
	_placeholderTexture = nullptr;

	delete _vertexBuffer;
	_vertexBuffer = nullptr;
	delete _uniformBuffer;
	_uniformBuffer = nullptr;
	delete _sampler;
	_sampler = nullptr;

	_initialized = false;
}

QColor OverlayWidget::RendererRhi::rhiClearColor() {
	return QColor(0, 0, 0, 0);
}

std::optional<QColor> OverlayWidget::RendererRhi::clearColor() {
	return QColor(0, 0, 0, 0);
}

void OverlayWidget::RendererRhi::drawTexturedQuad(
		QRhiGraphicsPipeline *pipeline,
		QRhiTexture *texture,
		const float *coords,
		float opacity,
		bool blend) {
	if (_nextVertexSlot >= kMaxDraws || !texture) {
		return;
	}
	const auto slot = _nextVertexSlot++;
	const auto vOffset = slot * kVertexSize;
	const auto uOffset = slot * 256;

	_rub->updateDynamicBuffer(
		_vertexBuffer, vOffset, kVertexSize, coords);

	ImageUniforms uniforms{};
	uniforms.viewport[0] = _viewport.width() * _factor;
	uniforms.viewport[1] = _viewport.height() * _factor;
	uniforms.g_opacity = opacity;
	_rub->updateDynamicBuffer(
		_uniformBuffer, uOffset, sizeof(ImageUniforms), &uniforms);

	auto *srb = _rhi->newShaderResourceBindings();
	srb->setBindings({
		QRhiShaderResourceBinding::uniformBuffer(
			0,
			QRhiShaderResourceBinding::VertexStage
				| QRhiShaderResourceBinding::FragmentStage,
			_uniformBuffer,
			uOffset,
			sizeof(ImageUniforms)),
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			texture,
			_sampler),
	});
	srb->create();
	_perDrawSrbs.push_back(srb);

	_drawCommands.push_back({
		.pipeline = blend ? _imageBlendPipeline : pipeline,
		.srb = srb,
		.vertexIndex = slot,
	});
}

void OverlayWidget::RendererRhi::paintUsingRaster(
		QRect rect,
		Fn<void(Painter&)> method,
		bool transparent) {
	if (!_imagePipeline || rect.isEmpty()) {
		return;
	}
	const auto size = rect.size() * _ifactor;
	auto raster = QImage(size, QImage::Format_ARGB32_Premultiplied);
	raster.setDevicePixelRatio(_factor);
	raster.fill(Qt::transparent);
	{
		auto painter = Painter(&raster);
		method(painter);
	}

	auto *tex = acquirePoolTexture(size);
	_rub->uploadTexture(
		tex,
		QRhiTextureUploadDescription(
			QRhiTextureUploadEntry(0, 0,
				QRhiTextureSubresourceUploadDescription(raster))));

	const auto rRect = transformRect(rect);
	const float coords[] = {
		rRect.left(), rRect.bottom(),
		0.f, 0.f,

		rRect.right(), rRect.bottom(),
		1.f, 0.f,

		rRect.left(), rRect.top(),
		0.f, 1.f,

		rRect.right(), rRect.top(),
		1.f, 1.f,
	};

	drawTexturedQuad(
		_imagePipeline,
		tex,
		coords,
		1.0f,
		transparent);
}

void OverlayWidget::RendererRhi::paintBackground() {
	const auto &bg = _owner->_fullScreenVideo
		? st::mediaviewVideoBg
		: st::mediaviewBg;
	const auto c = bg->c;
	const auto vw = float(_viewport.width() * _factor);
	const auto vh = float(_viewport.height() * _factor);

	auto bgImage = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
	bgImage.fill(c);

	auto *tex = acquirePoolTexture(QSize(1, 1));
	_rub->uploadTexture(
		tex,
		QRhiTextureUploadDescription(
			QRhiTextureUploadEntry(0, 0,
				QRhiTextureSubresourceUploadDescription(bgImage))));

	const float coords[] = {
		0.f, vh, 0.f, 0.f,
		vw,  vh, 1.f, 0.f,
		0.f, 0.f, 0.f, 1.f,
		vw,  0.f, 1.f, 1.f,
	};
	drawTexturedQuad(_imagePipeline, tex, coords);
}

void OverlayWidget::RendererRhi::paintVideoStream() {
}

void OverlayWidget::RendererRhi::paintTransformedVideoFrame(
		ContentGeometry geometry) {
	const auto data = _owner->videoFrameWithInfo();
	if (data.format == Streaming::FrameFormat::None) {
		return;
	}
	if (data.format == Streaming::FrameFormat::ARGB32) {
		Assert(!data.image.isNull());
		paintTransformedStaticContent(
			data.image, geometry, false, false);
		return;
	}
	paintTransformedStaticContent(
		_owner->videoFrame(),
		geometry,
		false,
		false);
}

void OverlayWidget::RendererRhi::paintTransformedStaticContent(
		const QImage &image,
		ContentGeometry geometry,
		bool semiTransparent,
		bool fillTransparentBackground,
		int index) {
	if (image.isNull() || !_imagePipeline) {
		return;
	}

	auto *tex = acquirePoolTexture(image.size());
	_rub->uploadTexture(
		tex,
		QRhiTextureUploadDescription(
			QRhiTextureUploadEntry(0, 0,
				QRhiTextureSubresourceUploadDescription(image))));

	const auto rect = geometry.rect;
	LOG(("QRhi Overlay: rect=(%1,%2,%3,%4) viewport=(%5,%6) factor=%7 image=(%8,%9)")
		.arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())
		.arg(_viewport.width()).arg(_viewport.height())
		.arg(_factor)
		.arg(image.width()).arg(image.height()));
	const auto rRect = transformRect(rect);
	std::array<std::array<float, 2>, 4> texcoords = { {
		{ { 0.f, 0.f } },
		{ { 1.f, 0.f } },
		{ { 0.f, 1.f } },
		{ { 1.f, 1.f } },
	} };
	if (const auto shift = int(geometry.rotation) / 90; shift != 0) {
		std::rotate(
			begin(texcoords),
			begin(texcoords) + shift,
			end(texcoords));
	}
	const float coords[] = {
		rRect.left(), rRect.bottom(),
		texcoords[0][0], texcoords[0][1],

		rRect.right(), rRect.bottom(),
		texcoords[1][0], texcoords[1][1],

		rRect.left(), rRect.top(),
		texcoords[2][0], texcoords[2][1],

		rRect.right(), rRect.top(),
		texcoords[3][0], texcoords[3][1],
	};

	drawTexturedQuad(_imagePipeline, tex, coords);
}

void OverlayWidget::RendererRhi::paintRadialLoading(
		QRect inner,
		bool radial,
		float64 radialOpacity) {
	paintUsingRaster(inner, [&](Painter &p) {
		const auto newInner = QRect(QPoint(), inner.size());
		_owner->paintRadialLoadingContent(p, newInner, radial, radialOpacity);
	}, true);
}

void OverlayWidget::RendererRhi::paintThemePreview(QRect outer) {
	paintUsingRaster(outer, [&](Painter &p) {
		const auto newOuter = QRect(QPoint(), outer.size());
		_owner->paintThemePreviewContent(p, newOuter, newOuter);
	});
}

void OverlayWidget::RendererRhi::paintDocumentBubble(
		QRect outer,
		QRect icon) {
	paintUsingRaster(outer, [&](Painter &p) {
		const auto newOuter = QRect(QPoint(), outer.size());
		const auto newIcon = icon.translated(-outer.topLeft());
		_owner->paintDocumentBubbleContent(p, newOuter, newIcon, newOuter);
	}, true);
}

void OverlayWidget::RendererRhi::paintSaveMsg(QRect outer) {
	paintUsingRaster(outer, [&](Painter &p) {
		const auto newOuter = QRect(QPoint(), outer.size());
		_owner->paintSaveMsgContent(p, newOuter, newOuter);
	}, true);
}

void OverlayWidget::RendererRhi::paintChapter(QRect outer) {
	paintUsingRaster(outer, [&](Painter &p) {
		const auto newOuter = QRect(QPoint(), outer.size());
		_owner->paintChapterContent(p, newOuter, newOuter);
	}, true);
}

void OverlayWidget::RendererRhi::paintSpeedBoost(QRect outer) {
	paintUsingRaster(outer, [&](Painter &p) {
		const auto newOuter = QRect(QPoint(), outer.size());
		_owner->paintSpeedBoostContent(p, newOuter, newOuter);
	}, true);
}

void OverlayWidget::RendererRhi::paintControlsStart() {
}

void OverlayWidget::RendererRhi::paintControl(
		Over control,
		QRect over,
		float64 overOpacity,
		QRect inner,
		float64 innerOpacity,
		const style::icon &icon) {
	paintUsingRaster(inner, [&](Painter &p) {
		const auto newInner = QRect(QPoint(), inner.size());
		icon.paint(p, 0, 0, newInner.width());
	}, true);
}

void OverlayWidget::RendererRhi::paintFooter(
		QRect outer,
		float64 opacity) {
	if (outer.isEmpty() || opacity <= 0.) {
		return;
	}
	paintUsingRaster(outer, [&](Painter &p) {
		const auto newOuter = QRect(QPoint(), outer.size());
		_owner->paintFooterContent(p, newOuter, newOuter, opacity);
	}, true);
}

void OverlayWidget::RendererRhi::paintCaption(
		QRect outer,
		float64 opacity) {
	if (outer.isEmpty() || opacity <= 0.) {
		return;
	}
	paintUsingRaster(outer, [&](Painter &p) {
		const auto newOuter = QRect(QPoint(), outer.size());
		_owner->paintCaptionContent(p, newOuter, newOuter, opacity);
	}, true);
}

void OverlayWidget::RendererRhi::paintGroupThumbs(
		QRect outer,
		float64 opacity) {
	if (outer.isEmpty() || opacity <= 0.) {
		return;
	}
	paintUsingRaster(outer, [&](Painter &p) {
		const auto newOuter = QRect(QPoint(), outer.size());
		_owner->paintGroupThumbsContent(p, newOuter, newOuter, opacity);
	}, true);
}

void OverlayWidget::RendererRhi::paintRoundedCorners(int radius) {
}

void OverlayWidget::RendererRhi::paintStoriesSiblingPart(
		int index,
		const QImage &image,
		QRect rect,
		float64 opacity) {
	paintTransformedStaticContent(
		image,
		{ .rect = QRectF(rect) },
		false,
		false,
		index);
}

Rect OverlayWidget::RendererRhi::transformRect(const Rect &raster) const {
	return TransformRect(raster, _viewport, _factor);
}

Rect OverlayWidget::RendererRhi::transformRect(const QRectF &raster) const {
	return TransformRect(raster, _viewport, _factor);
}

Rect OverlayWidget::RendererRhi::transformRect(const QRect &raster) const {
	return TransformRect(Rect(raster), _viewport, _factor);
}

} // namespace Media::View

#endif // Qt >= 6.7
