#include "export/output/export_output_whatsapp.h"

#include "export/output/export_output_result.h"
#include "export/output/export_output_stats.h"
#include "export/data/export_data_types.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Export {
namespace Output {
namespace {

QByteArray FormatWhatsAppDate(TimeId date) {
	const auto value = QDateTime::fromSecsSinceEpoch(date);
	return QString("%1.%2.%3, %4:%5"
	).arg(value.date().day(), 2, 10, QChar('0')
	).arg(value.date().month(), 2, 10, QChar('0')
	).arg(value.date().year(), 4, 10, QChar('0')
	).arg(value.time().hour(), 2, 10, QChar('0')
	).arg(value.time().minute(), 2, 10, QChar('0')
	).toUtf8();
}

QString FormatMediaDate(TimeId date) {
	return QDateTime::fromSecsSinceEpoch(date).toString(u"yyyyMMdd"_q);
}

QString MediaPrefix(const Data::Message &message) {
	if (std::get_if<Data::Photo>(&message.media.content)) {
		return u"IMG"_q;
	}
	if (const auto document = std::get_if<Data::Document>(
			&message.media.content)) {
		if (document->isSticker) {
			return u"STK"_q;
		} else if (document->isVoiceMessage) {
			return u"PTT"_q;
		} else if (document->isAudioFile) {
			return u"AUD"_q;
		} else if (document->isVideoFile
			|| document->isVideoMessage
			|| document->isAnimated) {
			return u"VID"_q;
		}
	}
	return u"DOC"_q;
}

QByteArray WrapLine(const QByteArray &line, const QByteArray &mark) {
	const auto size = int(line.size());
	auto from = 0;
	auto till = size;
	while (from < till && (line[from] == ' ' || line[from] == '\t')) {
		++from;
	}
	while (till > from && (line[till - 1] == ' ' || line[till - 1] == '\t')) {
		--till;
	}
	if (from == till) {
		return line;
	}
	return line.left(from)
		+ mark
		+ line.mid(from, till - from)
		+ mark
		+ line.mid(till);
}

QByteArray WrapText(const QByteArray &text, const QByteArray &mark) {
	if (mark == "```") {
		return mark + text + mark;
	}
	auto lines = text.split('\n');
	for (auto &line : lines) {
		line = WrapLine(line, mark);
	}
	return lines.join('\n');
}

QByteArray MarkFor(Data::TextPart::Type type) {
	using Type = Data::TextPart::Type;
	switch (type) {
	case Type::Bold: return "*";
	case Type::Italic: return "_";
	case Type::Strike: return "~";
	case Type::Code: return "`";
	case Type::Pre: return "```";
	default: return QByteArray();
	}
}

QByteArray JoinText(const std::vector<Data::TextPart> &data) {
	auto result = QByteArray();
	for (const auto &part : data) {
		const auto text = QByteArray(part.text).replace('\r', "");
		const auto mark = MarkFor(part.type);
		result.append(mark.isEmpty() ? text : WrapText(text, mark));
	}
	return result;
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

QByteArray DialogDisplayName(const Data::DialogInfo &dialog) {
	auto result = dialog.name;
	if (!dialog.lastName.isEmpty()) {
		result = result.isEmpty()
			? dialog.lastName
			: (result + ' ' + dialog.lastName);
	}
	return result;
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

QString UniqueZipPath(const QString &folder, const QString &name) {
	auto result = folder + name + u".zip"_q;
	auto index = 0;
	while (QFile::exists(result)) {
		result = folder + name + u" ("_q + QString::number(++index) + u").zip"_q;
	}
	return result;
}

} // namespace

WhatsAppWriter::~WhatsAppWriter() {
	removeWorkFolder();
}

void WhatsAppWriter::removeWorkFolder() {
	if (_settings.path.isEmpty()) {
		return;
	}
	auto folder = QDir(_settings.path);
	if (folder.dirName().startsWith(u"WhatsAppExport_tmp"_q)) {
		folder.removeRecursively();
	}
}

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
	auto parent = QDir(_settings.path);
	if (!parent.cdUp()) {
		return Result(Result::Type::FatalError, _settings.path);
	}
	_outputPath = parent.absolutePath() + '/';
	return Result::Success();
}

Result WhatsAppWriter::writePersonal(const Data::PersonalInfo &data) {
	return Result::Success();
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
	return Result::Success();
}

Result WhatsAppWriter::writeSessionsList(const Data::SessionsList &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeOtherData(const Data::File &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeDialogsStart(const Data::DialogsInfo &data) {
	return Result::Success();
}

Result WhatsAppWriter::writeDialogStart(const Data::DialogInfo &data) {
	Expects(!_zipChatEntryOpen);

	_dialog = data;
	_pendingMedia.clear();
	_mediaCounter = 0;
	_zip.emplace();

	const auto chatFileName = (u"WhatsApp Chat with "_q
		+ SanitizeFileName(QString::fromUtf8(DialogDisplayName(data)))
		+ u".txt"_q).toUtf8();
	zip_fileinfo zfi = { { 0, 0, 0, 0, 0, 0 }, 0, 0, 0 };
	_zip->openNewFile(
		chatFileName.constData(),
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
		const auto &file = message.file();
		const auto hasFile = !file.relativePath.isEmpty();
		const auto text = JoinText(message.text);
		if (!hasFile && text.isEmpty()) {
			continue;
		}
		const auto sender = SenderName(message.fromId, data.peers);
		block.append(FormatWhatsAppDate(message.date));
		block.append(" - ").append(sender).append(": ");

		if (hasFile) {
			const auto diskPath = _settings.path + file.relativePath;
			const auto suffix = QFileInfo(file.relativePath).suffix();
			const auto archiveName = QString("%1-%2-WA%3"
			).arg(MediaPrefix(message)
			).arg(FormatMediaDate(message.date)
			).arg(++_mediaCounter, 4, 10, QChar('0'))
				+ (suffix.isEmpty() ? QString() : (QChar('.') + suffix));
			_pendingMedia.push_back({ diskPath, archiveName });
			block.append(archiveName.toUtf8()).append(" (file attached)");
			if (!text.isEmpty()) {
				block.append('\n').append(text);
			}
		} else {
			block.append(text);
		}
		block.append('\n');
	}
	if (block.isEmpty()) {
		return Result::Success();
	}
	_zip->writeInFile(block.constData(), block.size());
	if (_stats) {
		_stats->incrementBytes(block.size());
	}
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

	const auto name = SanitizeFileName(
		QString::fromUtf8(DialogDisplayName(_dialog)));
	const auto path = UniqueZipPath(
		_outputPath,
		u"WhatsApp Chat with "_q + name);
	auto out = QFile(path);
	const auto result = _zip->result();
	if (!out.open(QIODevice::WriteOnly)
		|| out.write(result) != result.size()) {
		return Result(Result::Type::FatalError, path);
	}
	_lastZipPath = path;
	if (_stats) {
		_stats->incrementFiles();
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
	removeWorkFolder();
	return Result::Success();
}

QString WhatsAppWriter::mainFilePath() {
	return _lastZipPath.isEmpty() ? _outputPath : _lastZipPath;
}

} // namespace Output
} // namespace Export
