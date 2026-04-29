#pragma once

#include "iv/markdown/iv_markdown_document.h"

#include <memory>

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Iv::Markdown {

[[nodiscard]] std::unique_ptr<Ui::RpWidget> CreateMarkdownPreviewWidget(
	const PreparedDocument &document,
	const OpenOptions &options = {});

} // namespace Iv::Markdown
