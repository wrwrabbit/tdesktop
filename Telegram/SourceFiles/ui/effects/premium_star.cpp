/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/effects/premium_star.h"

#include "ui/effects/premium_star_renderer.h"
#include "ui/gl/gl_detection.h"
#include "ui/gl/gl_surface.h"
#include "ui/power_saving.h"
#include "base/debug_log.h"
#include "base/platform/base_platform_info.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#include <rhi/qrhi.h>
#if !defined(Q_OS_MAC) && !defined(Q_OS_WIN)
#include <QtGui/QOffscreenSurface>
#include <QtGui/QSurfaceFormat>
#endif // !Q_OS_MAC && !Q_OS_WIN
#endif // Qt >= 6.7

namespace Ui::Premium {
namespace {

constexpr auto kShimmerSpeed = 0.03;
constexpr auto kFadeInDuration = 0.22;
constexpr auto kIdleYawSpeed = 18.;
constexpr auto kDragYaw = 0.5;
constexpr auto kDragPitch = 0.3;
constexpr auto kMaxPitch = 50.;
constexpr auto kInertiaTau = 0.5;
constexpr auto kPitchReturnTau = 0.6;
constexpr auto kMaxDragVelocity = 720.;
constexpr auto kMaxFrameDelta = crl::time(66);
constexpr auto kMsInSecond = 1000.;

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
[[nodiscard]] bool ProbeGraphicsSupport() {
	auto rhi = std::unique_ptr<QRhi>();
#ifdef Q_OS_MAC
	if (::Platform::MetalSupported()) {
		auto params = QRhiMetalInitParams();
		rhi.reset(QRhi::create(QRhi::Metal, &params));
	}
	if (!rhi) {
		LOG(("PremiumStar: probe failed — no Metal RHI"));
		return false;
	}
#elif defined(Q_OS_WIN)
	auto params = QRhiD3D11InitParams();
	rhi.reset(QRhi::create(QRhi::D3D11, &params));
	if (!rhi) {
		LOG(("PremiumStar: probe failed — no D3D11 RHI"));
		return false;
	}
#else
	auto format = QSurfaceFormat::defaultFormat();
	auto offscreen = std::unique_ptr<QOffscreenSurface>(
		QRhiGles2InitParams::newFallbackSurface(format));
	if (!offscreen) {
		LOG(("PremiumStar: probe failed — no offscreen surface"));
		return false;
	}
	auto params = QRhiGles2InitParams();
	params.format = format;
	params.fallbackSurface = offscreen.get();
	rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
	if (!rhi) {
		LOG(("PremiumStar: probe failed — no GL RHI"));
		return false;
	}
#endif
	LOG(("PremiumStar: probe backend=%1 device=%2"
		).arg(rhi->backendName()
		).arg(rhi->driverInfo().deviceName));
	rhi.reset();
	return true;
}

[[nodiscard]] bool RhiGraphicsSupportedCached() {
	static const auto cached = ProbeGraphicsSupport();
	return cached;
}
#endif // Qt >= 6.7

} // namespace

Star::Star(QWidget *parent)
: RpWidget(parent)
, _animation([=] { frame(); }) {
}

Star::~Star() = default;

bool Star::Supported() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	if (PowerSaving::On(PowerSaving::kAnimations)) {
		return false;
	}
	if (!GL::WidgetsRhiEnabled()) {
		return false;
	}
	return RhiGraphicsSupportedCached();
#else
	return false;
#endif
}

void Star::setColors(QColor gradient1, QColor gradient2) {
	_gradient1 = gradient1;
	_gradient2 = gradient2;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	if (_renderer) {
		_renderer->setColors(_gradient1, _gradient2);
	}
#endif
}

void Star::setShownProgress(float64 progress) {
	progress = std::clamp(progress, 0., 1.);
	if (_opacity == progress) {
		return;
	}
	_opacity = progress;
	if (!_animation.animating()) {
		pushState();
		if (const auto w = surfaceWidget()) {
			w->update();
		}
	}
}

void Star::setPaused(bool paused) {
	if (_paused == paused) {
		return;
	}
	_paused = paused;
	if (_paused) {
		stopAnimation();
	} else if (!isHidden()) {
		startAnimation();
	}
}

QWidget *Star::surfaceWidget() const {
	return _surface ? _surface->rpWidget() : nullptr;
}

