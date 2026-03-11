/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "editor/color_picker.h"

#include "base/basic_types.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/rp_widget.h"
#include "styles/style_editor.h"

#include <QtGui/QLinearGradient>

namespace Editor {
namespace {

constexpr auto kPrecision = 1000;
constexpr auto kMinBrushSize = 0.1;
constexpr auto kMouseSkip = 1.4;
constexpr auto kMinBrushWidth = 1.;
constexpr auto kMaxBrushWidth = 25.;

constexpr auto kMinInnerHeight = 0.2;
constexpr auto kMaxInnerHeight = 0.8;

constexpr auto kCircleDuration = crl::time(200);

constexpr auto kMax = 1.0;

ColorPicker::OutlinedStop FindOutlinedStop(
		const QColor &color,
		const QGradientStops &stops,
		int width) {
	for (auto i = 0; i < stops.size(); i++) {
		const auto &current = stops[i];
		if (current.second == color) {
			const auto prev = ((i - 1) < 0)
				? std::nullopt
				: std::make_optional<int>(stops[i - 1].first * width);
			const auto next = ((i + 1) >= stops.size())
				? std::nullopt
				: std::make_optional<int>(stops[i + 1].first * width);
			return ColorPicker::OutlinedStop{
				.stopPos = (current.first * width),
				.prevStopPos = prev,
				.nextStopPos = next,
			};
		}
	}
	return ColorPicker::OutlinedStop();
}

QGradientStops Colors() {
	return QGradientStops{
		{ 0.00f, QColor(234, 39, 57) },
		{ 0.14f, QColor(219, 58, 210) },
		{ 0.24f, QColor(48, 81, 227) },
		{ 0.39f, QColor(73, 197, 237) },
		{ 0.49f, QColor(128, 200, 100) },
		{ 0.62f, QColor(252, 222, 101) },
		{ 0.73f, QColor(252, 150, 77) },
		{ 0.85f, QColor(0, 0, 0) },
		{ 1.00f, QColor(255, 255, 255) } };
}

QBrush GradientBrush(const QPoint &p, const QGradientStops &stops) {
	auto gradient = QLinearGradient(0, p.y(), p.x(), p.y());
	gradient.setStops(stops);
	return QBrush(std::move(gradient));
}

float64 RatioPrecise(float64 a) {
	return int(a * kPrecision) / float64(kPrecision);
}

inline float64 InterpolateF(float64 a, float64 b, float64 b_ratio) {
	return a + float64(b - a) * b_ratio;
};

inline float64 InterpolationRatio(int from, int to, int result) {
	return (result - from) / float64(to - from);
};

} // namespace

ColorPicker::ColorPicker(
	not_null<Ui::RpWidget*> parent,
	const Brush &savedBrush)
: _parent(parent)
, _circleColor(Qt::white)
, _width(st::photoEditorColorPickerWidth)
, _lineHeight(st::photoEditorColorPickerLineHeight)
, _colorLine(base::make_unique_q<Ui::RpWidget>(parent))
, _canvasForCircle(base::make_unique_q<Ui::RpWidget>(parent))
, _sizeControlHoverArea(std::in_place, parent)
, _sizeControl(std::in_place, parent)
, _gradientStops(Colors())
, _outlinedStop(FindOutlinedStop(_circleColor, _gradientStops, _width))
, _gradientBrush(
	GradientBrush(QPoint(_width, _lineHeight / 2), _gradientStops))
, _brush(Brush{
	.sizeRatio = (savedBrush.sizeRatio
		? savedBrush.sizeRatio
		: kMinBrushSize),
	.color = (savedBrush.color.isValid()
		? savedBrush.color
		: _gradientStops.front().second),
}) {
	_colorLine->resize(_width, _lineHeight);
	_canvasForCircle->resize(
		_width + circleHeight(kMax),
		st::photoEditorColorPickerCanvasHeight);
	_sizeControl->resize(
		st::photoEditorBrushSizeControlHitPadding * 2
			+ st::photoEditorBrushSizeControlExpandShift
			+ st::photoEditorBrushSizeControlExpandedTopWidth,
		st::photoEditorBrushSizeControlHeight);
	_sizeControlHoverArea->setMouseTracking(true);
	_sizeControl->setMouseTracking(true);

	_canvasForCircle->setAttribute(Qt::WA_TransparentForMouseEvents);

	_down.pos = QPoint(colorToPosition(savedBrush.color), 0);
	updateSizeControlPositionFromRatio();
	moveSizeControl(_parent->size());

	_colorLine->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(_colorLine);
		PainterHighQualityEnabler hq(p);

		p.setPen(Qt::NoPen);
		p.setBrush(_gradientBrush);

		const auto radius = _colorLine->height() / 2.;
		p.drawRoundedRect(_colorLine->rect(), radius, radius);
	}, _colorLine->lifetime());

	_canvasForCircle->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(_canvasForCircle);
		paintCircle(p);
	}, _canvasForCircle->lifetime());

	_sizeControl->paintOn([=](QPainter &p) {
		paintSizeControl(p);
	});

	_parent->sizeValue(
	) | rpl::on_next([=](const QSize &size) {
		moveSizeControl(size);
	}, _sizeControl->lifetime());

	_sizeControlHoverArea->events(
	) | rpl::on_next([=](not_null<QEvent*> event) {
		const auto type = event->type();
		if (type == QEvent::Enter) {
			_sizeHoverAreaHovered = true;
			updateSizeControlExpanded();
		} else if (type == QEvent::Leave) {
			_sizeHoverAreaHovered = false;
			updateSizeControlExpanded();
		}
	}, _sizeControlHoverArea->lifetime());

	_colorLine->events(
	) | rpl::on_next([=](not_null<QEvent*> event) {
		const auto type = event->type();
		const auto isPress = (type == QEvent::MouseButtonPress)
			|| (type == QEvent::MouseButtonDblClick);
		const auto isMove = (type == QEvent::MouseMove);
		const auto isRelease = (type == QEvent::MouseButtonRelease);
		if (!isPress && !isMove && !isRelease) {
			return;
		}
		_down.pressed = !isRelease;

		const auto progress = _circleAnimation.value(isPress ? 0. : 1.);
		if (!isMove) {
			const auto from = progress;
			const auto to = isPress ? 1. : 0.;
			_circleAnimation.stop();

			_circleAnimation.start(
				[=] { _canvasForCircle->update(); },
				from,
				to,
				kCircleDuration * std::abs(to - from),
				anim::easeOutCirc);
		}
		const auto e = static_cast<QMouseEvent*>(event.get());
		updateMousePosition(e->pos(), progress);
		if (isRelease) {
			_saveBrushRequests.fire_copy(_brush);
		}

		_canvasForCircle->update();
		_sizeControl->update();
	}, _colorLine->lifetime());

	_sizeControl->events(
	) | rpl::on_next([=](not_null<QEvent*> event) {
		const auto type = event->type();
		if (type == QEvent::Enter) {
			_sizeControlHovered = true;
			updateSizeControlExpanded();
			return;
		} else if (type == QEvent::Leave) {
			_sizeControlHovered = false;
			updateSizeControlExpanded();
			return;
		}
		const auto isPress = (type == QEvent::MouseButtonPress)
			|| (type == QEvent::MouseButtonDblClick);
		const auto isMove = (type == QEvent::MouseMove);
		const auto isRelease = (type == QEvent::MouseButtonRelease);
		if (!isPress && !isMove && !isRelease) {
			return;
		}

		const auto e = static_cast<QMouseEvent*>(event.get());
		if (isPress) {
			if (e->button() != Qt::LeftButton) {
				return;
			}
			const auto progress = _sizeControlAnimation.value(
				_sizeControlExpanded ? 1. : 0.);
			const auto inHandle = sizeControlHandleRect(progress).contains(
				e->pos());
			const auto inControl = sizeControlHitRect(progress).contains(
				e->pos());
			if (!inHandle && !inControl) {
				return;
			}
			_sizeDown.pressed = true;
			updateSizeControlMousePosition(e->pos().y());
			updateSizeControlExpanded();
			_canvasForCircle->update();
			_sizeControl->update();
			return;
		}
		if (!_sizeDown.pressed) {
			return;
		}
		if (isMove) {
			updateSizeControlMousePosition(e->pos().y());
			_canvasForCircle->update();
			_sizeControl->update();
			return;
		}
		if (isRelease && (e->button() == Qt::LeftButton)) {
			updateSizeControlMousePosition(e->pos().y());
			_sizeDown.pressed = false;
			updateSizeControlExpanded();
			_saveBrushRequests.fire_copy(_brush);
			_canvasForCircle->update();
			_sizeControl->update();
		}
	}, _sizeControl->lifetime());
}

