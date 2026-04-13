/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/sticker_creator_box.h"

#include "api/api_stickers_creator.h"
#include "chat_helpers/compose/compose_show.h"
#include "chat_helpers/emoji_list_widget.h"
#include "chat_helpers/tabbed_selector.h"
#include "core/file_utilities.h"
#include "editor/editor_layer_widget.h"
#include "editor/photo_editor.h"
#include "editor/photo_editor_common.h"
#include "editor/scene/scene.h"
#include "editor/scene/scene_item_image.h"
#include "info/channel_statistics/boosts/giveaway/boost_badge.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/abstract_button.h"
#include "ui/emoji_config.h"
#include "ui/image/image.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"

#include <QtCore/QBuffer>
#include <QtGui/QImageReader>

namespace {

constexpr auto kStickerSide = 512;
constexpr auto kPreviewSide = 256;
constexpr auto kWebpQuality = 95;
constexpr auto kMaxEmojis = 7;

[[nodiscard]] QImage LoadImageFromFile(const QString &path) {
	auto reader = QImageReader(path);
	reader.setAutoTransform(true);
	auto image = reader.read();
	if (image.format() != QImage::Format_ARGB32_Premultiplied
		&& image.format() != QImage::Format_ARGB32) {
		image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	return image;
}

class PreviewWidget final : public Ui::RpWidget {
public:
	PreviewWidget(QWidget *parent, QImage image)
	: RpWidget(parent)
	, _image(std::move(image)) {
		resize(kPreviewSide, kPreviewSide);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto target = QRect(0, 0, width(), height());
		p.drawImage(target, _image);
	}

private:
	const QImage _image;

};

void ShowEmojiPickerBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<ChatHelpers::Show> show,
		Fn<void(EmojiPtr)> chosen) {
	box->setTitle(tr::lng_stickers_pack_choose_emoji_title());
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_stickers_pack_choose_emoji_about(),
			st::boxLabel));

	const auto selector = box->addRow(
		object_ptr<ChatHelpers::EmojiListWidget>(
			box,
			ChatHelpers::EmojiListDescriptor{
				.show = show,
				.mode = ChatHelpers::EmojiListMode::TopicIcon,
				.paused = [] { return false; },
				.st = &st::reactPanelEmojiPan,
			}),
		QMargins());
	selector->refreshEmoji();

	selector->chosen(
	) | rpl::on_next([=, weak = base::make_weak(box.get())](
			const ChatHelpers::EmojiChosen &result) {
		if (chosen) {
			chosen(result.emoji);
		}
		if (const auto strong = weak.get()) {
			strong->closeBox();
		}
	}, selector->lifetime());

	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->setMaxHeight(st::stickersMaxHeight);
}

class ActionButton final : public Ui::AbstractButton {
public:
	enum class Kind {
		Plus,
		Minus,
	};

	ActionButton(QWidget *parent, Kind kind)
	: AbstractButton(parent)
	, _kind(kind) {
		resize(
			st::stickersCreatorActionSize,
			st::stickersCreatorActionSize);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = st::stickersCreatorActionSize;
		const auto rect = QRect(0, 0, size, size);
		const auto bgAlpha = isOver() ? 0.22 : 0.12;
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(st::windowSubTextFg->c, bgAlpha));
		p.drawEllipse(rect);
		p.setBrush(st::windowSubTextFg->c);
		const auto thickness = st::stickersAddCellPlusThickness;
		const auto glyphHalf = st::stickersCreatorEmojiSize / 4;
		const auto center = rect.center() + QPointF(0.5, 0.5);
		const auto horizontal = QRectF(
			center.x() - glyphHalf,
			center.y() - thickness / 2.,
			glyphHalf * 2,
			thickness);
		const auto radius = thickness / 2.;
		p.drawRoundedRect(horizontal, radius, radius);
		if (_kind == Kind::Plus) {
			const auto vertical = QRectF(
				center.x() - thickness / 2.,
				center.y() - glyphHalf,
				thickness,
				glyphHalf * 2);
			p.drawRoundedRect(vertical, radius, radius);
		}
	}

private:
	const Kind _kind;

};

class EmojiPickerRow final : public Ui::RpWidget {
public:
	EmojiPickerRow(
		QWidget *parent,
		std::shared_ptr<ChatHelpers::Show> show);

