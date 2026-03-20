/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/controls/history_view_compose_ai_button.h"

#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "styles/style_chat_helpers.h"

namespace HistoryView::Controls {
namespace {

void PaintSparkle(QPainter &p, QPoint center, int radius) {
	p.drawLine(
		center.x() - radius,
		center.y(),
		center.x() + radius,
		center.y());
	p.drawLine(
		center.x(),
		center.y() - radius,
		center.x(),
		center.y() + radius);
}

} // namespace

ComposeAiButton::ComposeAiButton(
	QWidget *parent,
	const style::IconButton &st)
: RippleButton(parent, st.ripple)
, _st(st) {
	resize(_st.width, _st.height);
	setCursor(style::cur_pointer);
}

void ComposeAiButton::paintEvent(QPaintEvent *e) {
	Painter p(this);
	PainterHighQualityEnabler hq(p);

	const auto over = isDown() || isOver() || forceRippled();
	const auto bg = over
		? st::historyAiComposeButtonBgOver
		: st::historyAiComposeButtonBg;
	if (bg->c.alpha()) {
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		p.drawEllipse(rect());
	}
	paintRipple(p, _st.rippleAreaPosition);

	const auto fg = over
		? st::historyAiComposeButtonTextFgOver
		: st::historyAiComposeButtonTextFg;
	auto pen = QPen(fg);
	pen.setWidth(st::historyAiComposeButtonSparkleStroke);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	PaintSparkle(
		p,
		st::historyAiComposeButtonSparkleBigPosition,
		st::historyAiComposeButtonSparkleBigRadius);
	PaintSparkle(
		p,
		st::historyAiComposeButtonSparkleSmallPosition,
		st::historyAiComposeButtonSparkleSmallRadius);

	p.setFont(st::historyAiComposeButtonFont);
	p.drawText(
		rect().translated(st::historyAiComposeButtonTextShift),
		Qt::AlignCenter,
		u"Ai"_q);
}

void ComposeAiButton::onStateChanged(State was, StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
	update();
}

QImage ComposeAiButton::prepareRippleMask() const {
	return Ui::RippleAnimation::EllipseMask(
		QSize(_st.rippleAreaSize, _st.rippleAreaSize));
}

QPoint ComposeAiButton::prepareRippleStartPosition() const {
	const auto result = mapFromGlobal(QCursor::pos()) - _st.rippleAreaPosition;
	const auto rect = QRect(0, 0, _st.rippleAreaSize, _st.rippleAreaSize);
	return rect.contains(result)
		? result
		: DisabledRippleStartPosition();
}

} // namespace HistoryView::Controls
