/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/sticker_creator_box.h"

#include "api/api_stickers_creator.h"
#include "base/event_filter.h"
#include "chat_helpers/compose/compose_show.h"
#include "chat_helpers/tabbed_panel.h"
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
#include "ui/vertical_list.h"
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

class ActionButton final : public Ui::AbstractButton {
public:
	enum class Kind {
		Emoji,
		Delete,
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
		if (isOver()) {
			p.setPen(Qt::NoPen);
			p.setBrush(anim::with_alpha(st::windowSubTextFg->c, 0.12));
			p.drawEllipse(rect);
		}

		if (_kind == Kind::Emoji) {
			const auto &icon = st::stickersCreatorEmojiIcon;
			const auto iconX = (size - icon.width()) / 2;
			const auto iconY = (size - icon.height()) / 2;
			icon.paint(p, iconX, iconY, size);

			const auto line = style::ConvertScaleExact(
				st::historyEmojiCircleLine);
			auto pen = st::windowSubTextFg->p;
			pen.setWidthF(line);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			const auto skipX = icon.width() / 4;
			const auto skipY = icon.height() / 4;
			p.drawEllipse(QRectF(
				iconX + skipX,
				iconY + skipY,
				icon.width() - 2 * skipX,
				icon.height() - 2 * skipY));
		} else {
			paintBackspaceGlyph(p, rect);
		}
	}

private:
	void paintBackspaceGlyph(QPainter &p, QRect rect) {
		const auto glyphW = style::ConvertScaleExact(18.);
		const auto glyphH = style::ConvertScaleExact(13.);
		const auto x = rect.x() + (rect.width() - glyphW) / 2.;
		const auto y = rect.y() + (rect.height() - glyphH) / 2.;
		const auto cornerCut = style::ConvertScaleExact(5.);
		const auto stroke = style::ConvertScaleExact(1.5);

		auto pen = st::windowSubTextFg->p;
		pen.setWidthF(stroke);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);

		auto path = QPainterPath();
		path.moveTo(x, y + glyphH / 2.);
		path.lineTo(x + cornerCut, y);
		path.lineTo(x + glyphW, y);
		path.lineTo(x + glyphW, y + glyphH);
		path.lineTo(x + cornerCut, y + glyphH);
		path.closeSubpath();
		p.drawPath(path);

		const auto cx = x + (cornerCut + glyphW) / 2. + stroke;
		const auto cy = y + glyphH / 2.;
		const auto half = style::ConvertScaleExact(2.5);
		p.drawLine(
			QPointF(cx - half, cy - half),
			QPointF(cx + half, cy + half));
		p.drawLine(
			QPointF(cx - half, cy + half),
			QPointF(cx + half, cy - half));
	}

	const Kind _kind;

};

class EmojiPickerRow final : public Ui::RpWidget {
public:
	EmojiPickerRow(
		QWidget *parent,
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<QWidget*> panelContainer);

	[[nodiscard]] QString value() const;
	[[nodiscard]] rpl::producer<int> countValue() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void relayout();
	void ensurePanel();
	void togglePanel();
	void updatePanelGeometry();
	void addEmoji(EmojiPtr emoji);
	void removeLast();
	[[nodiscard]] int rowContentWidth() const;

	const std::shared_ptr<ChatHelpers::Show> _show;
	const not_null<QWidget*> _panelContainer;
	std::vector<EmojiPtr> _emojis;
	rpl::variable<int> _count;
	ActionButton *_plus = nullptr;
	ActionButton *_minus = nullptr;
	base::unique_qptr<ChatHelpers::TabbedPanel> _panel;

};

EmojiPickerRow::EmojiPickerRow(
	QWidget *parent,
	std::shared_ptr<ChatHelpers::Show> show,
	not_null<QWidget*> panelContainer)
: RpWidget(parent)
, _show(std::move(show))
, _panelContainer(panelContainer)
, _plus(Ui::CreateChild<ActionButton>(this, ActionButton::Kind::Emoji))
, _minus(Ui::CreateChild<ActionButton>(this, ActionButton::Kind::Delete)) {
	resize(width(), st::stickersCreatorRowHeight);
	_plus->setClickedCallback([=] { togglePanel(); });
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
	const auto count = int(_emojis.size());
	const auto plusVisible = (count < kMaxEmojis);
	const auto minusVisible = (count > 0);
	auto width = 0;
	if (plusVisible) {
		width += st::stickersCreatorActionSize;
	}
	if (count > 0) {
		if (plusVisible) {
			width += st::stickersCreatorActionMargin;
		}
		width += count * st::stickersCreatorEmojiSize
			+ (count - 1) * st::stickersCreatorEmojiSkip;
		if (minusVisible) {
			width += st::stickersCreatorActionMargin;
		}
	}
	if (minusVisible) {
		width += st::stickersCreatorActionSize;
	}
	return width;
}