	[[nodiscard]] QString value() const;
	[[nodiscard]] rpl::producer<int> countValue() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void relayout();
	void openPicker();
	void addEmoji(EmojiPtr emoji);
	void removeLast();
	[[nodiscard]] int rowContentWidth() const;

	const std::shared_ptr<ChatHelpers::Show> _show;
	std::vector<EmojiPtr> _emojis;
	rpl::variable<int> _count;
	ActionButton *_plus = nullptr;
	ActionButton *_minus = nullptr;

};

EmojiPickerRow::EmojiPickerRow(
	QWidget *parent,
	std::shared_ptr<ChatHelpers::Show> show)
: RpWidget(parent)
, _show(std::move(show))
, _plus(Ui::CreateChild<ActionButton>(this, ActionButton::Kind::Plus))
, _minus(Ui::CreateChild<ActionButton>(this, ActionButton::Kind::Minus)) {
	resize(width(), st::stickersCreatorRowHeight);
	_plus->setClickedCallback([=] { openPicker(); });
	_minus->setClickedCallback([=] { removeLast(); });
	_minus->hide();
}

QString EmojiPickerRow::value() const {
	auto result = QString();
	for (const auto emoji : _emojis) {
		result.append(emoji->text());
	}
	return result;
}

rpl::producer<int> EmojiPickerRow::countValue() const {
	return _count.value();
}

int EmojiPickerRow::rowContentWidth() const {
	const auto emojiPart = int(_emojis.size())
		* (st::stickersCreatorEmojiSize + st::stickersCreatorEmojiSkip);
	const auto plusPart = st::stickersCreatorActionSize;
	const auto minusPart = _emojis.empty()
		? 0
		: (st::stickersCreatorActionSize
			+ st::stickersCreatorActionMargin);
	const auto margin = _emojis.empty()
		? 0
		: st::stickersCreatorActionMargin;
	return emojiPart + plusPart + minusPart + margin;
}

void EmojiPickerRow::relayout() {
	const auto contentWidth = rowContentWidth();
	const auto offsetX = (width() - contentWidth) / 2;
	const auto centerY = height() / 2;

	auto x = offsetX;

	if (!_emojis.empty()) {
		_minus->move(
			x,
			centerY - st::stickersCreatorActionSize / 2);
		_minus->show();
		x += st::stickersCreatorActionSize + st::stickersCreatorActionMargin;
	} else {
		_minus->hide();
	}

	x += int(_emojis.size())
		* (st::stickersCreatorEmojiSize + st::stickersCreatorEmojiSkip);
	if (!_emojis.empty()) {
		x += st::stickersCreatorActionMargin
			- st::stickersCreatorEmojiSkip;
	}

	_plus->move(x, centerY - st::stickersCreatorActionSize / 2);
	_plus->setVisible(int(_emojis.size()) < kMaxEmojis);

	update();
}

void EmojiPickerRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (_emojis.empty()) {
		return;
	}
	const auto contentWidth = rowContentWidth();
	const auto offsetX = (width() - contentWidth) / 2;
	const auto centerY = height() / 2;

	auto x = offsetX
		+ st::stickersCreatorActionSize
		+ st::stickersCreatorActionMargin;
	const auto y = centerY - st::stickersCreatorEmojiSize / 2;
	for (const auto emoji : _emojis) {
		Ui::Emoji::Draw(p, emoji, st::stickersCreatorEmojiSize, x, y);
		x += st::stickersCreatorEmojiSize + st::stickersCreatorEmojiSkip;
	}
}

void EmojiPickerRow::resizeEvent(QResizeEvent *e) {
	relayout();
}

void EmojiPickerRow::openPicker() {
	const auto addOne = crl::guard(this, [=](EmojiPtr emoji) {
		addEmoji(emoji);
	});
	_show->showBox(Box(ShowEmojiPickerBox, _show, addOne));
}

void EmojiPickerRow::addEmoji(EmojiPtr emoji) {
	if (!emoji || int(_emojis.size()) >= kMaxEmojis) {
		return;
	}
	_emojis.push_back(emoji);
	_count = int(_emojis.size());
	relayout();
}

