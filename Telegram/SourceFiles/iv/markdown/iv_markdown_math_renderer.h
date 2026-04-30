#pragma once

#include "iv/markdown/iv_markdown_microtex.h"

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtGui/qrgb.h>

#include <map>
#include <tuple>

namespace Iv::Markdown {

struct FormulaCacheKey {
	QString trimmedTex;
	MathKind kind = MathKind::Display;
	int textSize = 0;
	int renderWidthCap = 0;
	QRgb foregroundRgba = 0;
	int paletteVersion = 0;
	int devicePixelRatio = 1;

	friend inline bool operator==(const FormulaCacheKey &a, const FormulaCacheKey &b) {
		return std::tie(
			a.trimmedTex,
			a.kind,
			a.textSize,
			a.renderWidthCap,
			a.foregroundRgba,
			a.paletteVersion,
			a.devicePixelRatio) == std::tie(
			b.trimmedTex,
			b.kind,
			b.textSize,
			b.renderWidthCap,
			b.foregroundRgba,
			b.paletteVersion,
			b.devicePixelRatio);
	}

	friend inline bool operator<(const FormulaCacheKey &a, const FormulaCacheKey &b) {
		return std::tie(
			a.trimmedTex,
			a.kind,
			a.textSize,
			a.renderWidthCap,
			a.foregroundRgba,
			a.paletteVersion,
			a.devicePixelRatio) < std::tie(
			b.trimmedTex,
			b.kind,
			b.textSize,
			b.renderWidthCap,
			b.foregroundRgba,
			b.paletteVersion,
			b.devicePixelRatio);
	}
};

struct RenderedFormula {
	QImage image;
	QSize logicalSize;
	QString fallbackText;
	QString error;
	bool success = false;
	bool overflow = false;
	bool tooLarge = false;
};

struct FormulaDebugCounters {
	int rendered = 0;
	int failed = 0;
	int hits = 0;
	int misses = 0;
};

class FormulaCache {
public:
	[[nodiscard]] const RenderedFormula *find(
		const FormulaCacheKey &key) const;
	void put(FormulaCacheKey key, RenderedFormula value);
	void clear();

private:
	std::map<FormulaCacheKey, RenderedFormula> _entries;

};

class MathRenderer {
public:
	[[nodiscard]] RenderedFormula renderFormula(
		const MicrotexRenderRequest &request,
		int paletteVersion);
	void clearCache(bool resetDebugCounters = false);
	void invalidate(bool resetDebugCounters = false);
	void resetDebugCounters();

	[[nodiscard]] const FormulaDebugCounters &debugCounters() const;

private:
	[[nodiscard]] FormulaCacheKey makeKey(
		const MicrotexRenderRequest &request,
		int paletteVersion) const;
	[[nodiscard]] RenderedFormula makeFailure(
		const FormulaCacheKey &key,
		const QString &error,
		bool tooLarge) const;
	[[nodiscard]] bool rejectRequestByCaps(
		const FormulaCacheKey &key,
		QString *error) const;
	[[nodiscard]] bool rejectResultByCaps(
		const FormulaCacheKey &key,
		const MicrotexRenderResult &result,
		QString *error) const;

	FormulaCache _cache;
	FormulaDebugCounters _debugCounters;

};

} // namespace Iv::Markdown
