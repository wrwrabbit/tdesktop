/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/group/calls_group_viewport_rhi.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

#include "calls/group/calls_group_viewport_tile.h"
#include "calls/group/calls_group_members_row.h"
#include "webrtc/webrtc_video_track.h"
#include "media/view/media_view_pip.h"
#include "media/streaming/media_streaming_utility.h"
#include "ui/rhi/rhi_shader.h"
#include "ui/painter.h"
#include "data/data_peer.h"
#include "lang/lang_keys.h"
#include "styles/style_calls.h"
#include "styles/style_media_view.h"

#include <rhi/qrhi.h>

namespace Calls::Group {
namespace {

using namespace Ui::GL;

constexpr auto kNoiseTextureSize = 256;
constexpr auto kBlurTextureSizeFactor = 4.;
constexpr auto kBlurOpacity = 0.65f;

struct GroupFrameUniforms {
	float viewport[2];
	float _pad0[2];
	float frameBg[4];
	float shadow[4];
	float paused;
	float _pad1[3];
	float roundRect[4];
	float radiusOutline[2];
	float _pad2[2];
	float roundBg[4];
	float outlineFg[4];
};
static_assert(sizeof(GroupFrameUniforms) == 128);

struct BlurUniforms {
	float texelOffset;
	float _pad[3];
};
static_assert(sizeof(BlurUniforms) == 16);

struct ImageUniforms {
	float viewport[2];
	float g_opacity;
	float _pad;
};
static_assert(sizeof(ImageUniforms) == 16);

[[nodiscard]] QShader LoadShader(const QString &name) {
	return Ui::Rhi::ShaderFromFile(
		u":/shaders/"_q + name + u".qsb"_q);
}

[[nodiscard]] bool UseExpandForCamera(QSize original, QSize viewport) {
	using namespace ::Media::Streaming;
	return DecideFrameResize(viewport, original).expanding;
}

[[nodiscard]] QSize NonEmpty(QSize size) {
	return QSize(std::max(size.width(), 1), std::max(size.height(), 1));
}

[[nodiscard]] QSize CountBlurredSize(
		QSize unscaled,
		QSize outer,
		float factor) {
	factor *= kBlurTextureSizeFactor;
	const auto area = outer / int(base::SafeRound(factor * cScale() / 100));
	const auto scaled = unscaled.scaled(area, Qt::KeepAspectRatio);
	return (scaled.width() > unscaled.width()
		|| scaled.height() > unscaled.height())
		? unscaled
		: NonEmpty(scaled);
}

[[nodiscard]] QSize InterpolateScaledSize(
		QSize unscaled,
		QSize size,
		float64 ratio) {
	if (ratio == 0.) {
		return NonEmpty(unscaled.scaled(size, Qt::KeepAspectRatio));
	} else if (ratio == 1.) {
		return NonEmpty(unscaled.scaled(
			size,
			Qt::KeepAspectRatioByExpanding));
	}
	const auto notExpanded = NonEmpty(unscaled.scaled(
		size,
		Qt::KeepAspectRatio));
	const auto expanded = NonEmpty(unscaled.scaled(
		size,
		Qt::KeepAspectRatioByExpanding));
	return QSize(
		anim::interpolate(notExpanded.width(), expanded.width(), ratio),
		anim::interpolate(notExpanded.height(), expanded.height(), ratio));
}

[[nodiscard]] std::array<std::array<float, 2>, 4> CountTexCoords(
		QSize unscaled,
		QSize size,
		float64 expandRatio,
		bool swap = false) {
	const auto scaled = InterpolateScaledSize(unscaled, size, expandRatio);
	const auto left = (size.width() - scaled.width()) / 2;
	const auto top = (size.height() - scaled.height()) / 2;
	auto dleft = float(left) / scaled.width();
	auto dright = float(size.width() - left) / scaled.width();
	auto dtop = float(top) / scaled.height();
	auto dbottom = float(size.height() - top) / scaled.height();
	if (swap) {
		std::swap(dleft, dtop);
		std::swap(dright, dbottom);
	}
	return { {
		{ { -dleft, dtop } },
		{ { dright, dtop } },
		{ { -dleft, dbottom } },
		{ { dright, dbottom } },
	} };
}

} // namespace

Viewport::RendererRhi::RendererRhi(not_null<Viewport*> owner)
: _owner(owner)
, _pinIcon(st::groupCallVideoTile.pin)
, _muteIcon(st::groupCallVideoCrossLine)
, _pinBackground(
	(st::groupCallVideoTile.pinPadding.top()
		+ st::groupCallVideoTile.pin.icon.height()
		+ st::groupCallVideoTile.pinPadding.bottom()) / 2,
	st::radialBg) {

	style::PaletteChanged(
	) | rpl::on_next([=] {
		_buttons.invalidate();
	}, _lifetime);
}

Viewport::RendererRhi::~RendererRhi() {
	releaseResources();
}

QColor Viewport::RendererRhi::rhiClearColor() {
	return _owner->_fullscreen
		? QColor(0, 0, 0)
		: _owner->videoStream()
		? st::mediaviewBg->c
		: st::groupCallBg->c;
}

std::optional<QColor> Viewport::RendererRhi::clearColor() {
	return rhiClearColor();
}

void Viewport::RendererRhi::initialize(
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

	_offscreenVertexBuffer = rhi->newBuffer(
		QRhiBuffer::Dynamic,
		QRhiBuffer::VertexBuffer,
		4 * 4 * sizeof(float));
	_offscreenVertexBuffer->create();

	_onscreenVertexBuffer = rhi->newBuffer(
		QRhiBuffer::Dynamic,
		QRhiBuffer::VertexBuffer,
		32 * 4 * sizeof(float));
	_onscreenVertexBuffer->create();

	_uniformBuffer = rhi->newBuffer(
		QRhiBuffer::Dynamic,
		QRhiBuffer::UniformBuffer,
		4096);
	_uniformBuffer->create();

	_linearSampler = rhi->newSampler(
		QRhiSampler::Linear,
		QRhiSampler::Linear,
		QRhiSampler::None,
		QRhiSampler::ClampToEdge,
		QRhiSampler::ClampToEdge);
	_linearSampler->create();

	_nearestSampler = rhi->newSampler(
		QRhiSampler::Nearest,
		QRhiSampler::Nearest,
		QRhiSampler::None,
		QRhiSampler::ClampToEdge,
		QRhiSampler::ClampToEdge);
	_nearestSampler->create();

	_noiseRepeatSampler = rhi->newSampler(
		QRhiSampler::Nearest,
		QRhiSampler::Nearest,
		QRhiSampler::None,
		QRhiSampler::Repeat,
		QRhiSampler::Repeat);
	_noiseRepeatSampler->create();

	_placeholderTexture = rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
	_placeholderTexture->create();

	createPipelines();
	_initialized = true;
}

void Viewport::RendererRhi::createPipelines() {
	const auto rpDesc = _rt->renderPassDescriptor();

	const auto passthroughVert = LoadShader(u"passthrough.vert"_q);
	const auto argb32Vert = LoadShader(u"argb32.vert"_q);
	const auto groupFrameVert = LoadShader(u"group_frame.vert"_q);
	const auto argb32Frag = LoadShader(u"argb32.frag"_q);
	const auto yuv420Frag = LoadShader(u"yuv420.frag"_q);
	const auto blurHFrag = LoadShader(u"blur_h.frag"_q);
	const auto blurVFrag = LoadShader(u"blur_v.frag"_q);
	const auto groupFrameFrag = LoadShader(u"group_frame.frag"_q);
	const auto controlsFrag = LoadShader(u"controls.frag"_q);

	QRhiVertexInputLayout passthroughLayout;
	passthroughLayout.setBindings({ { 4 * sizeof(float) } });
	passthroughLayout.setAttributes({
		{ 0, 0, QRhiVertexInputAttribute::Float2, 0 },
		{ 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
	});

	QRhiVertexInputLayout argb32Layout;
	argb32Layout.setBindings({ { 4 * sizeof(float) } });
	argb32Layout.setAttributes({
		{ 0, 0, QRhiVertexInputAttribute::Float2, 0 },
		{ 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
	});

	QRhiVertexInputLayout groupFrameLayout;
	groupFrameLayout.setBindings({ { 6 * sizeof(float) } });
	groupFrameLayout.setAttributes({
		{ 0, 0, QRhiVertexInputAttribute::Float2, 0 },
		{ 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
		{ 0, 2, QRhiVertexInputAttribute::Float2, 4 * sizeof(float) },
	});

	_downscaleArgb32Srb = _rhi->newShaderResourceBindings();
	_downscaleArgb32Srb->setBindings({
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_linearSampler),
	});
	_downscaleArgb32Srb->create();
	_perDrawSrbs.push_back(_downscaleArgb32Srb);

	_downscaleYuv420Srb = _rhi->newShaderResourceBindings();
	_downscaleYuv420Srb->setBindings({
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_linearSampler),
		QRhiShaderResourceBinding::sampledTexture(
			2,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_linearSampler),
		QRhiShaderResourceBinding::sampledTexture(
			3,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_linearSampler),
	});
	_downscaleYuv420Srb->create();
	_perDrawSrbs.push_back(_downscaleYuv420Srb);

	_blurHSrb = _rhi->newShaderResourceBindings();
	_blurHSrb->setBindings({
		QRhiShaderResourceBinding::uniformBuffer(
			0,
			QRhiShaderResourceBinding::FragmentStage,
			_uniformBuffer,
			0,
			sizeof(BlurUniforms)),
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_nearestSampler),
	});
	_blurHSrb->create();
	_perDrawSrbs.push_back(_blurHSrb);

	{
		auto *tex = _rhi->newTexture(
			QRhiTexture::RGBA8,
			QSize(1, 1),
			1,
			QRhiTexture::RenderTarget);
		tex->create();
		auto colorAtt = QRhiColorAttachment(tex);
		auto *offscreenRT = _rhi->newTextureRenderTarget(
			QRhiTextureRenderTargetDescription(colorAtt));
		_offscreenRpDesc = offscreenRT->newCompatibleRenderPassDescriptor();
		offscreenRT->setRenderPassDescriptor(_offscreenRpDesc);
		offscreenRT->create();
		delete offscreenRT;
		delete tex;
	}

	_downscaleArgb32Pipeline = _rhi->newGraphicsPipeline();
	_downscaleArgb32Pipeline->setShaderStages({
		{ QRhiShaderStage::Vertex, passthroughVert },
		{ QRhiShaderStage::Fragment, argb32Frag },
	});
	_downscaleArgb32Pipeline->setVertexInputLayout(passthroughLayout);
	_downscaleArgb32Pipeline->setTopology(
		QRhiGraphicsPipeline::TriangleStrip);
	_downscaleArgb32Pipeline->setShaderResourceBindings(_downscaleArgb32Srb);
	_downscaleArgb32Pipeline->setRenderPassDescriptor(_offscreenRpDesc);
	_downscaleArgb32Pipeline->create();

	_downscaleYuv420Pipeline = _rhi->newGraphicsPipeline();
	_downscaleYuv420Pipeline->setShaderStages({
		{ QRhiShaderStage::Vertex, passthroughVert },
		{ QRhiShaderStage::Fragment, yuv420Frag },
	});
	_downscaleYuv420Pipeline->setVertexInputLayout(passthroughLayout);
	_downscaleYuv420Pipeline->setTopology(
		QRhiGraphicsPipeline::TriangleStrip);
	_downscaleYuv420Pipeline->setShaderResourceBindings(
		_downscaleYuv420Srb);
	_downscaleYuv420Pipeline->setRenderPassDescriptor(_offscreenRpDesc);
	_downscaleYuv420Pipeline->create();

	_blurHPipeline = _rhi->newGraphicsPipeline();
	_blurHPipeline->setShaderStages({
		{ QRhiShaderStage::Vertex, passthroughVert },
		{ QRhiShaderStage::Fragment, blurHFrag },
	});
	_blurHPipeline->setVertexInputLayout(passthroughLayout);
	_blurHPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
	_blurHPipeline->setShaderResourceBindings(_blurHSrb);
	_blurHPipeline->setRenderPassDescriptor(_offscreenRpDesc);
	_blurHPipeline->create();

	auto *frameSrb = _rhi->newShaderResourceBindings();
	frameSrb->setBindings({
		QRhiShaderResourceBinding::uniformBuffer(
			0,
			QRhiShaderResourceBinding::VertexStage
				| QRhiShaderResourceBinding::FragmentStage,
			_uniformBuffer,
			0,
			sizeof(GroupFrameUniforms)),
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_linearSampler),
		QRhiShaderResourceBinding::sampledTexture(
			2,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_linearSampler),
		QRhiShaderResourceBinding::sampledTexture(
			3,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_noiseRepeatSampler),
	});
	frameSrb->create();
	_perDrawSrbs.push_back(frameSrb);