void ColorPicker::updateMousePosition(const QPoint &pos, float64 progress) {
	const auto mapped = _canvasForCircle->mapFromParent(
		_colorLine->mapToParent(pos));

	const auto height = circleHeight(progress);
	const auto mappedY = int(mapped.y() - height * kMouseSkip);
	const auto bottom = _canvasForCircle->height() - circleHeight(kMax);
	const auto &skip = st::photoEditorColorPickerCircleSkip;

	_down.pos = QPoint(
		std::clamp(pos.x(), 0, _width),
		std::clamp(mappedY, 0, bottom - skip));

	// Convert Y to the brush size.
	const auto from = 0;
	const auto to = bottom - skip;

	// Don't change the brush size when we are on the color line.
	if (mappedY <= to) {
		_brush.sizeRatio = std::clamp(
			float64(1. - InterpolationRatio(from, to, _down.pos.y())),
			kMinBrushSize,
			1.);
		updateSizeControlPositionFromRatio();
	}
	_brush.color = positionToColor(_down.pos.x());
}

void ColorPicker::moveLine(const QPoint &position) {
	_colorLine->move(position
		- QPoint(_colorLine->width() / 2, _colorLine->height() / 2));

	_canvasForCircle->move(
		_colorLine->x() - circleHeight(kMax) / 2,
		_colorLine->y()
			+ _colorLine->height()
			+ ((circleHeight() - _colorLine->height()) / 2)
			- _canvasForCircle->height());
}

