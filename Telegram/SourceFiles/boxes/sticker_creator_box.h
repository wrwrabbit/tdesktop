/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/stickers/data_stickers.h"
#include "ui/layers/box_content.h"

#include <QtGui/QImage>

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Main {
class Session;
} // namespace Main

namespace Api {
class StickerUpload;
} // namespace Api

class StickerCreatorBox final : public Ui::BoxContent {
public:
	StickerCreatorBox(
		QWidget*,
		std::shared_ptr<ChatHelpers::Show> show,
		StickerSetIdentifier set,
		QImage image,
		Fn<void(MTPmessages_StickerSet)> done);
	~StickerCreatorBox();

protected:
	void prepare() override;

private:
	void startUpload();
	[[nodiscard]] QByteArray encodeWebp() const;

	const std::shared_ptr<ChatHelpers::Show> _show;
	const not_null<Main::Session*> _session;
	const StickerSetIdentifier _set;
	const QImage _image;
	const Fn<void(MTPmessages_StickerSet)> _done;

	rpl::variable<bool> _uploading = false;
	Fn<QString()> _emojiValue;

	std::unique_ptr<Api::StickerUpload> _upload;

};

namespace Api {

void OpenCreateStickerFlow(
	std::shared_ptr<ChatHelpers::Show> show,
	StickerSetIdentifier set,
	Fn<void(MTPmessages_StickerSet)> done = nullptr);

} // namespace Api
