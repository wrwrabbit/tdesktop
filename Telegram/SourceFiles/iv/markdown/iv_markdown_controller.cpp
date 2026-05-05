#include "iv/markdown/iv_markdown_controller.h"

#include "iv/markdown/iv_markdown_parse.h"
#include "iv/markdown/iv_markdown_view.h"
#include "iv/iv_delegate_impl.h"
#include "logs.h"
#include "ui/widgets/rp_window.h"

#include "styles/palette.h"
#include "styles/style_window.h"

#include <QtCore/QElapsedTimer>
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

constexpr auto kZoomStep = int(10);

[[nodiscard]] OpenOptions PrepareOpenOptions(
		OpenOptions options,
		not_null<Delegate*> delegate,
		const QString &title) {
	options.delegate = delegate;
	if (options.sourceName.isEmpty()) {
		options.sourceName = title;
	}
	return options;
}

struct OpenTarget {
	QString path;
	QString fragment;
};

[[nodiscard]] bool IsReadableLocalFile(const QFileInfo &info) {
	return info.exists() && info.isFile() && info.isReadable();
}

struct ReadSource {
	QString path;
	QString name;
	QByteArray bytes;

	explicit operator bool() const {
		return !path.isEmpty();
	}
};
[[nodiscard]] ReadSource ReadLocalSource(
		const QString &path,
		const MarkdownParseLimits &limits) {
	const auto info = QFileInfo(path);
	auto name = info.fileName();
	if (!IsReadableLocalFile(info) || !LooksLikeMarkdownFile(name)) {
		return {};
	} else if (info.size() > limits.maxSourceBytes) {
		DEBUG_LOG(("Native Markdown IV: rejected local file too large: %1"
			).arg(path));
		return {};
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	auto data = file.read(limits.maxSourceBytes);
	if (file.error() != QFileDevice::NoError || !file.atEnd()) {
		DEBUG_LOG(("Native Markdown IV: could not read local file: %1"
			).arg(path));
		return {};
	}
	return {
		.path = info.absoluteFilePath(),
		.name = std::move(name),
		.bytes = std::move(data),
	};
}

[[nodiscard]] QString NormalizeFragmentId(QString fragment) {
	fragment = QString::fromUtf8(
		QByteArray::fromPercentEncoding(fragment.toUtf8()));
	fragment = fragment.trimmed().toLower();
	while (fragment.startsWith(QChar('#'))) {
		fragment.remove(0, 1);
	}
	return fragment;
}

[[nodiscard]] OpenTarget ParseOpenTarget(QString path) {
	const auto direct = QFileInfo(path);
	if (direct.exists()) {
		return { path, QString() };
	}
	const auto hash = path.lastIndexOf(QChar('#'));
	if (hash <= 0) {
		return { path, QString() };
	}
	const auto candidate = path.mid(0, hash);
	if (candidate.isEmpty()) {
		return { path, QString() };
	}
	const auto info = QFileInfo(candidate);
	return info.exists()
		? OpenTarget{ candidate, NormalizeFragmentId(path.mid(hash + 1)) }
		: OpenTarget{ path, QString() };
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

void LogDocumentWarnings(
		const PreparedDocument &document,
		const QString &path) {
	for (const auto &warning : document.warnings) {
		DEBUG_LOG(("Native Markdown IV: warning (%1): %2"
			).arg(warning
			).arg(path));
	}
}

} // namespace

Controller::Controller(
	not_null<Delegate*> delegate,
	PreparedDocument document,
	QString title,
	OpenOptions options)
: _delegate(delegate)
, _document(std::make_shared<PreparedDocument>(std::move(document)))
, _title(std::move(title))
, _renderer(nullptr)
, _options(PrepareOpenOptions(std::move(options), delegate, _title)) {
	createWindow();
}

Controller::Controller(
	not_null<Delegate*> delegate,
	MarkdownArticleContent content,
	QString title,
	std::shared_ptr<MathRenderer> renderer,
	OpenOptions options)
: _delegate(delegate)
, _preparedContent(std::move(content))
, _title(std::move(title))
, _renderer(renderer ? std::move(renderer) : std::make_shared<MathRenderer>())
, _options(PrepareOpenOptions(std::move(options), delegate, _title)) {
	createWindow();
}

Controller::~Controller() = default;

rpl::lifetime &Controller::lifetime() {
	return _lifetime;
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

void Controller::update(
		MarkdownArticleContent content,
		QString title,
		OpenOptions options) {
	_preparedContent = std::move(content);
	_title = std::move(title);
	_options = PrepareOpenOptions(std::move(options), _delegate, _title);
	if (_window) {
		_window->setTitle(_title);
		_window->setWindowTitle(_title);
	}
	createPreview();
	if (_window && _window->isActiveWindow() && _preview) {
		_preview->setFocus();
	}
}

bool Controller::active() const {
	return _window && _window->isActiveWindow();
}

void Controller::minimize() {
	if (_window) {
		_window->setWindowState(_window->windowState() | Qt::WindowMinimized);
	}
}

void Controller::close() {
	_events.fire({ Event::Type::Close });
}

void Controller::createPreview() {
	if (!_window) {
		return;
	}
	const auto parent = _window->body();
	const auto callback = [=](Event event) {
		_events.fire(std::move(event));
	};
	_preview = nullptr;
	_preview = _preparedContent
		? CreateMarkdownPreviewWidget(
			parent,
			std::move(*_preparedContent),
			_renderer,
			callback,
			_options)
		: CreateMarkdownPreviewWidget(
			parent,
			*_document,
			callback,
			_options);
	_preparedContent.reset();
	_preview->setGeometry(parent->rect());
	parent->sizeValue() | rpl::on_next([=](QSize size) {
		_preview->resize(size);
	}, _preview->lifetime());
	_preview->show();
}

void Controller::createWindow() {
	_window = std::make_unique<Ui::RpWindow>();
	const auto window = _window.get();
	window->setTitle(_title);
	window->setWindowTitle(_title);
	window->setGeometry(_delegate->ivGeometry(window));
	window->setMinimumSize({ st::windowMinWidth, st::windowMinHeight });
	window->geometryValue(
	) | rpl::distinct_until_changed(
	) | rpl::skip(1) | rpl::on_next([=] {
		_delegate->ivSaveGeometry(window);
	}, window->lifetime());
	window->body()->paintRequest() | rpl::on_next([=](QRect clip) {
		QPainter(window->body().get()).fillRect(clip, st::windowBg);
	}, window->body()->lifetime());

	createPreview();

	window->events() | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Close) {
			close();
		} else if (e->type() == QEvent::KeyPress) {
			const auto event = static_cast<QKeyEvent*>(e.get());
			if (event->modifiers() & Qt::ControlModifier) {
				if (event->key() == Qt::Key_Plus
					|| event->key() == Qt::Key_Equal) {
					event->accept();
					_delegate->ivSetZoom(_delegate->ivZoom() + kZoomStep);
					return;
				} else if (event->key() == Qt::Key_Minus) {
					event->accept();
					_delegate->ivSetZoom(_delegate->ivZoom() - kZoomStep);
					return;
				} else if (event->key() == Qt::Key_0) {
					event->accept();
					_delegate->ivSetZoom(0);
					return;
				}
			}
			if (event->key() == Qt::Key_Escape) {
				event->accept();
				close();
			}
		}
	}, window->lifetime());

	window->show();
}

