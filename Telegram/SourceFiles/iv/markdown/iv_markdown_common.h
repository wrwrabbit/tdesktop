#pragma once

#include <QtCore/QString>

namespace Iv {
class Delegate;
} // namespace Iv

namespace Iv::Markdown {

struct OpenOptions {
	QString sourceName;
	QString sourcePath;
	QString initialFragment;
	Iv::Delegate *delegate = nullptr;
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
