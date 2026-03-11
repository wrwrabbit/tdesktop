/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "editor/photo_editor_inner_common.h"
#include "ui/effects/animations.h"

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Editor {

class ColorPicker final {
public:
	ColorPicker(not_null<Ui::RpWidget*> parent, const Brush &savedBrush);

	void moveLine(const QPoint &position);
	void setCanvasRect(const QRect &rect);
	void setVisible(bool visible);
	bool preventHandleKeyPress() const;

	rpl::producer<Brush> saveBrushRequests() const;

private:
	void paintColorButton(QPainter &p);
	void paintSizeControl(QPainter &p);
	void moveSizeControl(const QSize &size);
	void updateSizeControlExpanded();
	void updateSizeControlMousePosition(int y);
	void updateSizeControlPositionFromRatio();
	[[nodiscard]] int sizeControlShapeTop() const;
	[[nodiscard]] int sizeControlShapeBottom() const;
	[[nodiscard]] int sizeControlTop() const;
	[[nodiscard]] int sizeControlBottom() const;
	[[nodiscard]] float sizeControlRatioFromY(int y) const;
	[[nodiscard]] int sizeControlYFromRatio(float ratio) const;
	[[nodiscard]] QRectF sizeControlHandleRect(float64 progress) const;
	[[nodiscard]] QRectF sizeControlHitRect(float64 progress) const;
	[[nodiscard]] QPainterPath sizeControlShapePath(float64 progress) const;
	[[nodiscard]] float64 sizeControlCurrentCenterX(float64 progress) const;
	[[nodiscard]] float64 sizeControlHandleSize() const;

	const not_null<Ui::RpWidget*> _parent;

	const base::unique_qptr<Ui::RpWidget> _colorButton;
	const base::unique_qptr<Ui::RpWidget> _sizeControlHoverArea;
	const base::unique_qptr<Ui::RpWidget> _sizeControl;

	struct {
		int y = 0;
		bool pressed = false;
	} _sizeDown;
	bool _sizeHoverAreaHovered = false;
	bool _sizeControlHovered = false;
	bool _sizeControlExpanded = false;
	QRect _canvasRect;
	Brush _brush;

	Ui::Animations::Simple _sizeControlAnimation;

	rpl::event_stream<Brush> _saveBrushRequests;

};

} // namespace Editor
