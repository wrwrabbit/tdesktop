/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/effects/thanos_effect.h"

#include "ui/effects/thanos_effect_renderer.h"
#include "ui/gl/gl_surface.h"
#include "ui/power_saving.h"
#include "ui/rp_widget.h"
#include "base/debug_log.h"

#include <QTimer>

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#include <rhi/qrhi.h>
#endif

namespace Ui {

bool ThanosEffect::Supported() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	return !PowerSaving::On(PowerSaving::kChatEffects);
#else
	return false;
#endif
}

ThanosEffect::ThanosEffect(not_null<RpWidget*> parent)
: _parent(parent) {
}

ThanosEffect::~ThanosEffect() {
	stopUpdateTimer();
}

void ThanosEffect::ensureSurface() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	if (_surface) {
		return;
	}

	auto renderer = std::make_unique<ThanosEffectRenderer>();
	_renderer = renderer.get();

	_renderer->allDone() | rpl::on_next([=] {
		stopUpdateTimer();
		_allDone.fire({});
	}, _lifetime);

	_surface = GL::CreateSurface(
		_parent,
		GL::ChosenRenderer{
			.renderer = std::move(renderer),
			.backend = GL::Backend::QRhi,
		});

	if (const auto w = _surface ? _surface->rpWidget() : nullptr) {
		w->setGeometry(_parent->rect());
		w->show();
		w->raise();
	}
#endif
}

void ThanosEffect::addItem(QImage snapshot, QRect rect) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	ensureSurface();
	if (!_renderer) {
		return;
	}

	_renderer->addItem({
		.snapshot = std::move(snapshot),
		.rect = QRectF(rect),
	});

	startUpdateTimer();
#endif
}

bool ThanosEffect::animating() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	return _renderer && _renderer->hasActiveItems();
#else
	return false;
#endif
}

rpl::producer<> ThanosEffect::allDone() const {
	return _allDone.events();
}

void ThanosEffect::setGeometry(QRect rect) {
	if (const auto w = _surface ? _surface->rpWidget() : nullptr) {
		w->setGeometry(rect);
	}
}

void ThanosEffect::raise() {
	if (const auto w = _surface ? _surface->rpWidget() : nullptr) {
		w->raise();
	}
}

void ThanosEffect::startUpdateTimer() {
	if (_updateTimer) {
		return;
	}
	if (const auto w = _surface ? _surface->rpWidget() : nullptr) {
		_updateTimer = new QTimer(w);
		_updateTimer->setInterval(16);
		QObject::connect(_updateTimer, &QTimer::timeout, w, [w] {
			w->update();
		});
		_updateTimer->start();
	}
}

void ThanosEffect::stopUpdateTimer() {
	if (_updateTimer) {
		_updateTimer->stop();
		delete _updateTimer;
		_updateTimer = nullptr;
	}
}

} // namespace Ui