	_framePipeline = _rhi->newGraphicsPipeline();
	_framePipeline->setShaderStages({
		{ QRhiShaderStage::Vertex, groupFrameVert },
		{ QRhiShaderStage::Fragment, groupFrameFrag },
	});
	_framePipeline->setVertexInputLayout(groupFrameLayout);
	_framePipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
	_framePipeline->setShaderResourceBindings(frameSrb);
	_framePipeline->setRenderPassDescriptor(rpDesc);
	_framePipeline->create();

	auto *controlsSrb = _rhi->newShaderResourceBindings();
	controlsSrb->setBindings({
		QRhiShaderResourceBinding::uniformBuffer(
			0,
			QRhiShaderResourceBinding::VertexStage
				| QRhiShaderResourceBinding::FragmentStage,
			_uniformBuffer,
			0,
			sizeof(ImageUniforms)),
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			_placeholderTexture,
			_linearSampler),
	});
	controlsSrb->create();
	_perDrawSrbs.push_back(controlsSrb);

	QRhiGraphicsPipeline::TargetBlend blend;
	blend.enable = true;
	blend.srcColor = QRhiGraphicsPipeline::One;
	blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
	blend.srcAlpha = QRhiGraphicsPipeline::One;
	blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

	_controlsPipeline = _rhi->newGraphicsPipeline();
	_controlsPipeline->setShaderStages({
		{ QRhiShaderStage::Vertex, argb32Vert },
		{ QRhiShaderStage::Fragment, controlsFrag },
	});
	_controlsPipeline->setVertexInputLayout(argb32Layout);
	_controlsPipeline->setTargetBlends({ blend });
	_controlsPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
	_controlsPipeline->setShaderResourceBindings(controlsSrb);
	_controlsPipeline->setRenderPassDescriptor(rpDesc);
	_controlsPipeline->create();
}

