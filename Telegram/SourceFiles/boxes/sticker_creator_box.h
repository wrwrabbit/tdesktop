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

namespace Ui {
class FlatLabel;
class InputField;
class RoundButton;
} // namespace Ui

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
	enum class State {
		ChooseEmoji,
		Uploading,
	};

	void setState(State state);
	void startUpload();
	[[nodiscard]] QByteArray encodeWebp() const;

	const std::shared_ptr<ChatHelpers::Show> _show;
	const not_null<Main::Session*> _session;
	const StickerSetIdentifier _set;
	const QImage _image;
	const Fn<void(MTPmessages_StickerSet)> _done;

	State _state = State::ChooseEmoji;
	Ui::InputField *_emojiField = nullptr;
	Ui::FlatLabel *_status = nullptr;
	Ui::RoundButton *_addButton = nullptr;

	std::unique_ptr<Api::StickerUpload> _upload;

};

namespace Api {

void OpenCreateStickerFlow(
	std::shared_ptr<ChatHelpers::Show> show,
	StickerSetIdentifier set,
	Fn<void(MTPmessages_StickerSet)> done = nullptr);

} // namespace Api