void EmojiPickerRow::removeLast() {
	if (_emojis.empty()) {
		return;
	}
	_emojis.pop_back();
	_count = int(_emojis.size());
	relayout();
}

void OpenPhotoEditorForSticker(
		std::shared_ptr<ChatHelpers::Show> show,
		QImage image,
		Fn<void(QImage&&)> onDone) {
	if (image.isNull()) {
		show->showToast(tr::lng_stickers_create_open_failed(tr::now));
		return;
	}
	const auto sessionController = show->resolveWindow();
	if (!sessionController) {
		show->showToast(tr::lng_stickers_create_open_failed(tr::now));
		return;
	}
	const auto windowController = &sessionController->window();
	const auto parentWidget = sessionController->widget();

	if ((image.width() > 10 * image.height())
		|| (image.height() > 10 * image.width())) {
		show->showToast(tr::lng_stickers_create_open_failed(tr::now));
		return;
	}

	auto canvas = QImage(
		kStickerSide,
		kStickerSide,
		QImage::Format_ARGB32_Premultiplied);
	canvas.fill(Qt::transparent);
	const auto baseImage = std::make_shared<Image>(std::move(canvas));

	auto scene = std::make_shared<Editor::Scene>(
		QRectF(0, 0, kStickerSide, kStickerSide));

	const auto userPixmap = QPixmap::fromImage(std::move(image));
	const auto userSize = userPixmap.size();
	const auto fitted = userSize.scaled(
		QSize(kStickerSide, kStickerSide),
		Qt::KeepAspectRatio);
	auto itemData = Editor::ItemBase::Data{
		.initialZoom = 1.0,
		.zPtr = scene->lastZ(),
		.size = fitted.width(),
		.x = kStickerSide / 2,
		.y = kStickerSide / 2,
		.imageSize = userSize,
	};
	auto imageItem = std::make_shared<Editor::ItemImage>(
		QPixmap(userPixmap),
		std::move(itemData));
	scene->addItem(std::move(imageItem));

	auto modifications = Editor::PhotoModifications{
		.crop = QRect(0, 0, kStickerSide, kStickerSide),
		.paint = std::move(scene),
	};

	auto editor = base::make_unique_q<Editor::PhotoEditor>(
		parentWidget,
		windowController,
		baseImage,
		std::move(modifications),
		Editor::EditorData{
			.exactSize = QSize(kStickerSide, kStickerSide),
			.cropType = Editor::EditorData::CropType::RoundedRect,
			.keepAspectRatio = true,
		});
	const auto raw = editor.get();

	auto applyModifications = [=, done = std::move(onDone)](
			const Editor::PhotoModifications &mods) mutable {
		auto result = Editor::ImageModified(baseImage->original(), mods);
		if (result.size() != QSize(kStickerSide, kStickerSide)) {
			result = result.scaled(
				kStickerSide,
				kStickerSide,
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
		}
		done(std::move(result));
	};

	auto layer = std::make_unique<Editor::LayerWidget>(
		parentWidget,
		std::move(editor));
	Editor::InitEditorLayer(layer.get(), raw, std::move(applyModifications));
	windowController->showLayer(
		std::move(layer),
		Ui::LayerOption::KeepOther);
}

} // namespace

StickerCreatorBox::StickerCreatorBox(
	QWidget*,
	std::shared_ptr<ChatHelpers::Show> show,
	StickerSetIdentifier set,
	QImage image,
	Fn<void(MTPmessages_StickerSet)> done)
: _show(std::move(show))
, _session(&_show->session())
, _set(std::move(set))
, _image(std::move(image))
, _done(std::move(done)) {
}

StickerCreatorBox::~StickerCreatorBox() = default;

