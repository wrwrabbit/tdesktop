/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/effects/premium_star_model.h"

#include <QtCore/QFile>
#include <QtCore/QDataStream>

namespace Ui::Premium {
namespace {

constexpr auto kFloatsPerVertex = 8;

[[nodiscard]] std::vector<float> ReadFloats(QDataStream &stream, int count) {
	auto result = std::vector<float>(count);
	for (auto i = 0; i != count; ++i) {
		stream >> result[i];
	}
	return result;
}

} // namespace

StarModel LoadStarModel() {
	auto file = QFile(u":/gui/art/premium/star.binobj"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	auto stream = QDataStream(&file);
	stream.setByteOrder(QDataStream::BigEndian);
	stream.setFloatingPointPrecision(QDataStream::SinglePrecision);

	const auto readCount = [&]() -> int {
		auto value = qint32(0);
		stream >> value;
		return (stream.status() == QDataStream::Ok && value >= 0)
			? value
			: -1;
	};

	const auto positionFloats = readCount();
	if (positionFloats < 0) {
		return {};
	}
	const auto positions = ReadFloats(stream, positionFloats);

	const auto uvFloats = readCount();
	if (uvFloats < 0) {
		return {};
	}
	const auto uvs = ReadFloats(stream, uvFloats);

	const auto normalFloats = readCount();
	if (normalFloats < 0) {
		return {};
	}
	const auto normals = ReadFloats(stream, normalFloats);

	const auto faceCount = readCount();
	if (faceCount < 0 || stream.status() != QDataStream::Ok) {
		return {};
	}

	auto result = StarModel();
	result.vertices.reserve(faceCount * kFloatsPerVertex);
	for (auto i = 0; i != faceCount; ++i) {
		auto positionIndex = qint32(0);
		auto uvIndex = qint32(0);
		auto normalIndex = qint32(0);
		stream >> positionIndex >> uvIndex >> normalIndex;
		if (stream.status() != QDataStream::Ok) {
			return {};
		}

		const auto position = qint64(positionIndex) * 3;
		const auto normal = qint64(normalIndex) * 3;
		if (position < 0
			|| position + 2 >= positionFloats
			|| normal < 0
			|| normal + 2 >= normalFloats) {
			return {};
		}
		result.vertices.push_back(positions[position + 0]);
		result.vertices.push_back(positions[position + 1]);
		result.vertices.push_back(positions[position + 2]);

		result.vertices.push_back(normals[normal + 0]);
		result.vertices.push_back(normals[normal + 1]);
		result.vertices.push_back(normals[normal + 2]);

		const auto uv = qint64(uvIndex) * 2;
		const auto valid = (uv >= 0) && (uv + 1 < uvFloats);
		result.vertices.push_back(valid ? uvs[uv] : 0.f);
		result.vertices.push_back(valid ? (1.f - uvs[uv + 1]) : 0.f);
	}
	result.vertexCount = faceCount;
	return result;
}

} // namespace Ui::Premium
