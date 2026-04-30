#include "iv/markdown/iv_markdown_common.h"

#include <array>

namespace Iv::Markdown {

bool LooksLikeMarkdownFile(const QString &fileName, const QString &mimeType) {
	const auto suffixes = std::array{
		u".md"_q,
		u".markdown"_q,
	};
	auto suffixMatches = false;
	for (const auto &suffix : suffixes) {
		if (fileName.endsWith(suffix, Qt::CaseInsensitive)) {
			suffixMatches = true;
			break;
		}
	}
	if (!suffixMatches) {
		return false;
	}
	if (mimeType.isEmpty()) {
		return true;
	}

	const auto normalized = mimeType.trimmed().toLower();
	const auto acceptedMimeTypes = std::array{
		u"text/markdown"_q,
		u"text/x-markdown"_q,
		u"text/plain"_q,
	};
	for (const auto &accepted : acceptedMimeTypes) {
		if (normalized == accepted) {
			return true;
		}
	}
	return false;
}

} // namespace Iv::Markdown
