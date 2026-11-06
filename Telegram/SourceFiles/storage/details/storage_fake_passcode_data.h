/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDataStream>

#include <optional>
#include <vector>

namespace Storage::details {

struct FakePasscodeData {
	QByteArray salt;
	std::vector<QByteArray> keys;
	bool hardwareBound = false;
};

[[nodiscard]] std::optional<int> LegacyFakePasscodeCount(
	const QByteArray &bytes);
[[nodiscard]] std::optional<FakePasscodeData> ReadLegacyFakePasscodeData(
	QDataStream &stream,
	const QByteArray &count,
	const QByteArray &salt);
[[nodiscard]] std::optional<FakePasscodeData> ReadFakePasscodeData(
	QDataStream &stream);
[[nodiscard]] QByteArray SerializeFakePasscodeData(
	const FakePasscodeData &data);

[[nodiscard]] bool ValidateFakePasscodeCount(
	const QByteArray &info,
	int count);
[[nodiscard]] bool HasFakePasscodeCount(const QByteArray &info);
void AppendFakePasscodeCount(QDataStream &stream, int count);

} // namespace Storage::details
