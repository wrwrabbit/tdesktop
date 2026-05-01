#pragma once

#include "iv/markdown/iv_markdown_document.h"

#include <memory>

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Iv::Markdown {

[[nodiscard]] std::unique_ptr<Ui::RpWidget> CreateMarkdownPreviewWidget(
	QWidget *parent,
	const PreparedDocument &document,
	Fn<void(Event)> callback,
	const OpenOptions &options = {});

} // namespace Iv::Markdown
