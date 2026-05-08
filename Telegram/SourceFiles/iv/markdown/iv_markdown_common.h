/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include <rpl/never.h>
#include <rpl/producer.h>

#include <functional>
#include <memory>

namespace Ui {
class DynamicImage;
class Show;
} // namespace Ui

namespace Iv {
class Delegate;
} // namespace Iv

namespace Iv::Markdown {

class MediaBlock;
class HostedMediaBlockFactory;
class PhotoRuntime {
public:
	virtual ~PhotoRuntime() = default;

	[[nodiscard]] virtual std::shared_ptr<Ui::DynamicImage> thumbnail(
		QSize size) const = 0;
	[[nodiscard]] virtual std::shared_ptr<Ui::DynamicImage> full(
		QSize size) const = 0;
	[[nodiscard]] virtual bool loaded() const = 0;
	[[nodiscard]] virtual bool loading() const = 0;
	[[nodiscard]] virtual double progress() const = 0;
	virtual void open(Qt::MouseButton button) const = 0;
};

class DocumentRuntime {
public:
	virtual ~DocumentRuntime() = default;

	[[nodiscard]] virtual std::shared_ptr<Ui::DynamicImage> thumbnail(
		QSize size) const = 0;
	[[nodiscard]] virtual std::shared_ptr<Ui::DynamicImage> full(
		QSize size) const = 0;
	[[nodiscard]] virtual bool loaded() const = 0;
	[[nodiscard]] virtual bool loading() const = 0;
	[[nodiscard]] virtual double progress() const = 0;
	virtual void open(Qt::MouseButton button) const = 0;
};

class MapRuntime {
public:
	virtual ~MapRuntime() = default;

	[[nodiscard]] virtual std::shared_ptr<Ui::DynamicImage> thumbnail(
		QSize size) const = 0;
	[[nodiscard]] virtual std::shared_ptr<Ui::DynamicImage> full(
		QSize size) const = 0;
	[[nodiscard]] virtual bool loaded() const = 0;
	[[nodiscard]] virtual bool loading() const = 0;
	[[nodiscard]] virtual double progress() const = 0;
};

class ChannelRuntime {
public:
	virtual ~ChannelRuntime() = default;

	[[nodiscard]] virtual bool joinVisible() const = 0;
	virtual void open(Qt::MouseButton button) const = 0;
	virtual void join(Qt::MouseButton button) const = 0;
};

struct PreparedPhotoBlockData;
struct PreparedVideoBlockData;
struct PreparedAudioBlockData;
struct PreparedMapBlockData;

class HostedMediaBlockFactory {
public:
	virtual ~HostedMediaBlockFactory() = default;

	[[nodiscard]] virtual std::shared_ptr<MediaBlock> createPhoto(
		const PreparedPhotoBlockData &prepared) const {
		return nullptr;
	}
	[[nodiscard]] virtual std::shared_ptr<MediaBlock> createVideo(
		const PreparedVideoBlockData &prepared) const {
		return nullptr;
	}
	[[nodiscard]] virtual std::shared_ptr<MediaBlock> createAudio(
		const PreparedAudioBlockData &prepared) const {
		return nullptr;
	}
	[[nodiscard]] virtual std::shared_ptr<MediaBlock> createMap(
		const PreparedMapBlockData &prepared) const {
		return nullptr;
	}
};

class MediaRuntime {
public:
	virtual ~MediaRuntime() = default;

	[[nodiscard]] virtual std::shared_ptr<Ui::DynamicImage> resolveInlineImage(
		uint64 documentId,
		QSize size) const = 0;
	[[nodiscard]] virtual std::shared_ptr<PhotoRuntime> resolvePhoto(
		uint64 photoId) const = 0;
	[[nodiscard]] virtual std::shared_ptr<DocumentRuntime> resolveDocument(
		uint64 documentId) const {
		return nullptr;
	}
	[[nodiscard]] virtual std::shared_ptr<MapRuntime> resolveMap(
		double latitude,
		double longitude,
		uint64 accessHash,
		QSize size,
		int zoom) const {
		return nullptr;
	}
	[[nodiscard]] virtual std::shared_ptr<ChannelRuntime> resolveChannel(
		uint64 channelId,
		const QString &username) const {
		return nullptr;
	}
	[[nodiscard]] virtual rpl::producer<uint64> channelJoinedChanges() const {
		return rpl::never<uint64>();
	}
	[[nodiscard]] virtual std::shared_ptr<HostedMediaBlockFactory>
	hostedMediaBlockFactory() const {
		return nullptr;
	}
};

enum class MediaActivationKind {
	None,
	ExternalUrl,
	Photo,
	Document,
	OpenChannel,
	JoinChannel,
};

struct MediaActivation {
	MediaActivationKind kind = MediaActivationKind::None;
	QString url;
	std::shared_ptr<PhotoRuntime> photo;
	std::shared_ptr<DocumentRuntime> document;
	std::shared_ptr<ChannelRuntime> channel;
};

enum class ViewerKind {
	Auto,
	LocalFile,
	InstantView,
};

struct OpenOptions {
	QString sourceName;
	QString sourcePath;
	QString sourceUrl;
	QString initialFragment;
	ViewerKind viewerKind = ViewerKind::Auto;
	Iv::Delegate *delegate = nullptr;
	QVariant clickHandlerContext;
	std::shared_ptr<QVariant> clickHandlerContextRef;
	std::function<void()> openSource;
	std::function<void(std::shared_ptr<Ui::Show>)> share;
	std::function<bool(const MediaActivation &, Qt::MouseButton)> activateMedia;
};

struct ParseOptions {
	QString sourceName;
};

[[nodiscard]] bool LooksLikeMarkdownFile(
	const QString &fileName,
	const QString &mimeType = QString());

struct Event {
	enum class Type {
		Close,
		Quit,
		OpenFile,
	};
	Type type = Type::Close;
	QString url;
	QVariant context;
};

} // namespace Iv::Markdown
