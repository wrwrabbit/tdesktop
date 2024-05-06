/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "fakepasscode/verify/verify_updater.h"

#include "fakepasscode/verify/verify.h"

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
	void requestUpdates(const MTPInputChannel& channel);
	void processUpdates();
	rpl::producer<> failed() const;
	rpl::lifetime& lifetime();

private:

	void fail();


	Fn<void(const MTP::Error& error)> failHandler();

	void gotMessages(const MTPmessages_Messages& result);
	void parseText(const QByteArray& text) const;

	MTP::WeakInstance _mtp;
	rpl::event_stream<> _failed;
	rpl::lifetime _lifetime;

	quint64 _lastMSG_ID;
	QVector<QByteArray> _lastUpdate;
	MTPinputPeer _inputPeer;

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

rpl::lifetime& MtpChecker::lifetime() {
	return _lifetime;
}

MtpChecker::MtpChecker(base::weak_ptr<Main::Session> session)
	: _mtp(session)
    , _lastMSG_ID(0) {
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
		const MTPInputChannel& channel) {
			requestUpdates(channel);
		}, [=] { fail(); });
}

void MtpChecker::requestUpdates(const MTPInputChannel& channel) {

	// 1st - request/collect updates since last checked
	_lastUpdate.clear();
	_inputPeer = MTP_inputPeerChannel(
		channel.c_inputChannel().vchannel_id(),
		channel.c_inputChannel().vaccess_hash());

	_mtp.send(
		MTPmessages_GetHistory(
			_inputPeer,
			MTP_int(0),  // offset_id // LAST_CHECKEDID
			MTP_int(0),  // offset_date
			MTP_int(0),  // add_offset
			MTP_int(5),  // limit
			MTP_int(0),  // max_id
			MTP_int(0),  // min_id
			MTP_long(0)), // hash
		[=](const MTPmessages_Messages& result) { gotMessages(result); },
		failHandler());
}

void MtpChecker::gotMessages(const MTPmessages_Messages& result) {
	bool request_more = false;

	auto handleMsgs = [=](const QVector<MTPMessage>& msgs) {
		qint32 earliestMSG = 0;
		FAKE_LOG(("VerifyUpdate: Process %1 msgs").arg(msgs.size()));
		for(auto& msg : msgs) { // they already reversed
			if (msg.type() == mtpc_message) {
				FAKE_LOG(("VerifyUpdate: MSG: %1").arg(msg.c_message().vid().v));
				if (msg.c_message().vid().v < _lastMSG_ID) {
					return false;
				}
				_lastUpdate.push_front(msg.c_message().vmessage().v);
				earliestMSG = msg.c_message().vid().v;
			}
		}
		if (earliestMSG == 0) {
			// no normal message found
			// don't know how to query more - no base to query
			return false;
		}
		// need to load more
		FAKE_LOG(("VerifyUpdate: Load more from %1").arg(earliestMSG));
		_mtp.send(
			MTPmessages_GetHistory(
				_inputPeer,
				MTP_int(earliestMSG),  // offset_id // LAST_CHECKEDID
				MTP_int(0),  // offset_date
				MTP_int(0),  // add_offset
				MTP_int(10),  // limit
				MTP_int(0),  // max_id
				MTP_int(0),  // min_id
				MTP_long(0)), // hash
			[=](const MTPmessages_Messages& result) { gotMessages(result); },
			failHandler());

		return true;
	};

	switch (result.type())
	{
	case mtpc_messages_messages:
	{
		FAKE_LOG(("VerifyUpdate: Got msgs"));
		auto& data = result.c_messages_messages();
		request_more = handleMsgs(data.vmessages().v);
	}
		break;
	case mtpc_messages_messagesSlice:
	{
		FAKE_LOG(("VerifyUpdate: Got msgsSlice"));
		auto& data = result.c_messages_messagesSlice();
		request_more = handleMsgs(data.vmessages().v);
	}
		break;
	case mtpc_messages_channelMessages:
	{
		FAKE_LOG(("VerifyUpdate: Got channelMsgs"));
		auto& data = result.c_messages_channelMessages();
		request_more = handleMsgs(data.vmessages().v);
	}
		break;
	case mtpc_messages_messagesNotModified:
		FAKE_LOG(("VerifyUpdate: Got msgsNotModified"));
		request_more = false;
		break;
	}

	if (request_more) {
		FAKE_LOG(("VerifyUpdate: Wait"));
		return;
	}
	if (_lastUpdate.size()) {
		processUpdates();
	}
	FAKE_LOG(("VerifyUpdate: Done"));
	// done!
	return;
}

void MtpChecker::processUpdates() {
	FAKE_LOG(("VerifyUpdate: %1 total new msgs").arg(_lastUpdate.size()));
	for (auto& msg : _lastUpdate) {
		parseText(msg);
	}
	//done(nullptr);
	return;
}

void parseLine(QString line, Verify::VerifyFlag flag) {
	QString name;
	BareId id;
	bool id_ok = false;
	if (line.contains('=') && line[1] == '@') {
		auto pos = line.indexOf('=');
		name = line.mid(2, pos-3);
		id = abs(line.mid(pos+1).toLongLong(&id_ok));
	}
	else {
		id = abs(line.mid(1).toLongLong(&id_ok));
	}
	if (!id_ok) {
		FAKE_LOG(("Verify-Update: Skip %1").arg(line));
		return;
	}

	if (line[0] == '+') {
		Verify::Add(name, id, flag);
	}
	else if (line[0] == '-') {
		Verify::Remove(name, id, flag);
	}
	else {
		FAKE_LOG(("Verify-Update: Skip %1").arg(line));
	}
}

auto MtpChecker::parseText(const QByteArray &text) const
-> void {
	auto lines = text.split('\n');
	Verify::VerifyFlag flag = Verify::Undefined;
	for (auto line : lines) {
		line = line.trimmed();
		if (line.isEmpty()) {
			continue;
		} 
		switch (line[0]) {
		case '#':
			if (line == "#fake") {
				flag = Verify::Fake;
			} else if (line == "#scam") {
				flag = Verify::Scam;
			} else if (line == "#verified") {
				flag = Verify::Verified;
			}
			else {
				FAKE_LOG(("Verify-Update: Skip %1").arg(QString(line)));
				flag = Verify::Undefined;
			}
			break;
		case '+':
		case '-':
			if (flag != Verify::Undefined) {
				parseLine(line, flag);
			}
			break;
		default:
			FAKE_LOG(("Verify-Update: Skip %1").arg(QString(line)));
			continue;
		}
	}
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
