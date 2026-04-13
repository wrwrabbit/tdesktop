/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/layers/box_content.h"

class DocumentData;

namespace ChatHelpers {
class Show;
class StickersListWidget;
} // namespace ChatHelpers

class StickerPickerBox final : public Ui::BoxContent {
public:
	StickerPickerBox(
		QWidget*,
		std::shared_ptr<ChatHelpers::Show> show,
		Fn<void(not_null<DocumentData*>)> chosen);

protected:
	void prepare() override;
	void resizeEvent(QResizeEvent *e) override;

private:
	const std::shared_ptr<ChatHelpers::Show> _show;
	Fn<void(not_null<DocumentData*>)> _chosen;

	QPointer<ChatHelpers::StickersListWidget> _list;

};
