#include "iv/markdown/iv_markdown_controller.h"

#include <utility>

namespace Iv::Markdown {

Controller::Controller(OpenOptions options)
: _options(std::move(options)) {
}

const OpenOptions &Controller::options() const {
	return _options;
}

} // namespace Iv::Markdown
