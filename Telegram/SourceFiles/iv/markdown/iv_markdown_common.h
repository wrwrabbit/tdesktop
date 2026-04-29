#pragma once

#include <QtCore/QString>

namespace Iv::Markdown {

struct OpenOptions {
	QString sourceName;
};

struct ParseOptions {
	QString sourceName;
};

[[nodiscard]] bool LooksLikeMarkdownFile(
	const QString &fileName,
	const QString &mimeType = QString());

} // namespace Iv::Markdown
