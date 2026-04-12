/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "editor/scene/scene_item_text.h"

#include "editor/scene/scene.h"
#include "lang/lang_keys.h"
#include "ui/emoji_config.h"
#include "ui/painter.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_editor.h"
#include "styles/style_menu_icons.h"

#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneContextMenuEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
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
		const QSize &imageSize,
		TextStyle style) {
	const auto hasBackground = (style == TextStyle::Framed)
		|| (style == TextStyle::SemiTransparent);
	const auto padding = hasBackground ? int(fontSize * kPaddingFactor) : 0;
	const auto shortSide = std::min(imageSize.width(), imageSize.height());
	const auto textMaxWidth = int(shortSide * kMaxWidthFactor) - 2 * padding;

	const auto font = TextFont(fontSize);

	auto processedText = text;
	processedText.replace('\n', QChar::LineSeparator);

	auto option = QTextOption();
	option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

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
		maxWidth = std::max(maxWidth, float64(line.naturalTextWidth()));
	}
	layout.endLayout();

	return {
		.contentWidth = std::max(int(std::ceil(maxWidth)), kMinContentWidth),
		.contentHeight = int(std::ceil(totalHeight)),
		.padding = padding,
		.textMaxWidth = textMaxWidth,
	};
}

struct LineRect {
	float left = 0;
	float top = 0;
	float right = 0;
	float bottom = 0;
	[[nodiscard]] float width() const { return right - left; }
};

QPainterPath BuildConnectedBackground(
		const QTextLayout &layout,
		int contentWidth,
		int padding,
		float fontSize) {
	const auto linePadH = fontSize / 3.f;
	const auto linePadV = fontSize / 8.f;
	const auto cornerRadius = fontSize / 3.f;
	const auto mergeRadius = cornerRadius * 1.5f;
	const auto centerX = padding + contentWidth / 2.f;

	auto rects = std::vector<LineRect>();
	for (auto i = 0; i < layout.lineCount(); ++i) {
		const auto line = layout.lineAt(i);
		const auto hw = float(line.naturalTextWidth()) / 2.f + linePadH;
		rects.push_back({
			.left = centerX - hw,
			.top = padding + float(line.y()) - linePadV,
			.right = centerX + hw,
			.bottom = padding + float(line.y() + line.height()) + linePadV,
		});
	}

	if (rects.empty()) {
		return {};
	}
	if (rects.size() == 1) {
		auto path = QPainterPath();
		const auto &r = rects[0];
		path.addRoundedRect(
			QRectF(r.left, r.top, r.width(), r.bottom - r.top),
			cornerRadius,
			cornerRadius);
		return path;
	}

	for (auto i = 1; i < int(rects.size()); ++i) {
		rects[i - 1].bottom = rects[i].top;
	}

	for (auto i = 1; i < int(rects.size()); ++i) {
		auto traceback = false;
		if (std::abs(rects[i - 1].left - rects[i].left) < mergeRadius) {
			const auto v = std::min(rects[i - 1].left, rects[i].left);
			rects[i - 1].left = rects[i].left = v;
			traceback = true;
		}
		if (std::abs(rects[i - 1].right - rects[i].right) < mergeRadius) {
			const auto v = std::max(rects[i - 1].right, rects[i].right);
			rects[i - 1].right = rects[i].right = v;
			traceback = true;
		}
		if (traceback) {
			for (auto j = i; j >= 1; --j) {
				if (std::abs(rects[j - 1].left - rects[j].left) < mergeRadius) {
					const auto v = std::min(
						rects[j - 1].left,
						rects[j].left);
					rects[j - 1].left = rects[j].left = v;
				}
				if (std::abs(rects[j - 1].right - rects[j].right)
					< mergeRadius) {
					const auto v = std::max(
						rects[j - 1].right,
						rects[j].right);
					rects[j - 1].right = rects[j].right = v;
				}
			}
		}
	}

	struct V { float x, y; };
	auto verts = std::vector<V>();

	verts.push_back({ rects[0].left, rects[0].top });
	verts.push_back({ rects[0].right, rects[0].top });

	for (auto i = 1; i < int(rects.size()); ++i) {
		if (std::abs(rects[i].right - rects[i - 1].right) > 0.5f) {
			verts.push_back({ rects[i - 1].right, rects[i].top });
			verts.push_back({ rects[i].right, rects[i].top });
		}
	}

	const auto last = int(rects.size()) - 1;
	verts.push_back({ rects[last].right, rects[last].bottom });
	verts.push_back({ rects[last].left, rects[last].bottom });

	for (auto i = last - 1; i >= 0; --i) {
		if (std::abs(rects[i].left - rects[i + 1].left) > 0.5f) {
			verts.push_back({ rects[i + 1].left, rects[i + 1].top });
			verts.push_back({ rects[i].left, rects[i + 1].top });
		}
	}

	auto path = QPainterPath();
	const auto n = int(verts.size());
	for (auto i = 0; i < n; ++i) {
		const auto &prev = verts[(i + n - 1) % n];
		const auto &curr = verts[i];
		const auto &next = verts[(i + 1) % n];

		const auto dx1 = curr.x - prev.x;
		const auto dy1 = curr.y - prev.y;
		const auto len1 = std::sqrt(dx1 * dx1 + dy1 * dy1);

		const auto dx2 = next.x - curr.x;
		const auto dy2 = next.y - curr.y;
		const auto len2 = std::sqrt(dx2 * dx2 + dy2 * dy2);

		if (len1 < 0.1f || len2 < 0.1f) {
			if (i == 0) {
				path.moveTo(curr.x, curr.y);
			} else {
				path.lineTo(curr.x, curr.y);
			}
			continue;
		}

		const auto r = std::min({
			cornerRadius,
			len1 / 2.f,
			len2 / 2.f,
		});
		const auto bx = curr.x - dx1 / len1 * r;
		const auto by = curr.y - dy1 / len1 * r;
		const auto ax = curr.x + dx2 / len2 * r;
		const auto ay = curr.y + dy2 / len2 * r;

		if (i == 0) {
			path.moveTo(bx, by);
		} else {
			path.lineTo(bx, by);
		}
		path.quadTo(curr.x, curr.y, ax, ay);
	}
	path.closeSubpath();
	return path;
}

} // namespace