void ColorPicker::setCanvasRect(const QRect &rect) {
	_canvasRect = rect;
	moveSizeControl(_parent->size());
}

QColor ColorPicker::positionToColor(int x) const {
	const auto from = 0;
	const auto to = _width;
	const auto gradientRatio = InterpolationRatio(from, to, x);

	for (auto i = 1; i < _gradientStops.size(); i++) {
		const auto &previous = _gradientStops[i - 1];
		const auto &current = _gradientStops[i];
		const auto &fromStop = previous.first;
		const auto &toStop = current.first;
		const auto &fromColor = previous.second;
		const auto &toColor = current.second;

		if ((fromStop <= gradientRatio) && (toStop >= gradientRatio)) {
			const auto stopRatio = RatioPrecise(
				(gradientRatio - fromStop) / float64(toStop - fromStop));
			return anim::color(fromColor, toColor, stopRatio);
		}
	}
	return QColor();
}

void ColorPicker::paintCircle(QPainter &p) {
	PainterHighQualityEnabler hq(p);

	p.setPen(Qt::NoPen);
	p.setBrush(_circleColor);

	const auto progress = _circleAnimation.value(_down.pressed ? 1. : 0.);
	const auto h = circleHeight(progress);
	const auto bottom = _canvasForCircle->height() - h;

	const auto circleX = _down.pos.x() + (circleHeight(kMax) - h) / 2;
	const auto circleY = _circleAnimation.animating()
		? anim::interpolate(bottom, _down.pos.y(), progress)
		: _down.pressed
		? _down.pos.y()
		: bottom;

	const auto r = QRect(circleX, circleY, h, h);
	p.drawEllipse(r);

	const auto innerH = InterpolateF(
		h * kMinInnerHeight,
		h * kMaxInnerHeight,
		_brush.sizeRatio);

	p.setBrush(_brush.color);

	const auto innerRect = QRectF(
		r.x() + (r.width() - innerH) / 2.,
		r.y() + (r.height() - innerH) / 2.,
		innerH,
		innerH);

	paintOutline(p, innerRect);
	p.drawEllipse(innerRect);
}

void ColorPicker::paintSizeControl(QPainter &p) {
	auto hq = PainterHighQualityEnabler(p);

	const auto progress = _sizeControlAnimation.value(_sizeControlExpanded
		? 1.
		: 0.);
	const auto path = sizeControlShapePath(progress);

	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, anim::interpolate(96, 176, progress)));
	p.drawPath(path);

	const auto handleRect = sizeControlHandleRect(progress);
	p.setBrush(QColor(255, 255, 255, 244));
	p.drawEllipse(handleRect);
}

void ColorPicker::paintOutline(QPainter &p, const QRectF &rect) {
	const auto &s = _outlinedStop;
	if (!s.stopPos) {
		return;
	}
	const auto draw = [&](float64 opacity) {
		p.save();
		p.setOpacity(opacity);
		p.setPen(Qt::lightGray);
		p.setPen(Qt::NoBrush);
		p.drawEllipse(rect);
		p.restore();
	};
	const auto x = _down.pos.x();
	if (s.prevStopPos && (x >= s.prevStopPos && x <= s.stopPos)) {
		const auto from = *s.prevStopPos;
		const auto to = *s.stopPos;
		const auto ratio = InterpolationRatio(from, to, x);
		if (ratio >= 0. && ratio <= 1.) {
			draw(ratio);
		}
	} else if (s.nextStopPos && (x >= s.stopPos && x <= s.nextStopPos)) {
		const auto from = *s.stopPos;
		const auto to = *s.nextStopPos;
		const auto ratio = InterpolationRatio(from, to, x);
		if (ratio >= 0. && ratio <= 1.) {
			draw(1. - ratio);
		}
	}
}

