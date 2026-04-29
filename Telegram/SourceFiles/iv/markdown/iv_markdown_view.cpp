#include "iv/markdown/iv_markdown_view.h"

#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/rp_widget.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QStringList>

namespace Iv::Markdown {
namespace {

constexpr auto kDiagnosticDumpLineCount = 20;

[[nodiscard]] QString BuildDiagnosticText(
		const PreparedDocument &document,
		const OpenOptions &options) {
	auto lines = QStringList();
	const auto sourceName = options.sourceName.isEmpty()
		? document.sourceName
		: options.sourceName;
	lines.append(QStringLiteral("Source: %1").arg(sourceName));
	if (!document.title.isEmpty()) {
		lines.append(QStringLiteral("First heading: %1").arg(document.title));
	}
	lines.append(QStringLiteral("Top-level blocks: %1").arg(
		static_cast<qlonglong>(document.document.children.size())));
	lines.append(QStringLiteral("Formulas: %1").arg(
		static_cast<qlonglong>(document.formulas.size())));
	lines.append(QStringLiteral("Warnings: %1").arg(
		static_cast<qlonglong>(document.warnings.size())));
	lines.append(QString());
	lines.append(QStringLiteral("Debug dump:"));

	const auto dumpLines = DumpForDebug(document).split('\n');
	const auto shown = (dumpLines.size() < qsizetype(kDiagnosticDumpLineCount))
		? dumpLines.size()
		: qsizetype(kDiagnosticDumpLineCount);
	for (auto i = qsizetype(0); i != shown; ++i) {
		lines.append(dumpLines[i]);
	}
	if (dumpLines.size() > shown) {
		lines.append(QStringLiteral("..."));
	}
	return lines.join('\n');
}

} // namespace

std::unique_ptr<Ui::RpWidget> CreateMarkdownPreviewWidget(
	const PreparedDocument &document,
	const OpenOptions &options) {
	auto root = std::make_unique<Ui::RpWidget>();
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
		root.get(),
		st::boxScroll);
	auto label = object_ptr<Ui::FlatLabel>(
		scroll,
		BuildDiagnosticText(document, options),
		st::aboutLabel);
	const auto rawLabel = label.data();
	rawLabel->setSelectable(true);
	rawLabel->setBreakEverywhere(true);
	const auto wrap = scroll->setOwnedWidget(
		object_ptr<Ui::PaddingWrap<Ui::FlatLabel>>(
			scroll,
			std::move(label),
			st::boxPadding));

	root->sizeValue() | rpl::on_next([=](QSize size) {
		scroll->setGeometry(QRect(QPoint(), size));
		if (wrap) {
			wrap->resizeToWidth(scroll->width());
		}
	}, root->lifetime());

	scroll->show();
	if (wrap) {
		wrap->show();
	}
	rawLabel->show();
	return root;
}

} // namespace Iv::Markdown