void Star::ensureSurface() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	if (_surface) {
		return;
	}
	auto renderer = std::make_unique<StarRenderer>();
	_renderer = renderer.get();
	if (_gradient1.isValid() && _gradient2.isValid()) {
		_renderer->setColors(_gradient1, _gradient2);
	}
	_surface = GL::CreateSurface(
		this,
		GL::ChosenRenderer{
			.renderer = std::move(renderer),
			.backend = GL::Backend::QRhi,
		});
	if (const auto w = surfaceWidget()) {
		w->setAttribute(Qt::WA_TransparentForMouseEvents);
		w->setAttribute(Qt::WA_AlwaysStackOnTop);
		w->setGeometry(rect());
		w->show();
	}
#endif // Qt >= 6.7
}

void Star::startAnimation() {
	if (_paused || _animation.animating()) {
		return;
	}
	_lastFrame = crl::now();
	_animation.start();
}

void Star::stopAnimation() {
	_animation.stop();
}

void Star::advance(float64 dt) {
	_shimmer += dt * kShimmerSpeed;
	_shimmer -= std::floor(_shimmer);
	if (_fadeIn < 1.) {
		_fadeIn = std::min(1., _fadeIn + dt / kFadeInDuration);
	}
	if (!_dragging) {
		_yaw += _yawVelocity * dt;
		_pitch += _pitchVelocity * dt;
		const auto decay = std::exp(-dt / kInertiaTau);
		_yawVelocity *= decay;
		_pitchVelocity *= decay;
		_yaw += kIdleYawSpeed * dt;
		_pitch *= std::exp(-dt / kPitchReturnTau);
		_pitch = std::clamp(_pitch, -kMaxPitch, kMaxPitch);
	}
	_yaw -= 360. * std::floor(_yaw / 360.);
}

void Star::pushState() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	if (_renderer) {
		_renderer->setState({
			.yaw = float(_yaw),
			.pitch = float(_pitch),
			.shimmer = float(_shimmer),
			.alpha = float(_fadeIn * _opacity),
		});
	}
#endif // Qt >= 6.7
}

void Star::frame() {
	const auto now = crl::now();
	const auto delta = std::clamp(
		now - _lastFrame,
		crl::time(1),
		kMaxFrameDelta);
	_lastFrame = now;
	advance(delta / kMsInSecond);
	pushState();
	if (const auto w = surfaceWidget()) {
		w->update();
	}
}

void Star::resizeEvent(QResizeEvent *e) {
	if (const auto w = surfaceWidget()) {
		w->setGeometry(rect());
	}
}

void Star::showEvent(QShowEvent *e) {
	ensureSurface();
	startAnimation();
}

void Star::hideEvent(QHideEvent *e) {
	stopAnimation();
}

void Star::mousePressEvent(QMouseEvent *e) {
	_dragging = true;
	_lastDragPos = e->position().toPoint();
	_lastDragTime = crl::now();
	_yawVelocity = _pitchVelocity = 0.;
	_dragYawVelocity = _dragPitchVelocity = 0.;
}

void Star::mouseMoveEvent(QMouseEvent *e) {
	if (!_dragging) {
		return;
	}
	const auto now = crl::now();
	const auto pos = e->position().toPoint();
	const auto shift = pos - _lastDragPos;
	const auto dt = std::max(crl::time(1), now - _lastDragTime) / kMsInSecond;
	const auto yawShift = -shift.x() * kDragYaw;
	const auto pitchShift = -shift.y() * kDragPitch;
	_yaw += yawShift;
	_pitch = std::clamp(_pitch + pitchShift, -kMaxPitch, kMaxPitch);
	_dragYawVelocity = yawShift / dt;
	_dragPitchVelocity = pitchShift / dt;
	_lastDragPos = pos;
	_lastDragTime = now;
}

void Star::mouseReleaseEvent(QMouseEvent *e) {
	if (!_dragging) {
		return;
	}
	_dragging = false;
	_yawVelocity = std::clamp(
		_dragYawVelocity,
		-kMaxDragVelocity,
		kMaxDragVelocity);
	_pitchVelocity = std::clamp(
		_dragPitchVelocity,
		-kMaxDragVelocity,
		kMaxDragVelocity);
}

} // namespace Ui::Premium