int ColorPicker::circleHeight(float64 progress) const {
	return anim::interpolate(
		st::photoEditorColorPickerCircleSize,
		st::photoEditorColorPickerCircleBigSize,
		progress);
}

void ColorPicker::setVisible(bool visible) {
	if (!visible) {
		_sizeDown.pressed = false;
		_sizeHoverAreaHovered = false;
		_sizeControlHovered = false;
		_sizeControlExpanded = false;
		_sizeControlAnimation.stop();
	}
	_colorLine->setVisible(visible);
	_canvasForCircle->setVisible(visible);
	_sizeControlHoverArea->setVisible(visible);
	_sizeControl->setVisible(visible);
}

rpl::producer<Brush> ColorPicker::saveBrushRequests() const {
	return _saveBrushRequests.events_starting_with_copy(_brush);
}

int ColorPicker::colorToPosition(const QColor &color) const {
	const auto step = 1. / kPrecision;
	for (auto i = 0.; i <= 1.; i += step) {
		if (positionToColor(i * _width) == color) {
			return i * _width;
		}
	}
	return 0;
}

bool ColorPicker::preventHandleKeyPress() const {
	return _canvasForCircle->isVisible()
		&& (_circleAnimation.animating()
			|| _sizeControlAnimation.animating()
			|| _down.pressed
			|| _sizeDown.pressed);
}

void ColorPicker::moveSizeControl(const QSize &size) {
	if (size.isEmpty()) {
		return;
	}
	const auto areaWidth = std::min(
		size.width(),
		st::photoEditorBrushSizeControlHitPadding);
	const auto areaTop = (_canvasRect.height() > 0)
		? _canvasRect.y()
		: (size.height() - _sizeControl->height()) / 2;
	const auto areaHeight = (_canvasRect.height() > 0)
		? _canvasRect.height()
		: _sizeControl->height();
	_sizeControlHoverArea->setGeometry(
		0,
		std::clamp(areaTop, 0, std::max(0, size.height() - areaHeight)),
		areaWidth,
		std::min(areaHeight, size.height()));

	const auto collapsedCenterX = sizeControlCurrentCenterX(0.);
	const auto collapsedLeft = collapsedCenterX
		- (float64(st::photoEditorBrushSizeControlCollapsedWidth) / 2.);
	const auto y = (_canvasRect.height() > 0)
		? (rect::center(_canvasRect).y() - _sizeControl->height() / 2)
		: ((size.height() - _sizeControl->height()) / 2);
	const auto diff = size.height() - _sizeControl->height();
	_sizeControl->move(
		-int(base::SafeRound(collapsedLeft)),
		std::clamp(y, 0, std::max(0, diff)));
}

void ColorPicker::updateSizeControlExpanded() {
	const auto expanded = _sizeDown.pressed
		|| _sizeHoverAreaHovered
		|| _sizeControlHovered;
	if (_sizeControlExpanded == expanded
		&& !_sizeControlAnimation.animating()) {
		return;
	}
	_sizeControlExpanded = expanded;
	const auto from = _sizeControlAnimation.value(expanded ? 0. : 1.);
	const auto to = expanded ? 1. : 0.;
	_sizeControlAnimation.stop();
	_sizeControlAnimation.start(
		[=] { _sizeControl->update(); },
		from,
		to,
		kCircleDuration * std::abs(to - from),
		anim::easeOutCirc);
	_sizeControl->update();
}

void ColorPicker::updateSizeControlMousePosition(int y) {
	_sizeDown.y = std::clamp(y, sizeControlTop(), sizeControlBottom());
	_brush.sizeRatio = sizeControlRatioFromY(_sizeDown.y);
}

void ColorPicker::updateSizeControlPositionFromRatio() {
	_sizeDown.y = sizeControlYFromRatio(_brush.sizeRatio);
}

int ColorPicker::sizeControlShapeTop() const {
	return st::photoEditorBrushSizeControlHitPadding;
}

int ColorPicker::sizeControlShapeBottom() const {
	return _sizeControl->height() - st::photoEditorBrushSizeControlHitPadding;
}

