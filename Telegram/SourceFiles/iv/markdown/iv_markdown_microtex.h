#pragma once

#include "iv/markdown/iv_markdown_document.h"

#include <QtCore/QSize>
#include <QtGui/QImage>

namespace Iv::Markdown {

struct MicrotexRenderRequest {
	QString trimmedTex;
	MathKind kind = MathKind::Display;
	int textSize = 0;
	int renderWidthCap = 0;
	int renderHeightCap = 0;
	int devicePixelRatio = 1;
};

struct MicrotexRenderResult {
	QImage image;
	QSize logicalSize;
	int logicalDepth = 0;
	QString error;
	bool ok = false;
};

[[nodiscard]] bool EnsureMicrotexInitialized(QString *error = nullptr);
[[nodiscard]] MicrotexRenderResult RenderWithMicrotex(
	const MicrotexRenderRequest &request);
[[nodiscard]] bool MicrotexBackendLinked();

} // namespace Iv::Markdown
