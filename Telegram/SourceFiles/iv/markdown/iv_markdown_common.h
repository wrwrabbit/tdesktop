#pragma once

#include <QtCore/QSize>
#include <QtCore/QString>

#include <functional>
#include <memory>

namespace Ui {
class DynamicImage;
} // namespace Ui

namespace Iv {
class Delegate;
} // namespace Iv

namespace Iv::Markdown {

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

class MediaRuntime {
public:
	virtual ~MediaRuntime() = default;

	[[nodiscard]] virtual std::shared_ptr<Ui::DynamicImage> resolveInlineImage(
		uint64 documentId,
		QSize size) const = 0;
	[[nodiscard]] virtual std::shared_ptr<PhotoRuntime> resolvePhoto(
		uint64 photoId) const = 0;
};

enum class MediaActivationKind {
	None,
	ExternalUrl,
	Photo,
};

struct MediaActivation {
	MediaActivationKind kind = MediaActivationKind::None;
	QString url;
	std::shared_ptr<PhotoRuntime> photo;
};

struct OpenOptions {
	QString sourceName;
	QString sourcePath;
	QString initialFragment;
	Iv::Delegate *delegate = nullptr;
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
};

} // namespace Iv::Markdown
