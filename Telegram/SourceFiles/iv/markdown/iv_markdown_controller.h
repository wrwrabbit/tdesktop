#pragma once

#include "iv/markdown/iv_markdown_common.h"

namespace Iv::Markdown {

class Controller final {
public:
	explicit Controller(OpenOptions options = {});

	[[nodiscard]] const OpenOptions &options() const;

private:
	OpenOptions _options;

};

} // namespace Iv::Markdown
