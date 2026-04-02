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

ThanosEffect::ThanosEffect(not_null<QWidget*> parent)
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
		crl::on_main(_parent, [=] {
			hideSurface();
			_allDone.fire({});
		});
	}, _lifetime);

	_surface = GL::CreateSurface(
		_parent,
		GL::ChosenRenderer{
			.renderer = std::move(renderer),
			.backend = GL::Backend::QRhi,
		});

	if (const auto w = _surface ? _surface->rpWidget() : nullptr) {
		w->setAttribute(Qt::WA_TransparentForMouseEvents);
		w->setAttribute(Qt::WA_AlwaysStackOnTop);
		w->setGeometry(_parent->rect());
		w->hide();
	}
#endif
}

void ThanosEffect::showSurface() {
	if (const auto w = _surface ? _surface->rpWidget() : nullptr) {
		const auto r = _parent->rect();
		LOG(("ThanosEffect: showSurface, parent rect=%1,%2 %3x%4")
			.arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()));
		w->setGeometry(r);
		w->show();
		w->raise();
		startUpdateTimer();
	} else {
		LOG(("ThanosEffect: showSurface FAILED, no widget"));
	}
}

void ThanosEffect::hideSurface() {
	stopUpdateTimer();
	if (const auto w = _surface ? _surface->rpWidget() : nullptr) {
		w->hide();
	}
}

void ThanosEffect::addItem(QImage snapshot, QRect rect) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	ensureSurface();
	if (!_renderer) {
		return;
	}

	const auto wasAnimating = _renderer->hasActiveItems();

	_renderer->addItem({
		.snapshot = std::move(snapshot),
		.rect = QRectF(rect),
	});

	if (!wasAnimating) {
		showSurface();
	}
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
		if (w->isVisible()) {
			w->setGeometry(rect);
		}
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
