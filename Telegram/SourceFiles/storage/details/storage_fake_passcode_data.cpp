/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/details/storage_fake_passcode_data.h"

#include <QtCore/QBuffer>

#include <utility>

namespace Storage::details {
namespace {

constexpr auto kMagic = quint32(0x50544733);
constexpr auto kFormat = quint32(1);
constexpr auto kMaxCount = 1024;
constexpr auto kSaltSize = 32;
constexpr auto kMaxKeySize = 4096;
constexpr auto kMaxTrailerSize = kMaxCount * (kMaxKeySize + 4) + 64;

[[nodiscard]] std::optional<QByteArray> ReadBytes(
		QDataStream &stream,
		int limit) {
	auto size = quint32();
	stream >> size;
	if (stream.status() != QDataStream::Ok
		|| size > quint32(limit)
		|| !stream.device()
		|| size > stream.device()->bytesAvailable()) {
		return std::nullopt;
	}
	auto result = QByteArray(int(size), Qt::Uninitialized);
	if (stream.readRawData(result.data(), result.size()) != result.size()) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] bool ReadKeys(
		QDataStream &stream,
		int count,
		FakePasscodeData &data) {
	if (count < 0 || count > kMaxCount) {
		return false;
	}
	for (auto i = 0; i != count; ++i) {
		auto key = ReadBytes(stream, kMaxKeySize);
		if (!key || key->isEmpty()) {
			return false;
		}
		data.keys.push_back(std::move(*key));
	}
	return stream.status() == QDataStream::Ok;
}

} // namespace

std::optional<int> LegacyFakePasscodeCount(const QByteArray &bytes) {
	if (bytes.isEmpty() || bytes.size() > 4) {
		return std::nullopt;
	}
	for (const auto ch : bytes) {
		if (ch < '0' || ch > '9') {
			return std::nullopt;
		}
	}
	auto ok = false;
	const auto count = bytes.toInt(&ok);
	return (ok && count <= kMaxCount)
		? std::make_optional(count)
		: std::nullopt;
}

std::optional<FakePasscodeData> ReadLegacyFakePasscodeData(
		QDataStream &stream,
		const QByteArray &count,
		const QByteArray &salt) {
	const auto size = LegacyFakePasscodeCount(count);
	auto result = FakePasscodeData{ salt };
	if (!size || salt.size() != kSaltSize || (!ReadKeys(stream, *size, result) || !stream.atEnd())) {
		return std::nullopt;
	}
	return result;
}

std::optional<FakePasscodeData> ReadFakePasscodeData(QDataStream &stream) {
	if (stream.atEnd()) {
		return FakePasscodeData();
	}
	auto bytes = ReadBytes(stream, kMaxTrailerSize);
	if (!bytes || !stream.atEnd()) {
		return std::nullopt;
	}
	QDataStream inner(&*bytes, QIODevice::ReadOnly);
	inner.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32();
	auto format = quint32();
	auto count = qint32();
	auto result = FakePasscodeData();
	inner >> magic >> format;
	const auto salt = ReadBytes(inner, kSaltSize);
	if (!salt) {
		return std::nullopt;
	}
	result.salt = *salt;
	inner >> count;
	if (inner.status() != QDataStream::Ok
		|| magic != kMagic
		|| format != kFormat
		|| result.salt.size() != kSaltSize
		|| !ReadKeys(inner, count, result)) {
		return std::nullopt;
	}
	if (!inner.atEnd()) {
		auto hardware = quint8();
		inner >> hardware;
		if (hardware > 1) {
			return std::nullopt;
		}
		result.hardwareBound = (hardware != 0);
	}
	if (inner.status() != QDataStream::Ok || !inner.atEnd()) {
		return std::nullopt;
	}
	return result;
}

QByteArray SerializeFakePasscodeData(const FakePasscodeData &data) {
	auto result = QByteArray();
	QDataStream stream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kMagic << kFormat << data.salt << qint32(data.keys.size());
	for (const auto &key : data.keys) {
		stream << key;
	}
	stream << data.hardwareBound;
	return result;
}

bool HasFakePasscodeCount(const QByteArray &info) {
	return info.size() >= 8
		&& info.mid(info.size() - 8, 4) == QByteArray::fromHex("50544743");
}

bool ValidateFakePasscodeCount(const QByteArray &info, int count) {
	if (info.size() < 8 || count < 0 || count > kMaxCount) {
		return false;
	}
	auto tail = info.right(8);
	QDataStream stream(&tail, QIODevice::ReadOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32();
	auto stored = qint32();
	stream >> magic >> stored;
	return stream.status() == QDataStream::Ok
		&& magic == quint32(0x50544743)
		&& stored == count;
}

void AppendFakePasscodeCount(QDataStream &stream, int count) {
	stream << quint32(0x50544743) << qint32(count);
}

} // namespace Storage::details
