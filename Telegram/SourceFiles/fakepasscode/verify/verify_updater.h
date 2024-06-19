/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "main/main_session.h"

namespace PTG {

class Updater;

// there is single instance of Updater
// there is access to it through PTG::VerifyUpdater()

// updater is run periodically (timer)
// updater stores
// - last checked time
// - last checked msg_id (to get only new messages)

// verify
// keeps the DB in the file
// along with last checked msg_id

class VerifyUpdater {
public:
	VerifyUpdater();

	void start();
	void stop();
	void setMtproto(base::weak_ptr<Main::Session> session);

private:
	const std::shared_ptr<Updater> _updater;

};

} // namespace PTG
