/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/test_main.h"

#include "iv/markdown/iv_markdown_prepare.h"

#include "base/invoke_queued.h"
#include "base/integration.h"
#include "ui/effects/animations.h"
#include "ui/text/text.h"
#include "ui/text/text_custom_emoji.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/rp_window.h"
#include "ui/painter.h"

#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QThread>
#include <QDir>
#include <QImage>

#include <algorithm>
#include <utility>

namespace Test {

namespace {

[[nodiscard]] bool HasEntityType(
		const EntitiesInText &entities,
		EntityType type) {
	for (const auto &entity : entities) {
		if (entity.type() == type) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QImage MakeObjectImage(
		QSize size,
		QColor background,
		const QString &label) {
	auto image = QImage(size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto painter = QPainter(&image);
	auto hq = PainterHighQualityEnabler(painter);
	painter.setPen(Qt::NoPen);
	painter.setBrush(background);
	painter.drawRoundedRect(
		QRect(QPoint(), size),
		scale(6),
		scale(6));
	painter.setPen(Qt::black);
	painter.drawText(QRect(QPoint(), size), Qt::AlignCenter, label);
	return image;
}

class FormulaLikeObject final : public Ui::Text::CustomEmoji {
public:
	FormulaLikeObject(QString entityData, QString replacementText, QImage image);

	int width() override;
	QString entityData() override;
	std::optional<Ui::Text::CustomEmojiVerticalMetrics> vertical(
		const style::TextStyle &) override;
	QString replacementText() override;
	Ui::Text::CustomEmojiSemantics semantics() override;
	void paint(QPainter &p, const Context &context) override;
	void unload() override;
	bool ready() override;
	bool readyInDefaultState() override;

private:
	QString _entityData;
	QString _replacementText;
	QImage _image;

};

FormulaLikeObject::FormulaLikeObject(
	QString entityData,
	QString replacementText,
	QImage image)
: _entityData(std::move(entityData))
, _replacementText(std::move(replacementText))
, _image(std::move(image)) {
}

int FormulaLikeObject::width() {
	return _image.width();
}

QString FormulaLikeObject::entityData() {
	return _entityData;
}

std::optional<Ui::Text::CustomEmojiVerticalMetrics>
FormulaLikeObject::vertical(const style::TextStyle &) {
	const auto height = _image.height();
	const auto descent = std::max(height / 5, 1);
	return Ui::Text::CustomEmojiVerticalMetrics{
		.ascent = height - descent,
		.descent = descent,
	};
}

QString FormulaLikeObject::replacementText() {
	return _replacementText;
}

Ui::Text::CustomEmojiSemantics FormulaLikeObject::semantics() {
	return {
		.isEmoji = false,
		.isRealCustomEmoji = false,
		.exportEntity = false,
		.unloadPersistentAnimation = false,
		.allowCustomEmojiClick = false,
	};
}

void FormulaLikeObject::paint(QPainter &p, const Context &context) {
	p.drawImage(context.position, _image);
}

void FormulaLikeObject::unload() {
}

bool FormulaLikeObject::ready() {
	return true;
}

bool FormulaLikeObject::readyInDefaultState() {
	return true;
}

} // namespace

QString name() {
	return u"text"_q;
}

void test(not_null<Ui::RpWindow*> window, not_null<Ui::RpWidget*> body) {
	(void)window;

	const auto formulaEntityData = Iv::Markdown::SerializeInlineTextObjectEntity({
		.kind = Iv::Markdown::InlineTextObjectKind::Formula,
		.data = Iv::Markdown::InlineTextObjectFormulaData{
			.copySource = u"$\\frac{a}{b}$"_q,
			.trimmedTex = u"\\frac{a}{b}"_q,
		},
	});
	const auto controlEntityData = u"test-custom-emoji"_q;
	const auto formulaImage = MakeObjectImage(
		QSize(scale(64), scale(28)),
		QColor(32, 96, 192, 48),
		u"a / b"_q);
	const auto controlImage = MakeObjectImage(
		QSize(scale(24), scale(24)),
		QColor(224, 176, 32, 160),
		u"*"_q);

	auto context = Ui::Text::MarkedContext();
	context.customEmojiFactory = [
		formulaImage,
		controlImage
	](QStringView data, const Ui::Text::MarkedContext &)
	-> std::unique_ptr<Ui::Text::CustomEmoji> {
		if (const auto parsed = Iv::Markdown::ParseInlineTextObjectEntity(
				data.toString())) {
			if (parsed->kind == Iv::Markdown::InlineTextObjectKind::Formula) {
				if (const auto formula = std::get_if<
						Iv::Markdown::InlineTextObjectFormulaData>(&parsed->data)) {
					return std::make_unique<FormulaLikeObject>(
						data.toString(),
						formula->copySource,
						formulaImage);
				}
			}
			return std::unique_ptr<Ui::Text::CustomEmoji>();
		}
		if (data == u"test-custom-emoji"_q) {
			return std::make_unique<Ui::Text::PaletteDependentCustomEmoji>(
				[controlImage] {
					return controlImage;
				},
				data.toString());
		}
		return std::unique_ptr<Ui::Text::CustomEmoji>();
	};

	auto formulaData = TextWithEntities();
	formulaData.append(u"Alpha "_q);
	const auto formulaPosition = formulaData.text.size();
	formulaData.append(QChar::ObjectReplacementCharacter);
	formulaData.entities.push_back(EntityInText(
		EntityType::CustomEmoji,
		formulaPosition,
		1,
		formulaEntityData));
	formulaData.append(u" omega"_q);

	const auto formulaText = new Ui::Text::String(scale(64));
	formulaText->setMarkedText(
		st::defaultTextStyle,
		formulaData,
		kMarkupTextOptions,
		context);

	const auto formulaCollapsedText = Ui::Text::String(
		st::defaultTextStyle,
		u"Alpha   omega"_q,
		kMarkupTextOptions,
		scale(64));
	const auto objectPlaceholderWidth = st::defaultTextStyle.font->width(u" "_q);
	Expects(
		formulaText->maxWidth()
			>= formulaCollapsedText.maxWidth()
				- objectPlaceholderWidth
				+ formulaImage.width());

	const auto expectedFormulaExport = u"Alpha $\\frac{a}{b}$ omega"_q;
	Expects(formulaText->toString() == expectedFormulaExport);
	const auto formulaMime = formulaText->toTextForMimeData();
	Expects(formulaMime.expanded == expectedFormulaExport);
	Expects(formulaMime.rich.text == expectedFormulaExport);
	Expects(!HasEntityType(formulaMime.rich.entities, EntityType::CustomEmoji));
	const auto formulaRich = formulaText->toTextWithEntities();
	Expects(formulaRich.text == expectedFormulaExport);
	Expects(!HasEntityType(formulaRich.entities, EntityType::CustomEmoji));
	Expects(
		formulaText->adjustSelection(
			TextSelection(uint16(formulaPosition), uint16(formulaPosition)),
			TextSelectType::Words)
		== TextSelection(
			uint16(formulaPosition),
			uint16(formulaPosition + 1)));
	Expects(
		formulaText->adjustSelection(
			TextSelection(uint16(formulaPosition), uint16(formulaPosition + 1)),
			TextSelectType::Paragraphs)
		== TextSelection(
			uint16(formulaPosition),
			uint16(formulaPosition + 1)));
	Expects(!formulaText->hasCustomEmoji());
	Expects(!formulaText->isOnlyCustomEmoji());
	Expects(!formulaText->isIsolatedEmoji());

	auto controlData = TextWithEntities();
	controlData.append(QChar::ObjectReplacementCharacter);
	controlData.entities.push_back(EntityInText(
		EntityType::CustomEmoji,
		0,
		1,
		controlEntityData));

	const auto controlText = new Ui::Text::String(scale(64));
	controlText->setMarkedText(
		st::defaultTextStyle,
		controlData,
		kMarkupTextOptions,
		context);
	Expects(controlText->hasCustomEmoji());
	Expects(controlText->isOnlyCustomEmoji());
	Expects(controlText->isIsolatedEmoji());
	Expects(
		HasEntityType(
			controlText->toTextWithEntities().entities,
			EntityType::CustomEmoji));

	body->paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = QPainter(body);
		p.fillRect(clip, QColor(255, 255, 255));
		const auto left = scale(24);
		const auto top = scale(24);
		const auto width = body->width() - (2 * left);
		formulaText->draw(p, {
			.position = QPoint(left, top),
			.availableWidth = width,
		});
		controlText->draw(p, {
			.position = QPoint(
				left,
				top + formulaText->countHeight(width) + scale(20)),
			.availableWidth = width,
		});
	}, body->lifetime());
}

} // namespace Test
