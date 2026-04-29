#include "iv/markdown/iv_markdown_controller.h"

#include "iv/markdown/iv_markdown_parse.h"
#include "iv/markdown/iv_markdown_view.h"
#include "iv/iv_delegate_impl.h"
#include "logs.h"
#include "ui/widgets/rp_window.h"

#include "styles/palette.h"
#include "styles/style_window.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace Iv::Markdown {
namespace {

constexpr auto kMaxSourceBytes = 4 * 1024 * 1024;

[[nodiscard]] bool IsReadableLocalFile(const QFileInfo &info) {
	return info.exists() && info.isFile() && info.isReadable();
}

[[nodiscard]] bool ReadLocalSource(const QString &path, QByteArray *bytes) {
	const auto info = QFileInfo(path);
	if (!IsReadableLocalFile(info)) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto data = file.readAll();
	if (file.error() != QFileDevice::NoError) {
		return false;
	}
	if (bytes) {
		*bytes = data;
	}
	return true;
}

[[nodiscard]] bool HasUtf8Bom(const QByteArray &source) {
	return source.size() >= 3
		&& static_cast<unsigned char>(source[0]) == 0xEF
		&& static_cast<unsigned char>(source[1]) == 0xBB
		&& static_cast<unsigned char>(source[2]) == 0xBF;
}

[[nodiscard]] unsigned char ByteAt(const QByteArray &source, int index) {
	return static_cast<unsigned char>(source[index]);
}

[[nodiscard]] bool IsAllowedControl(unsigned char byte) {
	return byte == '\t' || byte == '\n' || byte == '\r';
}

[[nodiscard]] bool LooksBinaryOrNonText(const QByteArray &source) {
	const auto normalized = HasUtf8Bom(source) ? source.mid(3) : source;
	const auto size = normalized.size();
	if (size == 0) {
		return false;
	}
	auto controlBytes = 0;
	for (auto i = 0; i != size; ++i) {
		const auto byte = ByteAt(normalized, i);
		if (byte == 0) {
			return true;
		} else if ((byte < 0x20 || byte == 0x7F)
			&& !IsAllowedControl(byte)) {
			++controlBytes;
		}
	}
	return (controlBytes * 10) > size;
}

[[nodiscard]] bool HasPreviewableContent(const MarkdownNode &node) {
	switch (node.kind) {
	case NodeKind::Document:
	case NodeKind::Unsupported:
		break;
	default:
		return true;
	}
	return std::any_of(
		node.children.begin(),
		node.children.end(),
		[](const MarkdownNode &child) {
			return HasPreviewableContent(child);
		});
}

[[nodiscard]] bool AcceptsPreview(const PreparedDocument &document) {
	return HasPreviewableContent(document.document)
		|| !document.formulas.empty();
}

class Controller final {
public:
	Controller(PreparedDocument document, QString title);

	void activate();

private:
	void close();
	void createWindow();
	void finishClose();

