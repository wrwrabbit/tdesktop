/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/test_main.h"

#include "base/invoke_queued.h"
#include "base/integration.h"
#include "ui/effects/animations.h"
#include "ui/text/text.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/rp_window.h"
#include "ui/painter.h"

#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QThread>
#include <QDir>
#include <QImage>

namespace Test {

QString name() {
	return u"text"_q;
}

void test(not_null<Ui::RpWindow*> window, not_null<Ui::RpWidget*> body) {
	auto text = new Ui::Text::String(scale(64));

	const auto like = QString::fromUtf8("\xf0\x9f\x91\x8d");
	const auto dislike = QString::fromUtf8("\xf0\x9f\x91\x8e");
	const auto hebrew = QString() + QChar(1506) + QChar(1460) + QChar(1489);

	auto data = TextWithEntities();
	data.append(
		u"Lorem ipsu7m dolor sit amet, "_q
	).append(Ui::Text::Bold(
		u"consectetur adipiscing: "_q
		+ hebrew
		+ u" elit, sed do eiusmod tempor incididunt test"_q
	)).append(Ui::Text::Wrapped(Ui::Text::Bold(
		u". ut labore et dolore magna aliqua."_q
		+ like
		+ dislike
		+ u"Ut enim ad minim veniam"_q
	), EntityType::Italic)).append(
		u", quis nostrud exercitation ullamco laboris nisi ut aliquip ex \
ea commodo consequat. Duis aute irure dolor in reprehenderit in \
voluptate velit esse cillum dolore eu fugiat nulla pariatur. \
Excepteur sint occaecat cupidatat non proident, sunt in culpa \
qui officia deserunt mollit anim id est laborum."_q
).append(u"\n\n"_q).append(hebrew).append("\n\n").append(
		"Duisauteiruredolorinreprehenderitinvoluptatevelitessecillumdoloreeu\
fugiatnullapariaturExcepteursintoccaecatcupidatatnonproident, sunt in culpa \
qui officia deserunt mollit anim id est laborum. \
Duisauteiruredolorinreprehenderitinvoluptate.");
	data.append(data);
	data.append(u"\n\nInline formula image: "_q);
	data.append(QChar::ObjectReplacementCharacter);
	data.append(u" and fallback: "_q);
	data.append(QChar::ObjectReplacementCharacter);
	data.append(u" and provider image: "_q);
	data.append(QChar::ObjectReplacementCharacter);
	data.append(u" and provider fallback: "_q);
	data.append(QChar::ObjectReplacementCharacter);
	data.append(u" inside wrapped text."_q);

	auto formulaImage = QImage(
		QSize(scale(56), scale(24)),
		QImage::Format_ARGB32_Premultiplied);
	formulaImage.fill(Qt::transparent);
	{
		auto p = QPainter(&formulaImage);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(32, 96, 192, 48));
		p.drawRoundedRect(
			QRect(0, 0, formulaImage.width(), formulaImage.height()),
			scale(6),
			scale(6));
		p.setPen(QColor(32, 96, 192));
		p.drawText(
			QRect(0, 0, formulaImage.width(), formulaImage.height()),
			Qt::AlignCenter,
			u"a / b"_q);
	}

	const auto fallbackSource = u"$\\int_0^1 x^2 \\, dx$"_q;
	const auto providerImageSource = u"$\\sqrt{x + 1}$"_q;
	const auto providerFallbackSource = u"$\\sum_{n=1}^{4} n$"_q;
	auto inlineObjectPositions = std::vector<int>();
	for (auto i = data.text.indexOf(QChar::ObjectReplacementCharacter);
		i >= 0;
		i = data.text.indexOf(QChar::ObjectReplacementCharacter, i + 1)) {
		inlineObjectPositions.push_back(i);
	}
	Expects(inlineObjectPositions.size() == 4);
	auto inlineObjects = std::vector<Ui::Text::InlineObjectPlacement>();
	inlineObjects.push_back({
		.position = inlineObjectPositions[0],
		.object = {
			.width = formulaImage.width(),
			.align = Ui::Text::InlineObjectVerticalAlign::CenterInText,
			.copySource = u"$\\frac{a}{b}$"_q,
			.fallbackText = u"$\\frac{a}{b}$"_q,
			.image = formulaImage,
		},
	});
	inlineObjects.push_back({
		.position = inlineObjectPositions[1],
		.object = {
			.width = st::defaultTextStyle.font->width(fallbackSource),
			.align = Ui::Text::InlineObjectVerticalAlign::CenterInText,
			.copySource = fallbackSource,
			.fallbackText = fallbackSource,
		},
	});
	inlineObjects.push_back({
		.position = inlineObjectPositions[2],
		.object = {
			.width = formulaImage.width(),
			.align = Ui::Text::InlineObjectVerticalAlign::CenterInText,
			.copySource = providerImageSource,
			.fallbackText = providerImageSource,
			.imageProvider = [formulaImage](int) {
				return formulaImage;
			},
		},
	});
	inlineObjects.push_back({
		.position = inlineObjectPositions[3],
		.object = {
			.width = st::defaultTextStyle.font->width(providerFallbackSource),
			.align = Ui::Text::InlineObjectVerticalAlign::CenterInText,
			.fallbackText = providerFallbackSource,
			.imageProvider = [](int) {
				return QImage();
			},
		},
	});
	auto context = Ui::Text::MarkedContext();
	context.inlineObjects = Ui::Text::InlineObjectPlacements(
		inlineObjects.data(),
		inlineObjects.size());
	text->setMarkedText(st::defaultTextStyle, data, kMarkupTextOptions, context);
	const auto copyText = text->toString();
	Expects(copyText.contains(u"$\\frac{a}{b}$"_q));
	Expects(copyText.contains(fallbackSource));
	Expects(copyText.contains(providerImageSource));
	Expects(copyText.contains(providerFallbackSource));

	body->paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = QPainter(body);
		auto hq = PainterHighQualityEnabler(p);

		const auto width = body->width();
		const auto height = body->height();

		p.fillRect(clip, QColor(255, 255, 255));

		const auto border = QColor(0, 128, 0, 16);
		auto skip = scale(20);
		p.fillRect(0, 0, skip, height, border);
		p.fillRect(skip, 0, width - skip, skip, border);
		p.fillRect(skip, height - skip, width - skip, skip, border);
		p.fillRect(width - skip, skip, skip, height - skip * 2, border);

		const auto inner = body->rect().marginsRemoved(
			{ skip, skip, skip, skip });

		p.fillRect(QRect{
			inner.x(),
			inner.y(),
			inner.width(),
			text->countHeight(inner.width())
		}, QColor(128, 0, 0, 16));

		auto lines = text->countLinesGeometry(inner.width());
		auto top = 0;
		for (const auto &line : lines) {
			const auto lineHeight = std::max(line.bottom - top, 1);
			p.fillRect(QRect{
				inner.x() + line.left,
				inner.y() + top,
				line.width,
				lineHeight
			}, QColor(0, 0, 128, 16));
			top = line.bottom;
		}

		text->draw(p, {
			.position = inner.topLeft(),
			.availableWidth = inner.width(),
		});

		//const auto to = QRectF(
		//	inner.marginsRemoved({ 0, inner.height() / 2, 0, 0 }));
		//const auto t = u"hi\n\nguys"_q;
		//p.drawText(to, t);
	}, body->lifetime());
}

} // namespace Test