void Viewport::RendererRhi::releaseResources() {
	for (auto *srb : _perDrawSrbs) {
		delete srb;
	}
	_perDrawSrbs.clear();

	for (auto &entry : _texturePool) {
		delete entry.texture;
	}
	_texturePool.clear();

	for (auto &data : _tileData) {
		delete data.rgbaTexture;
		delete data.yTexture;
		delete data.uTexture;
		delete data.vTexture;
		delete data.downscaleTexture;
		delete data.downscaleRpDesc;
		delete data.downscaleRt;
		delete data.blurTexture;
		delete data.blurRpDesc;
		delete data.blurRt;
	}
	_tileData.clear();
	_tileDataIndices.clear();

	delete _downscaleArgb32Pipeline; _downscaleArgb32Pipeline = nullptr;
	delete _downscaleYuv420Pipeline; _downscaleYuv420Pipeline = nullptr;
	delete _blurHPipeline; _blurHPipeline = nullptr;
	delete _framePipeline; _framePipeline = nullptr;
	delete _controlsPipeline; _controlsPipeline = nullptr;

	delete _downscaleArgb32Srb; _downscaleArgb32Srb = nullptr;
	delete _downscaleYuv420Srb; _downscaleYuv420Srb = nullptr;
	delete _blurHSrb; _blurHSrb = nullptr;

	delete _offscreenRpDesc; _offscreenRpDesc = nullptr;
	delete _noiseTexture; _noiseTexture = nullptr;
	delete _offscreenVertexBuffer; _offscreenVertexBuffer = nullptr;
	delete _onscreenVertexBuffer; _onscreenVertexBuffer = nullptr;
	delete _uniformBuffer; _uniformBuffer = nullptr;
	delete _linearSampler; _linearSampler = nullptr;
	delete _nearestSampler; _nearestSampler = nullptr;
	delete _noiseRepeatSampler; _noiseRepeatSampler = nullptr;
	delete _placeholderTexture; _placeholderTexture = nullptr;

	_buttons.destroy();
	_names.destroy();

	_initialized = false;
}

