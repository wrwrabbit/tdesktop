#include "iv/markdown/iv_markdown_math_renderer.h"

#include <QtGui/QColor>

#include <algorithm>
#include <limits>
#include <utility>

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
		.renderHeightCap = request.renderHeightCap,
		.foregroundRgba = foreground.rgba(),
		.paletteVersion = paletteVersion,
		.devicePixelRatio = request.devicePixelRatio,
	};
}

[[nodiscard]] QString FallbackText(const FormulaCacheKey &key) {
	return key.trimmedTex.isEmpty()
		? u"[math]"_q
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
	return (error == u"render-width-cap-exceeded"_q)
		|| (error == u"physical-size-cap-exceeded"_q)
		|| (error == u"logical-size-cap-exceeded"_q)
		|| (error == u"physical-image-cap-exceeded"_q);
}

[[nodiscard]] int64 EstimateQStringBytes(const QString &value) {
	return int64(value.size()) * sizeof(QChar);
}

[[nodiscard]] int64 EstimateQImageBytes(const QImage &image) {
	return image.isNull() ? 0 : int64(image.sizeInBytes());
}

} // namespace

const RenderedFormula *FormulaCache::find(const FormulaCacheKey &key) {
	const auto i = _entries.find(key);
	if (i == _entries.end()) {
		return nullptr;
	}
	touch(i);
	return &i->second.value;
}

FormulaCacheMutation FormulaCache::put(
		FormulaCacheKey key,
		RenderedFormula value) {
	if (_budgetBytes <= 0) {
		if (const auto i = _entries.find(key); i != _entries.end()) {
			erase(i);
		}
		return {};
	}
	const auto sizeBytes = estimateBytes(key, value);
	if (sizeBytes > _budgetBytes) {
		if (const auto i = _entries.find(key); i != _entries.end()) {
			erase(i);
		}
		return {};
	}
	if (const auto i = _entries.find(key); i != _entries.end()) {
		erase(i);
	}
	_lru.push_back(key);
	const auto lru = std::prev(_lru.end());
	_entries.emplace(std::move(key), Entry{
		.value = std::move(value),
		.sizeBytes = sizeBytes,
		.lru = lru,
	});
	_sizeBytes += sizeBytes;
	return evictToBudget();
}

FormulaCacheMutation FormulaCache::setBudgetBytes(int64 bytes) {
	_budgetBytes = std::max<int64>(0, bytes);
	return evictToBudget();
}

int64 FormulaCache::budgetBytes() const {
	return _budgetBytes;
}

int64 FormulaCache::sizeBytes() const {
	return _sizeBytes;
}

int FormulaCache::size() const {
	return int(_entries.size());
}

void FormulaCache::clear() {
	_entries.clear();
	_lru.clear();
	_sizeBytes = 0;
}

int64 FormulaCache::estimateBytes(
		const FormulaCacheKey &key,
		const RenderedFormula &value) const {
	return sizeof(FormulaCacheKey)
		+ sizeof(Entry)
		+ EstimateQStringBytes(key.trimmedTex)
		+ EstimateQImageBytes(value.image)
		+ EstimateQStringBytes(value.fallbackText)
		+ EstimateQStringBytes(value.error);
}

void FormulaCache::touch(std::map<FormulaCacheKey, Entry>::iterator i) {
	_lru.erase(i->second.lru);
	_lru.push_back(i->first);
	i->second.lru = std::prev(_lru.end());
}

void FormulaCache::erase(std::map<FormulaCacheKey, Entry>::iterator i) {
	_sizeBytes -= i->second.sizeBytes;
	_lru.erase(i->second.lru);
	_entries.erase(i);
}

