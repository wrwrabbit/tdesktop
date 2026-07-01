/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/output/export_output_whatsapp.h"

#include "export/output/export_output_result.h"
#include "export/data/export_data_types.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Export {
namespace Output {
namespace {

QByteArray FormatWhatsAppDate(TimeId date) {
	if (!date) {
		return QByteArray();
	}
	const auto value = QDateTime::fromSecsSinceEpoch(date);
	return QString("[%1.%2.%3, %4:%5:%6]"
	).arg(value.date().day(), 2, 10, QChar('0')
	).arg(value.date().month(), 2, 10, QChar('0')
	).arg(value.date().year() % 100, 2, 10, QChar('0')
	).arg(value.time().hour(), 2, 10, QChar('0')
	).arg(value.time().minute(), 2, 10, QChar('0')
	).arg(value.time().second(), 2, 10, QChar('0')
	).toUtf8();
}

QByteArray FlattenText(const std::vector<Data::TextPart> &data) {
	auto result = QByteArray();
	for (const auto &part : data) {
		result.append(part.text);
	}
	return result.replace('\r', "").replace('\n', " ");
}

QByteArray SenderName(
		PeerId fromId,
		const std::map<PeerId, Data::Peer> &peers) {
	if (!fromId) {
		return "Unknown";
	}
	const auto i = peers.find(fromId);
	if (i != peers.end()) {
		const auto name = i->second.name();
		if (!name.isEmpty()) {
			return name;
		}
	}
	return "User " + Data::NumberToString(Data::PeerToBareId(fromId));
}

QString SanitizeFileName(const QString &name) {
	auto result = name;
	for (auto &ch : result) {
		if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?'
			|| ch == '"' || ch == '<' || ch == '>' || ch == '|') {
			ch = '_';
		}
	}
	result = result.trimmed();
	return result.isEmpty() ? u"chat"_q : result;
}

} // namespace

Result WhatsAppWriter::start(
		const Settings &settings,
		const Environment &environment,
		Stats *stats) {
	Expects(settings.path.endsWith('/'));

	_settings = base::duplicate(settings);
	_environment = environment;
	_stats = stats;
	if (!QDir().mkpath(_settings.path)) {
		return Result(Result::Type::FatalError, _settings.path);
	}
	return Result::Success();
}

Result WhatsAppWriter::writeTextFile(
		const QString &relativeName,
		const QByteArray &content) {
	auto file = File(_settings.path + unique(relativeName), _stats);
	return file.writeBlock(content);
}

QString WhatsAppWriter::unique(const QString &relativePath) const {
	return File::PrepareRelativePath(_settings.path, relativePath);
}

Result WhatsAppWriter::writePersonal(const Data::PersonalInfo &data) {
	const auto &info = data.user.info;
	auto block = QByteArray();
	block.append("Account information\n\n");
	block.append("Name: ").append(info.firstName);
	if (!info.lastName.isEmpty()) {
		block.append(' ').append(info.lastName);
	}
	block.append('\n');
	block.append("Phone: ").append(
		Data::FormatPhoneNumber(info.phoneNumber)).append('\n');
	if (!data.user.username.isEmpty()) {
		block.append("Username: @").append(data.user.username).append('\n');
	}
	if (!data.bio.isEmpty()) {
		block.append("Bio: ").append(data.bio).append('\n');
	}
	return writeTextFile("AccountInformation.txt", block);
}

