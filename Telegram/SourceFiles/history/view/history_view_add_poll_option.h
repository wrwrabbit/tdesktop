/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"

struct PollData;

namespace ChatHelpers {
class TabbedPanel;
} // namespace ChatHelpers

namespace PollMediaUpload {
class PollMediaButton;
struct PollMediaState;
class PollMediaUploader;
} // namespace PollMediaUpload

namespace Ui {
class InputField;
class IconButton;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace HistoryView {

class Element;
class ElementOverlayHost;

class AddPollOptionWidget final : public Ui::RpWidget {
public:
	AddPollOptionWidget(
		not_null<QWidget*> parent,
		not_null<PollData*> poll,
		FullMsgId itemId,
		not_null<Window::SessionController*> controller);

	void updatePosition(QPoint topLeft, int width);
	void triggerSubmit();

	[[nodiscard]] rpl::producer<> submitted() const;
	[[nodiscard]] rpl::producer<> cancelled() const;

private:
	void setupField();
	void setupEmojiPanel();
	void setupAttach();
	void subscribeToPollUpdates();
	[[nodiscard]] static QString mapErrorToText(const QString &error);

	const not_null<PollData*> _poll;
	const FullMsgId _itemId;
	const not_null<Window::SessionController*> _controller;
	const not_null<Main::Session*> _session;

	Ui::InputField *_field = nullptr;
	Ui::IconButton *_emoji = nullptr;
	PollMediaUpload::PollMediaButton *_attach = nullptr;
	base::unique_qptr<ChatHelpers::TabbedPanel> _emojiPanel;
	std::unique_ptr<PollMediaUpload::PollMediaUploader> _uploader;
	std::shared_ptr<PollMediaUpload::PollMediaState> _mediaState;

	rpl::event_stream<> _submittedEvents;
	rpl::event_stream<> _cancelledEvents;

};

void ShowAddPollOptionOverlay(
	ElementOverlayHost &host,
	not_null<QWidget*> parent,
	not_null<Element*> view,
	not_null<PollData*> poll,
	FullMsgId context,
	not_null<Window::SessionController*> controller);

} // namespace HistoryView
