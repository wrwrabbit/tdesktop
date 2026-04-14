/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "data/stickers/data_stickers.h"
#include "mtproto/sender.h"

class DocumentData;

namespace Main {
class Session;
} // namespace Main

namespace Api {

void AddExistingStickerToSet(
	not_null<Main::Session*> session,
	const StickerSetIdentifier &set,
	not_null<DocumentData*> document,
	const QString &emoji,
	Fn<void(MTPmessages_StickerSet)> done,
	Fn<void(QString)> fail);

void DeleteStickerSet(
	not_null<Main::Session*> session,
	const StickerSetIdentifier &set,
	Fn<void()> done,
	Fn<void(QString)> fail);

class StickerUpload final : public base::has_weak_ptr {
public:
	StickerUpload(
		not_null<Main::Session*> session,
		StickerSetIdentifier set,
		QByteArray webpBytes,
		QString emoji);
	~StickerUpload();

	void start(
		Fn<void(MTPmessages_StickerSet)> done,
		Fn<void(QString)> fail,
		Fn<void(int /*percent*/)> progress = nullptr);

	void cancel();

private:
	void uploadReady(const MTPInputFile &file);
	void uploadFailed();
	void uploadProgressed();
	void requestAddSticker(const MTPInputDocument &document);

	const not_null<Main::Session*> _session;
	StickerSetIdentifier _set;
	QByteArray _bytes;
	QString _emoji;
	MTP::Sender _api;
	rpl::lifetime _uploadLifetime;
	FullMsgId _uploadId;
	DocumentId _documentId = 0;
	mtpRequestId _addRequestId = 0;

	Fn<void(MTPmessages_StickerSet)> _done;
	Fn<void(QString)> _fail;
	Fn<void(int)> _progress;
	int _lastReportedPercent = -1;

};

} // namespace Api