Result WhatsAppWriter::writeUserpicsStart(const Data::UserpicsInfo &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeUserpicsSlice(const Data::UserpicsSlice &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeUserpicsEnd() {
	return Result::Success();
}

Result WhatsAppWriter::writeStoriesStart(const Data::StoriesInfo &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeStoriesSlice(const Data::StoriesSlice &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeStoriesEnd() {
	return Result::Success();
}

Result WhatsAppWriter::writeProfileMusicStart(
		const Data::ProfileMusicInfo &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeProfileMusicSlice(
		const Data::ProfileMusicSlice &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeProfileMusicEnd() {
	return Result::Success();
}

Result WhatsAppWriter::writeContactsList(const Data::ContactsList &data) {
	auto block = QByteArray("Contacts\n\n");
	for (const auto index : Data::SortedContactsIndices(data)) {
		const auto &contact = data.list[index];
		block.append(contact.name()).append(" - ").append(
			Data::FormatPhoneNumber(contact.phoneNumber)).append('\n');
	}
	return writeTextFile("Contacts.txt", block);
}

Result WhatsAppWriter::writeSessionsList(const Data::SessionsList &data) {
	auto block = QByteArray("Active sessions\n\n");
	for (const auto &session : data.list) {
		block.append(session.applicationName).append(' ').append(
			session.applicationVersion).append(" - ").append(
			session.deviceModel).append(", ").append(
			session.platform).append('\n');
	}
	return writeTextFile("Sessions.txt", block);
}

Result WhatsAppWriter::writeOtherData(const Data::File &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeDialogsStart(const Data::DialogsInfo &data) {
	return QDir().mkpath(_settings.path + "chats")
		? Result::Success()
		: Result(Result::Type::FatalError, _settings.path);
}

Result WhatsAppWriter::writeDialogStart(const Data::DialogInfo &data) {
	Expects(!_zipChatEntryOpen);

	if (!QDir().mkpath(_settings.path + "chats")) {
		return Result(Result::Type::FatalError, _settings.path);
	}

	_dialog = data;
	_pendingMedia.clear();
	_zip.emplace();

	zip_fileinfo zfi = { { 0, 0, 0, 0, 0, 0 }, 0, 0, 0 };
	_zip->openNewFile(
		"_chat.txt",
		&zfi,
		nullptr,
		0,
		nullptr,
		0,
		nullptr,
		Z_DEFLATED,
		Z_DEFAULT_COMPRESSION);
	_zipChatEntryOpen = true;
	return Result::Success();
}

Result WhatsAppWriter::writeDialogSlice(const Data::MessagesSlice &data) {
	Expects(_zipChatEntryOpen);
	Expects(_zip.has_value());

	auto block = QByteArray();
	for (const auto &message : data.list) {
		if (Data::SkipMessageByDate(message, _settings)) {
			continue;
		}
		const auto sender = SenderName(message.fromId, data.peers);
		block.append(FormatWhatsAppDate(message.date));
		block.append(' ').append(sender).append(": ");

		const auto &file = message.file();
		if (!file.relativePath.isEmpty()) {
			const auto diskPath = _settings.path + file.relativePath;
			const auto archiveName = SanitizeFileName(
				QFileInfo(file.relativePath).fileName());
			_pendingMedia.push_back({ diskPath, archiveName });
			block.append("<attached: ").append(
				archiveName.toUtf8()).append('>');
			const auto text = FlattenText(message.text);
			if (!text.isEmpty()) {
				block.append(' ').append(text);
			}
		} else {
			block.append(FlattenText(message.text));
		}
		block.append('\n');
	}
	if (block.isEmpty()) {
		return Result::Success();
	}
	_zip->writeInFile(block.constData(), block.size());
	return (_zip->error() == ZIP_OK)
		? Result::Success()
		: Result(Result::Type::FatalError, _settings.path);
}

Result WhatsAppWriter::writeDialogEnd() {
	Expects(_zipChatEntryOpen);
	Expects(_zip.has_value());

	_zip->closeFile();
	_zipChatEntryOpen = false;

	zip_fileinfo zfi = { { 0, 0, 0, 0, 0, 0 }, 0, 0, 0 };
	for (const auto &media : _pendingMedia) {
		auto file = QFile(media.diskPath);
		if (!file.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto content = file.readAll();
		_zip->openNewFile(
			media.archiveName.toUtf8().constData(),
			&zfi,
			nullptr,
			0,
			nullptr,
			0,
			nullptr,
			Z_DEFLATED,
			Z_DEFAULT_COMPRESSION);
		_zip->writeInFile(content.constData(), content.size());
		_zip->closeFile();
	}
	_zip->close();
	if (_zip->error() != ZIP_OK) {
		return Result(Result::Type::FatalError, _settings.path);
	}

	const auto name = SanitizeFileName(QString::fromUtf8(_dialog.name));
	const auto relative = u"chats/"_q + name + u".zip"_q;
	const auto path = _settings.path + unique(relative);
	auto out = QFile(path);
	const auto result = _zip->result();
	if (!out.open(QIODevice::WriteOnly)
		|| out.write(result) != result.size()) {
		return Result(Result::Type::FatalError, path);
	}

	_zip.reset();
	_pendingMedia.clear();
	_dialog = Data::DialogInfo();
	return Result::Success();
}

Result WhatsAppWriter::writeDialogsEnd() {
	return Result::Success();
}

Result WhatsAppWriter::finish() {
	return Result::Success();
}

QString WhatsAppWriter::mainFilePath() {
	return _settings.path;
}

} // namespace Output
} // namespace Export
