#include "iv/markdown/iv_markdown_math_renderer.h"

#include <QtGui/QColor>

#include <limits>
#include <utility>

#include "ui/style/style_core.h"

#include "styles/style_iv.h"

namespace Iv::Markdown {
namespace {

constexpr auto kBytesPerPixel = int64(4);
constexpr auto kMaxFormulaImageBytes = int64(128) * 1024 * 1024;

[[nodiscard]] QColor NormalizeForeground(QColor color) {
	return color.isValid() ? color : QColor(Qt::black);
}

[[nodiscard]] FormulaCacheKey NormalizeKey(
		const MicrotexRenderRequest &request,
		int paletteVersion) {
	const auto foreground = NormalizeForeground(request.foreground);
	return {
		.trimmedTex = request.trimmedTex.trimmed(),
		.kind = request.kind,
		.textSize = request.textSize,
		.renderWidthCap = request.renderWidthCap,
		.foregroundRgba = foreground.rgba(),
		.paletteVersion = paletteVersion,
		.devicePixelRatio = request.devicePixelRatio,
	};
}

[[nodiscard]] QString FallbackText(const FormulaCacheKey &key) {
	return key.trimmedTex.isEmpty()
		? QStringLiteral("[math]")
		: key.trimmedTex;
}

[[nodiscard]] bool PhysicalImageRejected(int64 width, int64 height) {
	if (width <= 0 || height <= 0) {
		return true;
	}
	return (width > std::numeric_limits<int>::max())
		|| (height > std::numeric_limits<int>::max())
		|| (width * height > (kMaxFormulaImageBytes / kBytesPerPixel));
}

[[nodiscard]] bool PhysicalImageRejected(QSize size) {
	return PhysicalImageRejected(size.width(), size.height());
}

[[nodiscard]] bool TooLargeFailure(const QString &error) {
	return (error == QStringLiteral("render-width-cap-exceeded"))
		|| (error == QStringLiteral("physical-size-cap-exceeded"))
		|| (error == QStringLiteral("logical-size-cap-exceeded"))
		|| (error == QStringLiteral("physical-image-cap-exceeded"));
}

} // namespace

const RenderedFormula *FormulaCache::find(const FormulaCacheKey &key) const {
	const auto i = _entries.find(key);
	return (i != _entries.end()) ? &i->second : nullptr;
}

void FormulaCache::put(FormulaCacheKey key, RenderedFormula value) {
	_entries[std::move(key)] = std::move(value);
}

void FormulaCache::clear() {
	_entries.clear();
}

RenderedFormula MathRenderer::renderFormula(
		const MicrotexRenderRequest &request,
		int paletteVersion) {
	const auto key = makeKey(request, paletteVersion);
	if (const auto cached = _cache.find(key)) {
		++_debugCounters.hits;
		return *cached;
	}
	++_debugCounters.misses;
	QString error;
	if (rejectRequestByCaps(key, &error)) {
		++_debugCounters.failed;
		auto failure = makeFailure(key, error, TooLargeFailure(error));
		_cache.put(key, failure);
		return failure;
	}
	auto normalized = request;
	normalized.trimmedTex = key.trimmedTex;
	normalized.textSize = key.textSize;
	normalized.renderWidthCap = key.renderWidthCap;
	normalized.foreground = QColor::fromRgba(key.foregroundRgba);
	normalized.devicePixelRatio = key.devicePixelRatio;
	auto rendered = RenderWithMicrotex(normalized);
	if (!rendered.ok) {
		++_debugCounters.failed;
		auto failure = makeFailure(
			key,
			rendered.error,
			TooLargeFailure(rendered.error));
		_cache.put(key, failure);
		return failure;
	}
	if (rejectResultByCaps(key, rendered, &error)) {
		++_debugCounters.failed;
		auto failure = makeFailure(key, error, TooLargeFailure(error));
		_cache.put(key, failure);
		return failure;
	}
	auto result = RenderedFormula();
	result.image = std::move(rendered.image);
	result.logicalSize = rendered.logicalSize;
	result.success = true;
	++_debugCounters.rendered;
	_cache.put(key, result);
	return result;
}

void MathRenderer::clearCache(bool resetDebugCounters) {
	_cache.clear();
	if (resetDebugCounters) {
		_debugCounters = FormulaDebugCounters();
	}
}

void MathRenderer::invalidate(bool resetDebugCounters) {
	clearCache(resetDebugCounters);
}

void MathRenderer::resetDebugCounters() {
	_debugCounters = FormulaDebugCounters();
}

const FormulaDebugCounters &MathRenderer::debugCounters() const {
	return _debugCounters;
}

FormulaCacheKey MathRenderer::makeKey(
		const MicrotexRenderRequest &request,
		int paletteVersion) const {
	return NormalizeKey(request, paletteVersion);
}

RenderedFormula MathRenderer::makeFailure(
		const FormulaCacheKey &key,
		const QString &error,
		bool tooLarge) const {
	return {
		.image = QImage(),
		.logicalSize = QSize(),
		.fallbackText = FallbackText(key),
		.error = error,
		.success = false,
		.overflow = tooLarge,
		.tooLarge = tooLarge,
	};
}

bool MathRenderer::rejectRequestByCaps(
		const FormulaCacheKey &key,
		QString *error) const {
	const auto maxWidth = st::ivMarkdownDisplayMathMaxRenderWidth;
	const auto maxHeight = st::ivMarkdownDisplayMathMaxRenderHeight;
	if (key.trimmedTex.isEmpty()) {
		if (error) {
			*error = QStringLiteral("empty-tex");
		}
		return true;
	}
	if (key.textSize <= 0) {
		if (error) {
			*error = QStringLiteral("invalid-text-size");
		}
		return true;
	}
	if (key.renderWidthCap <= 0) {
		if (error) {
			*error = QStringLiteral("invalid-render-width");
		}
		return true;
	}
	if (key.devicePixelRatio <= 0) {
		if (error) {
			*error = QStringLiteral("invalid-device-pixel-ratio");
		}
		return true;
	}
	if (maxWidth <= 0) {
		if (error) {
			*error = QStringLiteral("invalid-render-width-cap");
		}
		return true;
	}
	if (key.renderWidthCap > maxWidth) {
		if (error) {
			*error = QStringLiteral("render-width-cap-exceeded");
		}
		return true;
	}
	if (maxHeight <= 0) {
		if (error) {
			*error = QStringLiteral("invalid-render-height-cap");
		}
		return true;
	}
	if (PhysicalImageRejected(
		int64(key.renderWidthCap) * key.devicePixelRatio,
		int64(maxHeight) * key.devicePixelRatio)) {
		if (error) {
			*error = QStringLiteral("physical-size-cap-exceeded");
		}
		return true;
	}
	return false;
}

bool MathRenderer::rejectResultByCaps(
		const FormulaCacheKey &key,
		const MicrotexRenderResult &result,
		QString *error) const {
	const auto maxWidth = st::ivMarkdownDisplayMathMaxRenderWidth;
	const auto maxHeight = st::ivMarkdownDisplayMathMaxRenderHeight;
	if (result.logicalSize.width() <= 0 || result.logicalSize.height() <= 0) {
		if (error) {
			*error = QStringLiteral("invalid-logical-size");
		}
		return true;
	}
	if (result.logicalSize.width() > maxWidth
		|| result.logicalSize.height() > maxHeight) {
		if (error) {
			*error = QStringLiteral("logical-size-cap-exceeded");
		}
		return true;
	}
	if (PhysicalImageRejected(result.image.size())) {
		if (error) {
			*error = QStringLiteral("physical-image-cap-exceeded");
		}
		return true;
	}
	if (result.image.devicePixelRatio() != key.devicePixelRatio) {
		if (error) {
			*error = QStringLiteral("unexpected-device-pixel-ratio");
		}
		return true;
	}
	return false;
}

} // namespace Iv::Markdown
