#include "iv/markdown/iv_markdown_microtex.h"

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QPainter>

#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#include "latex.h"
#ifdef _MSC_VER
#pragma include_alias("config.h", "../../../ThirdParty/MicroTeX/src/config.h")
#endif // _MSC_VER
#include "platform/qt/graphic_qt.h"

namespace Iv::Markdown {
namespace {

constexpr auto kBytesPerPixel = int64(4);
constexpr auto kMaxFormulaImageBytes = int64(128) * 1024 * 1024;

std::once_flag MicrotexInitOnce;
bool MicrotexInitialized = false;
QString MicrotexInitError;

[[nodiscard]] QString ExceptionText(const std::exception &exception) {
	return QString::fromUtf8(exception.what());
}

[[nodiscard]] std::wstring ToWide(const QString &value) {
	auto result = std::wstring();
	result.reserve(size_t(value.size()));
	for (const auto ch : value) {
		result.push_back(wchar_t(ch.unicode()));
	}
	return result;
}

[[nodiscard]] bool PhysicalImageRejected(int64 width, int64 height) {
	if (width <= 0 || height <= 0) {
		return true;
	}
	return (width > std::numeric_limits<int>::max())
		|| (height > std::numeric_limits<int>::max())
		|| (width * height > (kMaxFormulaImageBytes / kBytesPerPixel));
}

[[nodiscard]] bool ComputePhysicalSize(
		QSize logicalSize,
		int ratio,
		QSize *physicalSize) {
	const auto width = int64(logicalSize.width()) * ratio;
	const auto height = int64(logicalSize.height()) * ratio;
	if (PhysicalImageRejected(width, height)) {
		return false;
	}
	if (physicalSize) {
		*physicalSize = QSize(int(width), int(height));
	}
	return true;
}

[[nodiscard]] QColor NormalizeForeground(QColor color) {
	return color.isValid() ? color : QColor(Qt::black);
}

[[nodiscard]] QString PreparedTeX(MathKind kind, const QString &trimmedTex) {
	auto result = trimmedTex;
	if (kind == MathKind::Display) {
		result = u"\\displaystyle "_q + result;
	}
	return result;
}

} // namespace

bool EnsureMicrotexInitialized(QString *error) {
	std::call_once(MicrotexInitOnce, [] {
		try {
			Q_INIT_RESOURCE(bundled);
			tex::LaTeX::initBundled();
			MicrotexInitialized = true;
		} catch (const std::exception &exception) {
			MicrotexInitError = ExceptionText(exception);
		} catch (...) {
			MicrotexInitError = u"unknown-exception"_q;
		}
	});
	if (!MicrotexInitialized && error) {
		*error = MicrotexInitError;
	} else if (MicrotexInitialized && error) {
		error->clear();
	}
	return MicrotexInitialized;
}

MicrotexRenderResult RenderWithMicrotex(const MicrotexRenderRequest &request) {
	auto result = MicrotexRenderResult();
	if (!EnsureMicrotexInitialized(&result.error)) {
		return result;
	}
	if (request.devicePixelRatio <= 0) {
		result.error = u"invalid-device-pixel-ratio"_q;
		return result;
	}
	if (request.textSize <= 0) {
		result.error = u"invalid-text-size"_q;
		return result;
	}
	if (request.renderWidthCap <= 0) {
		result.error = u"invalid-render-width"_q;
		return result;
	}
	if (request.renderHeightCap <= 0) {
		result.error = u"invalid-render-height"_q;
		return result;
	}
	const auto trimmedTex = request.trimmedTex.trimmed();
	if (trimmedTex.isEmpty()) {
		result.error = u"empty-tex"_q;
		return result;
	}
	try {
		const auto ratio = request.devicePixelRatio;
		const auto textSize = int64(request.textSize);
		const auto maxWidth = int64(request.renderWidthCap);
		if (textSize > std::numeric_limits<int>::max()) {
			result.error = u"text-size-overflow"_q;
			return result;
		}
		if (maxWidth > std::numeric_limits<int>::max()) {
			result.error = u"render-width-overflow"_q;
			return result;
		}
		const auto tex = PreparedTeX(request.kind, trimmedTex);
		auto render = std::unique_ptr<tex::TeXRender>(tex::LaTeX::parse(
			ToWide(tex),
			int(maxWidth),
			float(textSize),
			float(textSize) * 0.25f,
			NormalizeForeground(request.foreground).rgba()));
		if (!render) {
			result.error = u"parse-returned-null"_q;
			return result;
		}
		const auto logicalSize = QSize(
			render->getWidth(),
			render->getHeight());
		if (logicalSize.width() <= 0 || logicalSize.height() <= 0) {
			result.error = u"invalid-render-size"_q;
			return result;
		}
		if (logicalSize.width() > request.renderWidthCap
			|| logicalSize.height() > request.renderHeightCap) {
			result.error = u"logical-size-cap-exceeded"_q;
			return result;
		}
		auto physicalSize = QSize();
		if (!ComputePhysicalSize(logicalSize, ratio, &physicalSize)) {
			result.error = u"physical-image-cap-exceeded"_q;
			return result;
		}
		auto image = QImage(
			physicalSize,
			QImage::Format_ARGB32_Premultiplied);
		if (image.isNull()) {
			result.error = u"image-allocation-failed"_q;
			return result;
		}
		image.setDevicePixelRatio(ratio);
		image.fill(Qt::transparent);
		{
			QPainter painter(&image);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setRenderHint(QPainter::TextAntialiasing, true);
			tex::Graphics2D_qt graphics(&painter);
			render->draw(graphics, 0, 0);
		}
		result.image = std::move(image);
		result.logicalSize = logicalSize;
		result.ok = true;
		return result;
	} catch (const std::exception &exception) {
		result.error = ExceptionText(exception);
		return result;
	} catch (...) {
		result.error = u"unknown-exception"_q;
		return result;
	}
}

bool MicrotexBackendLinked() {
	return true;
}

} // namespace Iv::Markdown
