#pragma once

#include "iv/markdown/iv_markdown_document.h"

#include <QtCore/QByteArray>

namespace Iv::Markdown {

struct ValidatedMarkdownSource {
	QByteArray normalized;
	QString decoded;
	std::vector<int> lineStarts;
	QString sourceName;
};

struct MarkdownSourceValidationResult {
	ValidatedMarkdownSource source;
	QString error;
	bool ok = true;
};

[[nodiscard]] MarkdownSourceValidationResult ValidateMarkdownSourceForIv(
	const QByteArray &source,
	ParseOptions options = {});
[[nodiscard]] ParseResult ParseMarkdownForIv(ValidatedMarkdownSource source);
[[nodiscard]] ParseResult ParseMarkdownForIv(
	const QByteArray &source,
	ParseOptions options = {});

} // namespace Iv::Markdown