QRhiTexture *Viewport::RendererRhi::acquirePoolTexture(QSize size) {
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

void Viewport::RendererRhi::render(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) {
	_rhi = rhi;
	_rt = rt;
	_cb = cb;
	_nextPoolIndex = 0;

	const auto size = rt->pixelSize();
	const auto factor = style::DevicePixelRatio();
	if (_factor != factor) {
		_factor = factor;
		_ifactor = int(std::ceil(factor));
		_buttons.invalidate();
	}
	_viewport = QSize(
		int(size.width() / _factor),
		int(size.height() / _factor));

	validateDatas();
	ensureNoiseTexture();

	auto index = 0;
	for (const auto &tile : _owner->_tiles) {
		if (!tile->visible()) {
			index++;
			continue;
		}
		paintTile(
			tile.get(),
			_tileData[_tileDataIndices[index++]]);
	}
}

void Viewport::RendererRhi::ensureNoiseTexture() {
	if (_noiseTexture) {
		return;
	}

	auto noiseImage = QImage(
		kNoiseTextureSize,
		kNoiseTextureSize,
		QImage::Format_ARGB32_Premultiplied);
	noiseImage.fill(Qt::transparent);

	auto *rub = _rhi->nextResourceUpdateBatch();

	_noiseTexture = _rhi->newTexture(
		QRhiTexture::RGBA8,
		QSize(kNoiseTextureSize, kNoiseTextureSize));
	_noiseTexture->create();

	// Render noise via the noise.frag shader to an offscreen target,
	// then read back. For simplicity, generate procedurally on CPU.
	auto *noiseData = noiseImage.bits();
	for (int py = 0; py < kNoiseTextureSize; ++py) {
		for (int px = 0; px < kNoiseTextureSize; ++px) {
			const auto noise = float(rand() % 256) / 255.f;
			const auto v = quint8(noise * 255);
			auto *pixel = noiseData
				+ (py * noiseImage.bytesPerLine())
				+ (px * 4);
			pixel[0] = v;
			pixel[1] = v;
			pixel[2] = v;
			pixel[3] = 255;
		}
	}
	rub->uploadTexture(
		_noiseTexture,
		QRhiTextureUploadDescription(
			QRhiTextureUploadEntry(0, 0,
				QRhiTextureSubresourceUploadDescription(noiseImage))));

	_cb->beginPass(_rt, *clearColor(), { 1.0f, 0 }, rub);
	_cb->endPass();
}

void Viewport::RendererRhi::paintTile(
		not_null<VideoTile*> tile,
		TileData &tileData) {
	const auto track = tile->track();
	const auto markGuard = gsl::finally([&] {
		tile->track()->markFrameShown();
	});
	const auto data = track->frameWithInfo(false);
	_userpicFrame = (data.format == Webrtc::FrameFormat::None);
	validateUserpicFrame(tile, tileData);
	const auto frameSize = _userpicFrame
		? tileData.userpicFrame.size()
		: data.yuv420->size;
	const auto frameRotation = _userpicFrame ? 0 : data.rotation;
	Assert(!frameSize.isEmpty());

	_rgbaFrame = (data.format == Webrtc::FrameFormat::ARGB32)
		|| _userpicFrame;

	const auto geometry = tile->geometry().translated(
		_owner->borrowedOrigin());
	const auto unscaled = Media::View::FlipSizeByRotation(
		frameSize,
		frameRotation);

	validateOutlineAnimation(tile, tileData);
	validatePausedAnimation(tile, tileData);

	const auto blurSize = CountBlurredSize(
		unscaled,
		geometry.size(),
		_factor);

	uploadFrame(data, tileData);
	prepareOffscreenTargets(tileData, blurSize);
	drawDownscalePass(tileData, blurSize);
	drawBlurPass(tileData, blurSize);
	drawFramePass(tile, tileData, blurSize);
	drawControls(tile, tileData);
}

void Viewport::RendererRhi::uploadFrame(
		const Webrtc::FrameWithInfo &data,
		TileData &tileData) {
	const auto imageIndex = _userpicFrame ? 0 : (data.index + 1);
	const auto upload = (tileData.trackIndex != imageIndex);
	tileData.trackIndex = imageIndex;
	if (!upload) {
		return;
	}

	auto *rub = _rhi->nextResourceUpdateBatch();

	if (_rgbaFrame) {
		const auto &image = _userpicFrame
			? tileData.userpicFrame
			: data.original;
		if (!tileData.rgbaTexture
			|| tileData.rgbaSize != image.size()) {
			delete tileData.rgbaTexture;
			tileData.rgbaTexture = _rhi->newTexture(
				QRhiTexture::BGRA8, image.size());
			tileData.rgbaTexture->create();
			tileData.rgbaSize = image.size();
		}
		rub->uploadTexture(
			tileData.rgbaTexture,
			QRhiTextureUploadDescription(
				QRhiTextureUploadEntry(0, 0,
					QRhiTextureSubresourceUploadDescription(image))));
	} else {
		const auto yuv = data.yuv420;
		if (!tileData.yTexture || tileData.lumaSize != yuv->size) {
			delete tileData.yTexture;
			tileData.yTexture = _rhi->newTexture(
				QRhiTexture::R8, yuv->size);
			tileData.yTexture->create();
			tileData.lumaSize = yuv->size;
		}
		auto yDesc = QRhiTextureSubresourceUploadDescription(
			yuv->y.data,
			yuv->y.stride * yuv->size.height());
		yDesc.setDataStride(yuv->y.stride);
		rub->uploadTexture(
			tileData.yTexture,
			QRhiTextureUploadDescription(
				QRhiTextureUploadEntry(0, 0, yDesc)));

		if (!tileData.uTexture
			|| tileData.chromaSize != yuv->chromaSize) {
			delete tileData.uTexture;
			tileData.uTexture = _rhi->newTexture(
				QRhiTexture::R8, yuv->chromaSize);
			tileData.uTexture->create();
			delete tileData.vTexture;
			tileData.vTexture = _rhi->newTexture(
				QRhiTexture::R8, yuv->chromaSize);
			tileData.vTexture->create();
			tileData.chromaSize = yuv->chromaSize;
		}
		auto uDesc = QRhiTextureSubresourceUploadDescription(
			yuv->u.data,
			yuv->u.stride * yuv->chromaSize.height());
		uDesc.setDataStride(yuv->u.stride);
		rub->uploadTexture(
			tileData.uTexture,
			QRhiTextureUploadDescription(
				QRhiTextureUploadEntry(0, 0, uDesc)));
		auto vDesc = QRhiTextureSubresourceUploadDescription(
			yuv->v.data,
			yuv->v.stride * yuv->chromaSize.height());
		vDesc.setDataStride(yuv->v.stride);
		rub->uploadTexture(
			tileData.vTexture,
			QRhiTextureUploadDescription(
				QRhiTextureUploadEntry(0, 0, vDesc)));
	}

	_cb->beginPass(_rt, *clearColor(), { 1.0f, 0 }, rub);
	_cb->endPass();
}

void Viewport::RendererRhi::prepareOffscreenTargets(
		TileData &tileData,
		QSize blurSize) {
	if (tileData.blurSize == blurSize
		&& tileData.downscaleRt
		&& tileData.blurRt) {
		return;
	}
	tileData.blurSize = blurSize;

	delete tileData.downscaleRt;
	delete tileData.downscaleRpDesc;
	delete tileData.downscaleTexture;
	tileData.downscaleTexture = _rhi->newTexture(
		QRhiTexture::RGBA8,
		blurSize,
		1,
		QRhiTexture::RenderTarget);
	tileData.downscaleTexture->create();
	auto downscaleColorAtt = QRhiColorAttachment(
		tileData.downscaleTexture);
	tileData.downscaleRt = _rhi->newTextureRenderTarget(
		QRhiTextureRenderTargetDescription(downscaleColorAtt));
	tileData.downscaleRpDesc =
		tileData.downscaleRt->newCompatibleRenderPassDescriptor();
	tileData.downscaleRt->setRenderPassDescriptor(
		tileData.downscaleRpDesc);
	tileData.downscaleRt->create();

	delete tileData.blurRt;
	delete tileData.blurRpDesc;
	delete tileData.blurTexture;
	tileData.blurTexture = _rhi->newTexture(
		QRhiTexture::RGBA8,
		blurSize,
		1,
		QRhiTexture::RenderTarget);
	tileData.blurTexture->create();
	auto blurColorAtt = QRhiColorAttachment(tileData.blurTexture);
	tileData.blurRt = _rhi->newTextureRenderTarget(
		QRhiTextureRenderTargetDescription(blurColorAtt));
	tileData.blurRpDesc =
		tileData.blurRt->newCompatibleRenderPassDescriptor();
	tileData.blurRt->setRenderPassDescriptor(tileData.blurRpDesc);
	tileData.blurRt->create();
}

void Viewport::RendererRhi::drawDownscalePass(
		TileData &tileData,
		QSize blurSize) {
	const float w = float(blurSize.width());
	const float h = float(blurSize.height());
	const float coords[] = {
		-1.f, -1.f, 0.f, 1.f,
		 1.f, -1.f, 1.f, 1.f,
		-1.f,  1.f, 0.f, 0.f,
		 1.f,  1.f, 1.f, 0.f,
	};

	auto *rub = _rhi->nextResourceUpdateBatch();
	rub->updateDynamicBuffer(
		_offscreenVertexBuffer, 0, sizeof(coords), coords);

	auto *srb = _rhi->newShaderResourceBindings();
	_perDrawSrbs.push_back(srb);

	auto *pipeline = _downscaleArgb32Pipeline;
	if (_rgbaFrame) {
		srb->setBindings({
			QRhiShaderResourceBinding::sampledTexture(
				1,
				QRhiShaderResourceBinding::FragmentStage,
				tileData.rgbaTexture,
				_linearSampler),
		});
	} else {
		pipeline = _downscaleYuv420Pipeline;
		srb->setBindings({
			QRhiShaderResourceBinding::sampledTexture(
				1,
				QRhiShaderResourceBinding::FragmentStage,
				tileData.yTexture,
				_linearSampler),
			QRhiShaderResourceBinding::sampledTexture(
				2,
				QRhiShaderResourceBinding::FragmentStage,
				tileData.uTexture,
				_linearSampler),
			QRhiShaderResourceBinding::sampledTexture(
				3,
				QRhiShaderResourceBinding::FragmentStage,
				tileData.vTexture,
				_linearSampler),
		});
	}
	srb->create();

	_cb->beginPass(tileData.downscaleRt, Qt::black, { 1.0f, 0 }, rub);
	_cb->setGraphicsPipeline(pipeline);
	_cb->setShaderResources(srb);
	_cb->setViewport({ 0, 0, w, h });
	const QRhiCommandBuffer::VertexInput vbuf(
		_offscreenVertexBuffer, 0);
	_cb->setVertexInput(0, 1, &vbuf);
	_cb->draw(4);
	_cb->endPass();
}

void Viewport::RendererRhi::drawBlurPass(
		TileData &tileData,
		QSize blurSize) {
	const float w = float(blurSize.width());
	const float h = float(blurSize.height());
	const float coords[] = {
		-1.f, -1.f, 0.f, 1.f,
		 1.f, -1.f, 1.f, 1.f,
		-1.f,  1.f, 0.f, 0.f,
		 1.f,  1.f, 1.f, 0.f,
	};

	BlurUniforms blurUniforms{};
	blurUniforms.texelOffset = 1.f / w;

	auto *rub = _rhi->nextResourceUpdateBatch();
	rub->updateDynamicBuffer(
		_offscreenVertexBuffer, 0, sizeof(coords), coords);
	rub->updateDynamicBuffer(
		_uniformBuffer, 0, sizeof(blurUniforms), &blurUniforms);

	auto *srb = _rhi->newShaderResourceBindings();
	_perDrawSrbs.push_back(srb);
	srb->setBindings({
		QRhiShaderResourceBinding::uniformBuffer(
			0,
			QRhiShaderResourceBinding::FragmentStage,
			_uniformBuffer,
			0,
			sizeof(BlurUniforms)),
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			tileData.downscaleTexture,
			_nearestSampler),
	});
	srb->create();

	_cb->beginPass(tileData.blurRt, Qt::black, { 1.0f, 0 }, rub);
	_cb->setGraphicsPipeline(_blurHPipeline);
	_cb->setShaderResources(srb);
	_cb->setViewport({ 0, 0, w, h });
	const QRhiCommandBuffer::VertexInput vbuf(
		_offscreenVertexBuffer, 0);
	_cb->setVertexInput(0, 1, &vbuf);
	_cb->draw(4);
	_cb->endPass();
}

