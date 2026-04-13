/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_stickers_creator.h"

#include "apiwrap.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "main/main_session.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"

namespace Api {
namespace {

constexpr auto kStickerSide = 512;

[[nodiscard]] MTPInputStickerSetItem InputItem(
		const MTPInputDocument &document,
		const QString &emoji) {
	return MTP_inputStickerSetItem(
		MTP_flags(0),
		document,
		MTP_string(emoji),
		MTPMaskCoords(),
		MTPstring());
}

[[nodiscard]] std::shared_ptr<FilePrepareResult> PrepareStickerWebp(
		MTP::DcId dcId,
		DocumentId id,
		const QByteArray &bytes) {
	const auto filename = u"sticker.webp"_q;
	auto attributes = QVector<MTPDocumentAttribute>(
		1,
		MTP_documentAttributeFilename(MTP_string(filename)));
	attributes.push_back(MTP_documentAttributeImageSize(
		MTP_int(kStickerSide),
		MTP_int(kStickerSide)));

	auto result = MakePreparedFile({
		.id = id,
		.type = SendMediaType::File,
	});
	result->filename = filename;
	result->filemime = u"image/webp"_q;
	result->content = bytes;
	result->filesize = bytes.size();
	result->setFileData(bytes);
	result->document = MTP_document(
		MTP_flags(0),
		MTP_long(id),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(base::unixtime::now()),
		MTP_string("image/webp"),
		MTP_long(bytes.size()),
		MTP_vector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(dcId),
		MTP_vector<MTPDocumentAttribute>(std::move(attributes)));
	return result;
}

void FeedSetIfFull(
		not_null<Main::Session*> session,
		const MTPmessages_StickerSet &result) {
	result.match([&](const MTPDmessages_stickerSet &data) {
		session->data().stickers().feedSetFull(data);
		session->data().stickers().notifyUpdated(
			Data::StickersType::Stickers);
	}, [](const auto &) {
	});
}

} // namespace

void AddExistingStickerToSet(
		not_null<Main::Session*> session,
		const StickerSetIdentifier &set,
		not_null<DocumentData*> document,
		const QString &emoji,
		Fn<void(MTPmessages_StickerSet)> done,
		Fn<void(QString)> fail) {
	session->api().request(MTPstickers_AddStickerToSet(
		Data::InputStickerSet(set),
		InputItem(document->mtpInput(), emoji))
	).done([=](const MTPmessages_StickerSet &result) {
		FeedSetIfFull(session, result);
		if (done) {
			done(result);
		}
	}).fail([=](const MTP::Error &error) {
		if (fail) {
			fail(error.type());
		}
	}).handleFloodErrors().send();
}

StickerUpload::StickerUpload(
	not_null<Main::Session*> session,
	StickerSetIdentifier set,
	QByteArray webpBytes,
	QString emoji)
: _session(session)
, _set(std::move(set))
, _bytes(std::move(webpBytes))
, _emoji(std::move(emoji))
, _api(&session->mtp()) {
}

StickerUpload::~StickerUpload() {
	cancel();
}

void StickerUpload::start(
		Fn<void(MTPmessages_StickerSet)> done,
		Fn<void(QString)> fail,
		Fn<void(int)> progress) {
	Expects(!_uploadId);

	_done = std::move(done);
	_fail = std::move(fail);
	_progress = std::move(progress);

	_documentId = base::RandomValue<DocumentId>();
	auto ready = PrepareStickerWebp(
		_session->mtp().mainDcId(),
		_documentId,
		_bytes);
	_uploadId = FullMsgId(
		_session->userPeerId(),
		_session->data().nextLocalMessageId());

	const auto document = _session->data().document(_documentId);
	document->uploadingData = std::make_unique<Data::UploadState>(
		document->size > 0 ? document->size : int64(_bytes.size()));

	_session->uploader().documentReady(
	) | rpl::filter([=](const Storage::UploadedMedia &data) {
		return data.fullId == _uploadId;
	}) | rpl::on_next([=](const Storage::UploadedMedia &data) {
		uploadReady(data.info.file);
	}, _uploadLifetime);

	_session->uploader().documentFailed(
	) | rpl::filter([=](const FullMsgId &id) {
		return id == _uploadId;
	}) | rpl::on_next([=] {
		uploadFailed();
	}, _uploadLifetime);

	if (_progress) {
		_session->uploader().documentProgress(
		) | rpl::filter([=](const FullMsgId &id) {
			return id == _uploadId;
		}) | rpl::on_next([=] {
			uploadProgressed();
		}, _uploadLifetime);
	}

	_session->uploader().upload(_uploadId, ready);
}

void StickerUpload::cancel() {
	if (_uploadId) {
		_session->uploader().cancel(_uploadId);
		_uploadId = FullMsgId();
	}
	if (_addRequestId) {
		_api.request(_addRequestId).cancel();
		_addRequestId = 0;
	}
	_uploadLifetime.destroy();
	_done = nullptr;
	_fail = nullptr;
	_progress = nullptr;
}

void StickerUpload::uploadProgressed() {
	if (!_progress) {
		return;
	}
	const auto document = _session->data().document(_documentId);
	if (!document->uploading() || !document->uploadingData) {
		return;
	}
	const auto size = document->uploadingData->size;
	if (size <= 0) {
		return;
	}
	const auto percent = int(
		(document->uploadingData->offset * 100) / size);
	if (percent != _lastReportedPercent) {
		_lastReportedPercent = percent;
		_progress(percent);
	}
}

void StickerUpload::uploadFailed() {
	const auto fail = std::move(_fail);
	cancel();
	if (fail) {
		fail(QString());
	}
}

void StickerUpload::uploadReady(const MTPInputFile &file) {
	_uploadLifetime.destroy();
	_uploadId = FullMsgId();

	auto attributes = QVector<MTPDocumentAttribute>();
	attributes.push_back(MTP_documentAttributeSticker(
		MTP_flags(0),
		MTP_string(_emoji),
		MTP_inputStickerSetEmpty(),
		MTPMaskCoords()));
	attributes.push_back(MTP_documentAttributeImageSize(
		MTP_int(kStickerSide),
		MTP_int(kStickerSide)));

	const auto media = MTP_inputMediaUploadedDocument(
		MTP_flags(0),
		file,
		MTPInputFile(),
		MTP_string("image/webp"),
		MTP_vector<MTPDocumentAttribute>(std::move(attributes)),
		MTP_vector<MTPInputDocument>(),
		MTPInputPhoto(),
		MTP_int(0),
		MTP_int(0));

	_addRequestId = _api.request(MTPmessages_UploadMedia(
		MTP_flags(0),
		MTPstring(),
		MTP_inputPeerSelf(),
		media
	)).done(crl::guard(this, [=](const MTPMessageMedia &result) {
		_addRequestId = 0;
		auto inputDoc = (MTPInputDocument*)(nullptr);
		auto storage = MTPInputDocument();
		result.match([&](const MTPDmessageMediaDocument &data) {
			if (const auto doc = data.vdocument()) {
				doc->match([&](const MTPDdocument &d) {
					storage = MTP_inputDocument(
						d.vid(),
						d.vaccess_hash(),
						d.vfile_reference());
					inputDoc = &storage;
				}, [](const auto &) {
				});
			}
		}, [](const auto &) {
		});
		if (inputDoc) {
			requestAddSticker(*inputDoc);
		} else if (const auto fail = std::move(_fail)) {
			cancel();
			fail(QString());
		}
	})).fail(crl::guard(this, [=](const MTP::Error &error) {
		_addRequestId = 0;
		const auto fail = std::move(_fail);
		const auto type = error.type();
		cancel();
		if (fail) {
			fail(type);
		}
	})).handleFloodErrors().send();
}

void StickerUpload::requestAddSticker(const MTPInputDocument &document) {
	_addRequestId = _api.request(MTPstickers_AddStickerToSet(
		Data::InputStickerSet(_set),
		InputItem(document, _emoji))
	).done(crl::guard(this, [=](const MTPmessages_StickerSet &result) {
		_addRequestId = 0;
		FeedSetIfFull(_session, result);
		const auto done = std::move(_done);
		cancel();
		if (done) {
			done(result);
		}
	})).fail(crl::guard(this, [=](const MTP::Error &error) {
		_addRequestId = 0;
		const auto fail = std::move(_fail);
		const auto type = error.type();
		cancel();
		if (fail) {
			fail(type);
		}
	})).handleFloodErrors().send();
}

} // namespace Api