	PreparedDocument _document;
	QString _title;
	Iv::DelegateImpl _delegate;
	std::unique_ptr<Ui::RpWindow> _window;
	std::unique_ptr<Ui::RpWidget> _preview;
	bool _closing = false;

};

[[nodiscard]] auto &ActiveControllers() {
	static auto controllers = std::vector<std::unique_ptr<Controller>>();
	return controllers;
}

void RemoveController(Controller *controller) {
	auto &active = ActiveControllers();
	const auto i = std::find_if(
		active.begin(),
		active.end(),
		[=](const std::unique_ptr<Controller> &value) {
			return value.get() == controller;
		});
	if (i != active.end()) {
		active.erase(i);
	}
}

void OpenDocumentWindow(PreparedDocument document, QString title) {
	auto controller = std::make_unique<Controller>(
		std::move(document),
		std::move(title));
	const auto raw = controller.get();
	ActiveControllers().push_back(std::move(controller));
	raw->activate();
}

Controller::Controller(PreparedDocument document, QString title)
: _document(std::move(document))
, _title(std::move(title)) {
	createWindow();
}

void Controller::activate() {
	if (_window->isMinimized()) {
		_window->showNormal();
	} else if (_window->isHidden()) {
		_window->show();
	}
	_window->raise();
	_window->activateWindow();
	_window->setFocus();
	if (_preview) {
		_preview->setFocus();
	}
}

void Controller::close() {
	if (_window) {
		_window->close();
	}
}

void Controller::createWindow() {
	_window = std::make_unique<Ui::RpWindow>();
	const auto window = _window.get();
	window->setTitle(_title);
	window->setWindowTitle(_title);
	window->setGeometry(_delegate.ivGeometry(window));
	window->setMinimumSize({ st::windowMinWidth, st::windowMinHeight });
	window->geometryValue(
	) | rpl::distinct_until_changed(
	) | rpl::skip(1) | rpl::on_next([=] {
		_delegate.ivSaveGeometry(window);
	}, window->lifetime());
	window->body()->paintRequest() | rpl::on_next([=](QRect clip) {
		QPainter(window->body().get()).fillRect(clip, st::windowBg);
	}, window->body()->lifetime());

	_preview = CreateMarkdownPreviewWidget(
		_document,
		OpenOptions{ .sourceName = _title });
	_preview->setParent(window->body().get());
	_preview->setGeometry(QRect(QPoint(), window->body()->size()));
	window->body()->sizeValue() | rpl::on_next([=](QSize size) {
		_preview->setGeometry(QRect(QPoint(), size));
	}, _preview->lifetime());
	_preview->show();

	window->events() | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Close) {
			finishClose();
		} else if (e->type() == QEvent::KeyPress) {
			const auto event = static_cast<QKeyEvent*>(e.get());
			if (event->key() == Qt::Key_Escape) {
				event->accept();
				close();
			}
		}
	}, window->lifetime());

	window->show();
}

void Controller::finishClose() {
	if (_closing) {
		return;
	}
	_closing = true;
	crl::on_main([controller = this] {
		RemoveController(controller);
	});
}

} // namespace

bool TryOpenLocalFile(const QString &path) {
	const auto info = QFileInfo(path);
	if (!IsReadableLocalFile(info)) {
		return false;
	}
	if (info.size() > kMaxSourceBytes) {
		DEBUG_LOG(("Native Markdown IV: rejected local file too large: %1"
			).arg(path));
		return false;
	}

	auto bytes = QByteArray();
	if (!ReadLocalSource(path, &bytes)) {
		return false;
	}
	if (bytes.size() > kMaxSourceBytes) {
		DEBUG_LOG(("Native Markdown IV: rejected local file too large: %1"
			).arg(path));
		return false;
	}
	if (LooksBinaryOrNonText(bytes)) {
		DEBUG_LOG(("Native Markdown IV: rejected local file as binary/non-text: %1"
			).arg(path));
		return false;
	}

	const auto title = info.fileName();
	auto result = ParseMarkdownForIv(bytes, ParseOptions{ title });
	if (!result.ok) {
		const auto &error = result.error;
		if (error.startsWith(u"cmark-"_q)) {
			DEBUG_LOG(("Native Markdown IV: cmark parse failure (%1): %2"
				).arg(error
				).arg(path));
		} else {
			DEBUG_LOG(("Native Markdown IV: parse failure (%1): %2"
				).arg(error
				).arg(path));
		}
		return false;
	}
	if (!AcceptsPreview(result.document)) {
		DEBUG_LOG(("Native Markdown IV: unsupported or empty document: %1"
			).arg(path));
		return false;
	}

	OpenDocumentWindow(std::move(result.document), title);
	DEBUG_LOG(("Native Markdown IV: opened as native Markdown IV: %1"
		).arg(path));
	return true;
}

} // namespace Iv::Markdown