std::unique_ptr<Controller> TryOpenLocalFile(
		not_null<Delegate*> delegate,
		const QString &path) {
	const auto &limits = ParseLimitsForIv();
	const auto target = ParseOpenTarget(path);

	const auto source = ReadLocalSource(target.path, limits);
	if (!source) {
		return nullptr;
	}
	const auto &bytes = source.bytes;
	const auto &fallbackTitle = source.name;

	const auto start = crl::now();
	auto validateResult = ValidateMarkdownSourceForIv(
		bytes,
		ParseOptions{ fallbackTitle });
	const auto validated = crl::now();
	if (!validateResult.ok) {
		DEBUG_LOG(("Native Markdown IV: "
			"source validation failure (%1, %2 ms): %3"
			).arg(validateResult.error
			).arg(validated - start
			).arg(target.path));
		return nullptr;
	}

	auto parseResult = ParseMarkdownForIv(std::move(validateResult.source));
	const auto parsed = crl::now();
	if (!parseResult.ok) {
		const auto &error = parseResult.error;
		if (error.startsWith(u"cmark-"_q)) {
			DEBUG_LOG(("Native Markdown IV: "
				"cmark parse failure (%1, %2 ms): %3"
				).arg(error
				).arg(parsed - validated
				).arg(target.path));
		} else {
			DEBUG_LOG(("Native Markdown IV: parse failure (%1, %2 ms): %3"
				).arg(error
				).arg(parsed - validated
				).arg(target.path));
		}
		return nullptr;
	} else if (!AcceptsPreview(parseResult.document)) {
		DEBUG_LOG(("Native Markdown IV: "
			"unsupported or empty document (%1 ms): %2"
			).arg(crl::now() - parsed
			).arg(target.path));
		return nullptr;
	}
	LogDocumentWarnings(parseResult.document, target.path);

	DEBUG_LOG(("Native Markdown IV: "
		"opening as native Markdown IV (%1 ms validate, %2 ms parse): %3"
		).arg(validated - start
		).arg(parsed - validated
		).arg(target.path));

	auto options = OpenOptions();
	options.sourcePath = std::move(source.path);
	options.initialFragment = std::move(target.fragment);
	return std::make_unique<Controller>(
		delegate,
		std::move(parseResult.document),
		(parseResult.document.title.trimmed().isEmpty()
			? fallbackTitle
			: parseResult.document.title.trimmed()),
		std::move(options));
}

} // namespace Iv::Markdown
