#include "iv/markdown/iv_markdown_view.h"

#include "ui/rp_widget.h"

namespace Iv::Markdown {

std::unique_ptr<Ui::RpWidget> CreateMarkdownPreviewWidget(
	const PreparedDocument &,
	const OpenOptions &) {
	return std::make_unique<Ui::RpWidget>();
}

} // namespace Iv::Markdown
