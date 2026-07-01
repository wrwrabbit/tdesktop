/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "export/output/export_output_abstract.h"
#include "export/output/export_output_file.h"
#include "export/export_settings.h"
#include "export/data/export_data_types.h"

#include "base/zlib_help.h"

#include <optional>

namespace Export {
namespace Output {

class WhatsAppWriter : public AbstractWriter {
public:
	Format format() override {
		return Format::WhatsApp;
	}

	Result start(
		const Settings &settings,
		const Environment &environment,
		Stats *stats) override;

	Result writePersonal(const Data::PersonalInfo &data) override;

	Result writeUserpicsStart(const Data::UserpicsInfo &data) override;
	Result writeUserpicsSlice(const Data::UserpicsSlice &data) override;
	Result writeUserpicsEnd() override;

	Result writeStoriesStart(const Data::StoriesInfo &data) override;
	Result writeStoriesSlice(const Data::StoriesSlice &data) override;
	Result writeStoriesEnd() override;

	Result writeProfileMusicStart(const Data::ProfileMusicInfo &data) override;
	Result writeProfileMusicSlice(const Data::ProfileMusicSlice &data) override;
	Result writeProfileMusicEnd() override;

	Result writeContactsList(const Data::ContactsList &data) override;

	Result writeSessionsList(const Data::SessionsList &data) override;

	Result writeOtherData(const Data::File &data) override;

	Result writeDialogsStart(const Data::DialogsInfo &data) override;
	Result writeDialogStart(const Data::DialogInfo &data) override;
	Result writeDialogSlice(const Data::MessagesSlice &data) override;
	Result writeDialogEnd() override;
	Result writeDialogsEnd() override;

	Result finish() override;

	QString mainFilePath() override;

private:
	struct PendingMedia {
		QString diskPath;
		QString archiveName;
	};

	[[nodiscard]] Result writeTextFile(
		const QString &relativeName,
		const QByteArray &content);

	[[nodiscard]] QString unique(const QString &relativePath) const;

	Settings _settings;
	Environment _environment;
	Stats *_stats = nullptr;

	// Per-dialog state.
	std::optional<zlib::FileToWrite> _zip;
	bool _zipChatEntryOpen = false;
	Data::DialogInfo _dialog;
	std::vector<PendingMedia> _pendingMedia;

};

} // namespace Output
} // namespace Export
