/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "platform/platform_notifications_manager.h"

namespace Platform {
namespace Notifications {

class Manager : public Window::Notifications::NativeManager {
public:
	Manager(not_null<Window::Notifications::System*> system);
	void clearNotification(NotificationId id);
	~Manager();

protected:
	void doShowNativeNotification(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		Ui::PeerUserpicView &userpicView,
		MsgId msgId,
		const QString &title,
		const QString &subtitle,
		const QString &msg,
		DisplayOptions options) override;
	void doClearAllFast() override;
	void doClearFromItem(not_null<HistoryItem*> item) override;
	void doClearFromTopic(not_null<Data::ForumTopic*> topic) override;
	void doClearFromHistory(not_null<History*> history) override;
	void doClearFromSession(not_null<Main::Session*> session) override;
	bool doSkipToast() const override;
	void doMaybePlaySound(Fn<void()> playSound) override;
	void doMaybeFlashBounce(Fn<void()> flashBounce) override;

private:
	friend void Create(Window::Notifications::System *system);
	class Private;
	const std::unique_ptr<Private> _private;

};

} // namespace Notifications
} // namespace Platform
