/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "editor/scene/scene_item_text.h"

#include "editor/scene/scene.h"
#include "lang/lang_keys.h"
#include "ui/painter.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_editor.h"
#include "styles/style_menu_icons.h"

#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneContextMenuEvent>
#include <QTextLayout>
#include <QTextOption>

namespace Editor {
namespace {

constexpr auto kPaddingFactor = 0.4f;
constexpr auto kMaxWidthFactor = 0.8f;
constexpr auto kMinContentWidth = 20;

struct LayoutMetrics {
	int contentWidth = 0;
	int contentHeight = 0;
	int padding = 0;
	int textMaxWidth = 0;
};

QFont TextFont(float fontSize) {
	auto font = QFont();
	font.setPixelSize(std::max(int(fontSize), 1));
	font.setWeight(QFont::DemiBold);
	return font;
}

float ComputeBrightness(const QColor &color) {
	return (color.red() * 0.2126f
		+ color.green() * 0.7152f
		+ color.blue() * 0.0722f) / 255.f;
}

LayoutMetrics ComputeMetrics(
		const QString &text,
		float fontSize,
		const QSize &imageSize) {
	const auto padding = int(fontSize * kPaddingFactor);
	const auto shortSide = std::min(imageSize.width(), imageSize.height());
	const auto textMaxWidth = int(shortSide * kMaxWidthFactor) - 2 * padding;

	const auto font = TextFont(fontSize);

	auto processedText = text;
	processedText.replace('\n', QChar::LineSeparator);

	auto option = QTextOption();
	option.setWrapMode(QTextOption::WordWrap);

	auto layout = QTextLayout(processedText, font);
	layout.setTextOption(option);
	layout.beginLayout();

	auto totalHeight = 0.;
	auto maxWidth = 0.;
	while (true) {
		auto line = layout.createLine();
		if (!line.isValid()) {
			break;
		}
		line.setLineWidth(textMaxWidth);
		line.setPosition(QPointF(0, totalHeight));
		totalHeight += line.height();
		maxWidth = std::max(maxWidth, double(line.naturalTextWidth()));
	}
	layout.endLayout();

	return {
		.contentWidth = std::max(int(std::ceil(maxWidth)), kMinContentWidth),
		.contentHeight = int(std::ceil(totalHeight)),
		.padding = padding,
		.textMaxWidth = textMaxWidth,
	};
}

} // namespace

ItemText::ItemText(
	const QString &text,
	const QColor &color,
	float fontSize,
	TextStyle style,
	const QSize &imageSize,
	ItemBase::Data data)
: ItemBase(std::move(data))
, _text(text)
, _color(color)
, _fontSize(fontSize)
, _textStyle(style)
, _imageSize(imageSize) {
	renderContent();
}

void ItemText::renderContent() {
	if (_text.isEmpty()) {
		_pixmap = QPixmap();
		setAspectRatio(1.);
		return;
	}

	const auto m = ComputeMetrics(_text, _fontSize, _imageSize);
	const auto pixWidth = m.contentWidth + 2 * m.padding;
	const auto pixHeight = m.contentHeight + 2 * m.padding;

	const auto font = TextFont(_fontSize);

	auto processedText = _text;
	processedText.replace('\n', QChar::LineSeparator);

	auto option = QTextOption();
	option.setWrapMode(QTextOption::WordWrap);

	auto layout = QTextLayout(processedText, font);
	layout.setTextOption(option);
	layout.beginLayout();
	auto y = 0.;
	while (true) {
		auto line = layout.createLine();
		if (!line.isValid()) {
			break;
		}
		line.setLineWidth(m.textMaxWidth);
		line.setPosition(QPointF(0, y));
		y += line.height();
	}
	layout.endLayout();

	auto textColor = _color;
	auto bgColor = QColor(Qt::transparent);
	const auto brightness = ComputeBrightness(_color);
	const auto cornerRadius = _fontSize / 3.f;
	const auto hasPerLineBackground =
		(_textStyle == TextStyle::Framed)
		|| (_textStyle == TextStyle::SemiTransparent);

	switch (_textStyle) {
	case TextStyle::Framed:
		bgColor = _color;
		textColor = (brightness >= 0.721f)
			? QColor(0, 0, 0)
			: QColor(255, 255, 255);
		break;
	case TextStyle::SemiTransparent:
		bgColor = (brightness >= 0.25f)
			? QColor(0, 0, 0, 0x99)
			: QColor(255, 255, 255, 0x99);
		break;
	case TextStyle::Plain:
		break;
	}

	const auto dpr = style::DevicePixelRatio();
	auto pixmap = QPixmap(QSize(pixWidth, pixHeight) * dpr);
	pixmap.setDevicePixelRatio(dpr);
	pixmap.fill(Qt::transparent);

	{
		auto p = QPainter(&pixmap);
		auto hq = PainterHighQualityEnabler(p);

		if (hasPerLineBackground) {
			const auto linePadH = _fontSize / 3.f;
			const auto linePadV = _fontSize / 8.f;

			p.setPen(Qt::NoPen);
			p.setBrush(bgColor);

			for (auto i = 0; i < layout.lineCount(); ++i) {
				const auto line = layout.lineAt(i);
				const auto natWidth = line.naturalTextWidth();
				const auto xOffset =
					(m.contentWidth - natWidth) / 2.;
				const auto rect = QRectF(
					m.padding + xOffset - linePadH,
					m.padding + line.y() - linePadV,
					natWidth + 2. * linePadH,
					line.height() + 2. * linePadV);
				p.drawRoundedRect(rect, cornerRadius, cornerRadius);
			}
		}

		p.setPen(textColor);
		for (auto i = 0; i < layout.lineCount(); ++i) {
			const auto line = layout.lineAt(i);
			const auto xOffset =
				(m.contentWidth - line.naturalTextWidth()) / 2.;
			line.draw(&p, QPointF(m.padding + xOffset, m.padding));
		}
	}

	_pixmap = std::move(pixmap);
	const auto handleMargin = std::max(
		innerRect().width() - contentRect().width(),
		0.);
	setAspectRatio(
		(pixHeight + handleMargin) / float64(pixWidth + handleMargin));
}

QSize ItemText::computeContentSize(
		const QString &text,
		float fontSize,
		const QSize &imageSize) {
	if (text.isEmpty()) {
		return {};
	}
	auto processedText = text;
	processedText.replace('\n', QChar::LineSeparator);
	const auto m = ComputeMetrics(processedText, fontSize, imageSize);
	return QSize(
		m.contentWidth + 2 * m.padding,
		m.contentHeight + 2 * m.padding);
}

void ItemText::paint(
		QPainter *p,
		const QStyleOptionGraphicsItem *option,
		QWidget *w) {
	if (!_pixmap.isNull()) {
		const auto rect = contentRect();
		const auto pixmapSize = QSizeF(
			_pixmap.size() / style::DevicePixelRatio()
		).scaled(rect.size(), Qt::KeepAspectRatio);
		const auto resultRect = QRectF(
			rect.topLeft(),
			pixmapSize
		).translated(
			(rect.width() - pixmapSize.width()) / 2.,
			(rect.height() - pixmapSize.height()) / 2.);
		if (flipped()) {
			p->save();
			const auto center = resultRect.center();
			p->translate(center);
			p->scale(-1, 1);
			p->translate(-center);
			p->drawPixmap(resultRect.toRect(), _pixmap);
			p->restore();
		} else {
			p->drawPixmap(resultRect.toRect(), _pixmap);
		}
	}
	ItemBase::paint(p, option, w);
}

int ItemText::type() const {
	return Type;
}

const QString &ItemText::text() const {
	return _text;
}

void ItemText::setText(const QString &text) {
	_text = text;
	renderContent();
	update();
}

const QColor &ItemText::color() const {
	return _color;
}

void ItemText::setColor(const QColor &color) {
	_color = color;
	renderContent();
	update();
}

float ItemText::fontSize() const {
	return _fontSize;
}

float64 ItemText::editScale() const {
	const auto natural = computeContentSize(_text, _fontSize, _imageSize);
	if (natural.width() <= 0) {
		return 1.;
	}
	return size() / natural.width();
}

TextStyle ItemText::textStyle() const {
	return _textStyle;
}

void ItemText::setTextStyle(TextStyle style) {
	_textStyle = style;
	renderContent();
	update();
}

void ItemText::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) {
	if (const auto s = static_cast<Scene*>(scene())) {
		s->startTextEditing(this);
	}
}