void Viewport::RendererRhi::drawFramePass(
		not_null<VideoTile*> tile,
		TileData &tileData,
		QSize blurSize) {
	const auto geometry = tile->geometry().translated(
		_owner->borrowedOrigin());
	const auto x = geometry.x();
	const auto y = geometry.y();
	const auto width = geometry.width();
	const auto height = geometry.height();

	const auto data = tile->track()->frameWithInfo(false);
	const auto frameSize = _userpicFrame
		? tileData.userpicFrame.size()
		: data.yuv420->size;
	const auto frameRotation = _userpicFrame ? 0 : data.rotation;
	const auto unscaled = Media::View::FlipSizeByRotation(
		frameSize,
		frameRotation);
	const auto tileSize = geometry.size();
	const auto swap = (((frameRotation / 90) % 2) == 1);
	const auto expand = isExpanded(tile, unscaled, tileSize);
	const auto animation = tile->animation();
	const auto expandRatio = (animation.ratio >= 0.)
		? countExpandRatio(tile, unscaled, animation)
		: expand
		? 1.
		: 0.;

	auto texCoords = CountTexCoords(unscaled, tileSize, expandRatio, swap);
	auto blurTexCoords = (expandRatio == 1. && !swap)
		? texCoords
		: CountTexCoords(unscaled, tileSize, 1.);

	if (tile->mirror()) {
		std::swap(texCoords[0], texCoords[1]);
		std::swap(texCoords[2], texCoords[3]);
		std::swap(blurTexCoords[0], blurTexCoords[1]);
		std::swap(blurTexCoords[2], blurTexCoords[3]);
	}
	if (const auto shift = (frameRotation / 90); shift > 0) {
		std::rotate(
			texCoords.begin(),
			texCoords.begin() + shift,
			texCoords.end());
		std::rotate(
			blurTexCoords.begin(),
			blurTexCoords.begin() + shift,
			blurTexCoords.end());
	}

	const auto outline = tileData.outlined.value(
		tileData.outline ? 1. : 0.);
	const auto paused = tileData.paused.value(
		tileData.pause ? 1. : 0.);
	const auto &st = st::groupCallVideoTile;
	const auto fullscreen = _owner->_fullscreen;
	const auto shown = _owner->_controlsShownRatio;

	const auto pw = float(_rt->pixelSize().width());
	const auto ph = float(_rt->pixelSize().height());

	const auto rect = transformRect(geometry);

	// For group_frame.vert: position in pixels, v_texcoord, b_texcoord
	// Vertex layout: pos.x, pos.y, vtc.x, vtc.y, btc.x, btc.y
	// Triangle strip: BL, BR, TL, TR
	const float frameCoords[] = {
		float(x) * _factor, float(y + height) * _factor,
		texCoords[0][0], texCoords[0][1],
		blurTexCoords[0][0], blurTexCoords[0][1],

		float(x + width) * _factor, float(y + height) * _factor,
		texCoords[1][0], texCoords[1][1],
		blurTexCoords[1][0], blurTexCoords[1][1],

		float(x) * _factor, float(y) * _factor,
		texCoords[2][0], texCoords[2][1],
		blurTexCoords[2][0], blurTexCoords[2][1],

		float(x + width) * _factor, float(y) * _factor,
		texCoords[3][0], texCoords[3][1],
		blurTexCoords[3][0], blurTexCoords[3][1],
	};

	GroupFrameUniforms uniforms{};
	uniforms.viewport[0] = pw;
	uniforms.viewport[1] = ph;

	const auto bg = fullscreen ? QColor(0, 0, 0) : rhiClearColor();
	uniforms.frameBg[0] = bg.redF();
	uniforms.frameBg[1] = bg.greenF();
	uniforms.frameBg[2] = bg.blueF();
	uniforms.frameBg[3] = bg.alphaF();

	const auto shadowHeight = st.shadowHeight * _factor;
	const auto shadowAlpha = Viewport::kShadowMaxAlpha / 255.f;
	uniforms.shadow[0] = shadowHeight;
	uniforms.shadow[1] = shown;
	uniforms.shadow[2] = shadowAlpha;
	uniforms.shadow[3] = fullscreen ? 0.f : kBlurOpacity;

	uniforms.paused = float(paused);

	uniforms.roundRect[0] = float(x) * _factor;
	uniforms.roundRect[1] = float(y) * _factor;
	uniforms.roundRect[2] = float(width) * _factor;
	uniforms.roundRect[3] = float(height) * _factor;

	const auto radius = _owner->videoStream()
		? st::storiesRadius
		: st::roundRadiusLarge;
	uniforms.radiusOutline[0] = float(
		radius * _factor * (fullscreen ? 0. : 1.));
	uniforms.radiusOutline[1] = (outline > 0)
		? float(st::groupCallOutline * _factor)
		: 0.f;

	const auto roundBg = rhiClearColor();
	uniforms.roundBg[0] = roundBg.redF();
	uniforms.roundBg[1] = roundBg.greenF();
	uniforms.roundBg[2] = roundBg.blueF();
	uniforms.roundBg[3] = roundBg.alphaF();

	uniforms.outlineFg[0] = st::groupCallMemberActiveIcon->c.redF();
	uniforms.outlineFg[1] = st::groupCallMemberActiveIcon->c.greenF();
	uniforms.outlineFg[2] = st::groupCallMemberActiveIcon->c.blueF();
	uniforms.outlineFg[3] = st::groupCallMemberActiveIcon->c.alphaF()
		* outline;

	auto *rub = _rhi->nextResourceUpdateBatch();
	rub->updateDynamicBuffer(
		_onscreenVertexBuffer, 0, sizeof(frameCoords), frameCoords);
	rub->updateDynamicBuffer(
		_uniformBuffer, 0, sizeof(uniforms), &uniforms);

	auto *srb = _rhi->newShaderResourceBindings();
	_perDrawSrbs.push_back(srb);

	auto *sTex = _rgbaFrame
		? tileData.rgbaTexture
		: tileData.yTexture;
	srb->setBindings({
		QRhiShaderResourceBinding::uniformBuffer(
			0,
			QRhiShaderResourceBinding::VertexStage
				| QRhiShaderResourceBinding::FragmentStage,
			_uniformBuffer,
			0,
			sizeof(GroupFrameUniforms)),
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			sTex ? sTex : _placeholderTexture,
			_linearSampler),
		QRhiShaderResourceBinding::sampledTexture(
			2,
			QRhiShaderResourceBinding::FragmentStage,
			tileData.blurTexture
				? tileData.blurTexture
				: _placeholderTexture,
			_linearSampler),
		QRhiShaderResourceBinding::sampledTexture(
			3,
			QRhiShaderResourceBinding::FragmentStage,
			_noiseTexture ? _noiseTexture : _placeholderTexture,
			_noiseRepeatSampler),
	});
	srb->create();

	_cb->beginPass(_rt, *clearColor(), { 1.0f, 0 }, rub);
	_cb->setGraphicsPipeline(_framePipeline);
	_cb->setShaderResources(srb);
	_cb->setViewport({ 0, 0, pw, ph });
	const QRhiCommandBuffer::VertexInput vbuf(
		_onscreenVertexBuffer, 0);
	_cb->setVertexInput(0, 1, &vbuf);
	_cb->draw(4);
	_cb->endPass();
}