void StickerCreatorBox::prepare() {
	setTitle(tr::lng_stickers_create_image_title());

	const auto inner = setInnerWidget(
		object_ptr<Ui::VerticalLayout>(this));

	const auto previewHolder = inner->add(
		object_ptr<Ui::RpWidget>(inner),
		QMargins(0, st::boxRowPadding.left(), 0, st::boxRowPadding.left()),
		style::al_top);
	previewHolder->resize(st::boxWideWidth, kPreviewSide);
	const auto preview = Ui::CreateChild<PreviewWidget>(
		previewHolder,
		_image);
	previewHolder->widthValue(
	) | rpl::on_next([=](int width) {
		preview->move((width - kPreviewSide) / 2, 0);
	}, preview->lifetime());

	inner->add(
		object_ptr<Ui::FlatLabel>(
			inner,
			tr::lng_stickers_create_choose_emoji(),
			st::boxLabel),
		st::boxRowPadding);

	const auto emojiRow = inner->add(
		object_ptr<EmojiPickerRow>(inner, _show),
		QMargins(0, 0, 0, st::boxRowPadding.left()));
	emojiRow->resize(st::boxWideWidth, st::stickersCreatorRowHeight);
	_emojiValue = [=] { return emojiRow->value(); };

	const auto addButton = this->addButton(
		rpl::conditional(
			_uploading.value(),
			rpl::single(QString()),
			tr::lng_box_done()),
		[=] { startUpload(); });
	this->addButton(tr::lng_cancel(), [=] { closeBox(); });

	{
		using namespace Info::Statistics;
		const auto loadingAnimation = InfiniteRadialAnimationWidget(
			addButton,
			addButton->height() / 2,
			&st::editStickerSetNameLoading);
		AddChildToWidgetCenter(addButton, loadingAnimation);
		loadingAnimation->showOn(_uploading.value());
	}

	setDimensionsToContent(st::boxWideWidth, inner);

	boxClosing(
	) | rpl::on_next([=, this] {
		_upload = nullptr;
	}, lifetime());
}

QByteArray StickerCreatorBox::encodeWebp() const {
	auto image = _image;
	if (image.size() != QSize(kStickerSide, kStickerSide)) {
		image = image.scaled(
			kStickerSide,
			kStickerSide,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}
	if (image.format() != QImage::Format_ARGB32) {
		image = image.convertToFormat(QImage::Format_ARGB32);
	}
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "WEBP", kWebpQuality);
	return bytes;
}

void StickerCreatorBox::startUpload() {
	if (_uploading.current()) {
		return;
	}
	const auto emoji = _emojiValue ? _emojiValue() : QString();
	if (emoji.isEmpty()) {
		_show->showToast(tr::lng_stickers_create_emoji_required(tr::now));
		return;
	}
	const auto bytes = encodeWebp();
	if (bytes.isEmpty()) {
		_show->showToast(tr::lng_stickers_create_upload_failed(tr::now));
		return;
	}

	_uploading = true;
	_upload = std::make_unique<Api::StickerUpload>(
		_session,
		_set,
		bytes,
		emoji);

	const auto show = _show;
	const auto doneCallback = _done;
	_upload->start(
		crl::guard(this, [=, this](MTPmessages_StickerSet result) {
			_upload = nullptr;
			_uploading = false;
			show->showToast(tr::lng_stickers_create_added(tr::now));
			if (doneCallback) {
				doneCallback(result);
			}
			closeBox();
		}),
		crl::guard(this, [=, this](QString err) {
			_upload = nullptr;
			_uploading = false;
			show->showToast(err.isEmpty()
				? tr::lng_stickers_create_upload_failed(tr::now)
				: err);
		}));
}

namespace Api {

void OpenCreateStickerFlow(
		std::shared_ptr<ChatHelpers::Show> show,
		StickerSetIdentifier set,
		Fn<void(MTPmessages_StickerSet)> done) {
	const auto parent = QPointer<QWidget>(show->toastParent());

	const auto onChosen = [=, set = std::move(set), done = std::move(done)](
			FileDialog::OpenResult &&result) mutable {
		if (result.paths.isEmpty() && result.remoteContent.isEmpty()) {
			return;
		}
		const auto path = result.paths.isEmpty()
			? QString()
			: result.paths.front();
		auto image = path.isEmpty()
			? QImage::fromData(result.remoteContent)
			: LoadImageFromFile(path);
		OpenPhotoEditorForSticker(
			show,
			std::move(image),
			[=, set = std::move(set), done = std::move(done)](
					QImage &&prepared) mutable {
				show->showBox(Box<StickerCreatorBox>(
					show,
					std::move(set),
					std::move(prepared),
					std::move(done)));
			});
	};

	FileDialog::GetOpenPath(
		parent,
		tr::lng_stickers_create_choose_image(tr::now),
		FileDialog::ImagesFilter(),
		std::move(onChosen));
}

} // namespace Api
