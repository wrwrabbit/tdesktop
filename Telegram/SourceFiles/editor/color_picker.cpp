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


namespace Editor {
namespace {

constexpr auto kMinBrushSize = 0.1;
constexpr auto kMinBrushWidth = 1.;
constexpr auto kMaxBrushWidth = 25.;

constexpr auto kCircleDuration = crl::time(200);

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
, _colorButton(std::in_place, parent)
, _sizeControlHoverArea(std::in_place, parent)
, _sizeControl(std::in_place, parent)
, _brush(Brush{
	.sizeRatio = (savedBrush.sizeRatio
		? savedBrush.sizeRatio
		: kMinBrushSize),
	.color = (savedBrush.color.isValid()
		? savedBrush.color
		: QColor(234, 39, 57)),
}) {
	_colorButton->resize(
		st::photoEditorColorButtonSize,
		st::photoEditorColorButtonSize);
	_sizeControl->resize(
		st::photoEditorBrushSizeControlHitPadding * 2
			+ st::photoEditorBrushSizeControlExpandShift
			+ st::photoEditorBrushSizeControlExpandedTopWidth,
		st::photoEditorBrushSizeControlHeight);
	_sizeControlHoverArea->setMouseTracking(true);
	_sizeControl->setMouseTracking(true);

	updateSizeControlPositionFromRatio();
	moveSizeControl(_parent->size());

	_colorButton->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(_colorButton);
		paintColorButton(p);
	}, _colorButton->lifetime());

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
			_sizeControl->update();
			return;
		}
		if (!_sizeDown.pressed) {
			return;
		}
		if (isMove) {
			updateSizeControlMousePosition(e->pos().y());
			_sizeControl->update();
			return;
		}
		if (isRelease && (e->button() == Qt::LeftButton)) {
			updateSizeControlMousePosition(e->pos().y());
			_sizeDown.pressed = false;
			updateSizeControlExpanded();
			_saveBrushRequests.fire_copy(_brush);
			_sizeControl->update();
		}
	}, _sizeControl->lifetime());
}

void ColorPicker::moveLine(const QPoint &position) {
	_colorButton->move(position
		- QPoint(_colorButton->width() / 2, _colorButton->height() / 2));
}

void ColorPicker::setCanvasRect(const QRect &rect) {
	_canvasRect = rect;
	moveSizeControl(_parent->size());
}

void ColorPicker::paintColorButton(QPainter &p) {
	PainterHighQualityEnabler hq(p);

	const auto border = st::photoEditorColorButtonBorder;
	const auto half = border / 2.;
	const auto rect = QRectF(_colorButton->rect())
		.adjusted(half, half, -half, -half);

	if (border > 0) {
		p.setPen(QPen(st::photoEditorColorButtonBorderFg, border));
	} else {
		p.setPen(Qt::NoPen);
	}
	p.setBrush(_brush.color);
	p.drawEllipse(rect);
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

void ColorPicker::setVisible(bool visible) {
	if (!visible) {
		_sizeDown.pressed = false;
		_sizeHoverAreaHovered = false;
		_sizeControlHovered = false;
		_sizeControlExpanded = false;
		_sizeControlAnimation.stop();
	}
	_colorButton->setVisible(visible);
	_sizeControlHoverArea->setVisible(visible);
	_sizeControl->setVisible(visible);
}

rpl::producer<Brush> ColorPicker::saveBrushRequests() const {
	return _saveBrushRequests.events_starting_with_copy(_brush);
}

bool ColorPicker::preventHandleKeyPress() const {
	return _sizeControl->isVisible()
		&& (_sizeControlAnimation.animating() || _sizeDown.pressed);
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

float ColorPicker::sizeControlRatioFromY(int y) const {
	const auto top = sizeControlTop();
	const auto bottom = sizeControlBottom();
	if (bottom <= top) {
		return 1.f;
	}
	const auto ratio = 1. - InterpolationRatio(top, bottom, y);
	return std::clamp(ratio, kMinBrushSize, 1.0);
}

int ColorPicker::sizeControlYFromRatio(float ratio) const {
	const auto top = sizeControlTop();
	const auto bottom = sizeControlBottom();
	if (bottom <= top) {
		return top;
	}
	const auto normalized = std::clamp(
		(ratio - kMinBrushSize) / (1.0 - kMinBrushSize),
		0.0,
		1.0);
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