int ColorPicker::sizeControlTop() const {
	return sizeControlShapeTop()
		+ (st::photoEditorBrushSizeControlExpandedTopWidth / 2);
}

int ColorPicker::sizeControlBottom() const {
	return sizeControlShapeBottom()
		- (st::photoEditorBrushSizeControlExpandedBottomWidth / 2);
}

float64 ColorPicker::sizeControlRatioFromY(int y) const {
	const auto top = sizeControlTop();
	const auto bottom = sizeControlBottom();
	if (bottom <= top) {
		return 1.;
	}
	return std::clamp(
		float64(1. - InterpolationRatio(top, bottom, y)),
		kMinBrushSize,
		1.);
}

int ColorPicker::sizeControlYFromRatio(float64 ratio) const {
	const auto top = sizeControlTop();
	const auto bottom = sizeControlBottom();
	if (bottom <= top) {
		return top;
	}
	const auto normalized = std::clamp(
		(ratio - kMinBrushSize) / (1. - kMinBrushSize),
		0.,
		1.);
	return std::clamp(
		anim::interpolate(bottom, top, normalized),
		top,
		bottom);
}

QRectF ColorPicker::sizeControlHandleRect(float64 progress) const {
	const auto handleSize = sizeControlHandleSize();
	const auto centerX = sizeControlCurrentCenterX(progress);
	return QRectF(
		centerX - handleSize / 2.,
		_sizeDown.y - handleSize / 2.,
		handleSize,
		handleSize);
}

QRectF ColorPicker::sizeControlHitRect(float64 progress) const {
	const auto collapsed = st::photoEditorBrushSizeControlCollapsedWidth;
	const auto width = float64(anim::interpolate(
		collapsed,
		st::photoEditorBrushSizeControlExpandedTopWidth,
		progress));
	const auto centerX = sizeControlCurrentCenterX(progress);
	const auto top = float64(sizeControlShapeTop());
	const auto bottom = float64(sizeControlShapeBottom());
	return QRectF(
		centerX - width / 2.,
		top,
		width,
		bottom - top);
}

QPainterPath ColorPicker::sizeControlShapePath(float64 progress) const {
	const auto collapsed = st::photoEditorBrushSizeControlCollapsedWidth;
	const auto topWidth = float64(anim::interpolate(
		collapsed,
		st::photoEditorBrushSizeControlExpandedTopWidth,
		progress));
	const auto topInset = st::photoEditorBrushSizeControlTopInset;
	const auto adjustedTopWidth = std::max(
		0.,
		topWidth - float64(topInset * 2));
	const auto bottomWidth = float64(anim::interpolate(
		collapsed,
		st::photoEditorBrushSizeControlExpandedBottomWidth,
		progress));
	const auto centerX = sizeControlCurrentCenterX(progress);
	const auto top = float64(sizeControlShapeTop()) + topInset;
	const auto bottom = float64(sizeControlShapeBottom());
	const auto topRadius = adjustedTopWidth / 2.;
	const auto bottomRadius = bottomWidth / 2.;

	auto path = QPainterPath();
	const auto topRect = QRectF(
		centerX - topRadius,
		top,
		adjustedTopWidth,
		adjustedTopWidth);
	const auto bottomRect = QRectF(
		centerX - bottomRadius,
		bottom - bottomWidth,
		bottomWidth,
		bottomWidth);
	path.moveTo(centerX - topRadius, top + topRadius);
	path.arcTo(topRect, 180., -180.);
	path.lineTo(centerX + bottomRadius, bottom - bottomRadius);
	path.arcTo(bottomRect, 0., -180.);
	path.lineTo(centerX - topRadius, top + topRadius);
	path.closeSubpath();
	return path;
}

float64 ColorPicker::sizeControlCurrentCenterX(float64 progress) const {
	const auto from = float64(st::photoEditorBrushSizeControlCollapsedWidth)
		/ 2.;
	const auto to = float64(st::photoEditorBrushSizeControlExpandShift)
		+ float64(st::photoEditorBrushSizeControlExpandedTopWidth) / 2.;
	return float64(st::photoEditorBrushSizeControlHitPadding)
		+ InterpolateF(from, to, progress);
}

float64 ColorPicker::sizeControlHandleSize() const {
	const auto width = kMinBrushWidth
		+ (kMaxBrushWidth - kMinBrushWidth) * _brush.sizeRatio;
	return std::clamp(
		width,
		float64(st::photoEditorBrushSizeControlExpandedBottomWidth),
		float64(st::photoEditorBrushSizeControlExpandedTopWidth));
}

} // namespace Editor