void ItemText::contextMenuEvent(QGraphicsSceneContextMenuEvent *event) {
	if (scene()) {
		scene()->clearSelection();
		setSelected(true);
	}

	_contextMenu = base::make_unique_q<Ui::PopupMenu>(
		nullptr,
		st::popupMenuWithIcons);
	const auto add = [&](const QString &text, TextStyle style) {
		const auto checked = (_textStyle == style);
		auto action = _contextMenu->addAction(text, [=] {
			setTextStyle(style);
		});
		if (checked) {
			action->setChecked(true);
		}
	};
	add(u"Plain"_q, TextStyle::Plain);
	add(u"Framed"_q, TextStyle::Framed);
	add(u"Semi-Transparent"_q, TextStyle::SemiTransparent);

	_contextMenu->addSeparator();

	_contextMenu->addAction(
		tr::lng_photo_editor_menu_delete(tr::now),
		[=] { actionDelete(); },
		&st::menuIconDelete);
	_contextMenu->addAction(
		tr::lng_photo_editor_menu_duplicate(tr::now),
		[=] { actionDuplicate(); },
		&st::menuIconCopy);

	_contextMenu->popup(event->screenPos());
}

void ItemText::performFlip() {
	update();
}

std::shared_ptr<ItemBase> ItemText::duplicate(ItemBase::Data data) const {
	return std::make_shared<ItemText>(
		_text,
		_color,
		_fontSize,
		_textStyle,
		_imageSize,
		std::move(data));
}

void ItemText::save(SaveState state) {
	ItemBase::save(state);
	auto &saved = (state == SaveState::Keep) ? _keepedState : _savedState;
	saved = {
		.saved = true,
		.status = status(),
		.text = _text,
		.color = _color,
		.fontSize = _fontSize,
		.textStyle = _textStyle,
	};
}

void ItemText::restore(SaveState state) {
	if (!hasState(state)) {
		return;
	}
	const auto &saved = (state == SaveState::Keep) ? _keepedState : _savedState;
	_text = saved.text;
	_color = saved.color;
	_fontSize = saved.fontSize;
	_textStyle = saved.textStyle;
	renderContent();
	ItemBase::restore(state);
}

bool ItemText::hasState(SaveState state) const {
	return ItemBase::hasState(state);
}

} // namespace Editor
