/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/effects/premium_3d_support.h"

#include "ui/gl/gl_detection.h"
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

constexpr auto kBezierIterations = 40;
constexpr auto kBezierEpsilon = 0.00001;

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
[[nodiscard]] bool ProbeGraphicsSupport() {
	auto rhi = std::unique_ptr<QRhi>();
#ifdef Q_OS_MAC
	if (::Platform::MetalSupported()) {
		auto params = QRhiMetalInitParams();
		rhi.reset(QRhi::create(QRhi::Metal, &params));
	}
	if (!rhi) {
		LOG(("Premium3d: probe failed — no Metal RHI"));
		return false;
	}
#elif defined(Q_OS_WIN)
	auto params = QRhiD3D11InitParams();
	rhi.reset(QRhi::create(QRhi::D3D11, &params));
	if (!rhi) {
		LOG(("Premium3d: probe failed — no D3D11 RHI"));
		return false;
	}
#else
	auto format = QSurfaceFormat::defaultFormat();
	auto offscreen = std::unique_ptr<QOffscreenSurface>(
		QRhiGles2InitParams::newFallbackSurface(format));
	if (!offscreen) {
		LOG(("Premium3d: probe failed — no offscreen surface"));
		return false;
	}
	auto params = QRhiGles2InitParams();
	params.format = format;
	params.fallbackSurface = offscreen.get();
	rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
	if (!rhi) {
		LOG(("Premium3d: probe failed — no GL RHI"));
		return false;
	}
#endif
	LOG(("Premium3d: probe backend=%1 device=%2"
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

float64 CubicBezier(
		float64 x1,
		float64 y1,
		float64 x2,
		float64 y2,
		float64 x) {
	const auto curve = [](float64 t, float64 a, float64 b) {
		return (((1. - 3. * b + 3. * a) * t
			+ (3. * b - 6. * a)) * t
			+ (3. * a)) * t;
	};
	if (x <= 0.) {
		return 0.;
	} else if (x >= 1.) {
		return 1.;
	}
	auto start = 0.;
	auto end = 1.;
	auto t = x;
	for (auto i = 0; i != kBezierIterations; ++i) {
		t = (start + end) / 2.;
		const auto estimate = curve(t, x1, x2);
		if (std::abs(x - estimate) < kBezierEpsilon) {
			break;
		} else if (estimate < x) {
			start = t;
		} else {
			end = t;
		}
	}
	return curve(t, y1, y2);
}

bool Object3dSupported() {
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

} // namespace Ui::Premium