void EmojiPickerRow::relayout() {
	const auto count = int(_emojis.size());
	const auto plusVisible = (count < kMaxEmojis);
	const auto minusVisible = (count > 0);
	const auto contentWidth = rowContentWidth();
	const auto offsetX = (width() - contentWidth) / 2;
	const auto centerY = height() / 2;
	const auto top = centerY - st::stickersCreatorActionSize / 2;

	auto x = offsetX;

	if (plusVisible) {
		_plus->move(x, top);
		_plus->show();
		x += st::stickersCreatorActionSize;
	} else {
		_plus->hide();
	}

	if (count > 0) {
		if (plusVisible) {
			x += st::stickersCreatorActionMargin;
		}
		x += count * st::stickersCreatorEmojiSize
			+ (count - 1) * st::stickersCreatorEmojiSkip;
		if (minusVisible) {
			x += st::stickersCreatorActionMargin;
		}
	}

	if (minusVisible) {
		_minus->move(x, top);
		_minus->show();
	} else {
		_minus->hide();
	}

	update();
}

void EmojiPickerRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto count = int(_emojis.size());
	if (!count) {
		return;
	}
	const auto esize = Ui::Emoji::GetSizeLarge();
	const auto size = esize / style::DevicePixelRatio();
	const auto contentWidth = rowContentWidth();
	const auto offsetX = (width() - contentWidth) / 2;
	const auto centerY = height() / 2;
	const auto plusVisible = (count < kMaxEmojis);

	auto x = offsetX;
	if (plusVisible) {
		x += st::stickersCreatorActionSize
			+ st::stickersCreatorActionMargin;
	}
	const auto slot = st::stickersCreatorEmojiSize;
	const auto extra = (slot - size) / 2;
	const auto y = centerY - size / 2;
	for (const auto emoji : _emojis) {
		Ui::Emoji::Draw(p, emoji, esize, x + extra, y);
		x += slot + st::stickersCreatorEmojiSkip;
	}
}

void EmojiPickerRow::resizeEvent(QResizeEvent *e) {
	relayout();
}

void EmojiPickerRow::ensurePanel() {
	if (_panel) {
		return;
	}
	using Selector = ChatHelpers::TabbedSelector;
	_panel = base::make_unique_q<ChatHelpers::TabbedPanel>(
		_panelContainer.get(),
		ChatHelpers::TabbedPanelDescriptor{
			.ownedSelector = object_ptr<Selector>(
				nullptr,
				ChatHelpers::TabbedSelectorDescriptor{
					.show = _show,
					.st = st::defaultComposeControls.tabbed,
					.level = Window::GifPauseReason::Layer,
					.mode = Selector::Mode::PeerTitle,
				}),
		});
	_panel->setDesiredHeightValues(
		1.,
		st::emojiPanMinHeight / 2,
		st::emojiPanMinHeight);
	_panel->setDropDown(true);
	_panel->setShowAnimationOrigin(Ui::PanelAnimation::Origin::TopLeft);
	_panel->hide();

	_panel->selector()->emojiChosen(
	) | rpl::on_next([=](ChatHelpers::EmojiChosen data) {
		addEmoji(data.emoji);
		_panel->hideAnimated();
	}, _panel->lifetime());

	base::install_event_filter(this, _panelContainer, [=](
			not_null<QEvent*> event) {
		const auto type = event->type();
		if (type == QEvent::Move || type == QEvent::Resize) {
			crl::on_main(this, [=] { updatePanelGeometry(); });
		}
		return base::EventFilterResult::Continue;
	});
}

void EmojiPickerRow::updatePanelGeometry() {
	if (!_panel) {
		return;
	}
	const auto container = _panelContainer->size();
	const auto margins = st::emojiPanMargins;
	const auto panelWidth = st::emojiPanWidth
		+ margins.left()
		+ margins.right();
	const auto panelHeight = st::emojiPanMinHeight
		+ margins.top()
		+ margins.bottom();
	const auto top = std::max(0, (container.height() - panelHeight) / 2);
	const auto right = (container.width() + panelWidth) / 2;
	_panel->moveTopRight(top, right);
}

