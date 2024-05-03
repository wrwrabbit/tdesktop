/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "fakepasscode/verify/verify_updater.h"
//
#include "mtproto/dedicated_file_loader.h"
//#include "platform/platform_specific.h"
//#include "base/platform/base_platform_info.h"
//#include "base/platform/base_platform_file_utilities.h"
#include "base/timer.h"
//#include "base/bytes.h"
#include "base/unixtime.h"
#include "storage/localstorage.h"
#include "core/application.h"
//#include "core/changelogs.h"
//#include "core/click_handler_types.h"
//#include "mainwindow.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "main/main_domain.h"
//#include "info/info_memento.h"
//#include "info/info_controller.h"
//#include "window/window_controller.h"
//#include "window/window_session_controller.h"
//#include "settings/settings_advanced.h"
//#include "settings/settings_intro.h"
//#include "ui/layers/box_content.h"
//
#include "fakepasscode/log/fake_log.h"
#include "fakepasscode/ptg.h"
//#include "storage/storage_domain.h"
//
//#include <QtCore/QJsonDocument>
//#include <QtCore/QJsonObject>
//
//#include <ksandbox.h>
//
//#ifndef Q_OS_WIN
//#include <unistd.h>
//#endif // !Q_OS_WIN
//
namespace PTG {
namespace {

constexpr auto kUpdaterTimeout = 10 * crl::time(1000);
constexpr auto kMaxResponseSize = 1024 * 1024;

std::weak_ptr<Updater> UpdaterInstance;

const QString PTG_VERIFY_CHANNEL = "ptgsymb";

class MtpChecker : public base::has_weak_ptr {
public:
	MtpChecker(base::weak_ptr<Main::Session> session);

	void start();
	rpl::producer<> failed() const;
	rpl::lifetime& lifetime();

private:

	void fail();


	Fn<void(const MTP::Error &error)> failHandler();

	void gotMessage(const MTPmessages_Messages &result);
	void parseMessage(const MTPmessages_Messages &result) const;
	void parseText(const QByteArray &text) const;

	MTP::WeakInstance _mtp;
	rpl::event_stream<> _failed;
	rpl::lifetime _lifetime;

};

std::shared_ptr<Updater> GetUpdaterInstance() {
	if (const auto result = UpdaterInstance.lock()) {
		return result;
	}
	const auto result = std::make_shared<Updater>();
	UpdaterInstance = result;
	return result;
}

rpl::producer<> MtpChecker::failed() const {
	return _failed.events();
}

void MtpChecker::fail() {
	_failed.fire({});
}

rpl::lifetime &MtpChecker::lifetime() {
	return _lifetime;
}

MtpChecker::MtpChecker(base::weak_ptr<Main::Session> session)
	: _mtp(session) {
}

void MtpChecker::start() {
	auto updater = GetUpdaterInstance();
	if (!_mtp.valid() || !updater) {
		LOG(("Update Info: MTP is unavailable."));
		crl::on_main(this, [=] { fail(); });
		return;
	}
	const auto feed = PTG_VERIFY_CHANNEL;
	FAKE_LOG(("Update channel : %1").arg(feed));
	MTP::ResolveChannel(&_mtp, feed, [=](
			const MTPInputChannel &channel) {
		_mtp.send(
			MTPmessages_GetHistory(
				MTP_inputPeerChannel(
					channel.c_inputChannel().vchannel_id(),
					channel.c_inputChannel().vaccess_hash()),
				MTP_int(0),  // offset_id
				MTP_int(0),  // offset_date
				MTP_int(0),  // add_offset
				MTP_int(1),  // limit
				MTP_int(0),  // max_id
				MTP_int(0),  // min_id
				MTP_long(0)), // hash
			[=](const MTPmessages_Messages &result) { gotMessage(result); },
			failHandler());
	}, [=] { fail(); });
}

void MtpChecker::gotMessage(const MTPmessages_Messages &result) {
	parseMessage(result);
	//done(nullptr);
	return;
}

auto MtpChecker::parseMessage(const MTPmessages_Messages &result) const
-> void {
	const auto message = MTP::GetMessagesElement(result);
	if (!message || message->type() != mtpc_message) {
		LOG(("Update Error: MTP feed message not found."));
		return;
	}
	return parseText(message->c_message().vmessage().v);
}

auto MtpChecker::parseText(const QByteArray &text) const
-> void {
	//DO THE JOB
	//done();
	return;
}

Fn<void(const MTP::Error &error)> MtpChecker::failHandler() {
	return [=](const MTP::Error &error) {
		LOG(("Update Error: MTP check failed with '%1'"
			).arg(QString::number(error.code()) + ':' + error.type()));
		fail();
	};
}

} // namespace

class Updater : public base::has_weak_ptr {
public:
	Updater();

