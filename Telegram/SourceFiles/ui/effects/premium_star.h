/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"

#include <QtGui/QColor>

namespace Ui::Premium {

class StarRenderer;

class Star final : public RpWidget {
public:
	explicit Star(QWidget *parent);
	~Star();

	[[nodiscard]] static bool Supported();

	void setColors(QColor gradient1, QColor gradient2);
	void setShownProgress(float64 progress);
	void setPaused(bool paused);

	[[nodiscard]] rpl::producer<float64> flungStrength() const;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	void ensureSurface();
	[[nodiscard]] QWidget *surfaceWidget() const;
	void startAnimation();
	void stopAnimation();
	void frame();
	void advance(float64 dt);
	void pushState();

	StarRenderer *_renderer = nullptr;
	std::unique_ptr<RpWidgetWrap> _surface;
	Ui::Animations::Basic _animation;
	crl::time _lastFrame = 0;

	QColor _gradient1;
	QColor _gradient2;

	float64 _yaw = 0.;
	float64 _pitch = 0.;
	float64 _yawVelocity = 0.;
	float64 _pitchVelocity = 0.;
	float64 _shimmer = 0.;
	float64 _fadeIn = 0.;
	float64 _opacity = 1.;

	bool _dragging = false;
	QPoint _lastDragPos;
	crl::time _lastDragTime = 0;
	float64 _dragYawVelocity = 0.;
	float64 _dragPitchVelocity = 0.;
	float64 _dragYawTotal = 0.;
	float64 _dragPitchTotal = 0.;

	rpl::event_stream<float64> _flung;

	bool _paused = false;

};

} // namespace Ui::Premium
