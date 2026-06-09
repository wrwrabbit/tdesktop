/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/external_control.h"

#include "core/application.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Core {
namespace {

[[nodiscard]] QByteArray Pack(QJsonObject object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QByteArray Error(const QString &text) {
	auto object = QJsonObject();
	object.insert(u"ok"_q, false);
	object.insert(u"error"_q, text);
	return Pack(object);
}

} // namespace

QByteArray HandleExternalControl(const QString &command) {
	if (!IsAppLaunched()) {
		return Error(u"application is not launched"_q);
	} else if (command == u"ping"_q) {
		auto object = QJsonObject();
		object.insert(u"ok"_q, true);
		object.insert(u"result"_q, u"pong"_q);
		return Pack(object);
	}
	return Error(u"unknown control command"_q);
}

} // namespace Core
