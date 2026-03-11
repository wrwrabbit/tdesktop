/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "editor/color_picker.h"

#include "base/basic_types.h"
#include "lang/lang_keys.h"
#include "ui/abstract_button.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/rp_widget.h"
#include "ui/widgets/color_editor.h"
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

class PlusCircle final : public Ui::AbstractButton {
public:
	using Ui::AbstractButton::AbstractButton;

private:
	void paintEvent(QPaintEvent *event) override {
		auto p = QPainter(this);
		PainterHighQualityEnabler hq(p);

		const auto border = st::photoEditorColorButtonBorder;
		const auto half = border / 2.;
		const auto rect = QRectF(QWidget::rect())
			.adjusted(half, half, -half, -half);

		p.setPen(border > 0
			? QPen(st::photoEditorColorButtonBorderFg, border)
			: Qt::NoPen);
		p.setBrush(Qt::NoBrush);

		const auto lineWidth = st::photoEditorColorPalettePlusLine;
		auto pen = QPen(st::photoEditorColorPalettePlusFg, lineWidth);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);

		const auto c = rect::center(rect);
		const auto r = rect.width() / 2. - lineWidth * 1.75;
		p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
		p.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
	}
};

std::vector<QColor> PaletteColors() {
	return {
		QColor(0, 0, 0),
		QColor(255, 255, 255),
		QColor(234, 39, 57),
		QColor(252, 150, 77),
		QColor(252, 222, 101),
		QColor(128, 200, 100),
		QColor(73, 197, 237),
		QColor(48, 81, 227),
		QColor(219, 58, 210),
		QColor(255, 114, 169),
	};
}

} // namespace

ColorPicker::ColorPicker(
	not_null<Ui::RpWidget*> parent,
	std::shared_ptr<Ui::Show> show,
	const Brush &savedBrush)
: _parent(parent)
, _show(std::move(show))
, _colorButton(
	std::in_place,
	parent,
	[=](uint8) {
		auto set = Data::ColorProfileSet();
		set.palette = { _brush.color };
		return set;
	},
	uint8(0),
	true)
, _paletteWrap(std::in_place, parent)
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
	_colorButton->setSelectionCutout(true);
	_colorButton->setForceCircle(true);
	_paletteWrap->setVisible(false);
	_sizeControl->resize(
		st::photoEditorBrushSizeControlHitPadding * 2
			+ st::photoEditorBrushSizeControlExpandShift
			+ st::photoEditorBrushSizeControlExpandedTopWidth,
		st::photoEditorBrushSizeControlHeight);
	_sizeControlHoverArea->setMouseTracking(true);
	_sizeControl->setMouseTracking(true);

	updateSizeControlPositionFromRatio();
	moveSizeControl(_parent->size());

	_colorButton->setClickedCallback([=] {
		setPaletteVisible(!_paletteVisible);
	});

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

	rebuildPalette();
}

void ColorPicker::moveLine(const QPoint &position) {
	_colorButtonCenter = position;
	_colorButton->move(position
		- QPoint(_colorButton->width() / 2, _colorButton->height() / 2));
	updatePaletteGeometry();
}

void ColorPicker::setCanvasRect(const QRect &rect) {
	_canvasRect = rect;
	moveSizeControl(_parent->size());
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
		_paletteVisible = false;
		_sizeDown.pressed = false;
		_sizeHoverAreaHovered = false;
		_sizeControlHovered = false;
		_sizeControlExpanded = false;
		_sizeControlAnimation.stop();
	}
	_colorButton->setVisible(visible);
	_paletteWrap->setVisible(visible && _paletteVisible);
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

void ColorPicker::rebuildPalette() {
	_paletteButtons.clear();
	_palettePlus = nullptr;

	auto colors = PaletteColors();
	auto hasCurrent = false;
	for (const auto &c : colors) {
		if (c == _brush.color) {
			hasCurrent = true;
			break;
		}
	}
	if (!hasCurrent) {
		colors.push_back(_brush.color);
	}

	auto index = uint8(0);
	for (const auto &c : colors) {
		auto button = base::make_unique_q<Ui::ColorSample>(
			_paletteWrap,
			[c](uint8) {
				auto set = Data::ColorProfileSet();
				set.palette = { c };
				return set;
			},
			index++,
			c == _brush.color);
		button->setSelectionCutout(true);
		button->setClickedCallback([=] {
			_brush.color = c;
			rebuildPalette();
			_colorButton->update();
			_saveBrushRequests.fire_copy(_brush);
			setPaletteVisible(false);
		});
		button->show();
		_paletteButtons.push_back(std::move(button));
	}

	_palettePlus = base::make_unique_q<PlusCircle>(_paletteWrap);
	_palettePlus->setClickedCallback([=] {
		if (!_show) {
			return;
		}
		_show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
			struct State {
				QColor color;
			};
			const auto state = box->lifetime().make_state<State>();
			state->color = _brush.color;
			auto editor = box->addRow(
				object_ptr<ColorEditor>(
					box,
					ColorEditor::Mode::HSL,
					_brush.color),
				style::margins());
			box->setWidth(editor->width());
			editor->colorValue(
			) | rpl::on_next([=](QColor c) {
				state->color = c;
			}, editor->lifetime());
			box->addButton(tr::lng_box_done(), [=] {
				_brush.color = state->color;
				rebuildPalette();
				_colorButton->update();
				_saveBrushRequests.fire_copy(_brush);
				setPaletteVisible(false);
				box->closeBox();
			});
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		}));
	});
	_palettePlus->show();
	updatePaletteGeometry();
}

void ColorPicker::updatePaletteGeometry() {
	if (!_paletteWrap) {
		return;
	}
	const auto size = st::photoEditorColorPaletteItemSize;
	const auto gap = st::photoEditorColorPaletteGap;
	const auto count = int(_paletteButtons.size());
	const auto plusSize = size;
	const auto width = (count * size)
		+ ((count > 0) ? (count - 1) * gap : 0)
		+ gap + plusSize;
	const auto height = std::max(size, plusSize);

	_paletteWrap->resize(width, height);
	auto x = 0;
	for (const auto &button : _paletteButtons) {
		button->resize(size, size);
		button->move(x, (height - size) / 2);
		x += size + gap;
	}
	if (_palettePlus) {
		_palettePlus->resize(plusSize, plusSize);
		_palettePlus->move(x, (height - plusSize) / 2);
	}

	if (_colorButtonCenter.isNull()) {
		return;
	}
	_paletteWrap->move(_colorButtonCenter - QPoint(width / 2, height / 2));
}

void ColorPicker::setPaletteVisible(bool visible) {
	if (_paletteVisible == visible) {
		return;
	}
	_paletteVisible = visible;
	_paletteWrap->setVisible(visible);
	_colorButton->setVisible(!visible);
	if (visible) {
		rebuildPalette();
	}
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