FormulaCacheMutation FormulaCache::evictToBudget() {
	auto result = FormulaCacheMutation();
	while (((_budgetBytes <= 0) || (_sizeBytes > _budgetBytes)) && !_lru.empty()) {
		const auto oldest = _lru.front();
		const auto i = _entries.find(oldest);
		if (i == _entries.end()) {
			_lru.pop_front();
			continue;
		}
		result.evictedBytes += i->second.sizeBytes;
		++result.evictedEntries;
		erase(i);
	}
	return result;
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
		applyCacheMutation(_cache.put(key, failure));
		return failure;
	}
	auto normalized = request;
	normalized.trimmedTex = key.trimmedTex;
	normalized.textSize = key.textSize;
	normalized.renderWidthCap = key.renderWidthCap;
	normalized.renderHeightCap = key.renderHeightCap;
	normalized.foreground = QColor::fromRgba(key.foregroundRgba);
	normalized.devicePixelRatio = key.devicePixelRatio;
	auto rendered = RenderWithMicrotex(normalized);
	if (!rendered.ok) {
		++_debugCounters.failed;
		auto failure = makeFailure(
			key,
			rendered.error,
			TooLargeFailure(rendered.error));
		applyCacheMutation(_cache.put(key, failure));
		return failure;
	}
	if (rejectResultByCaps(key, rendered, &error)) {
		++_debugCounters.failed;
		auto failure = makeFailure(key, error, TooLargeFailure(error));
		applyCacheMutation(_cache.put(key, failure));
		return failure;
	}
	auto result = RenderedFormula();
	result.image = std::move(rendered.image);
	result.logicalSize = rendered.logicalSize;
	result.success = true;
	++_debugCounters.rendered;
	applyCacheMutation(_cache.put(key, result));
	return result;
}

void MathRenderer::clearCache(bool resetDebugCounters) {
	_cache.clear();
	if (resetDebugCounters) {
		_debugCounters = FormulaDebugCounters();
	} else {
		syncCacheCounters();
	}
}

void MathRenderer::invalidate(bool resetDebugCounters) {
	clearCache(resetDebugCounters);
}

void MathRenderer::resetDebugCounters() {
	_debugCounters = FormulaDebugCounters();
	syncCacheCounters();
}

void MathRenderer::setCacheBudgetBytes(int64 bytes) {
	applyCacheMutation(_cache.setBudgetBytes(bytes));
}

const FormulaDebugCounters &MathRenderer::debugCounters() const {
	return _debugCounters;
}

int64 MathRenderer::cacheBudgetBytes() const {
	return _cache.budgetBytes();
}

int64 MathRenderer::cacheUsageBytes() const {
	return _cache.sizeBytes();
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
	if (key.trimmedTex.isEmpty()) {
		if (error) {
			*error = u"empty-tex"_q;
		}
		return true;
	}
	if (key.textSize <= 0) {
		if (error) {
			*error = u"invalid-text-size"_q;
		}
		return true;
	}
	if (key.renderWidthCap <= 0) {
		if (error) {
			*error = u"invalid-render-width"_q;
		}
		return true;
	}
	if (key.renderHeightCap <= 0) {
		if (error) {
			*error = u"invalid-render-height"_q;
		}
		return true;
	}
	if (key.devicePixelRatio <= 0) {
		if (error) {
			*error = u"invalid-device-pixel-ratio"_q;
		}
		return true;
	}
	if (PhysicalImageRejected(
			int64(key.renderWidthCap) * key.devicePixelRatio,
			int64(key.renderHeightCap) * key.devicePixelRatio)) {
		if (error) {
			*error = u"physical-size-cap-exceeded"_q;
		}
		return true;
	}
	return false;
}

bool MathRenderer::rejectResultByCaps(
		const FormulaCacheKey &key,
		const MicrotexRenderResult &result,
		QString *error) const {
	if (result.logicalSize.width() <= 0 || result.logicalSize.height() <= 0) {
		if (error) {
			*error = u"invalid-logical-size"_q;
		}
		return true;
	}
	if (result.logicalSize.width() > key.renderWidthCap
		|| result.logicalSize.height() > key.renderHeightCap) {
		if (error) {
			*error = u"logical-size-cap-exceeded"_q;
		}
		return true;
	}
	if (PhysicalImageRejected(result.image.size())) {
		if (error) {
			*error = u"physical-image-cap-exceeded"_q;
		}
		return true;
	}
	if (result.image.devicePixelRatio() != key.devicePixelRatio) {
		if (error) {
			*error = u"unexpected-device-pixel-ratio"_q;
		}
		return true;
	}
	return false;
}

void MathRenderer::syncCacheCounters() {
	_debugCounters.cacheEntries = _cache.size();
	_debugCounters.cacheBytes = _cache.sizeBytes();
}

void MathRenderer::applyCacheMutation(FormulaCacheMutation mutation) {
	_debugCounters.evictedEntries += mutation.evictedEntries;
	_debugCounters.evictedBytes += mutation.evictedBytes;
	syncCacheCounters();
}

} // namespace Iv::Markdown