EmojiDocument::EmojiDocument(QObject *parent)
: QTextDocument(parent) {
}

QVariant EmojiDocument::loadResource(int type, const QUrl &name) {
	if (type != QTextDocument::ImageResource
		|| name.scheme() != u"emoji"_q) {
		return QTextDocument::loadResource(type, name);
	}
	const auto i = _cache.find(name);
	if (i != _cache.end()) {
		return i->second;
	}
	auto result = QVariant();
	if (const auto emoji = Ui::Emoji::FromUrl(name.toDisplayString())) {
		const auto factor = style::DevicePixelRatio();
		const auto logical = QFontMetrics(defaultFont()).height();
		const auto source = Ui::Emoji::GetSizeLarge();
		auto image = QImage(
			QSize(logical, logical) * factor,
			QImage::Format_ARGB32_Premultiplied);
		image.setDevicePixelRatio(factor);
		image.fill(Qt::transparent);
		{
			auto p = QPainter(&image);
			auto hq = PainterHighQualityEnabler(p);
			const auto enlarged = logical * 1.0;
			const auto sourceLogical = source / float64(factor);
			const auto scale = enlarged / sourceLogical;
			const auto offset = (logical - enlarged) / 2.;
			p.translate(offset, offset);
			p.scale(scale, scale);
			Ui::Emoji::Draw(p, emoji, source, 0, 0);
		}
		result = QVariant(QPixmap::fromImage(std::move(image)));
	}
	_cache.emplace(name, result);
	return result;
}

void ReplaceEmoji(QTextDocument *doc) {
	const auto fontHeight = QFontMetrics(doc->defaultFont()).height();
	auto cursor = QTextCursor(doc);
	auto block = doc->begin();
	while (block.isValid()) {
		auto text = block.text();
		auto start = text.constData();
		auto end = start + text.size();
		auto ch = start;
		while (ch < end) {
			auto emojiLength = 0;
			const auto emoji = Ui::Emoji::Find(ch, end, &emojiLength);
			if (!emoji) {
				++ch;
				continue;
			}
			const auto pos = block.position() + int(ch - start);
			cursor.setPosition(pos);
			cursor.setPosition(
				pos + emojiLength,
				QTextCursor::KeepAnchor);

			auto format = QTextImageFormat();
			format.setName(emoji->toUrl());
			format.setWidth(fontHeight);
			format.setHeight(fontHeight);
			format.setVerticalAlignment(
				QTextCharFormat::AlignBaseline);
			cursor.insertImage(format);

			block = doc->findBlock(pos);
			text = block.text();
			start = text.constData();
			end = start + text.size();
			ch = start + (pos - block.position()) + 1;
			continue;
		}
		block = block.next();
	}
}