void Viewport::RendererRhi::drawControls(
		not_null<VideoTile*> tile,
		TileData &tileData) {
	const auto geometry = tile->geometry().translated(
		_owner->borrowedOrigin());
	const auto x = geometry.x();
	const auto y = geometry.y();
	const auto width = geometry.width();
	const auto height = geometry.height();
	const auto &st = st::groupCallVideoTile;
	const auto shown = _owner->_controlsShownRatio;
	const auto fullNameShift = st.namePosition.y() + st::normalFont->height;
	const auto nameShift = anim::interpolate(fullNameShift, 0, shown);
	const auto row = tile->row();
	const auto paused = tileData.paused.value(
		tileData.pause ? 1. : 0.);

	const auto nameTop = y + (height
		- st.namePosition.y()
		- st::semiboldFont->height);
	const auto pinVisible = _owner->wide()
		&& (tile->pinInner().translated(x, y).bottom() > y);
	const auto nameVisible = (nameShift != fullNameShift);
	const auto pausedVisible = (paused > 0.);

	if (!nameVisible && !pinVisible && !pausedVisible) {
		return;
	}

	ensureButtonsImage();
	row->lazyInitialize(st::groupCallMembersListItem);

	auto drawRasterOverlay = [&](
			QRect rect,
			Fn<void(Painter&)> method,
			float opacity) {
		paintUsingRaster(rect, std::move(method), opacity);
	};

	if (pausedVisible) {
		const auto middle = (st::groupCallVideoPlaceholderHeight
			- st::groupCallPaused.height()) / 2;
		const auto pausedSpace = (nameTop - y)
			- st::groupCallPaused.height()
			- st::semiboldFont->height;
		const auto pauseIconSkip = middle
			- st::groupCallVideoPlaceholderIconTop;
		const auto pauseTextSkip = st::groupCallVideoPlaceholderTextTop
			- st::groupCallVideoPlaceholderIconTop;
		const auto pauseIconTop = !_owner->wide()
			? (y + (height - st::groupCallPaused.height()) / 2)
			: (pausedSpace < 3 * st::semiboldFont->height)
			? (pausedSpace / 3)
			: std::min(
				y + (height / 2) - pauseIconSkip,
				(nameTop
					- st::semiboldFont->height * 3
					- st::groupCallPaused.height()));

		const auto iconRect = QRect(
			x + (width - st::groupCallPaused.width()) / 2,
			pauseIconTop,
			st::groupCallPaused.width(),
			st::groupCallPaused.height());
		drawRasterOverlay(iconRect, [&](Painter &p) {
			st::groupCallPaused.paint(
				p,
				iconRect.x(),
				iconRect.y(),
				width);
		}, float(paused));

		if (_owner->wide()) {
			const auto pauseTextTop =
				(pausedSpace < 3 * st::semiboldFont->height)
				? (nameTop - (pausedSpace / 3) - st::semiboldFont->height)
				: std::min(
					pauseIconTop + pauseTextSkip,
					nameTop - st::semiboldFont->height * 2);
			const auto pausedText =
				tr::lng_group_call_video_paused(tr::now);
			const auto textWidth =
				st::semiboldFont->width(pausedText);
			const auto textRect = QRect(
				x + (width - textWidth) / 2,
				pauseTextTop,
				textWidth,
				st::semiboldFont->height);
			drawRasterOverlay(textRect, [&](Painter &p) {
				p.setPen(st::groupCallVideoTextFg);
				p.setFont(st::semiboldFont);
				p.drawText(
					textRect,
					pausedText,
					style::al_top);
			}, float(paused));
		}
	}

	if (pinVisible) {
		const auto pinInner = tile->pinInner();
		const auto pinRect = pinInner.translated(x, y);
		drawRasterOverlay(pinRect, [&](Painter &p) {
			auto hq = PainterHighQualityEnabler(p);
			VideoTile::PaintPinButton(
				p,
				tile->pinned(),
				pinRect.x(),
				pinRect.y(),
				_owner->widget()->width(),
				&_pinBackground,
				&_pinIcon);
		}, 1.f);

		const auto backInner = tile->backInner();
		const auto backRect = backInner.translated(x, y);
		drawRasterOverlay(backRect, [&](Painter &p) {
			auto hq = PainterHighQualityEnabler(p);
			VideoTile::PaintBackButton(
				p,
				backRect.x(),
				backRect.y(),
				_owner->widget()->width(),
				&_pinBackground);
		}, 1.f);
	}

	if (nameVisible) {
		const auto &icon = st::groupCallVideoCrossLine.icon;
		const auto iconLeft = x + width
			- st.iconPosition.x() - icon.width();
		const auto iconTop = y + (height
			- st.iconPosition.y()
			- icon.height()
			+ nameShift);
		const auto muteRect = QRect(
			iconLeft,
			iconTop,
			icon.width(),
			icon.height());
		if (!muteRect.isEmpty()) {
			drawRasterOverlay(muteRect, [&](Painter &p) {
				row->paintMuteIcon(
					p,
					muteRect,
					MembersRowStyle::Video);
			}, 1.f);
		}

		const auto hasWidth = width
			- st.iconPosition.x() - icon.width()
			- st.namePosition.x();
		const auto nameLeft = x + st.namePosition.x();
		const auto nameRect = QRect(
			nameLeft,
			nameTop + nameShift,
			hasWidth,
			st::semiboldFont->height);
		if (!nameRect.isEmpty()) {
			drawRasterOverlay(nameRect, [&](Painter &p) {
				p.setPen(st::groupCallVideoTextFg);
				row->name().drawLeftElided(
					p,
					nameRect.x(),
					nameRect.y(),
					nameRect.width(),
					nameRect.x() + nameRect.width());
			}, 1.f);
		}
	}
}

