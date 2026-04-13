/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/sticker_creator_box.h"

#include "api/api_stickers_creator.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/file_utilities.h"
#include "editor/editor_layer_widget.h"
#include "editor/photo_editor.h"
#include "editor/photo_editor_common.h"
#include "editor/scene/scene.h"
#include "editor/scene/scene_item_image.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/emoji_config.h"
#include "ui/image/image.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/box_content.h"
#include "ui/layers/layer_widget.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"

#include <QtCore/QBuffer>
#include <QtGui/QImageReader>

namespace {

constexpr auto kStickerSide = 512;
constexpr auto kPreviewSide = 256;
constexpr auto kWebpQuality = 95;

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
	inner->resizeToWidth(st::boxWideWidth);

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

	_emojiField = inner->add(
		object_ptr<Ui::InputField>(
			inner,
			st::editStickerSetNameField,
			tr::lng_stickers_create_choose_emoji(),
			QString()),
		st::boxRowPadding);
	_emojiField->setMaxLength(32);

	_status = inner->add(
		object_ptr<Ui::FlatLabel>(
			inner,
			QString(),
			st::boxLabel),
		st::boxRowPadding);
	_status->hide();

	_addButton = addButton(
		tr::lng_box_done(),
		[=] { startUpload(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

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

void StickerCreatorBox::setState(State state) {
	_state = state;
	const auto uploading = (state == State::Uploading);
	_emojiField->setEnabled(!uploading);
	if (_addButton) {
		_addButton->setDisabled(uploading);
	}
	if (uploading) {
		_status->show();
		_status->setText(tr::lng_stickers_create_uploading(tr::now));
	} else {
		_status->setText(QString());
		_status->hide();
	}
}

void StickerCreatorBox::startUpload() {
	if (_state == State::Uploading) {
		return;
	}
	const auto text = _emojiField->getLastText().trimmed();
	auto emojiLen = 0;
	const auto emoji = Ui::Emoji::Find(text, &emojiLen);
	if (!emoji || emojiLen != text.size()) {
		_emojiField->showError();
		_show->showToast(tr::lng_stickers_create_emoji_required(tr::now));
		return;
	}
	const auto bytes = encodeWebp();
	if (bytes.isEmpty()) {
		_show->showToast(tr::lng_stickers_create_upload_failed(tr::now));
		return;
	}

	setState(State::Uploading);
	_upload = std::make_unique<Api::StickerUpload>(
		_session,
		_set,
		bytes,
		emoji->text());

	const auto show = _show;
	const auto doneCallback = _done;
	_upload->start(
		crl::guard(this, [=, this](MTPmessages_StickerSet result) {
			_upload = nullptr;
			show->showToast(tr::lng_stickers_create_added(tr::now));
			if (doneCallback) {
				doneCallback(result);
			}
			closeBox();
		}),
		crl::guard(this, [=, this](QString err) {
			_upload = nullptr;
			setState(State::ChooseEmoji);
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
