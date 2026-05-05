#include "iv/markdown/iv_markdown_article.h"
#include "iv/markdown/iv_markdown_document.h"
#include "iv/markdown/iv_markdown_math_renderer.h"
#include "iv/markdown/iv_markdown_microtex.h"
#include "iv/markdown/iv_markdown_parse.h"
#include "iv/markdown/iv_markdown_prepare.h"
#include "iv/iv_prepare.h"
#include "scheme.h"

#include "ui/dynamic_image.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_scale.h"

#include "styles/style_iv.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QGuiApplication>
#include <QtGui/QColor>
#include <QtGui/QImage>

#include <rpl/never.h>

#include <algorithm>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace Iv::Markdown;

struct Args {
	QString markdownPath;
	QString latexMarkdownPath;
	bool dump = false;
	bool inlineHtml = false;
	bool ok = true;
	QString error;
};

struct PreparedFixture {
	QString label;
	QString path;
	PreparedDocument parsed;
	MarkdownArticleContent prepared;
};

class TestDynamicImage final : public Ui::DynamicImage {
public:
	explicit TestDynamicImage(QImage frame = QImage())
	: _frame(std::move(frame)) {
	}

	[[nodiscard]] std::shared_ptr<DynamicImage> clone() override {
		return std::make_shared<TestDynamicImage>(_frame);
	}

	[[nodiscard]] QImage image(int size) override {
		requestedSizes.push_back(size);
		return _frame;
	}

	void subscribeToUpdates(Fn<void()> callback) override {
		++subscriptionCount;
		_callback = std::move(callback);
	}

	void setFrame(QImage frame) {
		_frame = std::move(frame);
	}

	void notify() const {
		if (_callback) {
			_callback();
		}
	}

	mutable std::vector<int> requestedSizes;
	mutable int subscriptionCount = 0;

private:
	QImage _frame;
	mutable Fn<void()> _callback;
};

class TestPhotoRuntime final : public PhotoRuntime {
public:
	[[nodiscard]] std::shared_ptr<Ui::DynamicImage> thumbnail(
			QSize size) const override {
		thumbnailSizes.push_back(size);
		return thumbnailImage;
	}

	[[nodiscard]] std::shared_ptr<Ui::DynamicImage> full(
			QSize size) const override {
		fullSizes.push_back(size);
		return fullImage;
	}

	[[nodiscard]] bool loaded() const override {
		return loadedValue;
	}

	[[nodiscard]] bool loading() const override {
		return loadingValue;
	}

	[[nodiscard]] double progress() const override {
		return progressValue;
	}

	void open(Qt::MouseButton button) const override {
		openedButtons.push_back(button);
	}

	std::shared_ptr<TestDynamicImage> thumbnailImage;
	std::shared_ptr<TestDynamicImage> fullImage;
	bool loadedValue = false;
	bool loadingValue = false;
	double progressValue = 0.;
	mutable std::vector<QSize> thumbnailSizes;
	mutable std::vector<QSize> fullSizes;
	mutable std::vector<Qt::MouseButton> openedButtons;
};

class TestMediaRuntime final : public MediaRuntime {
public:
	[[nodiscard]] std::shared_ptr<Ui::DynamicImage> resolveInlineImage(
			uint64 documentId,
			QSize size) const override {
		inlineRequests.push_back({ documentId, size });
		return (documentId == inlineDocumentId) ? inlineImage : nullptr;
	}

	[[nodiscard]] std::shared_ptr<PhotoRuntime> resolvePhoto(
			uint64 photoId) const override {
		photoRequests.push_back(photoId);
		return (photoId == preparedPhotoId) ? photoRuntime : nullptr;
	}

	uint64 inlineDocumentId = 0;
	uint64 preparedPhotoId = 0;
	std::shared_ptr<TestDynamicImage> inlineImage;
	std::shared_ptr<TestPhotoRuntime> photoRuntime;
	mutable std::vector<std::pair<uint64, QSize>> inlineRequests;
	mutable std::vector<uint64> photoRequests;
};

enum class NativeIvPlaceholderKind {
	Video,
	Embed,
	EmbedPost,
	Collage,
	Slideshow,
	Audio,
	Map,
};

struct NativeIvPlaceholderFixture {
	NativeIvPlaceholderKind kind = NativeIvPlaceholderKind::Video;
	QString caption;
	QString expectedLabel;
	MTPPageBlock block;
};