void Viewport::RendererRhi::paintUsingRaster(
		QRect rect,
		Fn<void(Painter&)> method,
		float opacity) {
	if (!_controlsPipeline || rect.isEmpty()) {
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

	auto *rub = _rhi->nextResourceUpdateBatch();

	auto *tex = acquirePoolTexture(size);
	rub->uploadTexture(
		tex,
		QRhiTextureUploadDescription(
			QRhiTextureUploadEntry(0, 0,
				QRhiTextureSubresourceUploadDescription(raster))));

	ImageUniforms imgUniforms{};
	const auto pw = float(_rt->pixelSize().width());
	const auto ph = float(_rt->pixelSize().height());
	imgUniforms.viewport[0] = pw;
	imgUniforms.viewport[1] = ph;
	imgUniforms.g_opacity = opacity;

	rub->updateDynamicBuffer(
		_uniformBuffer, 0, sizeof(imgUniforms), &imgUniforms);

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
	rub->updateDynamicBuffer(
		_onscreenVertexBuffer, 0, sizeof(coords), coords);

	auto *srb = _rhi->newShaderResourceBindings();
	_perDrawSrbs.push_back(srb);
	srb->setBindings({
		QRhiShaderResourceBinding::uniformBuffer(
			0,
			QRhiShaderResourceBinding::VertexStage
				| QRhiShaderResourceBinding::FragmentStage,
			_uniformBuffer,
			0,
			sizeof(ImageUniforms)),
		QRhiShaderResourceBinding::sampledTexture(
			1,
			QRhiShaderResourceBinding::FragmentStage,
			tex,
			_linearSampler),
	});
	srb->create();

	_cb->beginPass(_rt, *clearColor(), { 1.0f, 0 }, rub);
	_cb->setGraphicsPipeline(_controlsPipeline);
	_cb->setShaderResources(srb);
	_cb->setViewport({ 0, 0, pw, ph });
	const QRhiCommandBuffer::VertexInput vbuf(
		_onscreenVertexBuffer, 0);
	_cb->setVertexInput(0, 1, &vbuf);
	_cb->draw(4);
	_cb->endPass();
}

Rect Viewport::RendererRhi::transformRect(const QRect &raster) const {
	return TransformRect(Rect(raster), _viewport, _factor);
}

Rect Viewport::RendererRhi::transformRect(const Rect &raster) const {
	return TransformRect(raster, _viewport, _factor);
}

bool Viewport::RendererRhi::isExpanded(
		not_null<VideoTile*> tile,
		QSize unscaled,
		QSize tileSize) const {
	return !tile->screencast()
		&& (!_owner->wide() || UseExpandForCamera(unscaled, tileSize));
}

float64 Viewport::RendererRhi::countExpandRatio(
		not_null<VideoTile*> tile,
		QSize unscaled,
		const TileAnimation &animation) const {
	const auto expandedFrom = isExpanded(tile, unscaled, animation.from);
	const auto expandedTo = isExpanded(tile, unscaled, animation.to);
	return (expandedFrom && expandedTo)
		? 1.
		: (!expandedFrom && !expandedTo)
		? 0.
		: expandedFrom
		? (1. - animation.ratio)
		: animation.ratio;
}

void Viewport::RendererRhi::ensureButtonsImage() {
	if (_buttons) {
		return;
	}
	const auto pinOnSize = VideoTile::PinInnerSize(true);
	const auto pinOffSize = VideoTile::PinInnerSize(false);
	const auto backSize = VideoTile::BackInnerSize();
	const auto muteSize = st::groupCallVideoCrossLine.icon.size();
	const auto pausedSize = st::groupCallPaused.size();

	const auto fullSize = QSize(
		std::max({
			pinOnSize.width(),
			pinOffSize.width(),
			backSize.width(),
			2 * muteSize.width(),
			pausedSize.width(),
		}),
		(pinOnSize.height()
			+ pinOffSize.height()
			+ backSize.height()
			+ muteSize.height()
			+ pausedSize.height()));
	const auto imageSize = fullSize * _ifactor;
	auto image = _buttons.takeImage();
	if (image.size() != imageSize) {
		image = QImage(imageSize, QImage::Format_ARGB32_Premultiplied);
	}
	image.fill(Qt::transparent);
	image.setDevicePixelRatio(_ifactor);
	{
		auto p = Painter(&image);
		auto hq = PainterHighQualityEnabler(p);

		_pinOn = QRect(QPoint(), pinOnSize * _ifactor);
		VideoTile::PaintPinButton(
			p, true, 0, 0, fullSize.width(),
			&_pinBackground, &_pinIcon);

		const auto pinOffTop = pinOnSize.height();
		_pinOff = QRect(
			QPoint(0, pinOffTop) * _ifactor,
			pinOffSize * _ifactor);
		VideoTile::PaintPinButton(
			p, false, 0, pinOnSize.height(), fullSize.width(),
			&_pinBackground, &_pinIcon);

		const auto backTop = pinOffTop + pinOffSize.height();
		_back = QRect(QPoint(0, backTop) * _ifactor, backSize * _ifactor);
		VideoTile::PaintBackButton(
			p, 0, backTop, fullSize.width(), &_pinBackground);

		const auto muteTop = backTop + backSize.height();
		_muteOn = QRect(
			QPoint(0, muteTop) * _ifactor, muteSize * _ifactor);
		_muteIcon.paint(p, { 0, muteTop }, 1.);

		_muteOff = QRect(
			QPoint(muteSize.width(), muteTop) * _ifactor,
			muteSize * _ifactor);
		_muteIcon.paint(p, { muteSize.width(), muteTop }, 0.);

		const auto pausedTop = muteTop + muteSize.height();
		_pausedIcon = QRect(
			QPoint(0, pausedTop) * _ifactor,
			pausedSize * _ifactor);
		st::groupCallPaused.paint(p, 0, pausedTop, fullSize.width());
	}
	_buttons.setImage(std::move(image));
}

void Viewport::RendererRhi::validateDatas() {
	const auto &tiles = _owner->_tiles;
	const auto count = int(tiles.size());

	for (auto &data : _tileData) {
		data.stale = true;
	}
	_tileDataIndices.resize(count);
	for (auto i = 0; i != count; ++i) {
		tiles[i]->row()->lazyInitialize(st::groupCallMembersListItem);
		const auto id = quintptr(tiles[i]->track().get());
		const auto j = ranges::find(_tileData, id, &TileData::id);
		if (j != end(_tileData)) {
			j->stale = false;
			_tileDataIndices[i] = int(j - begin(_tileData));
			const auto peer = tiles[i]->peer();
			if (j->peer != peer
				|| j->nameVersion != peer->nameVersion()) {
				j->peer = peer;
				j->nameVersion = peer->nameVersion();
			}
		} else {
			const auto peer = tiles[i]->peer();
			const auto paused = (tiles[i]->track()->state()
				== Webrtc::VideoState::Paused);
			_tileDataIndices[i] = int(_tileData.size());
			_tileData.push_back({
				.id = id,
				.peer = peer,
				.nameVersion = peer->nameVersion(),
				.pause = paused,
			});
		}
	}
	for (auto it = _tileData.begin(); it != _tileData.end();) {
		if (it->stale) {
			delete it->rgbaTexture;
			delete it->yTexture;
			delete it->uTexture;
			delete it->vTexture;
			delete it->downscaleTexture;
			delete it->downscaleRpDesc;
			delete it->downscaleRt;
			delete it->blurTexture;
			delete it->blurRpDesc;
			delete it->blurRt;
			it = _tileData.erase(it);
		} else {
			++it;
		}
	}
}

void Viewport::RendererRhi::validateOutlineAnimation(
		not_null<VideoTile*> tile,
		TileData &data) {
	const auto outline = tile->row()->speaking();
	if (data.outline == outline) {
		return;
	}
	data.outline = outline;
	data.outlined.start(
		[=] { _owner->widget()->update(); },
		outline ? 0. : 1.,
		outline ? 1. : 0.,
		st::fadeWrapDuration);
}

void Viewport::RendererRhi::validatePausedAnimation(
		not_null<VideoTile*> tile,
		TileData &data) {
	const auto paused = (_userpicFrame
		&& tile->track()->frameSize().isEmpty())
		|| (tile->track()->state() == Webrtc::VideoState::Paused);
	if (data.pause == paused) {
		return;
	}
	data.pause = paused;
	data.paused.start(
		[=] { _owner->widget()->update(); },
		paused ? 0. : 1.,
		paused ? 1. : 0.,
		st::fadeWrapDuration);
}

void Viewport::RendererRhi::validateUserpicFrame(
		not_null<VideoTile*> tile,
		TileData &data) {
	if (!_userpicFrame) {
		data.userpicFrame = QImage();
		return;
	} else if (!data.userpicFrame.isNull()) {
		return;
	}
	const auto size = tile->trackOrUserpicSize();
	data.userpicFrame = PeerData::GenerateUserpicImage(
		tile->peer(),
		tile->row()->ensureUserpicView(),
		size.width(),
		0);
}

} // namespace Calls::Group

#endif // Qt >= 6.7
