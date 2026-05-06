#pragma once

#include "iv/markdown/iv_markdown_parse.h"
#include "iv/markdown/iv_markdown_prepare.h"
#include "iv/iv_prepare.h"

#include <QtCore/QByteArray>

#include <memory>
#include <vector>

namespace Iv::Markdown {

struct PrepareContext {
	int listDepth = 0;
	int quoteDepth = 0;
};

struct FootnoteDefinitionEntry {
	const MarkdownNode *node = nullptr;
};

struct PrepareState {
	const PrepareRequest *request = nullptr;
	MarkdownArticleContent result;
	QByteArray sourceUtf8;
	std::vector<FootnoteDefinitionEntry> footnoteDefinitions;
	std::vector<std::pair<QString, QString>> firstFootnoteReferences;
	int nextGeneratedId = 0;

	void rememberFormula(
		int index,
		MathKind kind,
		QString formulaTex,
		int textSize,
		int renderWidthCap,
		int renderHeightCap);
	void rememberFormula(const PreparedBlock &block);
	void addPrepareWarning();
	void addFormulaWarning();
	void addPrepareWarnings(int count);
	void addFormulaWarnings(int count);
	void setTerminalFailure(
		PrepareTerminalFailure terminal,
		QString debugReason);
	[[nodiscard]] QString formulaSourceText(int index) const;
	[[nodiscard]] QString firstFootnoteReferenceAnchor(
		const QString &label) const;
	[[nodiscard]] QString rememberFootnoteReferenceAnchor(
		const QString &label,
		QString *blockAnchorId);
};

struct NativeIvPhotoInfo {
	uint64 id = 0;
	int width = 0;
	int height = 0;
};

struct NativeIvPrepareState {
	MarkdownArticleContent result;
	std::vector<NativeIvPhotoInfo> photos;
	int nextGeneratedId = 0;

	void setFailure(
		PrepareTerminalFailure terminal,
		QString debugReason);
	[[nodiscard]] bool blocked() const;
};

[[nodiscard]] QString InvalidStyleReason(
	const MarkdownPrepareDimensions &dimensions);
void ClearPreparedOutput(MarkdownArticleContent *result);
[[nodiscard]] QString FootnoteDefinitionAnchor(const MarkdownNode &node);

} // namespace Iv::Markdown