void EmojiPickerRow::togglePanel() {
	ensurePanel();
	updatePanelGeometry();
	_panel->toggleAnimated();
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
			.fixedCrop = true,
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

[[nodiscard]] QByteArray EncodeWebp(QImage image) {
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

} // namespace

namespace Api {

void CreateStickerBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<ChatHelpers::Show> show,
		StickerSetIdentifier set,
		QImage image,
		Fn<void(MTPmessages_StickerSet)> done) {
	struct State {
		rpl::variable<bool> uploading = false;
		std::unique_ptr<StickerUpload> upload;
		QPointer<Ui::RoundButton> addButton;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto session = &show->session();

	box->setTitle(tr::lng_stickers_create_image_title());

	const auto inner = box->verticalLayout();

	const auto previewHolder = inner->add(
		object_ptr<Ui::RpWidget>(inner),
		QMargins(0, 0, 0, 0),
		// QMargins(0, st::boxRowPadding.left(), 0, st::boxRowPadding.left()),
		style::al_top);
	previewHolder->resize(st::boxWideWidth, kPreviewSide);
	const auto preview = Ui::CreateChild<PreviewWidget>(
		previewHolder,
		image);
	previewHolder->widthValue(
	) | rpl::on_next([=](int width) {
		preview->move((width - kPreviewSide) / 2, 0);
	}, preview->lifetime());

	Ui::AddSkip(inner);
	Ui::AddSkip(inner);

	inner->add(
		object_ptr<Ui::FlatLabel>(
			inner,
			tr::lng_stickers_pack_choose_emoji_about(),
			st::boxDividerLabel),
		st::boxRowPadding);

	const auto emojiRow = inner->add(
		object_ptr<EmojiPickerRow>(
			inner,
			show,
			box->getDelegate()->outerContainer()),
		QMargins(0, 0, 0, st::boxRowPadding.left()));
	emojiRow->resize(st::boxWideWidth, st::stickersCreatorRowHeight);

	const auto startUpload = [=, set = std::move(set), done = std::move(done)](
			) mutable {
		if (state->uploading.current()) {
			return;
		}
		const auto emoji = emojiRow->value();
		if (emoji.isEmpty()) {
			show->showToast(
				tr::lng_stickers_create_emoji_required(tr::now));
			return;
		}
		const auto bytes = EncodeWebp(image);
		if (bytes.isEmpty()) {
			show->showToast(
				tr::lng_stickers_create_upload_failed(tr::now));
			return;
		}

		const auto lockedWidth = state->addButton
			? state->addButton->width()
			: 0;
		state->uploading = true;
		if (state->addButton && lockedWidth > 0) {
			state->addButton->resizeToWidth(lockedWidth);
		}
		state->upload = std::make_unique<StickerUpload>(
			session,
			set,
			bytes,
			emoji);

		const auto doneCallback = done;
		state->upload->start(
			crl::guard(box, [=](MTPmessages_StickerSet result) {
				state->upload = nullptr;
				state->uploading = false;
				show->showToast(tr::lng_stickers_create_added(tr::now));
				if (doneCallback) {
					doneCallback(result);
				}
				box->closeBox();
			}),
			crl::guard(box, [=](QString err) {
				state->upload = nullptr;
				state->uploading = false;
				show->showToast(err.isEmpty()
					? tr::lng_stickers_create_upload_failed(tr::now)
					: err);
			}));
	};

	const auto addButton = box->addButton(
		rpl::conditional(
			state->uploading.value(),
			rpl::single(QString()),
			tr::lng_box_done()),
		startUpload);
	state->addButton = addButton;
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });

	{
		using namespace Info::Statistics;
		const auto loadingAnimation = InfiniteRadialAnimationWidget(
			addButton,
			addButton->height() / 2,
			&st::editStickerSetNameLoading);
		AddChildToWidgetCenter(addButton, loadingAnimation);
		loadingAnimation->showOn(state->uploading.value());
	}

	box->setWidth(st::boxWideWidth);

	box->boxClosing(
	) | rpl::on_next([=] {
		state->upload = nullptr;
	}, box->lifetime());
}

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
				show->showBox(Box(
					CreateStickerBox,
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