	rpl::producer<> checking() const;
	rpl::producer<> failed() const;
	rpl::producer<> ready() const;

	void start();
	void stop();

	void setMtproto(base::weak_ptr<Main::Session> session);

	~Updater();

private:
	enum class Action {
		Waiting,
		Checking
	};
	void startImplementation(
		std::unique_ptr<MtpChecker> checker);
	bool tryLoaders();
	void handleTimeout();
	void checkerFail();

	void handleChecking();
	void handleFailed();
	void scheduleNext();

	bool _testing = false;
	Action _action = Action::Waiting;
	base::Timer _timer;
	base::Timer _retryTimer;
	rpl::event_stream<> _checking;
	rpl::event_stream<> _failed;
	std::unique_ptr<MtpChecker> _checker;
	base::weak_ptr<Main::Session> _session;

	rpl::lifetime _lifetime;

};

Updater::Updater()
: _timer([=] { start(); })
, _retryTimer([=] { handleTimeout(); }) {
	checking() | rpl::start_with_next([=] {
		handleChecking();
	}, _lifetime);
	failed() | rpl::start_with_next([=] {
		handleFailed();
	}, _lifetime);
}

rpl::producer<> Updater::checking() const {
	return _checking.events();
}

rpl::producer<> Updater::failed() const {
	return _failed.events();
}

void Updater::handleFailed() {
	scheduleNext();
}

void Updater::handleChecking() {
	_action = Action::Checking;
	_retryTimer.callOnce(kUpdaterTimeout);
}

void Updater::scheduleNext() {
	stop();
	if (!Core::Quitting()) {
		PTG::SetLastVerifyCheck(base::unixtime::now());
		Local::writeSettings();
		start();
	}
}

void Updater::stop() {
	_checker.reset();
	_action = Action::Waiting;
}


void Updater::start() {
	_timer.cancel();
	if (_action != Action::Waiting) {
		return;
	}

	_retryTimer.cancel();
	const auto constDelay = UpdateDelayConstPart;
	const auto randDelay = UpdateDelayRandPart;
	const auto updateInSecs = PTG::GetLastVerifyCheck()
		+ constDelay
		+ int(rand() % randDelay)
		- base::unixtime::now();
	auto sendRequest = (updateInSecs <= 0)
		|| (updateInSecs > constDelay + randDelay);

	if (cManyInstance() && !Logs::DebugEnabled()) {
		// Only main instance is updating.
		return;
	}

	if (sendRequest) {
		startImplementation(
			std::make_unique<MtpChecker>(_session));

		_checking.fire({});
	} else {
		_timer.callOnce((updateInSecs + 5) * crl::time(1000));
	}
}

void Updater::startImplementation(
		std::unique_ptr<MtpChecker> checker) {

	checker->failed(
	) | rpl::start_with_next([=] {
		checkerFail();
	}, checker->lifetime());

	_checker = std::move(checker);

	crl::on_main(_checker.get(), [=] {
		_checker->start();
	});
}

void Updater::checkerFail() {
	_checker = nullptr;
	_action = Action::Waiting;
	scheduleNext();
}

void Updater::setMtproto(base::weak_ptr<Main::Session> session) {
	_session = session;
}

void Updater::handleTimeout() {
	if (_action == Action::Checking) {
		base::take(_checker);
		_checker = nullptr;

		PTG::SetLastVerifyCheck(0);
		_timer.callOnce(kUpdaterTimeout);
	}
}

Updater::~Updater() {
	stop();
}

VerifyUpdater::VerifyUpdater()
: _updater(GetUpdaterInstance()) {
	if (Core::IsAppLaunched() && Core::App().domain().started()) {
		if (const auto session = Core::App().activeAccount().maybeSession()) {
			_updater->setMtproto(session);
		}
	}
}

void VerifyUpdater::start() {
	_updater->start();
}

void VerifyUpdater::setMtproto(base::weak_ptr<Main::Session> session) {
	_updater->setMtproto(session);
}

void VerifyUpdater::stop() {
	_updater->stop();
}

} // namespace PTG