[[nodiscard]] QImage SolidTestImage(
		int width,
		int height,
		const QColor &color) {
	auto result = QImage(
		QSize(std::max(width, 1), std::max(height, 1)),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(color);
	return result;
}

[[nodiscard]] MTPRichText NativeIvText(QString text) {
	return text.isEmpty()
		? MTP_textEmpty()
		: MTP_textPlain(MTP_string(text));
}

[[nodiscard]] MTPRichText NativeIvConcat(
		std::initializer_list<MTPRichText> parts) {
	auto list = QVector<MTPRichText>();
	list.reserve(int(parts.size()));
	for (const auto &part : parts) {
		list.push_back(part);
	}
	return MTP_textConcat(MTP_vector<MTPRichText>(std::move(list)));
}

[[nodiscard]] MTPPageCaption NativeIvCaption(
		QString text = QString(),
		QString credit = QString()) {
	return MTP_pageCaption(
		NativeIvText(std::move(text)),
		NativeIvText(std::move(credit)));
}

[[nodiscard]] MTPPhoto NativeIvPhoto(uint64 id, int width, int height) {
	auto sizes = QVector<MTPPhotoSize>();
	sizes.push_back(MTP_photoSize(
		MTP_string("y"),
		MTP_int(width),
		MTP_int(height),
		MTP_int(0)));
	return MTP_photo(
		MTP_flags(0),
		MTP_long(id),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(0),
		MTP_vector<MTPPhotoSize>(std::move(sizes)),
		MTPVector<MTPVideoSize>(),
		MTP_int(0));
}

[[nodiscard]] MTPPageBlock NativeIvPhotoBlock(
		uint64 photoId,
		QString caption = QString(),
		QString credit = QString(),
		QString urlOverride = QString()) {
	const auto flags = urlOverride.isEmpty()
		? MTP_flags(0)
		: MTP_flags(MTPDpageBlockPhoto::Flag::f_url);
	return MTP_pageBlockPhoto(
		flags,
		MTP_long(photoId),
		NativeIvCaption(std::move(caption), std::move(credit)),
		MTP_string(urlOverride),
		MTP_long(0));
}

[[nodiscard]] MTPPageBlock NativeIvCoveredPhotoBlock(
		uint64 photoId,
		QString caption = QString(),
		QString credit = QString(),
		QString urlOverride = QString()) {
	return MTP_pageBlockCover(NativeIvPhotoBlock(
		photoId,
		std::move(caption),
		std::move(credit),
		std::move(urlOverride)));
}

[[nodiscard]] QString NativeIvPlaceholderLabel(NativeIvPlaceholderKind kind) {
	switch (kind) {
	case NativeIvPlaceholderKind::Video:
		return u"Video Placeholder"_q;
	case NativeIvPlaceholderKind::Embed:
	case NativeIvPlaceholderKind::EmbedPost:
		return u"Embed Placeholder"_q;
	case NativeIvPlaceholderKind::Collage:
		return u"Collage placeholder"_q;
	case NativeIvPlaceholderKind::Slideshow:
		return u"Grouped Media Placeholder"_q;
	case NativeIvPlaceholderKind::Audio:
		return u"Audio File Placeholder"_q;
	case NativeIvPlaceholderKind::Map:
		return u"Map Placeholder"_q;
	}
	return QString();
}

[[nodiscard]] MTPPageBlock NativeIvPlaceholderBlock(
		NativeIvPlaceholderKind kind,
		QString caption = QString()) {
	const auto pageCaption = NativeIvCaption(std::move(caption));
	switch (kind) {
	case NativeIvPlaceholderKind::Video:
		return MTP_pageBlockVideo(
			MTP_flags(0),
			MTP_long(7001),
			pageCaption);
	case NativeIvPlaceholderKind::Embed:
		return MTP_pageBlockEmbed(
			MTP_flags(0),
			MTP_string(),
			MTP_string(),
			MTP_long(0),
			MTP_int(0),
			MTP_int(0),
			pageCaption);
	case NativeIvPlaceholderKind::EmbedPost:
		return MTP_pageBlockEmbedPost(
			MTP_string("https://example.com/embed-post"),
			MTP_long(0),
			MTP_long(0),
			MTP_string("Author"),
			MTP_int(0),
			MTP_vector<MTPPageBlock>(),
			pageCaption);
	case NativeIvPlaceholderKind::Collage:
		return MTP_pageBlockCollage(
			MTP_vector<MTPPageBlock>(),
			pageCaption);
	case NativeIvPlaceholderKind::Slideshow:
		return MTP_pageBlockSlideshow(
			MTP_vector<MTPPageBlock>(),
			pageCaption);
	case NativeIvPlaceholderKind::Audio:
		return MTP_pageBlockAudio(MTP_long(7002), pageCaption);
	case NativeIvPlaceholderKind::Map:
		return MTP_pageBlockMap(
			MTP_geoPointEmpty(),
			MTP_int(10),
			MTP_int(120),
			MTP_int(72),
			pageCaption);
	}
	return MTP_pageBlockUnsupported();
}

[[nodiscard]] MTPRichText NativeIvInlineImageText(
		uint64 documentId,
		int width,
		int height) {
	return MTP_textImage(
		MTP_long(documentId),
		MTP_int(width),
		MTP_int(height));
}

[[nodiscard]] MTPPageBlock NativeIvInlineImageParagraph(
		uint64 documentId,
		int width,
		int height,
		QString prefix,
		QString suffix) {
	return MTP_pageBlockParagraph(NativeIvConcat({
		NativeIvText(std::move(prefix)),
		NativeIvInlineImageText(documentId, width, height),
		NativeIvText(std::move(suffix)),
	}));
}

[[nodiscard]] Iv::Source NativeIvSource(
		QVector<MTPPageBlock> blocks,
		QVector<MTPPhoto> photos = {},
		QVector<MTPDocument> documents = {}) {
	auto result = Iv::Source();
	result.pageId = 1;
	result.page = MTP_page(
		MTP_flags(0),
		MTP_string("https://example.com/native-iv"),
		MTP_vector<MTPPageBlock>(std::move(blocks)),
		MTP_vector<MTPPhoto>(std::move(photos)),
		MTP_vector<MTPDocument>(std::move(documents)),
		MTP_int(0));
	result.name = u"native-iv-test"_q;
	return result;
}

[[nodiscard]] QString FromLatin1(const char *value) {
	return QString::fromLatin1(value);
}

void PrintStreamLine(std::ostream &stream, const QString &line) {
	const auto bytes = line.toUtf8();
	stream.write(bytes.constData(), static_cast<std::streamsize>(bytes.size()));
	stream << '\n';
}

void PrintLine(const QString &line) {
	PrintStreamLine(std::cout, line);
}

void PrintError(const QString &line) {
	PrintStreamLine(std::cerr, line);
}

[[nodiscard]] Args ParseArgs(int argc, char **argv) {
	auto result = Args();
	for (auto i = 1; i != argc; ++i) {
		const auto argument = QString::fromLocal8Bit(argv[i]);
		if (argument == FromLatin1("--dump")) {
			result.dump = true;
		} else if (argument == FromLatin1("--inline-html")) {
			result.inlineHtml = true;
		} else if (argument == FromLatin1("--markdown")
			|| argument == FromLatin1("--latex-md")) {
			if (i + 1 == argc) {
				result.ok = false;
				result.error = FromLatin1("missing value for ") + argument;
				return result;
			}
			const auto path = QString::fromLocal8Bit(argv[++i]);
			if (argument == FromLatin1("--markdown")) {
				result.markdownPath = path;
			} else {
				result.latexMarkdownPath = path;
			}
		} else {
			result.ok = false;
			result.error = FromLatin1("unknown argument: ") + argument;
			return result;
		}
	}
	return result;
}

[[nodiscard]] QString DefaultFixturePath(const QString &name) {
	const auto applicationDir = QDir(QCoreApplication::applicationDirPath());
	const auto applicationCandidate = applicationDir.filePath(name);
	if (QFileInfo::exists(applicationCandidate)) {
		return applicationCandidate;
	}
	const auto outDebug = QDir::current().filePath(
		FromLatin1("out/Debug/") + name);
	if (QFileInfo::exists(outDebug)) {
		return outDebug;
	}
	const auto repoFixtureFromApplication = QDir::cleanPath(
		applicationDir.filePath(
			FromLatin1(
				"../../Telegram/SourceFiles/tests/fixtures/markdown_iv/")
				+ name));
	if (QFileInfo::exists(repoFixtureFromApplication)) {
		return repoFixtureFromApplication;
	}
	const auto repoFixtureFromCurrent = QDir::current().filePath(
		FromLatin1("Telegram/SourceFiles/tests/fixtures/markdown_iv/")
			+ name);
	if (QFileInfo::exists(repoFixtureFromCurrent)) {
		return repoFixtureFromCurrent;
	}
	return outDebug;
}

[[nodiscard]] bool ReadFile(const QString &path, QByteArray *bytes) {
	if (!bytes) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	*bytes = file.readAll();
	return true;
}

[[nodiscard]] int CountNodes(const MarkdownNode &node) {
	auto result = 1;
	for (const auto &child : node.children) {
		result += CountNodes(child);
	}
	return result;
}

[[nodiscard]] bool HasKind(const MarkdownNode &node, NodeKind kind) {
	if (node.kind == kind) {
		return true;
	}
	for (const auto &child : node.children) {
		if (HasKind(child, kind)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasTextContaining(
		const MarkdownNode &node,
		const QString &text) {
	if (node.text.contains(text) || node.raw.contains(text)) {
		return true;
	}
	for (const auto &child : node.children) {
		if (HasTextContaining(child, text)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasExactInlineHtmlTriplet(
		const MarkdownNode &node,
		const QString &openingTag,
		const QString &innerText,
		const QString &closingTag) {
	const auto count = int(node.children.size());
	for (auto i = 0; (i + 2) < count; ++i) {
		const auto &opening = node.children[i];
		const auto &text = node.children[i + 1];
		const auto &closing = node.children[i + 2];
		if (opening.kind == NodeKind::HtmlInline
			&& opening.raw == openingTag
			&& text.kind == NodeKind::Text
			&& text.text == innerText
			&& closing.kind == NodeKind::HtmlInline
			&& closing.raw == closingTag) {
			return true;
		}
	}
	for (const auto &child : node.children) {
		if (HasExactInlineHtmlTriplet(
				child,
				openingTag,
				innerText,
				closingTag)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasTaskState(const MarkdownNode &node, TaskState state) {
	if (node.taskState == state) {
		return true;
	}
	for (const auto &child : node.children) {
		if (HasTaskState(child, state)) {
			return true;
		}
	}
	return false;
}

void CollectTables(
		const MarkdownNode &node,
		std::vector<const MarkdownNode*> *out) {
	if (!out) {
		return;
	}
	if (node.kind == NodeKind::Table) {
		out->push_back(&node);
	}
	for (const auto &child : node.children) {
		CollectTables(child, out);
	}
}

[[nodiscard]] int TableHeaderRowCount(const MarkdownNode &table) {
	auto result = 0;
	for (const auto &row : table.children) {
		if (row.kind == NodeKind::TableRow && row.tableHeader) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] bool HasTableHeaderRow(const MarkdownNode &table) {
	return TableHeaderRowCount(table) > 0;
}

[[nodiscard]] bool HasSequentialTableColumns(const MarkdownNode &table) {
	for (const auto &row : table.children) {
		if (row.kind != NodeKind::TableRow) {
			return false;
		}
		auto expectedColumn = 0;
		for (const auto &cell : row.children) {
			if (cell.kind != NodeKind::TableCell
				|| cell.tableColumn != expectedColumn) {
				return false;
			}
			++expectedColumn;
		}
	}
	return !table.children.empty();
}

[[nodiscard]] bool HasTableAlignments(
		const MarkdownNode &table,
		const std::vector<TableAlignment> &expected) {
	if (table.tableAlignments.size() != expected.size()) {
		return false;
	}
	for (auto i = 0, count = int(expected.size()); i != count; ++i) {
		if (table.tableAlignments[i] != expected[i]) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] int TableColumnCount(const MarkdownNode &table) {
	auto result = 0;
	for (const auto &row : table.children) {
		if (row.kind != NodeKind::TableRow) {
			return 0;
		}
		const auto count = int(row.children.size());
		if (count > result) {
			result = count;
		}
	}
	return result;
}

[[nodiscard]] int CountDisplayMathNodes(const MarkdownNode &node) {
	auto result = (node.kind == NodeKind::DisplayMath) ? 1 : 0;
	for (const auto &child : node.children) {
		result += CountDisplayMathNodes(child);
	}
	return result;
}

[[nodiscard]] bool SameRange(
		const SourceRange &range,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	return range.available
		&& (range.startLine == startLine)
		&& (range.startColumn == startColumn)
		&& (range.endLine == endLine)
		&& (range.endColumn == endColumn);
}

[[nodiscard]] bool CoversLineRange(
		const SourceRange &range,
		int firstLine,
		int lastLine) {
	return range.available
		&& (range.startLine <= firstLine)
		&& (range.endLine >= lastLine);
}

[[nodiscard]] const MarkdownNode *FindNodeByKindAndRange(
		const MarkdownNode &node,
		NodeKind kind,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	if (node.kind == kind
		&& SameRange(
			node.range,
			startLine,
			startColumn,
			endLine,
			endColumn)) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindNodeByKindAndRange(
				child,
				kind,
				startLine,
				startColumn,
				endLine,
				endColumn)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] int CountNodesByKindAndRange(
		const MarkdownNode &node,
		NodeKind kind,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	auto result = (node.kind == kind
		&& SameRange(
			node.range,
			startLine,
			startColumn,
			endLine,
			endColumn))
		? 1
		: 0;
	for (const auto &child : node.children) {
		result += CountNodesByKindAndRange(
			child,
			kind,
			startLine,
			startColumn,
			endLine,
			endColumn);
	}
	return result;
}

using NodeKindPath = std::initializer_list<NodeKind>;
using NodeKindPathIter = NodeKindPath::const_iterator;

[[nodiscard]] const MarkdownNode *FindNodeByPathAndRange(
		const MarkdownNode &node,
		NodeKindPathIter begin,
		NodeKindPathIter end,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	if (begin == end || node.kind != *begin) {
		return nullptr;
	}
	if (std::next(begin) == end) {
		return SameRange(
			node.range,
			startLine,
			startColumn,
			endLine,
			endColumn)
			? &node
			: nullptr;
	}
	const auto next = std::next(begin);
	for (const auto &child : node.children) {
		if (const auto found = FindNodeByPathAndRange(
				child,
				next,
				end,
				startLine,
				startColumn,
				endLine,
				endColumn)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] const MarkdownNode *FindNodeByPathAndRange(
		const MarkdownNode &node,
		NodeKindPath path,
		int startLine,
		int startColumn,
		int endLine,
		int endColumn) {
	return path.size()
		? FindNodeByPathAndRange(
			node,
			path.begin(),
			path.end(),
			startLine,
			startColumn,
			endLine,
			endColumn)
		: nullptr;
}

[[nodiscard]] const MarkdownNode *FindNodeByKindAndLineRange(
		const MarkdownNode &node,
		NodeKind kind,
		int firstLine,
		int lastLine) {
	if (node.kind == kind && CoversLineRange(node.range, firstLine, lastLine)) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindNodeByKindAndLineRange(
				child,
				kind,
				firstLine,
				lastLine)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] const MarkdownNode *FindHtmlInlineByRaw(
		const MarkdownNode &node,
		const QString &raw) {
	if (node.kind == NodeKind::HtmlInline && node.raw == raw) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindHtmlInlineByRaw(child, raw)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] const MarkdownNode *FindHtmlBlockContaining(
		const MarkdownNode &node,
		const QString &text) {
	if (node.kind == NodeKind::HtmlBlock && node.raw.contains(text)) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindHtmlBlockContaining(child, text)) {
			return found;
		}
	}
	return nullptr;
}

void CollectNodesByKind(
		const MarkdownNode &node,
		NodeKind kind,
		std::vector<const MarkdownNode*> *out) {
	if (!out) {
		return;
	}
	if (node.kind == kind) {
		out->push_back(&node);
	}
	for (const auto &child : node.children) {
		CollectNodesByKind(child, kind, out);
	}
}

[[nodiscard]] const MarkdownNode *FindLinkByTarget(
		const MarkdownNode &node,
		const QString &target) {
	if (node.kind == NodeKind::Link && node.url == target) {
		return &node;
	}
	for (const auto &child : node.children) {
		if (const auto found = FindLinkByTarget(child, target)) {
			return found;
		}
	}
	return nullptr;
}

[[nodiscard]] bool WarningContains(
		const PreparedDocument &document,
		const QString &snippet) {
	for (const auto &warning : document.warnings) {
		if (warning.contains(snippet, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] int CountFormulas(
		const PreparedDocument &document,
		MathKind kind) {
	auto result = 0;
	for (const auto &formula : document.formulas) {
		if (formula.kind == kind) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] bool HasFormulaOnLine(
		const PreparedDocument &document,
		int line,
		const QString &tex) {
	for (const auto &formula : document.formulas) {
		if (formula.range.available
			&& formula.range.startLine == line
			&& formula.tex == tex) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasFormulaInLineRange(
		const PreparedDocument &document,
		int firstLine,
		int lastLine) {
	if (lastLine < firstLine) {
		return false;
	}
	for (const auto &formula : document.formulas) {
		if (!formula.range.available) {
			continue;
		}
		if (formula.range.startLine <= lastLine
			&& formula.range.endLine >= firstLine) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] int CountFormulasInLineRange(
		const PreparedDocument &document,
		MathKind kind,
		int firstLine,
		int lastLine) {
	auto result = 0;
	for (const auto &formula : document.formulas) {
		if (!formula.range.available || formula.kind != kind) {
			continue;
		}
		if (formula.range.startLine <= lastLine
			&& formula.range.endLine >= firstLine) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] QString YesNo(bool value) {
	return FromLatin1(value ? "yes" : "no");
}

[[nodiscard]] bool HasBothTaskStates(const PreparedDocument &document) {
	return HasTaskState(document.document, TaskState::Checked)
		&& HasTaskState(document.document, TaskState::Unchecked);
}

[[nodiscard]] bool ExclusionsPass(const PreparedDocument &document) {
	return !HasFormulaInLineRange(document, 281, 281)
		&& HasFormulaOnLine(document, 285, FromLatin1("5x + 3"))
		&& !HasFormulaInLineRange(document, 332, 340);
}

[[nodiscard]] bool HasFormula(
		const PreparedDocument &document,
		MathKind kind,
		const QString &tex) {
	for (const auto &formula : document.formulas) {
		if (formula.kind == kind && formula.tex == tex) {
			return true;
		}
	}
	return false;
}

void Check(bool condition, const QString &message, bool *ok);

[[nodiscard]] MarkdownSourceValidationResult CheckValidationSuccess(
		const QByteArray &source,
		const QString &label,
		bool *ok) {
	auto validated = ValidateMarkdownSourceForIv(source, ParseOptions{ label });
	Check(
		validated.ok,
		label + FromLatin1(" validation failed: ") + validated.error,
		ok);
	return validated;
}

void CheckValidationFailure(
		const QByteArray &source,
		const QString &label,
		const QString &expectedError,
		bool *ok) {
	const auto validated = ValidateMarkdownSourceForIv(
		source,
		ParseOptions{ label });
	Check(
		!validated.ok,
		label + FromLatin1(" validation should fail"),
		ok);
	if (!validated.ok) {
		Check(
			validated.error == expectedError,
			label + FromLatin1(" validation error should be ")
				+ expectedError
				+ FromLatin1(", got ")
				+ validated.error,
			ok);
	}
}

void CheckMatchingParseCounts(
		const PreparedDocument &legacy,
		const PreparedDocument &validated,
		const QString &label,
		bool *ok) {
	Check(
		legacy.stats.cmarkNodeCount == validated.stats.cmarkNodeCount,
		label + FromLatin1(" validated path cmark node count"),
		ok);
	Check(
		CountNodes(legacy.document) == CountNodes(validated.document),
		label + FromLatin1(" validated path converted node count"),
		ok);
	Check(
		legacy.formulas.size() == validated.formulas.size(),
		label + FromLatin1(" validated path formula count"),
		ok);
	Check(
		CountFormulas(legacy, MathKind::Inline)
			== CountFormulas(validated, MathKind::Inline),
		label + FromLatin1(" validated path inline formula count"),
		ok);
	Check(
		CountFormulas(legacy, MathKind::Display)
			== CountFormulas(validated, MathKind::Display),
		label + FromLatin1(" validated path display formula count"),
		ok);
	Check(
		CountDisplayMathNodes(legacy.document)
			== CountDisplayMathNodes(validated.document),
		label + FromLatin1(" validated path display math node count"),
		ok);
}

void AppendSummaryCounts(QString *line, const PreparedDocument &document) {
	line->append(FromLatin1(" nodes="));
	line->append(QString::number(document.stats.cmarkNodeCount));
	line->append(FromLatin1(" converted="));
	line->append(QString::number(CountNodes(document.document)));
	line->append(FromLatin1(" formulas_inline="));
	line->append(QString::number(CountFormulas(document, MathKind::Inline)));
	line->append(FromLatin1(" formulas_display="));
	line->append(QString::number(CountFormulas(document, MathKind::Display)));
}

void PrintSummary(const PreparedDocument &document, const QString &label) {
	auto line = label;
	AppendSummaryCounts(&line, document);
	line.append(FromLatin1(" tables="));
	line.append(YesNo(HasKind(document.document, NodeKind::Table)));
	if (label == FromLatin1("markdown-example.md")) {
		line.append(FromLatin1(" tasks="));
		line.append(YesNo(HasBothTaskStates(document)));
		line.append(FromLatin1(" strike="));
		line.append(YesNo(HasKind(document.document, NodeKind::Strike)));
	} else if (label == FromLatin1("latex-markdown-test.md")) {
		line.append(FromLatin1(" exclusions="));
		line.append(YesNo(ExclusionsPass(document)));
	}
	PrintLine(line);
}

[[nodiscard]] bool ParseFixture(
		const QString &path,
		const QString &label,
		PreparedDocument *document) {
	auto bytes = QByteArray();
	if (!ReadFile(path, &bytes)) {
		PrintError(label + FromLatin1(" read-failed: ") + path);
		return false;
	}
	auto parsed = ParseMarkdownForIv(bytes, ParseOptions{ label });
	if (!parsed.ok) {
		PrintError(label + FromLatin1(" parse-failed: ") + parsed.error);
		return false;
	}
	auto validated = ValidateMarkdownSourceForIv(
		bytes,
		ParseOptions{ label });
	if (!validated.ok) {
		PrintError(label + FromLatin1(" validate-failed: ") + validated.error);
		return false;
	}
	auto parsedValidated = ParseMarkdownForIv(std::move(validated.source));
	if (!parsedValidated.ok) {
		PrintError(
			label + FromLatin1(" validated-parse-failed: ")
				+ parsedValidated.error);
		return false;
	}
	auto countsOk = true;
	CheckMatchingParseCounts(
		parsed.document,
		parsedValidated.document,
		label,
		&countsOk);
	if (!countsOk) {
		return false;
	}
	PrintSummary(parsed.document, label);
	if (document) {
		*document = std::move(parsed.document);
	}
	return true;
}

[[nodiscard]] QString AbsolutePath(const QString &path) {
	return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

[[nodiscard]] QString PrepareFailureReason(
		const PrepareFailureStatus &failure) {
	return !failure.debugReason.isEmpty()
		? failure.debugReason
		: QString::number(int(failure.terminal));
}

[[nodiscard]] MarkdownArticleContent PrepareParsedDocumentForTest(
		std::shared_ptr<const PreparedDocument> document,
		const QString &sourcePath,
		const std::shared_ptr<MathRenderer> &renderer,
		MarkdownPrepareDimensions dimensions = CaptureMarkdownPrepareDimensions()) {
	return PrepareSynchronously({
		.document = std::move(document),
		.renderer = renderer,
		.dimensions = std::move(dimensions),
		.sourcePath = AbsolutePath(sourcePath),
	});
}

[[nodiscard]] MarkdownArticleContent PrepareParsedDocumentForTest(
		const PreparedDocument &document,
		const QString &sourcePath,
		const std::shared_ptr<MathRenderer> &renderer,
		MarkdownPrepareDimensions dimensions = CaptureMarkdownPrepareDimensions()) {
	return PrepareParsedDocumentForTest(
		std::make_shared<const PreparedDocument>(document),
		sourcePath,
		renderer,
		std::move(dimensions));
}

[[nodiscard]] int CountPreparedFormulaSlots(
		const MarkdownArticleContent &prepared) {
	auto result = 0;
	for (const auto &slot : prepared.formulas) {
		if (slot.present) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] int CountMeasuredFormulaSlots(
		const MarkdownArticleContent &prepared) {
	auto result = 0;
	for (const auto &slot : prepared.formulas) {
		if (slot.present && slot.measured.success) {
			++result;
		}
	}
	return result;
}

void PrintPrepareSummary(
		const QString &label,
		const MarkdownArticleContent &prepared) {
	auto line = label;
	line.append(FromLatin1(" prepare_ms="));
	line.append(QString::number(prepared.debug.prepareMs));
	line.append(FromLatin1(" formula_measure_ms="));
	line.append(QString::number(prepared.debug.formulaMeasureMs));
	line.append(FromLatin1(" formula_render_ms="));
	line.append(QString::number(prepared.debug.formulaRenderMs));
	line.append(FromLatin1(" prepare_warnings="));
	line.append(QString::number(prepared.debug.prepareWarningCount));
	line.append(FromLatin1(" formula_warnings="));
	line.append(QString::number(prepared.debug.formulaWarningCount));
	line.append(FromLatin1(" prepared_formulas="));
	line.append(QString::number(CountPreparedFormulaSlots(prepared)));
	PrintLine(line);
}

[[nodiscard]] bool PrepareFixture(
		const QString &path,
		const QString &label,
		const std::shared_ptr<MathRenderer> &renderer,
		PreparedFixture *fixture) {
	auto parsed = PreparedDocument();
	if (!ParseFixture(path, label, &parsed)) {
		return false;
	}
	auto prepared = PrepareParsedDocumentForTest(parsed, path, renderer);
	if (prepared.failure.failed()) {
		PrintError(
			label + FromLatin1(" prepare-failed: ")
				+ PrepareFailureReason(prepared.failure));
		return false;
	}
	PrintPrepareSummary(label, prepared);
	if (fixture) {
		fixture->label = label;
		fixture->path = AbsolutePath(path);
		fixture->parsed = std::move(parsed);
		fixture->prepared = std::move(prepared);
	}
	return true;
}

[[nodiscard]] std::unique_ptr<MarkdownArticle> BuildArticleForTest(
		MarkdownArticleContent content,
		const std::shared_ptr<MathRenderer> &renderer,
		int width,
		int *height = nullptr) {
	auto article = std::make_unique<MarkdownArticle>(renderer);
	article->setContent(std::move(content));
	if (height) {
		*height = article->resizeGetHeight(width);
	} else {
		article->resizeGetHeight(width);
	}
	return article;
}

[[nodiscard]] QImage PaintArticleForTest(
		MarkdownArticle *article,
		int width,
		int height,
		int devicePixelRatio = 1) {
	const auto previousDevicePixelRatio = style::DevicePixelRatio();
	style::SetDevicePixelRatio(devicePixelRatio);

	auto image = QImage(
		QSize(width * devicePixelRatio, height * devicePixelRatio),
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(devicePixelRatio);
	image.fill(Qt::transparent);
	{
		auto painter = Painter(&image);
		article->paint(
			painter,
			QRect(0, 0, width, height),
			MarkdownArticlePaintCaches());
	}

	style::SetDevicePixelRatio(previousDevicePixelRatio);
	return image;
}

[[nodiscard]] bool HasPaintedPixels(const QImage &image) {
	const auto bits = image.constBits();
	if (!bits) {
		return false;
	}
	const auto count = image.sizeInBytes();
	for (auto i = qsizetype(0); i != count; ++i) {
		if (bits[i] != 0) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool PaintTouchesBottomOrRightImageEdge(const QImage &image) {
	if (image.isNull()) {
		return false;
	}
	const auto width = image.width();
	const auto height = image.height();
	const auto painted = [&](int x, int y) {
		return (image.pixel(x, y) & 0xFF000000U) != 0;
	};
	for (auto x = 0; x != width; ++x) {
		if (painted(x, height - 1)) {
			return true;
		}
	}
	for (auto y = 0; y != height; ++y) {
		if (painted(width - 1, y)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] std::optional<QRect> PaintedBoundsInRect(
		const QImage &image,
		QRect rect) {
	rect = rect.intersected(QRect(QPoint(), image.size()));
	if (rect.isEmpty()) {
		return std::nullopt;
	}
	auto left = rect.right();
	auto top = rect.bottom();
	auto right = rect.left() - 1;
	auto bottom = rect.top() - 1;
	for (auto y = rect.top(); y <= rect.bottom(); ++y) {
		for (auto x = rect.left(); x <= rect.right(); ++x) {
			if (!(image.pixel(x, y) & 0xFF000000U)) {
				continue;
			}
			left = std::min(left, x);
			top = std::min(top, y);
			right = std::max(right, x);
			bottom = std::max(bottom, y);
		}
	}
	return (right >= left) && (bottom >= top)
		? std::make_optional(QRect(QPoint(left, top), QPoint(right, bottom)))
		: std::nullopt;
}

[[nodiscard]] std::optional<QRect> SymbolHitBounds(
		MarkdownArticle *article,
		int width,
		int height,
		int offset) {
	if (!article || (width <= 0) || (height <= 0) || (offset < 0)) {
		return std::nullopt;
	}
	auto flags = Ui::Text::StateRequest::Flags();
	flags |= Ui::Text::StateRequest::Flag::LookupSymbol;
	auto left = width;
	auto top = height;
	auto right = -1;
	auto bottom = -1;
	for (auto y = 0; y != height; ++y) {
		for (auto x = 0; x != width; ++x) {
			const auto hit = article->hitTest(QPoint(x, y), flags);
			if (!hit.valid()
				|| !hit.state.uponSymbol
				|| (int(hit.state.symbol) != offset)) {
				continue;
			}
			left = std::min(left, x);
			top = std::min(top, y);
			right = std::max(right, x);
			bottom = std::max(bottom, y);
		}
	}
	return (right >= left) && (bottom >= top)
		? std::make_optional(QRect(QPoint(left, top), QPoint(right, bottom)))
		: std::nullopt;
}

[[nodiscard]] std::optional<QRect> SegmentHitBounds(
		MarkdownArticle *article,
		int width,
		int height,
		int expectedSegmentIndex) {
	if (!article
		|| (width <= 0)
		|| (height <= 0)
		|| (expectedSegmentIndex < 0)) {
		return std::nullopt;
	}
	auto flags = Ui::Text::StateRequest::Flags();
	flags |= Ui::Text::StateRequest::Flag::LookupSymbol;
	auto left = width;
	auto top = height;
	auto right = -1;
	auto bottom = -1;
	for (auto y = 0; y != height; ++y) {
		for (auto x = 0; x != width; ++x) {
			const auto hit = article->hitTest(QPoint(x, y), flags);
			if (!hit.valid()
				|| !hit.direct
				|| (hit.segmentIndex != expectedSegmentIndex)) {
				continue;
			}
			left = std::min(left, x);
			top = std::min(top, y);
			right = std::max(right, x);
			bottom = std::max(bottom, y);
		}
	}
	return (right >= left) && (bottom >= top)
		? std::make_optional(QRect(QPoint(left, top), QPoint(right, bottom)))
		: std::nullopt;
}

[[nodiscard]] std::vector<const MeasuredFormula*> MeasuredFormulaPointers(
		const MarkdownArticleContent &prepared) {
	auto result = std::vector<const MeasuredFormula*>();
	for (const auto &slot : prepared.formulas) {
		if (slot.present && slot.measuredData) {
			result.push_back(slot.measuredData.get());
		}
	}
	return result;
}

template <typename Callback>
void ForEachPreparedBlock(
		const std::vector<PreparedBlock> &blocks,
		Callback &&callback) {
	for (const auto &block : blocks) {
		callback(block);
		ForEachPreparedBlock(block.children, callback);
	}
}

template <typename Callback>
void ForEachPreparedLink(
		const std::vector<PreparedBlock> &blocks,
		Callback &&callback) {
	ForEachPreparedBlock(blocks, [&](const PreparedBlock &block) {
		for (const auto &link : block.links) {
			callback(link);
		}
		for (const auto &row : block.tableRows) {
			for (const auto &cell : row.cells) {
				for (const auto &link : cell.links) {
					callback(link);
				}
			}
		}
	});
}

struct InlineTextObjectMatch {
	EntityInText entity;
	InlineTextObjectEntity object;
};

[[nodiscard]] std::vector<InlineTextObjectMatch> CollectInlineTextObjectMatches(
		const TextWithEntities &text) {
	auto result = std::vector<InlineTextObjectMatch>();
	for (const auto &entity : text.entities) {
		if (entity.type() != EntityType::CustomEmoji
			|| entity.length() != 1
			|| entity.offset() < 0
			|| entity.offset() >= text.text.size()
			|| text.text[entity.offset()] != QChar::ObjectReplacementCharacter) {
			continue;
		}
		if (const auto parsed = ParseInlineTextObjectEntity(entity.data())) {
			result.push_back(InlineTextObjectMatch{
				.entity = entity,
				.object = *parsed,
			});
		}
	}
	return result;
}

[[nodiscard]] bool TextHasInlineFormulaEntity(
		const TextWithEntities &text,
		const QString &copySource,
		const QString &trimmedTex = QString()) {
	for (const auto &match : CollectInlineTextObjectMatches(text)) {
		if (match.object.kind != InlineTextObjectKind::Formula) {
			continue;
		}
		const auto formula = std::get_if<InlineTextObjectFormulaData>(
			&match.object.data);
		if (!formula) {
			continue;
		}
		if ((formula->copySource == copySource)
			&& (trimmedTex.isEmpty() || formula->trimmedTex == trimmedTex)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasEntityType(
		const EntitiesInText &entities,
		EntityType type) {
	for (const auto &entity : entities) {
		if (entity.type() == type) {
			return true;
		}
	}
	return false;
}

void Check(bool condition, const QString &message, bool *ok) {
	if (condition) {
		return;
	}
	if (ok) {
		*ok = false;
	}
	PrintError(FromLatin1("assertion failed: ") + message);
}

void CheckParseSuccess(
		const QByteArray &source,
		const QString &label,
		bool *ok) {
	const auto parsed = ParseMarkdownForIv(source, ParseOptions{ label });
	Check(
		parsed.ok,
		label + FromLatin1(" parse failed: ") + parsed.error,
		ok);
}

void CheckParseFailure(
		const QByteArray &source,
		const QString &label,
		const QString &expectedError,
		bool *ok) {
	const auto parsed = ParseMarkdownForIv(source, ParseOptions{ label });
	Check(
		!parsed.ok,
		label + FromLatin1(" should fail"),
		ok);
	if (!parsed.ok) {
		Check(
			parsed.error == expectedError,
			label + FromLatin1(" error should be ")
				+ expectedError
				+ FromLatin1(", got ")
				+ parsed.error,
			ok);
	}
}

void CheckValidationEdges(bool *ok) {
	const auto &limits = ParseLimitsForIv();
	auto utf8BomSource = QByteArray::fromHex("EFBBBF");
	utf8BomSource.append("# Title\n");
	auto validatedUtf8Bom = CheckValidationSuccess(
		utf8BomSource,
		FromLatin1("utf8 bom"),
		ok);
	if (validatedUtf8Bom.ok) {
		Check(
			validatedUtf8Bom.source.normalized == QByteArray("# Title\n"),
			FromLatin1("utf8 bom normalized bytes"),
			ok);
		const auto parsedValidated = ParseMarkdownForIv(
			std::move(validatedUtf8Bom.source));
		Check(
			parsedValidated.ok,
			FromLatin1("utf8 bom validated parse failed: ")
				+ parsedValidated.error,
			ok);
	}
	CheckParseSuccess(
		utf8BomSource,
		FromLatin1("utf8 bom"),
		ok);

	CheckValidationFailure(
		QByteArray::fromHex("FFFE2300"),
		FromLatin1("utf16 bom"),
		FromLatin1("source-unsupported-bom"),
		ok);
	CheckParseFailure(
		QByteArray::fromHex("FFFE2300"),
		FromLatin1("utf16 bom"),
		FromLatin1("source-unsupported-bom"),
		ok);
	CheckValidationFailure(
		QByteArray("a\0b", 3),
		FromLatin1("nul byte"),
		FromLatin1("source-binary"),
		ok);
	CheckParseFailure(
		QByteArray("a\0b", 3),
		FromLatin1("nul byte"),
		FromLatin1("source-binary"),
		ok);
	CheckValidationFailure(
		QByteArray::fromHex("C328"),
		FromLatin1("invalid utf8"),
		FromLatin1("source-invalid-utf8"),
		ok);
	CheckParseFailure(
		QByteArray::fromHex("C328"),
		FromLatin1("invalid utf8"),
		FromLatin1("source-invalid-utf8"),
		ok);

	const auto oversizedSource = QByteArray(limits.maxSourceBytes + 1, 'a');
	CheckValidationFailure(
		oversizedSource,
		FromLatin1("source size"),
		FromLatin1("source-too-large"),
		ok);
	CheckParseFailure(
		oversizedSource,
		FromLatin1("source size"),
		FromLatin1("source-too-large"),
		ok);

	auto oversizedFormula = QByteArray();
	oversizedFormula.reserve(limits.maxFormulaBytes + 2);
	oversizedFormula.append('$');
	oversizedFormula.append(QByteArray(limits.maxFormulaBytes + 1, '+'));
	oversizedFormula.append('$');
	CheckParseFailure(
		oversizedFormula,
		FromLatin1("formula size"),
		FromLatin1("formula-too-large"),
		ok);

	const auto generated = QByteArray(
		"Inline code `$code$`.\n"
		"```\n"
		"$block$\n"
		"```\n"
		"Escaped \\$ and price $5.99$.\n"
		"Real $x + y$ done.\n");
	const auto parsed = ParseMarkdownForIv(
		generated,
		ParseOptions{ FromLatin1("generated-edge-checks.md") });
	Check(
		parsed.ok,
		FromLatin1("generated exclusions parse failed: ") + parsed.error,
		ok);
	if (parsed.ok) {
		Check(
			static_cast<int>(parsed.document.formulas.size()) == 1,
			FromLatin1("generated exclusions formula count"),
			ok);
		Check(
			CountFormulas(parsed.document, MathKind::Inline) == 1,
			FromLatin1("generated exclusions inline formula count"),
			ok);
		Check(
			CountFormulas(parsed.document, MathKind::Display) == 0,
			FromLatin1("generated exclusions display formula count"),
			ok);
		Check(
			HasFormula(parsed.document, MathKind::Inline, FromLatin1("x + y")),
			FromLatin1("generated exclusions real formula"),
			ok);
	}
}

void CheckInlineHtmlCoverage(bool dump, bool *ok) {
	const auto source = QByteArray(
		"H<sub>2</sub>O\n"
		"E = mc<sup>2</sup>\n"
		"<mark>Highlighted text using HTML mark</mark>\n"
		"<br>\n");
	const auto label = FromLatin1("generated-inline-html.md");
	const auto parsed = ParseMarkdownForIv(source, ParseOptions{ label });
	Check(
		parsed.ok,
		label + FromLatin1(" parse failed: ") + parsed.error,
		ok);
	if (!parsed.ok) {
		return;
	}
	const auto validated = CheckValidationSuccess(source, label, ok);
	if (!validated.ok) {
		return;
	}
	const auto parsedValidated = ParseMarkdownForIv(std::move(validated.source));
	Check(
		parsedValidated.ok,
		label + FromLatin1(" validated parse failed: ")
			+ parsedValidated.error,
		ok);
	if (!parsedValidated.ok) {
		return;
	}
	CheckMatchingParseCounts(
		parsed.document,
		parsedValidated.document,
		label,
		ok);
	if (dump) {
		PrintLine(DumpForDebug(parsed.document));
	}
	const auto &document = parsed.document.document;
	const auto subscriptParagraph = FindNodeByKindAndLineRange(
		document,
		NodeKind::Paragraph,
		1,
		1);
	Check(
		subscriptParagraph != nullptr,
		label + FromLatin1(" subscript paragraph range"),
		ok);
	if (subscriptParagraph) {
		Check(
			HasExactInlineHtmlTriplet(
				*subscriptParagraph,
				FromLatin1("<sub>"),
				FromLatin1("2"),
				FromLatin1("</sub>")),
			label + FromLatin1(" subscript HTML triplet"),
			ok);
	}
	const auto superscriptParagraph = FindNodeByKindAndLineRange(
		document,
		NodeKind::Paragraph,
		2,
		2);
	Check(
		superscriptParagraph != nullptr,
		label + FromLatin1(" superscript paragraph range"),
		ok);
	if (superscriptParagraph) {
		Check(
			HasExactInlineHtmlTriplet(
				*superscriptParagraph,
				FromLatin1("<sup>"),
				FromLatin1("2"),
				FromLatin1("</sup>")),
			label + FromLatin1(" superscript HTML triplet"),
			ok);
	}
	const auto markParagraph = FindNodeByKindAndLineRange(
		document,
		NodeKind::Paragraph,
		3,
		3);
	Check(
		markParagraph != nullptr,
		label + FromLatin1(" mark paragraph range"),
		ok);
	if (markParagraph) {
		Check(
			HasExactInlineHtmlTriplet(
				*markParagraph,
				FromLatin1("<mark>"),
				FromLatin1("Highlighted text using HTML mark"),
				FromLatin1("</mark>")),
			label + FromLatin1(" mark HTML triplet"),
			ok);
	}
	const auto htmlLineBreakParagraph = FindNodeByKindAndLineRange(
		document,
		NodeKind::Paragraph,
		4,
		4);
	Check(
		htmlLineBreakParagraph != nullptr,
		label + FromLatin1(" html line break paragraph range"),
		ok);
	if (htmlLineBreakParagraph) {
		const auto brInline = FindHtmlInlineByRaw(
			*htmlLineBreakParagraph,
			FromLatin1("<br>"));
		Check(
			brInline != nullptr,
			label + FromLatin1(" html line break raw node"),
			ok);
		if (brInline) {
			Check(
				brInline->text.isEmpty() && brInline->children.empty(),
				label + FromLatin1(" html line break lone HtmlInline"),
				ok);
		}
	}
}

void CheckFixtureSemanticCoverage(
		const PreparedDocument &document,
		const QString &path,
		bool *ok) {
	auto footnoteReferences = std::vector<const MarkdownNode*>();
	CollectNodesByKind(
		document.document,
		NodeKind::FootnoteReference,
		&footnoteReferences);
	Check(
		footnoteReferences.size() >= 2,
		FromLatin1("markdown-example.md footnote reference count"),
		ok);
	if (footnoteReferences.size() >= 2) {
		Check(
			footnoteReferences[0]->footnoteLabel == FromLatin1("1")
				&& footnoteReferences[0]->footnoteOrdinal == 1,
			FromLatin1("markdown-example.md first footnote reference label"),
			ok);
		Check(
			footnoteReferences[1]->footnoteLabel == FromLatin1("long-note")
				&& footnoteReferences[1]->footnoteOrdinal == 2,
			FromLatin1("markdown-example.md second footnote reference label"),
			ok);
	}

	auto footnoteDefinitions = std::vector<const MarkdownNode*>();
	CollectNodesByKind(
		document.document,
		NodeKind::FootnoteDefinition,
		&footnoteDefinitions);
	Check(
		footnoteDefinitions.size() >= 2,
		FromLatin1("markdown-example.md footnote definition count"),
		ok);
	if (footnoteDefinitions.size() >= 2) {
		Check(
			footnoteDefinitions[0]->anchorId == FromLatin1("fn-1")
				&& footnoteDefinitions[1]->anchorId == FromLatin1("fn-2"),
			FromLatin1("markdown-example.md footnote anchors"),
			ok);
	}

	const auto headingsLink = FindLinkByTarget(document.document, FromLatin1("#headings"));
	Check(
		headingsLink != nullptr,
		FromLatin1("markdown-example.md toc fragment link"),
		ok);

	const auto relativeLink = FindLinkByTarget(
		document.document,
		FromLatin1("./docs/getting-started.md"));
	Check(
		relativeLink != nullptr,
		FromLatin1("markdown-example.md relative link parse"),
		ok);

	const auto headings = FindNodeByKindAndLineRange(
		document.document,
		NodeKind::Heading,
		27,
		27);
	Check(
		headings != nullptr && headings->anchorId == FromLatin1("headings"),
		FromLatin1("markdown-example.md headings anchor id"),
		ok);
	const auto definitionLists = FindNodeByKindAndLineRange(
		document.document,
		NodeKind::Heading,
		266,
		266);
	Check(
		definitionLists != nullptr
			&& definitionLists->anchorId
				== FromLatin1("definition-lists-renderer-dependent"),
		FromLatin1("markdown-example.md punctuation heading anchor id"),
		ok);

	const auto details = FindNodeByKindAndLineRange(
		document.document,
		NodeKind::HtmlBlock,
		261,
		264);
	Check(
		details != nullptr,
		FromLatin1("markdown-example.md details block range"),
		ok);
	if (details) {
		Check(
			details->htmlBlockKind == HtmlBlockKind::Details
				&& details->detailsSummary
					== FromLatin1("Click to expand details/summary block"),
			FromLatin1("markdown-example.md details classification"),
			ok);
	}
	const auto comment = FindHtmlBlockContaining(
		document.document,
		FromLatin1("markdown-renderer-test"));
	Check(
		comment != nullptr && comment->htmlBlockKind == HtmlBlockKind::Comment,
		FromLatin1("markdown-example.md comment classification"),
		ok);
	Check(
		WarningContains(document, FromLatin1("Unsupported HTML block")),
		FromLatin1("markdown-example.md unsupported html warning"),
		ok);

	const auto duplicateHeadings = ParseMarkdownForIv(
		QByteArray("## Same\n## Same\n"),
		ParseOptions{ FromLatin1("generated-duplicate-headings.md") });
	Check(
		duplicateHeadings.ok,
		FromLatin1("generated duplicate headings parse failed"),
		ok);
	if (duplicateHeadings.ok) {
		auto duplicateNodes = std::vector<const MarkdownNode*>();
		CollectNodesByKind(
			duplicateHeadings.document.document,
			NodeKind::Heading,
			&duplicateNodes);
		Check(
			duplicateNodes.size() == 2
				&& duplicateNodes[0]->anchorId == FromLatin1("same")
				&& duplicateNodes[1]->anchorId == FromLatin1("same-2"),
			FromLatin1("generated duplicate headings anchors"),
			ok);
	}

}

void CheckInlineTextObjectPrepareCoverage(bool *ok) {
	const auto label = FromLatin1("generated-inline-text-objects.md");
	const auto parsed = ParseMarkdownForIv(
		QByteArray(
			"Paragraph $a^2 + b^2 = c^2$ text.\n\n"
			"#### Heading $ax^2 + bx + c = 0$\n\n"
			"| Formula | Value |\n"
			"| --- | --- |\n"
			"| $x^n$ | n |\n"),
		ParseOptions{ label });
	Check(
		parsed.ok,
		label + FromLatin1(" parse failed: ") + parsed.error,
		ok);
	if (!parsed.ok) {
		return;
	}
	const auto prepared = PrepareParsedDocumentForTest(
		parsed.document,
		label,
		std::make_shared<MathRenderer>());
	Check(
		!prepared.failure.failed(),
		label + FromLatin1(" prepare failure: ")
			+ PrepareFailureReason(prepared.failure),
		ok);
	if (prepared.failure.failed()) {
		return;
	}

	const PreparedBlock *paragraph = nullptr;
	const PreparedBlock *heading = nullptr;
	const PreparedBlock *table = nullptr;
	ForEachPreparedBlock(prepared.blocks.blocks, [&](const PreparedBlock &block) {
		if (!paragraph
			&& block.kind == PreparedBlockKind::Paragraph
			&& block.text.text.contains(FromLatin1("Paragraph"))) {
			paragraph = &block;
		}
		if (!heading
			&& block.kind == PreparedBlockKind::Heading
			&& block.text.text.contains(FromLatin1("Heading"))) {
			heading = &block;
		}
		if (!table && block.kind == PreparedBlockKind::Table) {
			table = &block;
		}
	});
	Check(
		paragraph != nullptr,
		label + FromLatin1(" prepared paragraph block"),
		ok);
	if (paragraph) {
		Check(
			paragraph->text.text.count(QChar::ObjectReplacementCharacter) == 1,
			label + FromLatin1(" paragraph ORC count"),
			ok);
		Check(
			TextHasInlineFormulaEntity(
				paragraph->text,
				FromLatin1("$a^2 + b^2 = c^2$"),
				FromLatin1("a^2 + b^2 = c^2")),
			label + FromLatin1(" paragraph inline formula entity"),
			ok);
	}
	Check(
		heading != nullptr,
		label + FromLatin1(" prepared heading block"),
		ok);
	if (heading) {
		Check(
			heading->text.text.count(QChar::ObjectReplacementCharacter) == 1,
			label + FromLatin1(" heading ORC count"),
			ok);
		Check(
			TextHasInlineFormulaEntity(
				heading->text,
				FromLatin1("$ax^2 + bx + c = 0$"),
				FromLatin1("ax^2 + bx + c = 0")),
			label + FromLatin1(" heading inline formula entity"),
			ok);
	}
	Check(
		table != nullptr,
		label + FromLatin1(" prepared table block"),
		ok);
	if (table) {
		Check(
			table->tableRows.size() == 2,
			label + FromLatin1(" table row count"),
			ok);
		if (table->tableRows.size() == 2
			&& table->tableRows[1].cells.size() == 2) {
			const auto &formulaCell = table->tableRows[1].cells[0];
			Check(
				formulaCell.text.text.count(QChar::ObjectReplacementCharacter) == 1,
				label + FromLatin1(" table cell ORC count"),
				ok);
			Check(
				TextHasInlineFormulaEntity(
					formulaCell.text,
					FromLatin1("$x^n$"),
					FromLatin1("x^n")),
				label + FromLatin1(" table cell inline formula entity"),
				ok);
		}
	}
}

void CheckNativeInstantViewPrepareCoverage(bool *ok) {
	const auto entityLabel = u"native-iv-inline-image-entity"_q;
	const auto serialized = SerializeInlineTextObjectEntity({
		.kind = InlineTextObjectKind::IvImage,
		.data = InlineTextObjectIvImageData{
			.documentId = 8801,
			.width = 24,
			.height = 18,
			.replacementText = u"[inline image]"_q,
		},
	});
	Check(
		!serialized.isEmpty(),
		entityLabel + u" serialize"_q,
		ok);
	const auto parsedEntity = ParseInlineTextObjectEntity(serialized);
	Check(
		parsedEntity.has_value(),
		entityLabel + u" parse"_q,
		ok);
	if (parsedEntity) {
		Check(
			parsedEntity->kind == InlineTextObjectKind::IvImage,
			entityLabel + u" parsed kind"_q,
			ok);
		if (const auto image = std::get_if<InlineTextObjectIvImageData>(
				&parsedEntity->data)) {
			Check(
				image->documentId == 8801,
				entityLabel + u" parsed document id"_q,
				ok);
			Check(
				image->width == 24 && image->height == 18,
				entityLabel + u" parsed size"_q,
				ok);
			Check(
				image->replacementText == u"[inline image]"_q,
				entityLabel + u" parsed replacement text"_q,
				ok);
		} else {
			Check(false, entityLabel + u" parsed payload"_q, ok);
		}
	}

	auto placeholderFixtures = std::vector<NativeIvPlaceholderFixture>();
	const auto addPlaceholder = [&](
			NativeIvPlaceholderKind kind,
			QString caption) {
		placeholderFixtures.push_back({
			.kind = kind,
			.caption = caption,
			.expectedLabel = NativeIvPlaceholderLabel(kind),
			.block = NativeIvPlaceholderBlock(kind, caption),
		});
	};
	addPlaceholder(NativeIvPlaceholderKind::Video, u"Video caption"_q);
	addPlaceholder(NativeIvPlaceholderKind::Embed, u"Embed caption"_q);
	addPlaceholder(NativeIvPlaceholderKind::EmbedPost, u"Embed post caption"_q);
	addPlaceholder(NativeIvPlaceholderKind::Collage, u"Collage caption"_q);
	addPlaceholder(
		NativeIvPlaceholderKind::Slideshow,
		u"Grouped caption"_q);
	addPlaceholder(NativeIvPlaceholderKind::Audio, u"Audio caption"_q);
	addPlaceholder(NativeIvPlaceholderKind::Map, u"Map caption"_q);

	auto supportedBlocks = QVector<MTPPageBlock>();
	supportedBlocks.push_back(NativeIvInlineImageParagraph(
		8801,
		24,
		18,
		u"Lead "_q,
		u" tail."_q));
	supportedBlocks.push_back(NativeIvPhotoBlock(
		9001,
		u"Photo caption"_q,
		u"Photo credit"_q,
		u"https://example.com/photo"_q));
	for (const auto &fixture : placeholderFixtures) {
		supportedBlocks.push_back(fixture.block);
	}
	supportedBlocks.push_back(NativeIvCoveredPhotoBlock(
		9002,
		u"Cover caption"_q));
	auto supportedPhotos = QVector<MTPPhoto>();
	supportedPhotos.push_back(NativeIvPhoto(9001, 640, 360));
	supportedPhotos.push_back(NativeIvPhoto(9002, 320, 200));
	auto supportedSource = NativeIvSource(
		std::move(supportedBlocks),
		std::move(supportedPhotos));
	const auto supported = TryPrepareNativeInstantView({
		.source = &supportedSource,
	});
	Check(
		supported.supported(),
		u"native-iv supported source classification"_q,
		ok);
	Check(
		!supported.unsupported() && !supported.failed(),
		u"native-iv supported source nonterminal classification"_q,
		ok);
	Check(
		!supported.content.failure.failed(),
		u"native-iv supported source failure state"_q,
		ok);
	Check(
		supported.content.blocks.blocks.size() == 10,
		u"native-iv supported prepared block count"_q,
		ok);

	const PreparedBlock *inlineParagraph = nullptr;
	const PreparedBlock *photo = nullptr;
	const PreparedBlock *coveredPhoto = nullptr;
	auto preparedPlaceholders = std::vector<const PreparedBlock*>();
	ForEachPreparedBlock(
		supported.content.blocks.blocks,
		[&](const PreparedBlock &block) {
			if (!inlineParagraph
				&& block.kind == PreparedBlockKind::Paragraph
				&& block.text.text.contains(QChar::ObjectReplacementCharacter)) {
				inlineParagraph = &block;
			}
			if (!photo
				&& block.kind == PreparedBlockKind::Photo
				&& block.photo.photoId == 9001) {
				photo = &block;
			}
			if (!coveredPhoto
				&& block.kind == PreparedBlockKind::Photo
				&& block.photo.photoId == 9002) {
				coveredPhoto = &block;
			}
			if (block.kind == PreparedBlockKind::Placeholder) {
				preparedPlaceholders.push_back(&block);
			}
		});
	Check(
		inlineParagraph != nullptr,
		u"native-iv inline-image paragraph block"_q,
		ok);
	if (inlineParagraph) {
		const auto matches = CollectInlineTextObjectMatches(
			inlineParagraph->text);
		Check(
			matches.size() == 1,
			u"native-iv inline-image entity count"_q,
			ok);
		Check(
			inlineParagraph->text.text.count(
				QChar::ObjectReplacementCharacter) == 1,
			u"native-iv inline-image ORC count"_q,
			ok);
		if (matches.size() == 1) {
			Check(
				matches[0].object.kind == InlineTextObjectKind::IvImage,
				u"native-iv inline-image prepared kind"_q,
				ok);
			if (const auto image = std::get_if<InlineTextObjectIvImageData>(
					&matches[0].object.data)) {
				Check(
					image->documentId == 8801,
					u"native-iv inline-image prepared document id"_q,
					ok);
				Check(
					image->width == 24 && image->height == 18,
					u"native-iv inline-image prepared size"_q,
					ok);
				Check(
					image->replacementText == u"[image]"_q,
					u"native-iv inline-image prepared replacement text"_q,
					ok);
			} else {
				Check(false, u"native-iv inline-image prepared payload"_q, ok);
			}
		}
	}
	Check(photo != nullptr, u"native-iv photo block"_q, ok);
	if (photo) {
		Check(
			photo->text.text == u"Photo caption\nPhoto credit"_q,
			u"native-iv photo caption text"_q,
			ok);
		Check(
			photo->photo.photoId == 9001,
			u"native-iv photo id"_q,
			ok);
		Check(
			photo->photo.width == 640 && photo->photo.height == 360,
			u"native-iv photo aspect metadata"_q,
			ok);
		Check(
			photo->photo.urlOverride == u"https://example.com/photo"_q,
			u"native-iv photo url override"_q,
			ok);
		Check(
			photo->photo.viewerOpen,
			u"native-iv photo viewer flag"_q,
			ok);
	}
	Check(
		coveredPhoto != nullptr,
		u"native-iv covered photo unwrapped"_q,
		ok);
	if (coveredPhoto) {
		Check(
			coveredPhoto->text.text == u"Cover caption"_q,
			u"native-iv covered photo caption"_q,
			ok);
		Check(
			coveredPhoto->photo.photoId == 9002,
			u"native-iv covered photo id"_q,
			ok);
		Check(
			coveredPhoto->photo.width == 320 && coveredPhoto->photo.height == 200,
			u"native-iv covered photo size"_q,
			ok);
	}
	Check(
		preparedPlaceholders.size() == placeholderFixtures.size(),
		u"native-iv placeholder prepared count"_q,
		ok);
	for (const auto &fixture : placeholderFixtures) {
		const auto it = std::find_if(
			preparedPlaceholders.begin(),
			preparedPlaceholders.end(),
			[&](const PreparedBlock *block) {
				return block
					&& block->placeholder.label == fixture.expectedLabel
					&& block->text.text == fixture.caption;
			});
		Check(
			it != preparedPlaceholders.end(),
			fixture.expectedLabel + u" prepared placeholder"_q,
			ok);
		if (it != preparedPlaceholders.end()) {
			const auto *block = *it;
			Check(
				block->placeholder.copyText
					== (fixture.expectedLabel + u"\n"_q + fixture.caption),
				fixture.expectedLabel + u" placeholder copy text"_q,
				ok);
		}
	}

	auto unsupportedBlocks = QVector<MTPPageBlock>();
	unsupportedBlocks.push_back(MTP_pageBlockCover(MTP_pageBlockUnsupported()));
	auto unsupportedSource = NativeIvSource(std::move(unsupportedBlocks));
	const auto unsupported = TryPrepareNativeInstantView({
		.source = &unsupportedSource,
	});
	Check(
		unsupported.supported(),
		u"native-iv unsupported block placeholder classification"_q,
		ok);
	Check(
		!unsupported.unsupported() && !unsupported.failed(),
		u"native-iv unsupported block nonterminal classification"_q,
		ok);
	Check(
		unsupported.content.blocks.blocks.size() == 1,
		u"native-iv unsupported block placeholder count"_q,
		ok);
	if (unsupported.content.blocks.blocks.size() == 1) {
		const auto &block = unsupported.content.blocks.blocks.front();
		Check(
			block.kind == PreparedBlockKind::Placeholder,
			u"native-iv unsupported block placeholder kind"_q,
			ok);
		Check(
			block.placeholder.label == u"Unsupported Block Placeholder"_q,
			u"native-iv unsupported block placeholder label"_q,
			ok);
	}

	auto missingPhotoBlocks = QVector<MTPPageBlock>();
	missingPhotoBlocks.push_back(NativeIvPhotoBlock(9999, u"Missing photo"_q));
	auto missingPhotoSource = NativeIvSource(std::move(missingPhotoBlocks));
	const auto missingPhoto = TryPrepareNativeInstantView({
		.source = &missingPhotoSource,
	});
	Check(
		missingPhoto.supported(),
		u"native-iv missing-photo placeholder classification"_q,
		ok);
	Check(
		!missingPhoto.unsupported() && !missingPhoto.failed(),
		u"native-iv missing-photo nonterminal classification"_q,
		ok);
	Check(
		missingPhoto.content.blocks.blocks.size() == 1,
		u"native-iv missing-photo placeholder count"_q,
		ok);
	if (missingPhoto.content.blocks.blocks.size() == 1) {
		const auto &block = missingPhoto.content.blocks.blocks.front();
		Check(
			block.kind == PreparedBlockKind::Placeholder,
			u"native-iv missing-photo placeholder kind"_q,
			ok);
		Check(
			block.placeholder.label == u"Photo Placeholder"_q,
			u"native-iv missing-photo placeholder label"_q,
			ok);
		Check(
			block.text.text == u"Missing photo"_q,
			u"native-iv missing-photo placeholder caption"_q,
			ok);
	}
}

void CheckNativeInstantViewArticleCoverage(bool *ok) {
	const auto placeholderLabel = u"native-iv-placeholder-article"_q;
	auto placeholderBlocks = QVector<MTPPageBlock>();
	placeholderBlocks.push_back(NativeIvPlaceholderBlock(
		NativeIvPlaceholderKind::Video,
		u"Placeholder caption"_q));
	auto placeholderSource = NativeIvSource(std::move(placeholderBlocks));
	auto placeholderPrepared = TryPrepareNativeInstantView({
		.source = &placeholderSource,
	});
	Check(
		placeholderPrepared.supported(),
		placeholderLabel + u" prepare supported"_q,
		ok);
	if (placeholderPrepared.supported()) {
		auto placeholderHeight = 0;
		auto placeholderArticle = BuildArticleForTest(
			std::move(placeholderPrepared.content),
			std::make_shared<MathRenderer>(),
			420,
			&placeholderHeight);
		const auto placeholderImage = PaintArticleForTest(
			placeholderArticle.get(),
			420,
			placeholderHeight);
		Check(
			HasPaintedPixels(placeholderImage),
			placeholderLabel + u" paint produced pixels"_q,
			ok);
		const auto placeholderBounds = SegmentHitBounds(
			placeholderArticle.get(),
			420,
			placeholderHeight,
			0);
		Check(
			placeholderBounds.has_value(),
			placeholderLabel + u" media hit bounds"_q,
			ok);
		if (placeholderBounds) {
			auto flags = Ui::Text::StateRequest::Flags();
			flags |= Ui::Text::StateRequest::Flag::LookupSymbol;
			const auto hit = placeholderArticle->hitTest(
				placeholderBounds->center(),
				flags);
			Check(
				hit.valid() && hit.direct && (hit.segmentIndex == 0),
				placeholderLabel + u" direct media hit"_q,
				ok);
			Check(
				hit.mediaActivation.kind == MediaActivationKind::None,
				placeholderLabel + u" no media activation"_q,
				ok);
			Check(
				!placeholderArticle->segmentIsText(0),
				placeholderLabel + u" segment is media"_q,
				ok);
			Check(
				placeholderArticle->segmentLength(0) == 1,
				placeholderLabel + u" media segment length"_q,
				ok);
			const auto context = placeholderArticle->textForContext(hit);
			Check(
				context.expanded
					== u"Video Placeholder\nPlaceholder caption"_q,
				placeholderLabel + u" context export text"_q,
				ok);
			const auto selection = placeholderArticle->textForSelection({
				.from = { .segment = 0, .offset = 0 },
				.to = { .segment = 0, .offset = 1 },
			}, nullptr);
			Check(
				selection.expanded == context.expanded,
				placeholderLabel + u" selection export text"_q,
				ok);
		}
	}

	const auto mixedLabel = u"native-iv-mixed-article"_q;
	auto mixedRuntime = std::make_shared<TestMediaRuntime>();
	mixedRuntime->inlineDocumentId = 8801;
	mixedRuntime->inlineImage = std::make_shared<TestDynamicImage>();
	mixedRuntime->preparedPhotoId = 9101;
	mixedRuntime->photoRuntime = std::make_shared<TestPhotoRuntime>();
	mixedRuntime->photoRuntime->thumbnailImage
		= std::make_shared<TestDynamicImage>();
	mixedRuntime->photoRuntime->fullImage
		= std::make_shared<TestDynamicImage>();
	mixedRuntime->photoRuntime->loadingValue = true;
	mixedRuntime->photoRuntime->progressValue = 0.5;

	auto mixedBlocks = QVector<MTPPageBlock>();
	mixedBlocks.push_back(NativeIvInlineImageParagraph(
		8801,
		24,
		18,
		u"Intro "_q,
		u" tail."_q));
	mixedBlocks.push_back(NativeIvPlaceholderBlock(
		NativeIvPlaceholderKind::Video));
	mixedBlocks.push_back(NativeIvPhotoBlock(9101));
	mixedBlocks.push_back(MTP_pageBlockParagraph(NativeIvText(
		u"Outro paragraph."_q)));
	auto mixedPhotos = QVector<MTPPhoto>();
	mixedPhotos.push_back(NativeIvPhoto(9101, 320, 180));
	auto mixedSource = NativeIvSource(
		std::move(mixedBlocks),
		std::move(mixedPhotos));
	auto mixedPrepared = TryPrepareNativeInstantView({
		.source = &mixedSource,
		.mediaRuntime = mixedRuntime,
	});
	Check(
		mixedPrepared.supported(),
		mixedLabel + u" prepare supported"_q,
		ok);
	if (!mixedPrepared.supported()) {
		return;
	}
	Check(
		mixedPrepared.content.mediaRuntime.get() == mixedRuntime.get(),
		mixedLabel + u" media runtime forwarded"_q,
		ok);
	auto renderer = std::make_shared<MathRenderer>();
	auto wideHeight = 0;
	auto article = BuildArticleForTest(
		std::move(mixedPrepared.content),
		renderer,
		520,
		&wideHeight);
	const auto wideImage = PaintArticleForTest(article.get(), 520, wideHeight);
	Check(
		HasPaintedPixels(wideImage),
		mixedLabel + u" wide paint produced pixels"_q,
		ok);
	Check(
		!mixedRuntime->inlineRequests.empty(),
		mixedLabel + u" inline image resolve request"_q,
		ok);
	if (!mixedRuntime->inlineRequests.empty()) {
		Check(
			mixedRuntime->inlineRequests.front().first == 8801,
			mixedLabel + u" inline image resolved document id"_q,
			ok);
		Check(
			mixedRuntime->inlineRequests.front().second == QSize(24, 18),
			mixedLabel + u" inline image resolved size"_q,
			ok);
	}
	Check(
		!mixedRuntime->photoRequests.empty()
			&& (mixedRuntime->photoRequests.front() == 9101),
		mixedLabel + u" photo runtime resolve request"_q,
		ok);
	Check(
		!mixedRuntime->photoRuntime->thumbnailSizes.empty()
			&& !mixedRuntime->photoRuntime->fullSizes.empty(),
		mixedLabel + u" photo image size requests"_q,
		ok);
	if (!mixedRuntime->photoRuntime->thumbnailSizes.empty()
		&& !mixedRuntime->photoRuntime->fullSizes.empty()) {
		Check(
			mixedRuntime->photoRuntime->thumbnailSizes.front()
				== mixedRuntime->photoRuntime->fullSizes.front(),
			mixedLabel + u" photo image request sizes match"_q,
			ok);
	}
	Check(
		article->segmentIsText(0),
		mixedLabel + u" inline-image paragraph remains text segment"_q,
		ok);
	if (article->segmentIsText(0)) {
		const auto exported = article->textForSelection({
			.from = { .segment = 0, .offset = 0 },
			.to = {
				.segment = 0,
				.offset = article->segmentLength(0),
			},
		}, nullptr);
		Check(
			exported.expanded == u"Intro [image] tail."_q,
			mixedLabel + u" inline-image export text"_q,
			ok);
		Check(
			!HasEntityType(exported.rich.entities, EntityType::CustomEmoji),
			mixedLabel + u" inline-image export drops custom entity"_q,
			ok);
	}

	const auto narrowHeight = article->resizeGetHeight(260);
	const auto narrowImage = PaintArticleForTest(
		article.get(),
		260,
		narrowHeight);
	Check(
		HasPaintedPixels(narrowImage),
		mixedLabel + u" narrow paint produced pixels"_q,
		ok);
	Check(
		narrowHeight > wideHeight,
		mixedLabel + u" narrow relayout grows height"_q,
		ok);
	Check(
		mixedRuntime->inlineImage->subscriptionCount >= 1,
		mixedLabel + u" inline image subscribed for repaint"_q,
		ok);
	Check(
		!mixedRuntime->inlineImage->requestedSizes.empty()
			&& (mixedRuntime->inlineImage->requestedSizes.front() == 24),
		mixedLabel + u" inline image paint request size"_q,
		ok);

	auto segmentBounds = std::vector<std::optional<QRect>>();
	for (auto segmentIndex = 0; segmentIndex != 4; ++segmentIndex) {
		segmentBounds.push_back(SegmentHitBounds(
			article.get(),
			260,
			narrowHeight,
			segmentIndex));
		Check(
			segmentBounds.back().has_value(),
			mixedLabel + u" segment hit bounds "_q + QString::number(segmentIndex),
			ok);
	}
	auto haveBounds = true;
	for (const auto &bounds : segmentBounds) {
		if (!bounds) {
			haveBounds = false;
			break;
		}
	}
	if (!haveBounds) {
		return;
	}
	for (auto segmentIndex = 1; segmentIndex != 4; ++segmentIndex) {
		Check(
			segmentBounds[segmentIndex]->top()
				> segmentBounds[segmentIndex - 1]->bottom(),
			mixedLabel + u" segment order "_q + QString::number(segmentIndex),
			ok);
	}
	auto flags = Ui::Text::StateRequest::Flags();
	flags |= Ui::Text::StateRequest::Flag::LookupSymbol;
	const auto photoHit = article->hitTest(segmentBounds[2]->center(), flags);
	Check(
		photoHit.valid() && photoHit.direct && (photoHit.segmentIndex == 2),
		mixedLabel + u" photo direct hit"_q,
		ok);
	Check(
		photoHit.mediaActivation.kind == MediaActivationKind::Photo,
		mixedLabel + u" photo activation kind"_q,
		ok);
	Check(
		!photoHit.preparedLink.has_value(),
		mixedLabel + u" photo hit has no external prepared link"_q,
		ok);
	Check(
		photoHit.mediaActivation.photo.get() == mixedRuntime->photoRuntime.get(),
		mixedLabel + u" photo runtime forwarded to hit"_q,
		ok);
	if (photoHit.mediaActivation.photo) {
		photoHit.mediaActivation.photo->open(Qt::LeftButton);
	}
	Check(
		mixedRuntime->photoRuntime->openedButtons.size() == 1
			&& mixedRuntime->photoRuntime->openedButtons.front()
				== Qt::LeftButton,
		mixedLabel + u" photo open intent"_q,
		ok);

	mixedRuntime->inlineImage->setFrame(SolidTestImage(
		24,
		18,
		QColor(0, 160, 220)));
	mixedRuntime->inlineImage->notify();
	mixedRuntime->photoRuntime->thumbnailImage->setFrame(SolidTestImage(
		160,
		90,
		QColor(220, 160, 0)));
	mixedRuntime->photoRuntime->fullImage->setFrame(SolidTestImage(
		320,
		180,
		QColor(0, 200, 90)));
	mixedRuntime->photoRuntime->loadingValue = false;
	mixedRuntime->photoRuntime->loadedValue = true;
	mixedRuntime->photoRuntime->progressValue = 1.;

	const auto narrowHeightAfterUpdate = article->resizeGetHeight(260);
	Check(
		narrowHeightAfterUpdate == narrowHeight,
		mixedLabel + u" media repaint keeps layout height"_q,
		ok);
	const auto updatedImage = PaintArticleForTest(
		article.get(),
		260,
		narrowHeightAfterUpdate);
	Check(
		HasPaintedPixels(updatedImage),
		mixedLabel + u" updated paint produced pixels"_q,
		ok);
	const auto updatedPhotoBounds = SegmentHitBounds(
		article.get(),
		260,
		narrowHeightAfterUpdate,
		2);
	const auto updatedInlineBounds = SegmentHitBounds(
		article.get(),
		260,
		narrowHeightAfterUpdate,
		0);
	Check(
		updatedPhotoBounds.has_value()
			&& (*updatedPhotoBounds == *segmentBounds[2]),
		mixedLabel + u" photo repaint keeps bounds"_q,
		ok);
	Check(
		updatedInlineBounds.has_value()
			&& (*updatedInlineBounds == *segmentBounds[0]),
		mixedLabel + u" inline-image repaint keeps bounds"_q,
		ok);
}

void CheckPrepareCoverage(
		const PreparedFixture &markdownFixture,
		const PreparedFixture &latexFixture,
		bool *ok) {
	const auto &markdown = markdownFixture.prepared;
	const auto &latex = latexFixture.prepared;

	auto markdownTables = std::vector<const PreparedBlock*>();
	const PreparedBlock *markdownDisplayMath = nullptr;
	const PreparedBlock *detailsBlock = nullptr;
	const PreparedBlock *footnoteList = nullptr;
	ForEachPreparedBlock(markdown.blocks.blocks, [&](const PreparedBlock &block) {
		if (block.kind == PreparedBlockKind::Table) {
			markdownTables.push_back(&block);
		}
		if (!markdownDisplayMath
			&& block.kind == PreparedBlockKind::DisplayMath
			&& block.formulaTex.trimmed()
				== FromLatin1("\\int_0^1 x^2\\,dx = \\frac{1}{3}")) {
			markdownDisplayMath = &block;
		}
		if (!detailsBlock
			&& block.kind == PreparedBlockKind::Details
			&& block.text.text.contains(
				FromLatin1("Click to expand details/summary block"))) {
			detailsBlock = &block;
		}
		if (!footnoteList
			&& block.kind == PreparedBlockKind::List
			&& block.listKind == ListKind::Ordered
			&& block.children.size() >= 2
			&& block.children[0].anchorId == FromLatin1("fn-1")
			&& block.children[1].anchorId == FromLatin1("fn-2")) {
			footnoteList = &block;
		}
	});
	Check(
		markdownDisplayMath != nullptr,
		FromLatin1("markdown-example.md prepared display math formulaTex"),
		ok);
	Check(
		markdownTables.size() >= 2,
		FromLatin1("markdown-example.md prepared table count"),
		ok);
	if (markdownTables.size() >= 2) {
		const auto &firstTable = *markdownTables[0];
		const auto &secondTable = *markdownTables[1];
		Check(
			firstTable.tableColumnCount == 3,
			FromLatin1("markdown-example.md prepared first table column count"),
			ok);
		Check(
			firstTable.tableRows.size() == 4,
			FromLatin1("markdown-example.md prepared first table row count"),
			ok);
		if (firstTable.tableRows.size() == 4) {
			Check(
				firstTable.tableRows[0].header,
				FromLatin1("markdown-example.md prepared first table header row"),
				ok);
			Check(
				firstTable.tableRows[0].cells.size() == 3
					&& firstTable.tableRows[1].cells.size() == 3,
				FromLatin1("markdown-example.md prepared first table cell shape"),
				ok);
		}
		Check(
			secondTable.tableAlignments.size() == 3
				&& secondTable.tableAlignments[0] == TableAlignment::Left
				&& secondTable.tableAlignments[1] == TableAlignment::Center
				&& secondTable.tableAlignments[2] == TableAlignment::Right,
			FromLatin1("markdown-example.md prepared second table alignments"),
			ok);
	}
	Check(
		detailsBlock != nullptr,
		FromLatin1("markdown-example.md prepared details block"),
		ok);
	if (detailsBlock) {
		Check(
			detailsBlock->collapsed,
			FromLatin1("markdown-example.md prepared details collapsed"),
			ok);
		Check(
			!detailsBlock->children.empty()
				&& detailsBlock->children[0].kind == PreparedBlockKind::Paragraph
				&& detailsBlock->children[0].text.text.contains(
					FromLatin1("Hidden content inside details.")),
			FromLatin1("markdown-example.md prepared details body"),
			ok);
	}
	Check(
		footnoteList != nullptr,
		FromLatin1("markdown-example.md prepared footnote list"),
		ok);
	auto footnoteReferenceOne = false;
	auto footnoteReferenceTwo = false;
	auto footnoteBacklinkFound = false;
	ForEachPreparedLink(markdown.blocks.blocks, [&](const PreparedLink &link) {
		if (link.kind == PreparedLinkKind::Footnote
			&& link.target == FromLatin1("fn-1")) {
			footnoteReferenceOne = true;
		}
		if (link.kind == PreparedLinkKind::Footnote
			&& link.target == FromLatin1("fn-2")) {
			footnoteReferenceTwo = true;
		}
		if (link.kind == PreparedLinkKind::FootnoteBacklink
			&& !link.target.isEmpty()) {
			footnoteBacklinkFound = true;
		}
	});
	Check(
		footnoteReferenceOne && footnoteReferenceTwo,
		FromLatin1("markdown-example.md prepared footnote references"),
		ok);
	Check(
		footnoteBacklinkFound,
		FromLatin1("markdown-example.md prepared footnote backlink"),
		ok);

	auto latexTables = std::vector<const PreparedBlock*>();
	ForEachPreparedBlock(latex.blocks.blocks, [&](const PreparedBlock &block) {
		if (block.kind == PreparedBlockKind::Table) {
			latexTables.push_back(&block);
		}
	});
	Check(
		!latexTables.empty(),
		FromLatin1("latex-markdown-test.md prepared table count"),
		ok);
	if (!latexTables.empty()) {
		const auto &table = *latexTables[0];
		Check(
			table.tableColumnCount == 3,
			FromLatin1("latex-markdown-test.md prepared table column count"),
			ok);
		Check(
			table.tableRows.size() == 5,
			FromLatin1("latex-markdown-test.md prepared table row count"),
			ok);
		if (table.tableRows.size() == 5) {
			Check(
				table.tableRows[0].header,
				FromLatin1("latex-markdown-test.md prepared table header row"),
				ok);
			Check(
				table.tableRows[1].cells.size() == 3,
				FromLatin1("latex-markdown-test.md prepared table cell count"),
				ok);
		}
	}

	const auto &limits = PrepareTableRenderLimitsForIv();
	auto overflowTable = QByteArray("| A | B |\n| --- | --- |\n");
	for (auto i = 0; i != limits.maxRows; ++i) {
		overflowTable.append("| row ");
		overflowTable.append(QByteArray::number(i));
		overflowTable.append(" | value |\n");
	}
	const auto overflowLabel = FromLatin1("generated-overflow-table.md");
	const auto overflowParsed = ParseMarkdownForIv(
		overflowTable,
		ParseOptions{ overflowLabel });
	Check(
		overflowParsed.ok,
		overflowLabel + FromLatin1(" parse failed: ") + overflowParsed.error,
		ok);
	if (overflowParsed.ok) {
		const auto overflowPrepared = PrepareParsedDocumentForTest(
			overflowParsed.document,
			overflowLabel,
			std::make_shared<MathRenderer>());
		Check(
			!overflowPrepared.failure.failed(),
			overflowLabel + FromLatin1(" prepare failure: ")
				+ PrepareFailureReason(overflowPrepared.failure),
			ok);
		auto overflowTableBlocks = 0;
		ForEachPreparedBlock(
			overflowPrepared.blocks.blocks,
			[&](const PreparedBlock &block) {
				if (block.kind == PreparedBlockKind::Table) {
					++overflowTableBlocks;
				}
			});
		Check(
			overflowPrepared.debug.prepareWarningCount > 0,
			overflowLabel + FromLatin1(" flatten warning count"),
			ok);
		Check(
			overflowTableBlocks == 0,
			overflowLabel + FromLatin1(" flattened table block removed"),
			ok);
		Check(
			!overflowPrepared.blocks.blocks.empty(),
			overflowLabel + FromLatin1(" flattened fallback blocks present"),
			ok);
	}
}

void CheckPrepareLinkClassification(
		const QString &sourcePath,
		bool *ok) {
	const auto label = FromLatin1("generated-relative-links.md");
	const auto parsed = ParseMarkdownForIv(
		QByteArray(
			"[Local](./docs/getting-started.md#section-1)\n"
			"[Rejected](../outside.md)\n"),
		ParseOptions{ label });
	Check(
		parsed.ok,
		label + FromLatin1(" parse failed: ") + parsed.error,
		ok);
	if (!parsed.ok) {
		return;
	}
	const auto prepared = PrepareParsedDocumentForTest(
		parsed.document,
		sourcePath,
		std::make_shared<MathRenderer>());
	Check(
		!prepared.failure.failed(),
		label + FromLatin1(" prepare failure: ")
			+ PrepareFailureReason(prepared.failure),
		ok);
	const auto expectedLocalTarget = QDir(
		QFileInfo(sourcePath).absolutePath()).absoluteFilePath(
			FromLatin1("docs/getting-started.md"));
	auto foundLocal = false;
	auto foundRejected = false;
	ForEachPreparedLink(prepared.blocks.blocks, [&](const PreparedLink &link) {
		if (link.kind == PreparedLinkKind::LocalFile
			&& link.target == QDir::cleanPath(expectedLocalTarget)
			&& link.fragment == FromLatin1("section-1")
			&& link.copyText
				== FromLatin1("./docs/getting-started.md#section-1")) {
			foundLocal = true;
		}
		if (link.kind == PreparedLinkKind::RejectedRelative
			&& link.copyText == FromLatin1("../outside.md")) {
			foundRejected = true;
		}
	});
	Check(
		foundLocal,
		FromLatin1("generated-relative-links.md local markdown classification"),
		ok);
	Check(
		foundRejected,
		FromLatin1("generated-relative-links.md rejected relative classification"),
		ok);
}

void CheckInlineTextObjectArticleCoverage(bool *ok) {
	const auto selectionLabel = FromLatin1("generated-inline-selection.md");
	const auto selectionParsed = ParseMarkdownForIv(
		QByteArray("Inline formula $\\frac{a}{b}$ export.\n"),
		ParseOptions{ selectionLabel });
	Check(
		selectionParsed.ok,
		selectionLabel + FromLatin1(" parse failed: ")
			+ selectionParsed.error,
		ok);
	if (selectionParsed.ok) {
		auto selectionRenderer = std::make_shared<MathRenderer>();
		auto selectionPrepared = PrepareParsedDocumentForTest(
			selectionParsed.document,
			selectionLabel,
			selectionRenderer);
		Check(
			!selectionPrepared.failure.failed(),
			selectionLabel + FromLatin1(" prepare failure: ")
				+ PrepareFailureReason(selectionPrepared.failure),
			ok);
		if (!selectionPrepared.failure.failed()) {
			auto selectionObjectOffset = -1;
			ForEachPreparedBlock(
				selectionPrepared.blocks.blocks,
				[&](const PreparedBlock &block) {
					if (selectionObjectOffset >= 0) {
						return;
					}
					const auto matches = CollectInlineTextObjectMatches(block.text);
					if (!matches.empty()) {
						selectionObjectOffset = matches.front().entity.offset();
					}
				});
			auto selectionFormulaWidth = 0;
			for (const auto &slot : selectionPrepared.formulas) {
				if (slot.present
					&& (slot.kind == MathKind::Inline)
					&& slot.measured.success) {
					selectionFormulaWidth = std::max(
						slot.measured.logicalSize.width(),
						1);
					break;
				}
			}
			auto selectionArticleHeight = 0;
			auto selectionArticle = BuildArticleForTest(
				std::move(selectionPrepared),
				selectionRenderer,
				480,
				&selectionArticleHeight);
			const auto selectionImage = PaintArticleForTest(
				selectionArticle.get(),
				480,
				selectionArticleHeight);
			Check(
				HasPaintedPixels(selectionImage),
				selectionLabel + FromLatin1(" paint produced pixels"),
				ok);
			const auto selectionHitBounds = SymbolHitBounds(
				selectionArticle.get(),
				480,
				selectionArticleHeight,
				selectionObjectOffset);
			Check(
				selectionHitBounds.has_value(),
				selectionLabel + FromLatin1(" inline formula hit bounds"),
				ok);
			if (selectionHitBounds && (selectionFormulaWidth > 0)) {
				Check(
					selectionHitBounds->width() >= selectionFormulaWidth,
					selectionLabel + FromLatin1(
						" inline formula hit width uses object width"),
					ok);
			}
			Check(
				selectionArticle->segmentIsText(0),
				selectionLabel + FromLatin1(" first segment is text"),
				ok);
			if (selectionArticle->segmentIsText(0)) {
				const auto exported = selectionArticle->textForSelection({
					.from = { .segment = 0, .offset = 0 },
					.to = {
						.segment = 0,
						.offset = selectionArticle->segmentLength(0),
					},
				}, nullptr);
				const auto expected = FromLatin1(
					"Inline formula $\\frac{a}{b}$ export.");
				Check(
					exported.expanded == expected,
					selectionLabel + FromLatin1(" expanded export"),
					ok);
				Check(
					exported.rich.text == expected,
					selectionLabel + FromLatin1(" rich export text"),
					ok);
				Check(
					!HasEntityType(exported.rich.entities, EntityType::CustomEmoji),
					selectionLabel + FromLatin1(" export drops custom emoji entity"),
					ok);
			}
		}
	}

	const auto repeatedLabel = FromLatin1("generated-repeated-inline-formulas.md");
	const auto repeatedParsed = ParseMarkdownForIv(
		QByteArray("Repeated $x^2$ then $x^2$ then $x^2$.\n"),
		ParseOptions{ repeatedLabel });
	Check(
		repeatedParsed.ok,
		repeatedLabel + FromLatin1(" parse failed: ") + repeatedParsed.error,
		ok);
	if (!repeatedParsed.ok) {
		return;
	}
	auto repeatedRenderer = std::make_shared<MathRenderer>();
	auto repeatedPrepared = PrepareParsedDocumentForTest(
		repeatedParsed.document,
		repeatedLabel,
		repeatedRenderer);
	Check(
		!repeatedPrepared.failure.failed(),
		repeatedLabel + FromLatin1(" prepare failure: ")
			+ PrepareFailureReason(repeatedPrepared.failure),
		ok);
	if (repeatedPrepared.failure.failed()) {
		return;
	}
	const auto repeatedPointers = MeasuredFormulaPointers(repeatedPrepared);
	Check(
		repeatedPointers.size() == 3,
		repeatedLabel + FromLatin1(" measured pointer count"),
		ok);
	if (repeatedPointers.size() == 3) {
		Check(
			repeatedPointers[0] == repeatedPointers[1]
				&& repeatedPointers[1] == repeatedPointers[2],
			repeatedLabel + FromLatin1(" measured pointer reuse in one prepare"),
			ok);
	}
	auto repeatedArticleHeight = 0;
	auto repeatedArticle = BuildArticleForTest(
		std::move(repeatedPrepared),
		repeatedRenderer,
		480,
		&repeatedArticleHeight);
	const auto repeatedFirstImage = PaintArticleForTest(
		repeatedArticle.get(),
		480,
		repeatedArticleHeight);
	Check(
		HasPaintedPixels(repeatedFirstImage),
		repeatedLabel + FromLatin1(" first paint produced pixels"),
		ok);
	const auto repeatedFirstCounters = repeatedRenderer->debugCounters();
	Check(
		repeatedFirstCounters.misses >= 1,
		repeatedLabel + FromLatin1(" first paint raster misses"),
		ok);
	Check(
		repeatedFirstCounters.rendered >= 1,
		repeatedLabel + FromLatin1(" first paint raster rendered"),
		ok);
	const auto repeatedSecondImage = PaintArticleForTest(
		repeatedArticle.get(),
		480,
		repeatedArticleHeight);
	Check(
		HasPaintedPixels(repeatedSecondImage),
		repeatedLabel + FromLatin1(" second paint produced pixels"),
		ok);
	const auto repeatedSecondCounters = repeatedRenderer->debugCounters();
	Check(
		repeatedSecondCounters.hits == repeatedFirstCounters.hits
			&& repeatedSecondCounters.misses == repeatedFirstCounters.misses
			&& repeatedSecondCounters.rendered == repeatedFirstCounters.rendered,
		repeatedLabel + FromLatin1(" same article paint cache reuse"),
		ok);
	repeatedRenderer->resetDebugCounters();
	auto repeatedPreparedAgain = PrepareParsedDocumentForTest(
		repeatedParsed.document,
		repeatedLabel,
		repeatedRenderer);
	Check(
		!repeatedPreparedAgain.failure.failed(),
		repeatedLabel + FromLatin1(" second prepare failure: ")
			+ PrepareFailureReason(repeatedPreparedAgain.failure),
		ok);
	if (!repeatedPreparedAgain.failure.failed()) {
		auto repeatedArticleAgainHeight = 0;
		auto repeatedArticleAgain = BuildArticleForTest(
			std::move(repeatedPreparedAgain),
			repeatedRenderer,
			480,
			&repeatedArticleAgainHeight);
		const auto repeatedCachedImage = PaintArticleForTest(
			repeatedArticleAgain.get(),
			480,
			repeatedArticleAgainHeight);
		Check(
			HasPaintedPixels(repeatedCachedImage),
			repeatedLabel + FromLatin1(" cached paint produced pixels"),
			ok);
		const auto repeatedReuseCounters = repeatedRenderer->debugCounters();
		Check(
			repeatedReuseCounters.hits >= 1,
			repeatedLabel + FromLatin1(" renderer cache reuse hits"),
			ok);
		Check(
			repeatedReuseCounters.misses == 0,
			repeatedLabel + FromLatin1(" renderer cache reuse misses"),
			ok);
		Check(
			repeatedReuseCounters.rendered == 0,
			repeatedLabel + FromLatin1(" renderer cache reuse rendered"),
			ok);
	}
}

void CheckArticleRenderSmoke(
		const PreparedFixture &markdownFixture,
		const PreparedFixture &latexFixture,
		bool *ok) {
	Check(
		MicrotexBackendLinked(),
		FromLatin1("microtex backend should be linked"),
		ok);
	auto marginRenderer = MathRenderer();
	const auto dimensions = CaptureMarkdownPrepareDimensions();
	const auto checkFormulaRasterMargins = [&](
			const QString &label,
			const QString &tex,
			MathKind kind,
			int textSize) {
		const auto rendered = marginRenderer.renderFormula({
			.trimmedTex = tex,
			.kind = kind,
			.textSize = textSize,
			.renderWidthCap = dimensions.displayMathMaxRenderWidth,
			.renderHeightCap = dimensions.displayMathMaxRenderHeight,
			.devicePixelRatio = 2,
		});
		Check(
			rendered.success,
			label + FromLatin1(" formula raster success"),
			ok);
		if (rendered.success) {
			Check(
				!PaintTouchesBottomOrRightImageEdge(rendered.image),
				label + FromLatin1(" formula raster margin"),
				ok);
		}
	};
	checkFormulaRasterMargins(
		FromLatin1("inline-x"),
		FromLatin1("x"),
		MathKind::Inline,
		dimensions.bodyTextSize);
	checkFormulaRasterMargins(
		FromLatin1("inline-y"),
		FromLatin1("y"),
		MathKind::Inline,
		dimensions.bodyTextSize);
	checkFormulaRasterMargins(
		FromLatin1("inline-alpha"),
		FromLatin1("\\alpha"),
		MathKind::Inline,
		dimensions.bodyTextSize);
	checkFormulaRasterMargins(
		FromLatin1("inline-beta"),
		FromLatin1("\\beta"),
		MathKind::Inline,
		dimensions.bodyTextSize);
	checkFormulaRasterMargins(
		FromLatin1("display-quadratic"),
		FromLatin1("x = \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}"),
		MathKind::Display,
		dimensions.displayMathTextSize);
	auto renderer = std::make_shared<MathRenderer>();
	auto firstMarkdown = PrepareParsedDocumentForTest(
		markdownFixture.parsed,
		markdownFixture.path,
		renderer);
	auto firstLatex = PrepareParsedDocumentForTest(
		latexFixture.parsed,
		latexFixture.path,
		renderer);
	Check(
		!firstMarkdown.failure.failed() && !firstLatex.failure.failed(),
		FromLatin1("article prepare smoke first pass failed"),
		ok);
	Check(
		renderer->cacheUsageBytes() == 0,
		FromLatin1("article prepare smoke keeps renderer cache empty"),
		ok);
	const auto prepareCounters = renderer->debugCounters();
	Check(
		prepareCounters.hits == 0
			&& prepareCounters.misses == 0
			&& prepareCounters.rendered == 0,
		FromLatin1("article prepare smoke has no raster work"),
		ok);
	auto prepareSmokeLine = FromLatin1("prepare-lazy-smoke");
	prepareSmokeLine.append(FromLatin1(" measured_slots="));
	prepareSmokeLine.append(QString::number(
		CountMeasuredFormulaSlots(firstMarkdown)
			+ CountMeasuredFormulaSlots(firstLatex)));
	prepareSmokeLine.append(FromLatin1(" hits="));
	prepareSmokeLine.append(QString::number(prepareCounters.hits));
	prepareSmokeLine.append(FromLatin1(" misses="));
	prepareSmokeLine.append(QString::number(prepareCounters.misses));
	prepareSmokeLine.append(FromLatin1(" rendered="));
	prepareSmokeLine.append(QString::number(prepareCounters.rendered));
	PrintLine(prepareSmokeLine);

	const auto articleWidth = 640;
	const auto expectedArticleFormulaHits = CountMeasuredFormulaSlots(
		firstMarkdown);
	auto firstArticleHeight = 0;
	auto firstArticle = BuildArticleForTest(
		std::move(firstMarkdown),
		renderer,
		articleWidth,
		&firstArticleHeight);
	const auto firstImage = PaintArticleForTest(
		firstArticle.get(),
		articleWidth,
		firstArticleHeight);
	Check(
		HasPaintedPixels(firstImage),
		FromLatin1("article first paint produced pixels"),
		ok);
	const auto firstPaintCounters = renderer->debugCounters();
	Check(
		firstPaintCounters.misses >= expectedArticleFormulaHits,
		FromLatin1("article first paint lazy raster misses"),
		ok);
	Check(
		firstPaintCounters.rendered >= expectedArticleFormulaHits,
		FromLatin1("article first paint lazy raster rendered"),
		ok);
	Check(
		renderer->cacheUsageBytes() > 0,
		FromLatin1("article first paint populated renderer cache"),
		ok);
	const auto secondImage = PaintArticleForTest(
		firstArticle.get(),
		articleWidth,
		firstArticleHeight);
	Check(
		HasPaintedPixels(secondImage),
		FromLatin1("article second paint produced pixels"),
		ok);
	const auto secondPaintCounters = renderer->debugCounters();
	Check(
		secondPaintCounters.hits == firstPaintCounters.hits
			&& secondPaintCounters.misses == firstPaintCounters.misses
			&& secondPaintCounters.rendered == firstPaintCounters.rendered,
		FromLatin1("article same-instance raster cache reuse"),
		ok);
	auto articleSmokeLine = FromLatin1("article-lazy-smoke");
	articleSmokeLine.append(FromLatin1(" first_hits="));
	articleSmokeLine.append(QString::number(firstPaintCounters.hits));
	articleSmokeLine.append(FromLatin1(" first_misses="));
	articleSmokeLine.append(QString::number(firstPaintCounters.misses));
	articleSmokeLine.append(FromLatin1(" first_rendered="));
	articleSmokeLine.append(QString::number(firstPaintCounters.rendered));
	articleSmokeLine.append(FromLatin1(" second_hits="));
	articleSmokeLine.append(QString::number(secondPaintCounters.hits));
	articleSmokeLine.append(FromLatin1(" second_misses="));
	articleSmokeLine.append(QString::number(secondPaintCounters.misses));
	PrintLine(articleSmokeLine);

	renderer->resetDebugCounters();
	auto secondMarkdown = PrepareParsedDocumentForTest(
		markdownFixture.parsed,
		markdownFixture.path,
		renderer);
	Check(
		!secondMarkdown.failure.failed(),
		FromLatin1("article renderer cache smoke second prepare failed"),
		ok);
	auto secondArticleHeight = 0;
	auto secondArticle = BuildArticleForTest(
		std::move(secondMarkdown),
		renderer,
		articleWidth,
		&secondArticleHeight);
	const auto cachedImage = PaintArticleForTest(
		secondArticle.get(),
		articleWidth,
		secondArticleHeight);
	Check(
		HasPaintedPixels(cachedImage),
		FromLatin1("article renderer cache paint produced pixels"),
		ok);
	const auto rendererReuseCounters = renderer->debugCounters();
	Check(
		rendererReuseCounters.hits >= expectedArticleFormulaHits,
		FromLatin1("article renderer cache reuse hits"),
		ok);
	Check(
		rendererReuseCounters.misses == 0,
		FromLatin1("article renderer cache reuse misses stay zero"),
		ok);
	Check(
		rendererReuseCounters.rendered == 0,
		FromLatin1("article renderer cache reuse rendered stays zero"),
		ok);
	auto rendererSmokeLine = FromLatin1("renderer-cache-smoke");
	rendererSmokeLine.append(FromLatin1(" hits="));
	rendererSmokeLine.append(QString::number(rendererReuseCounters.hits));
	rendererSmokeLine.append(FromLatin1(" misses="));
	rendererSmokeLine.append(QString::number(rendererReuseCounters.misses));
	rendererSmokeLine.append(FromLatin1(" rendered="));
	rendererSmokeLine.append(QString::number(rendererReuseCounters.rendered));
	PrintLine(rendererSmokeLine);

	const auto sharedMarkdown = std::make_shared<const PreparedDocument>(
		markdownFixture.parsed);
	auto measurementFirst = PrepareParsedDocumentForTest(
		sharedMarkdown,
		markdownFixture.path,
		std::make_shared<MathRenderer>());
	Check(
		!measurementFirst.failure.failed(),
		FromLatin1("document measurement cache smoke first pass failed"),
		ok);
	auto measurementSecond = PrepareParsedDocumentForTest(
		sharedMarkdown,
		markdownFixture.path,
		std::make_shared<MathRenderer>());
	Check(
		!measurementSecond.failure.failed(),
		FromLatin1("document measurement cache smoke second pass failed"),
		ok);
	const auto firstMeasurements = MeasuredFormulaPointers(measurementFirst);
	const auto secondMeasurements = MeasuredFormulaPointers(measurementSecond);
	Check(
		!firstMeasurements.empty()
			&& (firstMeasurements.size() == secondMeasurements.size()),
		FromLatin1("document measurement cache smoke pointer shape"),
		ok);
	for (auto i = 0, count = int(firstMeasurements.size()); i != count; ++i) {
		Check(
			firstMeasurements[i] == secondMeasurements[i],
			FromLatin1("document measurement cache smoke pointer reuse"),
			ok);
	}
	auto documentSmokeLine = FromLatin1("document-cache-smoke");
	documentSmokeLine.append(FromLatin1(" slots="));
	documentSmokeLine.append(QString::number(firstMeasurements.size()));
	documentSmokeLine.append(FromLatin1(" reused="));
	documentSmokeLine.append(YesNo(firstMeasurements == secondMeasurements));
	PrintLine(documentSmokeLine);

	const auto failureLabel = FromLatin1("generated-formula-cap.md");
	const auto failureParsed = ParseMarkdownForIv(
		QByteArray("$$\nE = mc^2\n$$\n"),
		ParseOptions{ failureLabel });
	Check(
		failureParsed.ok,
		failureLabel + FromLatin1(" parse failed: ") + failureParsed.error,
		ok);
	if (!failureParsed.ok) {
		return;
	}
	auto failureDimensions = CaptureMarkdownPrepareDimensions();
	failureDimensions.displayMathMaxRenderWidth = 1;
	auto failureRenderer = std::make_shared<MathRenderer>();
	auto failurePrepared = PrepareParsedDocumentForTest(
		failureParsed.document,
		failureLabel,
		failureRenderer,
		std::move(failureDimensions));
	Check(
		!failurePrepared.failure.failed(),
		failureLabel + FromLatin1(" terminal prepare failure"),
		ok);
	Check(
		failurePrepared.debug.formulaWarningCount > 0,
		failureLabel + FromLatin1(" formula warning count"),
		ok);
	auto failedFormulaFound = false;
	for (const auto &slot : failurePrepared.formulas) {
		if (!slot.present) {
			continue;
		}
		if (!slot.measured.success
			&& (slot.measured.tooLarge || slot.measured.overflow)
			&& !slot.measured.fallbackText.isEmpty()) {
			failedFormulaFound = true;
		}
	}
	Check(
		failedFormulaFound,
		failureLabel + FromLatin1(" formula cap fallback result"),
		ok);
	auto fallbackArticleHeight = 0;
	auto fallbackArticle = BuildArticleForTest(
		std::move(failurePrepared),
		failureRenderer,
		articleWidth,
		&fallbackArticleHeight);
	const auto fallbackImage = PaintArticleForTest(
		fallbackArticle.get(),
		articleWidth,
		fallbackArticleHeight);
	Check(
		HasPaintedPixels(fallbackImage),
		failureLabel + FromLatin1(" fallback paint produced pixels"),
		ok);
	const auto fallbackCounters = failureRenderer->debugCounters();
	Check(
		fallbackCounters.hits == 0
			&& fallbackCounters.misses == 0
			&& fallbackCounters.rendered == 0,
		failureLabel + FromLatin1(" fallback paint avoided raster renderer"),
		ok);

	const auto inlineFailureLabel = FromLatin1("generated-inline-formula-cap.md");
	const auto inlineFailureParsed = ParseMarkdownForIv(
		QByteArray("Inline fallback $\\frac{a}{b}$ text.\n"),
		ParseOptions{ inlineFailureLabel });
	Check(
		inlineFailureParsed.ok,
		inlineFailureLabel + FromLatin1(" parse failed: ")
			+ inlineFailureParsed.error,
		ok);
	if (!inlineFailureParsed.ok) {
		return;
	}
	auto inlineFailureDimensions = CaptureMarkdownPrepareDimensions();
	inlineFailureDimensions.displayMathMaxRenderWidth = 1;
	auto inlineFailureRenderer = std::make_shared<MathRenderer>();
	auto inlineFailurePrepared = PrepareParsedDocumentForTest(
		inlineFailureParsed.document,
		inlineFailureLabel,
		inlineFailureRenderer,
		std::move(inlineFailureDimensions));
	Check(
		!inlineFailurePrepared.failure.failed(),
		inlineFailureLabel + FromLatin1(" terminal prepare failure"),
		ok);
	auto inlineMeasuredFallbackText = QString();
	for (const auto &slot : inlineFailurePrepared.formulas) {
		if (!slot.present || (slot.kind != MathKind::Inline)) {
			continue;
		}
		if (!slot.measured.success
			&& (slot.measured.tooLarge || slot.measured.overflow)
			&& (slot.measured.logicalSize.width() > 0)
			&& !slot.measured.fallbackText.isEmpty()) {
			inlineMeasuredFallbackText = slot.measured.fallbackText;
			break;
		}
	}
	auto inlineDisplayedFallbackText = QString();
	auto inlineFailureObjectOffset = -1;
	ForEachPreparedBlock(
		inlineFailurePrepared.blocks.blocks,
		[&](const PreparedBlock &block) {
			if (!inlineDisplayedFallbackText.isEmpty()) {
				return;
			}
			for (const auto &match : CollectInlineTextObjectMatches(block.text)) {
				if (match.object.kind != InlineTextObjectKind::Formula) {
					continue;
				}
				const auto formula = std::get_if<InlineTextObjectFormulaData>(
					&match.object.data);
				if (!formula || formula->copySource.isEmpty()) {
					continue;
				}
				inlineDisplayedFallbackText = formula->copySource;
				inlineFailureObjectOffset = match.entity.offset();
				break;
			}
		});
	Check(
		!inlineMeasuredFallbackText.isEmpty(),
		inlineFailureLabel + FromLatin1(" inline fallback result"),
		ok);
	Check(
		!inlineDisplayedFallbackText.isEmpty(),
		inlineFailureLabel + FromLatin1(" inline displayed fallback text"),
		ok);
	Check(
		inlineFailureObjectOffset >= 0,
		inlineFailureLabel + FromLatin1(" inline object offset"),
		ok);
	Check(
		inlineDisplayedFallbackText != inlineMeasuredFallbackText,
		inlineFailureLabel + FromLatin1(
			" displayed fallback differs from trimmed fallback"),
		ok);
	if (!inlineFailurePrepared.failure.failed()
		&& !inlineDisplayedFallbackText.isEmpty()
		&& (inlineFailureObjectOffset >= 0)) {
		auto inlineFailureArticleHeight = 0;
		auto inlineFailureArticle = BuildArticleForTest(
			std::move(inlineFailurePrepared),
			inlineFailureRenderer,
			articleWidth,
			&inlineFailureArticleHeight);
		const auto inlineFailureImage = PaintArticleForTest(
			inlineFailureArticle.get(),
			articleWidth,
			inlineFailureArticleHeight);
		Check(
			HasPaintedPixels(inlineFailureImage),
			inlineFailureLabel + FromLatin1(" fallback paint produced pixels"),
			ok);
		const auto inlineFailureCounters = inlineFailureRenderer->debugCounters();
		Check(
			inlineFailureCounters.hits == 0
				&& inlineFailureCounters.misses == 0
				&& inlineFailureCounters.rendered == 0,
			inlineFailureLabel
				+ FromLatin1(" fallback paint avoided raster renderer"),
			ok);
		const auto inlineFailureHitBounds = SymbolHitBounds(
			inlineFailureArticle.get(),
			articleWidth,
			inlineFailureArticleHeight,
			inlineFailureObjectOffset);
		Check(
			inlineFailureHitBounds.has_value(),
			inlineFailureLabel + FromLatin1(" inline hit bounds"),
			ok);
		const auto inlineFailurePaintStrip = inlineFailureHitBounds
			? QRect(
				inlineFailureHitBounds->x(),
				0,
				inlineFailureHitBounds->width(),
				inlineFailureImage.height())
			: QRect();
		const auto inlineFailurePaintedBounds = inlineFailureHitBounds
			? PaintedBoundsInRect(inlineFailureImage, inlineFailurePaintStrip)
			: std::nullopt;
		Check(
			inlineFailurePaintedBounds.has_value(),
			inlineFailureLabel + FromLatin1(" fallback paint strip bounds"),
			ok);
		if (inlineFailureHitBounds && inlineFailurePaintedBounds) {
			Check(
				inlineFailurePaintedBounds->top()
					>= inlineFailureHitBounds->top()
					&& inlineFailurePaintedBounds->bottom()
						<= inlineFailureHitBounds->bottom(),
				inlineFailureLabel
					+ FromLatin1(" fallback paint stays within line bounds"),
				ok);
		}

		const auto inlinePlainLabel = FromLatin1(
			"generated-inline-formula-cap-plain.md");
		auto inlinePlainText = inlineDisplayedFallbackText;
		inlinePlainText.replace(u"$"_q, u"\\$"_q);
		const auto inlinePlainSource = QByteArray("Inline fallback ")
			+ inlinePlainText.toUtf8()
			+ QByteArray(" text.\n");
		const auto inlinePlainParsed = ParseMarkdownForIv(
			inlinePlainSource,
			ParseOptions{ inlinePlainLabel });
		Check(
			inlinePlainParsed.ok,
			inlinePlainLabel + FromLatin1(" parse failed: ")
				+ inlinePlainParsed.error,
			ok);
		if (inlinePlainParsed.ok) {
			auto inlinePlainRenderer = std::make_shared<MathRenderer>();
			auto inlinePlainPrepared = PrepareParsedDocumentForTest(
				inlinePlainParsed.document,
				inlinePlainLabel,
				inlinePlainRenderer);
			Check(
				!inlinePlainPrepared.failure.failed(),
				inlinePlainLabel + FromLatin1(" prepare failure: ")
					+ PrepareFailureReason(inlinePlainPrepared.failure),
				ok);
			if (!inlinePlainPrepared.failure.failed()) {
				auto inlinePlainArticleHeight = 0;
				auto inlinePlainArticle = BuildArticleForTest(
					std::move(inlinePlainPrepared),
					inlinePlainRenderer,
					articleWidth,
					&inlinePlainArticleHeight);
				const auto inlinePlainImage = PaintArticleForTest(
					inlinePlainArticle.get(),
					articleWidth,
					inlinePlainArticleHeight);
				Check(
					inlineFailureArticle->maxWidth()
						== inlinePlainArticle->maxWidth(),
					inlineFailureLabel
						+ FromLatin1(" fallback max width matches plain text"),
					ok);
				Check(
					inlineFailureArticleHeight == inlinePlainArticleHeight,
					inlineFailureLabel
						+ FromLatin1(" fallback height matches plain text"),
					ok);
				const auto inlinePlainPaintedBounds = inlineFailureHitBounds
					? PaintedBoundsInRect(inlinePlainImage, inlineFailurePaintStrip)
					: std::nullopt;
				Check(
					inlinePlainPaintedBounds.has_value(),
					inlinePlainLabel + FromLatin1(" fallback paint strip bounds"),
					ok);
				if (inlineFailurePaintedBounds && inlinePlainPaintedBounds) {
					Check(
						inlineFailurePaintedBounds->top()
							== inlinePlainPaintedBounds->top()
							&& inlineFailurePaintedBounds->bottom()
								== inlinePlainPaintedBounds->bottom(),
						inlineFailureLabel
							+ FromLatin1(
								" fallback paint vertical bounds match plain text"),
						ok);
				}
			}
		}
	}

	const auto &prepareLimits = PrepareLimitsForIv();
	auto blockLimitSource = QByteArray();
	for (auto i = 0; i != (prepareLimits.maxPreparedBlocks + 1); ++i) {
		blockLimitSource.append("Paragraph ");
		blockLimitSource.append(QByteArray::number(i));
		blockLimitSource.append("\n\n");
	}
	const auto blockLimitLabel = FromLatin1("generated-prepare-block-limit.md");
	const auto blockLimitParsed = ParseMarkdownForIv(
		blockLimitSource,
		ParseOptions{ blockLimitLabel });
	Check(
		blockLimitParsed.ok,
		blockLimitLabel + FromLatin1(" parse failed: ") + blockLimitParsed.error,
		ok);
	if (blockLimitParsed.ok) {
		const auto blockLimitPrepared = PrepareParsedDocumentForTest(
			blockLimitParsed.document,
			blockLimitLabel,
			std::make_shared<MathRenderer>());
		Check(
			blockLimitPrepared.failure.failed(),
			blockLimitLabel + FromLatin1(
				" missing real terminal prepare failure"),
			ok);
		Check(
			blockLimitPrepared.failure.terminal
				== PrepareTerminalFailure::DocumentTooLarge,
			blockLimitLabel + FromLatin1(
				" real terminal prepare failure kind"),
			ok);
		Check(
			PrepareFailureReason(blockLimitPrepared.failure)
				== FromLatin1("prepared-block-limit"),
			blockLimitLabel + FromLatin1(
				" real terminal prepare failure reason"),
			ok);
		Check(
			blockLimitPrepared.blocks.blocks.empty(),
			blockLimitLabel + FromLatin1(
				" real terminal prepare clears blocks"),
			ok);
	}
}

void CheckArticleHorizontalRelayoutRegression(bool *ok) {
	const auto label = FromLatin1("generated-horizontal-relayout-regression.md");
	const auto parsed = ParseMarkdownForIv(
		QByteArray(R"(# Horizontal resize regression heading that wraps when narrowed

This body paragraph also needs to reflow after a horizontal resize so the article must rebuild later block offsets instead of leaving them behind.

$$
\int_0^1 x^2 \, dx = \frac{1}{3}
$$

ThisIsALongUnbrokenStringToTestWrappingBehavior_ABCD1234EFGH5678IJKL
)"),
		ParseOptions{ label });
	Check(
		parsed.ok,
		label + FromLatin1(" parse failed: ") + parsed.error,
		ok);
	if (!parsed.ok) {
		return;
	}
	const auto &topLevelBlocks = parsed.document.document.children;
	Check(
		topLevelBlocks.size() == 4,
		label + FromLatin1(" parse top-level block count"),
		ok);
	if (topLevelBlocks.size() != 4) {
		return;
	}
	Check(
		topLevelBlocks[0].kind == NodeKind::Heading,
		label + FromLatin1(" parse heading block kind"),
		ok);
	Check(
		topLevelBlocks[1].kind == NodeKind::Paragraph,
		label + FromLatin1(" parse body paragraph block kind"),
		ok);
	Check(
		topLevelBlocks[2].kind == NodeKind::DisplayMath,
		label + FromLatin1(" parse display math block kind"),
		ok);
	Check(
		topLevelBlocks[3].kind == NodeKind::Paragraph,
		label + FromLatin1(" parse trailing paragraph block kind"),
		ok);
	auto renderer = std::make_shared<MathRenderer>();
	auto prepared = PrepareParsedDocumentForTest(
		parsed.document,
		label,
		renderer);
	Check(
		!prepared.failure.failed(),
		label + FromLatin1(" prepare failure: ")
			+ PrepareFailureReason(prepared.failure),
		ok);
	if (prepared.failure.failed()) {
		return;
	}
	Check(
		prepared.blocks.blocks.size() == 4,
		label + FromLatin1(" prepared top-level block count"),
		ok);
	if (prepared.blocks.blocks.size() != 4) {
		return;
	}
	const auto wideWidth = 640;
	const auto narrowWidth = 280;
	auto wideHeight = 0;
	auto article = BuildArticleForTest(
		std::move(prepared),
		renderer,
		wideWidth,
		&wideHeight);
	const auto wideImage = PaintArticleForTest(
		article.get(),
		wideWidth,
		wideHeight);
	Check(
		HasPaintedPixels(wideImage),
		label + FromLatin1(" wide paint produced pixels"),
		ok);
	const auto wideFinalBounds = SegmentHitBounds(
		article.get(),
		wideWidth,
		wideHeight,
		3);
	Check(
		wideFinalBounds.has_value(),
		label + FromLatin1(" wide final segment hit bounds"),
		ok);
	const auto narrowHeight = article->resizeGetHeight(narrowWidth);
	const auto narrowImage = PaintArticleForTest(
		article.get(),
		narrowWidth,
		narrowHeight);
	Check(
		HasPaintedPixels(narrowImage),
		label + FromLatin1(" narrow paint produced pixels"),
		ok);
	Check(
		narrowHeight > wideHeight,
		label + FromLatin1(" narrow relayout height grows"),
		ok);
	auto segmentBounds = std::vector<std::optional<QRect>>();
	segmentBounds.reserve(4);
	for (auto segmentIndex = 0; segmentIndex != 4; ++segmentIndex) {
		segmentBounds.push_back(SegmentHitBounds(
			article.get(),
			narrowWidth,
			narrowHeight,
			segmentIndex));
		Check(
			segmentBounds.back().has_value(),
			label + FromLatin1(" segment hit bounds ")
				+ QString::number(segmentIndex),
			ok);
	}
	auto haveAllSegmentBounds = true;
	for (const auto &bounds : segmentBounds) {
		if (!bounds.has_value()) {
			haveAllSegmentBounds = false;
			break;
		}
	}
	if (!haveAllSegmentBounds) {
		return;
	}
	for (auto segmentIndex = 1; segmentIndex != 4; ++segmentIndex) {
		const auto &previousBounds = *segmentBounds[segmentIndex - 1];
		const auto &currentBounds = *segmentBounds[segmentIndex];
		Check(
			currentBounds.top() > previousBounds.top(),
			label + FromLatin1(" segment document order ")
				+ QString::number(segmentIndex),
			ok);
		Check(
			currentBounds.top() > previousBounds.bottom(),
			label + FromLatin1(" segment vertical separation ")
				+ QString::number(segmentIndex),
			ok);
	}
	const auto &finalBounds = *segmentBounds.back();
	if (wideFinalBounds) {
		Check(
			finalBounds.height() > wideFinalBounds->height(),
			label + FromLatin1(" long final segment wraps when narrowed"),
			ok);
	}
	Check(
		finalBounds.height() > st::defaultMarkdown.body.lineHeight,
		label + FromLatin1(" long final segment spans multiple lines"),
		ok);
	auto lookupFlags = Ui::Text::StateRequest::Flags();
	lookupFlags |= Ui::Text::StateRequest::Flag::LookupSymbol;
	auto finalProbePoint = std::optional<QPoint>();
	for (auto y = finalBounds.top();
		(y <= finalBounds.bottom()) && !finalProbePoint;
		++y) {
		for (auto x = finalBounds.left(); x <= finalBounds.right(); ++x) {
			const auto hit = article->hitTest(QPoint(x, y), lookupFlags);
			if (hit.valid() && hit.direct && (hit.segmentIndex == 3)) {
				finalProbePoint = QPoint(x, y);
				break;
			}
		}
	}
	Check(
		finalProbePoint.has_value(),
		label + FromLatin1(" final segment direct probe point"),
		ok);
	if (!finalProbePoint) {
		return;
	}
	const auto finalHit = article->hitTest(*finalProbePoint, lookupFlags);
	Check(
		finalBounds.contains(*finalProbePoint),
		label + FromLatin1(" final probe point inside final segment bounds"),
		ok);
	Check(
		finalHit.valid()
			&& finalHit.direct
			&& (finalHit.segmentIndex == 3),
		label + FromLatin1(" final segment hit after relayout"),
		ok);
}

[[nodiscard]] int RunTests(int argc, char **argv) {
	auto args = ParseArgs(argc, argv);
	if (!args.ok) {
		PrintError(args.error);
		return 1;
	}
	if (args.inlineHtml) {
		auto ok = true;
		CheckInlineHtmlCoverage(args.dump, &ok);
		return ok ? 0 : 1;
	}
	if (args.markdownPath.isEmpty()) {
		args.markdownPath = DefaultFixturePath(FromLatin1("markdown-example.md"));
	}
	if (args.latexMarkdownPath.isEmpty()) {
		args.latexMarkdownPath = DefaultFixturePath(
			FromLatin1("latex-markdown-test.md"));
	}

	auto fixtureRenderer = std::make_shared<MathRenderer>();
	auto markdownFixture = PreparedFixture();
	if (!PrepareFixture(
			args.markdownPath,
			FromLatin1("markdown-example.md"),
			fixtureRenderer,
			&markdownFixture)) {
		return 1;
	}
	if (args.dump) {
		PrintLine(DumpForDebug(markdownFixture.parsed));
	}

	auto latexFixture = PreparedFixture();
	if (!PrepareFixture(
			args.latexMarkdownPath,
			FromLatin1("latex-markdown-test.md"),
			fixtureRenderer,
			&latexFixture)) {
		return 1;
	}
	if (args.dump) {
		PrintLine(DumpForDebug(latexFixture.parsed));
	}

	const auto &markdown = markdownFixture.parsed;
	const auto &latex = latexFixture.parsed;
	auto ok = true;
	Check(
		markdown.stats.cmarkNodeCount == 562,
		FromLatin1("markdown-example.md cmark node count"),
		&ok);
	Check(
		CountFormulas(markdown, MathKind::Inline) == 1,
		FromLatin1("markdown-example.md inline formula count"),
		&ok);
	Check(
		CountFormulas(markdown, MathKind::Display) == 1,
		FromLatin1("markdown-example.md display formula count"),
		&ok);
	Check(
		CountDisplayMathNodes(markdown.document) == 1,
		FromLatin1("markdown-example.md display math node count"),
		&ok);
	Check(
		FindNodeByKindAndRange(
			markdown.document,
			NodeKind::DisplayMath,
			281,
			1,
			283,
			2) != nullptr,
		FromLatin1("markdown-example.md display formula range"),
		&ok);
	Check(
		CountNodesByKindAndRange(
			markdown.document,
			NodeKind::Paragraph,
			281,
			1,
			283,
			2) == 0,
		FromLatin1("markdown-example.md duplicate display paragraph removed"),
		&ok);
	Check(
		HasKind(markdown.document, NodeKind::Table),
		FromLatin1("markdown-example.md table coverage"),
		&ok);
	Check(
		HasKind(markdown.document, NodeKind::Strike),
		FromLatin1("markdown-example.md strikethrough coverage"),
		&ok);
	Check(
		HasTaskState(markdown.document, TaskState::Checked),
		FromLatin1("markdown-example.md checked task"),
		&ok);
	Check(
		HasTaskState(markdown.document, TaskState::Unchecked),
		FromLatin1("markdown-example.md unchecked task"),
		&ok);
	auto markdownTables = std::vector<const MarkdownNode*>();
	CollectTables(markdown.document, &markdownTables);
	Check(
		int(markdownTables.size()) >= 2,
		FromLatin1("markdown-example.md table count"),
		&ok);
	if (markdownTables.size() >= 2) {
		const auto &firstTable = *markdownTables[0];
		const auto &secondTable = *markdownTables[1];
		Check(
			HasTableHeaderRow(firstTable),
			FromLatin1("markdown-example.md first table header row"),
			&ok);
		Check(
			TableHeaderRowCount(firstTable) == 1,
			FromLatin1("markdown-example.md first table header row count"),
			&ok);
		Check(
			TableColumnCount(firstTable) == 3,
			FromLatin1("markdown-example.md first table column count"),
			&ok);
		Check(
			HasSequentialTableColumns(firstTable),
			FromLatin1("markdown-example.md first table column order"),
			&ok);
		Check(
			HasTableAlignments(
				secondTable,
				{
					TableAlignment::Left,
					TableAlignment::Center,
					TableAlignment::Right,
				}),
			FromLatin1("markdown-example.md second table alignments"),
			&ok);
	}
	CheckFixtureSemanticCoverage(markdown, args.markdownPath, &ok);
	Check(
		latex.stats.cmarkNodeCount == 532,
		FromLatin1("latex-markdown-test.md cmark node count"),
		&ok);
	Check(
		CountFormulas(latex, MathKind::Inline) == 100,
		FromLatin1("latex-markdown-test.md inline formula count"),
		&ok);
	Check(
		CountFormulas(latex, MathKind::Display) == 31,
		FromLatin1("latex-markdown-test.md display formula count"),
		&ok);
	Check(
		CountDisplayMathNodes(latex.document) == 31,
		FromLatin1("latex-markdown-test.md display math node count"),
		&ok);
	Check(
		FindNodeByPathAndRange(
			latex.document,
			{
				NodeKind::Document,
				NodeKind::List,
				NodeKind::ListItem,
				NodeKind::DisplayMath,
			},
			299,
			4,
			301,
			5) != nullptr,
		FromLatin1("latex-markdown-test.md list display formula nested"),
		&ok);
	Check(
		FindNodeByPathAndRange(
			latex.document,
			{
				NodeKind::Document,
				NodeKind::DisplayMath,
			},
			299,
			4,
			301,
			5) == nullptr,
		FromLatin1("latex-markdown-test.md list display formula not hoisted"),
		&ok);
	Check(
		FindNodeByPathAndRange(
			latex.document,
			{
				NodeKind::Document,
				NodeKind::Blockquote,
				NodeKind::DisplayMath,
			},
			307,
			3,
			309,
			4) != nullptr,
		FromLatin1("latex-markdown-test.md blockquote display formula nested"),
		&ok);
	Check(
		FindNodeByPathAndRange(
			latex.document,
			{
				NodeKind::Document,
				NodeKind::DisplayMath,
			},
			307,
			3,
			309,
			4) == nullptr,
		FromLatin1("latex-markdown-test.md blockquote display formula not hoisted"),
		&ok);
	Check(
		HasKind(latex.document, NodeKind::Table),
		FromLatin1("latex-markdown-test.md table coverage"),
		&ok);
	auto latexTables = std::vector<const MarkdownNode*>();
	CollectTables(latex.document, &latexTables);
	Check(
		!latexTables.empty(),
		FromLatin1("latex-markdown-test.md table count"),
		&ok);
	if (!latexTables.empty()) {
		const auto &table = *latexTables[0];
		Check(
			HasTableHeaderRow(table),
			FromLatin1("latex-markdown-test.md table header row"),
			&ok);
		Check(
			TableColumnCount(table) == 3,
			FromLatin1("latex-markdown-test.md table column count"),
			&ok);
		Check(
			HasSequentialTableColumns(table),
			FromLatin1("latex-markdown-test.md table column order"),
			&ok);
	}
	const auto tableMath = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::Table,
		313,
		318);
	Check(
		tableMath != nullptr,
		FromLatin1("latex-markdown-test.md table range"),
		&ok);
	if (tableMath) {
		Check(
			!HasKind(*tableMath, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md table display math stays literal"),
			&ok);
		Check(
			!HasKind(*tableMath, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md table inline math stays literal"),
			&ok);
	}
	Check(
		CountFormulasInLineRange(latex, MathKind::Inline, 315, 318) == 12,
		FromLatin1("latex-markdown-test.md table inline formulas preserved"),
		&ok);
	Check(
		CountFormulasInLineRange(latex, MathKind::Display, 315, 318) == 0,
		FromLatin1("latex-markdown-test.md table display formulas absent"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 71, FromLatin1("\\int_0^1 x \\, dx")),
		FromLatin1("latex-markdown-test.md line 71 inline formula"),
		&ok);
	Check(
		HasFormulaOnLine(
			latex,
			259,
			FromLatin1(
				"\\mathbb{E}[X] = \\int_{-\\infty}^{\\infty} x f(x) \\, dx")),
		FromLatin1("latex-markdown-test.md line 259 inline formula"),
		&ok);
	const auto headerMath = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::Heading,
		322,
		322);
	Check(
		headerMath != nullptr,
		FromLatin1("latex-markdown-test.md heading range"),
		&ok);
	if (headerMath) {
		Check(
			!HasKind(*headerMath, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md heading display math stays inline"),
			&ok);
	}
	Check(
		HasFormulaOnLine(latex, 322, FromLatin1("ax^2 + bx + c = 0")),
		FromLatin1("latex-markdown-test.md line 322 inline formula"),
		&ok);
	const auto strongMath = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::Strong,
		326,
		326);
	Check(
		strongMath != nullptr,
		FromLatin1("latex-markdown-test.md strong range"),
		&ok);
	if (strongMath) {
		Check(
			!HasKind(*strongMath, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md strong display math stays inline"),
			&ok);
		Check(
			!HasKind(*strongMath, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md strong inline math stays literal"),
			&ok);
	}
	Check(
		HasFormulaOnLine(latex, 326, FromLatin1("E = mc^2")),
		FromLatin1("latex-markdown-test.md strong inline formula preserved"),
		&ok);
	const auto emphasisMath = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::Emphasis,
		328,
		328);
	Check(
		emphasisMath != nullptr,
		FromLatin1("latex-markdown-test.md emphasis range"),
		&ok);
	if (emphasisMath) {
		Check(
			!HasKind(*emphasisMath, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md emphasis display math stays inline"),
			&ok);
		Check(
			!HasKind(*emphasisMath, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md emphasis inline math stays literal"),
			&ok);
	}
	Check(
		HasFormulaOnLine(latex, 328, FromLatin1("\\pi \\approx 3.14")),
		FromLatin1("latex-markdown-test.md emphasis inline formula preserved"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 381, FromLatin1("a\\,b")),
		FromLatin1("latex-markdown-test.md line 381 inline formula"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 383, FromLatin1("a\\:b")),
		FromLatin1("latex-markdown-test.md line 383 inline formula"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 385, FromLatin1("a\\;b")),
		FromLatin1("latex-markdown-test.md line 385 inline formula"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 391, FromLatin1("a\\!b")),
		FromLatin1("latex-markdown-test.md line 391 inline formula"),
		&ok);
	const auto fencedCode = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::CodeBlock,
		332,
		334);
	Check(
		fencedCode != nullptr,
		FromLatin1("latex-markdown-test.md fenced code range"),
		&ok);
	if (fencedCode) {
		Check(
			!HasKind(*fencedCode, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md fenced code display math stays literal"),
			&ok);
		Check(
			!HasKind(*fencedCode, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md fenced code inline math stays literal"),
			&ok);
	}
	const auto inlineCode = FindNodeByKindAndLineRange(
		latex.document,
		NodeKind::InlineCode,
		336,
		336);
	Check(
		inlineCode != nullptr,
		FromLatin1("latex-markdown-test.md inline code range"),
		&ok);
	if (inlineCode) {
		Check(
			!HasKind(*inlineCode, NodeKind::DisplayMath),
			FromLatin1("latex-markdown-test.md inline code display math stays literal"),
			&ok);
		Check(
			!HasKind(*inlineCode, NodeKind::InlineMath),
			FromLatin1("latex-markdown-test.md inline code math stays literal"),
			&ok);
	}
	Check(
		CountFormulasInLineRange(latex, MathKind::Inline, 332, 336) == 0,
		FromLatin1("latex-markdown-test.md code inline formulas excluded"),
		&ok);
	Check(
		CountFormulasInLineRange(latex, MathKind::Display, 332, 336) == 0,
		FromLatin1("latex-markdown-test.md code display formulas excluded"),
		&ok);
	Check(
		!HasFormulaInLineRange(latex, 281, 281),
		FromLatin1("latex-markdown-test.md line 281 exclusion"),
		&ok);
	Check(
		HasFormulaOnLine(latex, 285, FromLatin1("5x + 3")),
		FromLatin1("latex-markdown-test.md line 285 formula"),
		&ok);
	Check(
		!HasFormulaInLineRange(latex, 332, 340),
		FromLatin1("latex-markdown-test.md lines 332-340 exclusions"),
		&ok);

	CheckInlineTextObjectPrepareCoverage(&ok);
	CheckNativeInstantViewPrepareCoverage(&ok);
	CheckPrepareCoverage(markdownFixture, latexFixture, &ok);
	CheckPrepareLinkClassification(markdownFixture.path, &ok);
	CheckArticleRenderSmoke(markdownFixture, latexFixture, &ok);
	CheckArticleHorizontalRelayoutRegression(&ok);
	CheckInlineTextObjectArticleCoverage(&ok);
	CheckNativeInstantViewArticleCoverage(&ok);
	CheckInlineHtmlCoverage(args.dump, &ok);
	CheckValidationEdges(&ok);

	return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	QCoreApplication::setAttribute(Qt::AA_Use96Dpi);
	auto application = QGuiApplication(argc, argv);
	(void)application;

	style::SetDevicePixelRatio(1);
	style::StartManager(style::kScaleDefault);
	const auto result = RunTests(argc, argv);
	style::StopManager();
	return result;
}

namespace crl {

rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}

} // namespace crl
