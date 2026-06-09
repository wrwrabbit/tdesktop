/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/external_control.h"

#include "core/application.h"
#include "window/window_controller.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Core {
namespace {

struct WindowEntry {
	not_null<Window::Controller*> controller;
	QWidget *window = nullptr;
};

[[nodiscard]] QByteArray Pack(QJsonObject object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QByteArray Error(const QString &text) {
	auto object = QJsonObject();
	object.insert(u"ok"_q, false);
	object.insert(u"error"_q, text);
	return Pack(object);
}

[[nodiscard]] std::vector<WindowEntry> CollectWindows() {
	auto result = std::vector<WindowEntry>();
	for (const auto widget : QApplication::topLevelWidgets()) {
		const auto controller = App().findWindow(widget);
		if (!controller) {
			continue;
		}
		auto already = false;
		for (const auto &entry : result) {
			if (entry.controller.get() == controller) {
				already = true;
				break;
			}
		}
		if (!already) {
			result.push_back({ controller, widget->window() });
		}
	}
	ranges::sort(result, [](const WindowEntry &a, const WindowEntry &b) {
		if (a.controller->isPrimary() != b.controller->isPrimary()) {
			return a.controller->isPrimary();
		}
		return a.controller.get() < b.controller.get();
	});
	return result;
}

[[nodiscard]] QByteArray HandleWindows() {
	const auto active = App().activeWindow();
	auto list = QJsonArray();
	auto index = 0;
	for (const auto &entry : CollectWindows()) {
		auto object = QJsonObject();
		object.insert(u"id"_q, index++);
		object.insert(u"title"_q, entry.window->windowTitle());
		object.insert(u"primary"_q, entry.controller->isPrimary());
		object.insert(u"active"_q, (entry.controller.get() == active));
		list.append(object);
	}
	auto object = QJsonObject();
	object.insert(u"ok"_q, true);
	object.insert(u"windows"_q, list);
	return Pack(object);
}

[[nodiscard]] QByteArray HandleActivate(int index) {
	const auto windows = CollectWindows();
	if (index < 0 || index >= int(windows.size())) {
		return Error(u"no such window"_q);
	}
	windows[index].controller->activate();
	auto object = QJsonObject();
	object.insert(u"ok"_q, true);
	object.insert(u"activated"_q, index);
	return Pack(object);
}

[[nodiscard]] QByteArray HandleCycle() {
	const auto windows = CollectWindows();
	if (windows.empty()) {
		return Error(u"no windows"_q);
	}
	const auto active = App().activeWindow();
	auto current = 0;
	for (auto i = 0, count = int(windows.size()); i != count; ++i) {
		if (windows[i].controller.get() == active) {
			current = i;
			break;
		}
	}
	const auto next = (current + 1) % int(windows.size());
	windows[next].controller->activate();
	auto object = QJsonObject();
	object.insert(u"ok"_q, true);
	object.insert(u"activated"_q, next);
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
	} else if (command == u"windows"_q) {
		return HandleWindows();
	} else if (command.startsWith(u"activate:"_q)) {
		return HandleActivate(command.mid(9).toInt());
	} else if (command == u"cycle"_q) {
		return HandleCycle();
	}
	return Error(u"unknown control command"_q);
}

} // namespace Core