QString RecoverTextFromDocument(QTextDocument *doc) {
	auto result = QString();
	auto block = doc->begin();
	while (block.isValid()) {
		if (block != doc->begin()) {
			result += '\n';
		}
		auto it = block.begin();
		while (!it.atEnd()) {
			const auto fragment = it.fragment();
			if (!fragment.isValid()) {
				++it;
				continue;
			}
			const auto text = fragment.text();
			const auto format = fragment.charFormat();
			for (const auto &ch : text) {
				if (ch == QChar::ObjectReplacementCharacter) {
					if (format.isImageFormat()) {
						const auto name = format.toImageFormat().name();
						if (const auto emoji = Ui::Emoji::FromUrl(name)) {
							result += emoji->text();
							continue;
						}
					}
				}
				result += ch;
			}
			++it;
		}
		block = block.next();
	}
	return result;
}

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

	const auto m = ComputeMetrics(_text, _fontSize, _imageSize, _textStyle);
	const auto pixWidth = m.contentWidth + 2 * m.padding;
	const auto pixHeight = m.contentHeight + 2 * m.padding;

	const auto font = TextFont(_fontSize);

	auto processedText = _text;
	processedText.replace('\n', QChar::LineSeparator);

	auto option = QTextOption();
	option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

	auto layout = QTextLayout(processedText, font);
	layout.setTextOption(option);

	auto emojiFormats = QList<QTextLayout::FormatRange>();
	{
		auto pos = 0;
		const auto begin = processedText.constData();
		const auto end = begin + processedText.size();
		while (pos < processedText.size()) {
			auto emojiLen = 0;
			const auto emoji = Ui::Emoji::Find(
				begin + pos,
				end,
				&emojiLen);
			if (emoji) {
				auto fmt = QTextCharFormat();
				fmt.setForeground(QColor(0, 0, 0, 0));
				emojiFormats.append({ pos, emojiLen, fmt });
				pos += emojiLen;
			} else {
				++pos;
			}
		}
	}
	layout.setFormats(emojiFormats);

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
	const auto hasBackground =
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

		if (hasBackground) {
			const auto bgPath = BuildConnectedBackground(
				layout,
				m.contentWidth,
				m.padding,
				_fontSize);
			if (_textStyle == TextStyle::SemiTransparent) {
				auto opaque = bgColor;
				opaque.setAlpha(255);
				auto mask = QPixmap(pixmap.size());
				mask.setDevicePixelRatio(dpr);
				mask.fill(Qt::transparent);
				{
					auto mp = QPainter(&mask);
					auto mhq = PainterHighQualityEnabler(mp);
					mp.setPen(Qt::NoPen);
					mp.setBrush(opaque);
					mp.drawPath(bgPath);
				}
				p.setOpacity(bgColor.alphaF());
				p.drawPixmap(0, 0, mask);
				p.setOpacity(1.0);
			} else {
				p.setPen(Qt::NoPen);
				p.setBrush(bgColor);
				p.drawPath(bgPath);
			}
		}

		const auto lineShift = _fontSize / 7.f;
		const auto lineCount = layout.lineCount();
		p.setPen(textColor);
		for (auto i = 0; i < lineCount; ++i) {
			const auto line = layout.lineAt(i);
			const auto xOffset =
				(m.contentWidth - line.naturalTextWidth()) / 2.;
			const auto yShift = (i < lineCount - 1) ? -lineShift : 0.f;
			line.draw(
				&p,
				QPointF(m.padding + xOffset, m.padding + yShift));
		}

		p.setRenderHint(QPainter::SmoothPixmapTransform, true);
		const auto factor = style::DevicePixelRatio();
		const auto source = Ui::Emoji::GetSizeLarge();
		const auto sourceLogical = source / float64(factor);
		const auto emojiSize = float64(QFontMetrics(font).height());
		const auto emojiScale = emojiSize / sourceLogical;
		for (auto i = 0; i < lineCount; ++i) {
			const auto line = layout.lineAt(i);
			const auto xOffset =
				(m.contentWidth - line.naturalTextWidth()) / 2.;
			const auto yShift = (i < lineCount - 1) ? -lineShift : 0.f;
			const auto lineStart = line.textStart();
			const auto lineText = processedText.mid(
				lineStart,
				line.textLength());
			auto pos = 0;
			while (pos < lineText.size()) {
				auto emojiLen = 0;
				const auto emoji = Ui::Emoji::Find(
					lineText.constData() + pos,
					lineText.constData() + lineText.size(),
					&emojiLen);
				if (!emoji) {
					++pos;
					continue;
				}
				const auto x = line.cursorToX(lineStart + pos);
				const auto nextX = line.cursorToX(
					lineStart + pos + emojiLen);
				const auto glyphWidth = float64(nextX - x);
				const auto drawX = m.padding
					+ xOffset
					+ x
					+ (glyphWidth - emojiSize) / 2.;
				const auto drawY = m.padding
					+ yShift
					+ line.y()
					+ (line.height() - emojiSize) / 2.;
				p.save();
				p.translate(drawX, drawY);
				p.scale(emojiScale, emojiScale);
				Ui::Emoji::Draw(p, emoji, source, 0, 0);
				p.restore();
				pos += emojiLen;
			}
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
		const QSize &imageSize,
		TextStyle style) {
	if (text.isEmpty()) {
		return {};
	}
	auto processedText = text;
	processedText.replace('\n', QChar::LineSeparator);
	const auto m = ComputeMetrics(processedText, fontSize, imageSize, style);
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
	const auto natural = computeContentSize(
		_text,
		_fontSize,
		_imageSize,
		_textStyle);
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
