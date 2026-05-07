/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_controller.h"
#include "base/event_filter.h"
#include "base/weak_ptr.h"
#include "core/click_handler_types.h"
#include "core/credits_amount.h"
#include "core/file_utilities.h"
#include "iv/markdown/iv_markdown_parse.h"
#include "iv/markdown/iv_markdown_view.h"
#include "iv/iv_delegate_impl.h"
#include "lang/lang_keys.h"
#include "ui/layers/layer_manager.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/rp_window.h"
#include "ui/wrap/fade_wrap.h"
#include "logs.h"

#include "styles/palette.h"
#include "styles/style_iv.h"
#include "styles/style_menu_icons.h"
#include "styles/style_window.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
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
	Q_UNUSED(title);
	return options;
}

[[nodiscard]] ViewerKind ResolveViewerKind(const OpenOptions &options) {
	return (options.viewerKind != ViewerKind::Auto)
		? options.viewerKind
		: options.sourcePath.isEmpty()
		? ViewerKind::InstantView
		: ViewerKind::LocalFile;
}

[[nodiscard]] QString SubtitleText(
		const OpenOptions &options,
		const QString &title) {
	if (!options.sourceName.trimmed().isEmpty()) {
		return options.sourceName.trimmed();
	}
	const auto host = QUrl(options.sourceUrl).host().trimmed();
	return !host.isEmpty() ? host : title.trimmed();
}

[[nodiscard]] QString OpenSourceLabel(ViewerKind kind) {
	return (kind == ViewerKind::InstantView)
		? tr::lng_iv_open_in_browser(tr::now)
		: tr::lng_markdown_preview_open_file(tr::now);
}

[[nodiscard]] const style::icon *OpenSourceIcon(ViewerKind kind) {
	return (kind == ViewerKind::InstantView)
		? &st::menuIconIpAddress
		: &st::menuIconFile;
}

[[nodiscard]] QVariant ExtendClickHandlerContext(
		QVariant context,
		const std::shared_ptr<Ui::Show> &show) {
	if (!show) {
		return context;
	} else if (!context.isValid()
		|| context.canConvert<ClickHandlerContext>()) {
		auto clickContext = context.isValid()
			? context.value<ClickHandlerContext>()
			: ClickHandlerContext();
		clickContext.show = show;
		return QVariant::fromValue(clickContext);
	}
	return context;
}

[[nodiscard]] std::shared_ptr<QVariant> ResolveClickHandlerContextRef(
		const std::shared_ptr<QVariant> &current,
		const OpenOptions &options) {
	return options.clickHandlerContextRef
		? options.clickHandlerContextRef
		: (current ? current : std::make_shared<QVariant>());
}

void ProcessZoomShortcut(not_null<Delegate*> delegate, QKeyEvent *event) {
	if (!(event->modifiers() & Qt::ControlModifier)) {
		return;
	}
	if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
		event->accept();
		delegate->ivSetZoom(delegate->ivZoom() + kZoomStep);
	} else if (event->key() == Qt::Key_Minus) {
		event->accept();
		delegate->ivSetZoom(delegate->ivZoom() - kZoomStep);
	} else if (event->key() == Qt::Key_0) {
		event->accept();
		delegate->ivSetZoom(0);
	}
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
	_clickHandlerContextRef = ResolveClickHandlerContextRef(
		_clickHandlerContextRef,
		_options);
	_options.clickHandlerContextRef = _clickHandlerContextRef;
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
	_clickHandlerContextRef = ResolveClickHandlerContextRef(
		_clickHandlerContextRef,
		_options);
	_options.clickHandlerContextRef = _clickHandlerContextRef;
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
	_clickHandlerContextRef = ResolveClickHandlerContextRef(
		_clickHandlerContextRef,
		_options);
	_options.clickHandlerContextRef = _clickHandlerContextRef;
	if (_menu) {
		_menu = nullptr;
		_menuToggle->setForceRippled(false);
	}
	refreshTitle();
	createPreview();
	if (_window && _window->isActiveWindow() && _preview) {
		_preview->setFocus();
	}
}

void Controller::updateOptions(OpenOptions options) {
	const auto initialFragment = options.initialFragment;
	auto refreshed = PrepareOpenOptions(std::move(options), _delegate, _title);
	if (refreshed.sourceName.isEmpty()) {
		refreshed.sourceName = _options.sourceName;
	}
	if (refreshed.sourcePath.isEmpty()) {
		refreshed.sourcePath = _options.sourcePath;
	}
	if (refreshed.sourceUrl.isEmpty()) {
		refreshed.sourceUrl = _options.sourceUrl;
	}
	if (refreshed.viewerKind == ViewerKind::Auto) {
		refreshed.viewerKind = _options.viewerKind;
	}
	if (!refreshed.openSource) {
		refreshed.openSource = _options.openSource;
	}
	if (!refreshed.activateMedia) {
		refreshed.activateMedia = _options.activateMedia;
	}
	_options = std::move(refreshed);
	if (!_clickHandlerContextRef) {
		_clickHandlerContextRef = std::make_shared<QVariant>();
	}
	_options.clickHandlerContextRef = _clickHandlerContextRef;
	if (_clickHandlerContextRef) {
		*_clickHandlerContextRef = ExtendClickHandlerContext(
			_options.clickHandlerContext,
			_show);
	}
	if (_menu) {
		_menu = nullptr;
		_menuToggle->setForceRippled(false);
	}
	refreshTitle();
	if (!initialFragment.isEmpty() && _preview) {
		const auto scrolled = ScrollMarkdownPreviewToAnchor(
			_preview.get(),
			initialFragment);
		static_cast<void>(scrolled);
	}
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

ViewerKind Controller::viewerKind() const {
	return ResolveViewerKind(_options);
}

QString Controller::subtitleText() const {
	return SubtitleText(_options, _title);
}

bool Controller::canOpenSource() const {
	if (_options.openSource) {
		return true;
	}
	return (viewerKind() == ViewerKind::InstantView)
		? !_options.sourceUrl.isEmpty()
		: !_options.sourcePath.isEmpty();
}

bool Controller::canShare() const {
	return static_cast<bool>(_options.share);
}

void Controller::refreshTitle() {
	if (_window) {
		_window->setTitle(_title);
		_window->setWindowTitle(_title);
	}
	if (_subtitle) {
		_subtitle->setText(subtitleText());
		updateTitleGeometry(_window->body()->width());
	}
}

void Controller::updateTitleGeometry(int newWidth) const {
	_subtitleWrap->setGeometry(0, 0, newWidth, st::ivSubtitleHeight);
	_subtitle->resizeToWidth(newWidth
		- st::ivSubtitleLeft
		- _menuToggle->width());
	_subtitle->moveToLeft(st::ivSubtitleLeft, st::ivSubtitleTop);
	_menuToggle->moveToRight(0, 0);
	if (_titleShadow) {
		_titleShadow->resizeToWidth(newWidth);
		_titleShadow->move(0, st::ivSubtitleHeight);
	}
}

void Controller::openSource() {
	if (_options.openSource) {
		_options.openSource();
	} else if (viewerKind() == ViewerKind::InstantView) {
		File::OpenUrl(_options.sourceUrl);
	} else {
		File::Launch(_options.sourcePath);
	}
}

void Controller::showMenu() {
	if (!_window || _menu) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		_window.get(),
		st::popupMenuWithIcons);
	_menu->setDestroyedCallback(crl::guard(_window.get(), [
			this,
			menu = _menu.get()] {
		if (_menu == menu) {
			_menuToggle->setForceRippled(false);
		}
	}));
	_menuToggle->setForceRippled(true);

	const auto action = _menu->addAction(
		OpenSourceLabel(viewerKind()),
		crl::guard(_window.get(), [=] {
			openSource();
		}),
		OpenSourceIcon(viewerKind()));
	action->setEnabled(canOpenSource());

	if (canShare()) {
		_menu->addAction(
			tr::lng_iv_share(tr::now),
			crl::guard(_window.get(), [=, share = _options.share] {
				share(_show);
			}),
			&st::menuIconShare);
	}

	_menu->setForcedOrigin(Ui::PanelAnimation::Origin::TopRight);
	_menu->popup(_window->body()->mapToGlobal(
		QPoint(_window->body()->width(), 0) + st::ivMenuPosition));
}

void Controller::createLayerManager() {
	if (!_window || _layerManager) {
		return;
	}
	_layerManager = std::make_unique<Ui::LayerManager>(
		not_null{ _window->body().get() });
	_layerManager->setHideByBackgroundClick(false);
	_show = _layerManager->uiShow();
}

void Controller::createPreview() {
	if (!_container) {
		return;
	}
	const auto parent = _container;
	auto options = _options;
	options.clickHandlerContextRef = _clickHandlerContextRef;
	options.clickHandlerContext = ExtendClickHandlerContext(
		std::move(options.clickHandlerContext),
		_show);
	if (options.clickHandlerContextRef) {
		*options.clickHandlerContextRef = options.clickHandlerContext;
	}
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
			options)
		: CreateMarkdownPreviewWidget(
			parent,
			*_document,
			callback,
			options);
	_preparedContent.reset();
	_preview->setGeometry(parent->rect());
	parent->sizeValue() | rpl::on_next([=](QSize size) {
		_preview->resize(size);
	}, _preview->lifetime());
	if (_titleShadow) {
		MarkdownPreviewScrollTopValue(
			_preview.get()
		) | rpl::on_next([=](int scrollTop) {
			_titleShadow->toggle(
				(scrollTop > 0),
				anim::type::normal);
		}, _preview->lifetime());
	}
	_preview->show();
}

void Controller::createWindow() {
	_window = std::make_unique<Ui::RpWindow>();
	const auto window = _window.get();
	_subtitleWrap = std::make_unique<Ui::RpWidget>(window->body().get());
	_subtitle = std::make_unique<Ui::FlatLabel>(
		_subtitleWrap.get(),
		subtitleText(),
		st::ivSubtitle);
	_subtitle->setSelectable(true);
	_menuToggle.create(_subtitleWrap.get(), st::ivMenuToggle);
	_menuToggle->setClickedCallback([=] {
		showMenu();
	});
	_subtitleWrap->paintRequest() | rpl::on_next([=](QRect clip) {
		QPainter(_subtitleWrap.get()).fillRect(clip, st::windowBg);
	}, _subtitleWrap->lifetime());
	window->body()->widthValue() | rpl::on_next([=](int width) {
		updateTitleGeometry(width);
	}, _subtitle->lifetime());
	window->setTitle(_title);
	window->setWindowTitle(_title);
	window->setGeometry(_delegate->ivGeometry(window));
	window->setMinimumSize({ st::ivWidthMin, st::ivHeightMin });
	window->geometryValue(
	) | rpl::distinct_until_changed(
	) | rpl::skip(1) | rpl::on_next([=] {
		_delegate->ivSaveGeometry(window);
	}, window->lifetime());
	window->body()->paintRequest() | rpl::on_next([=](QRect clip) {
		QPainter(window->body().get()).fillRect(clip, st::windowBg);
	}, window->body()->lifetime());
	_container = Ui::CreateChild<Ui::RpWidget>(window->body().get());
	rpl::combine(
		window->body()->sizeValue(),
		_subtitleWrap->heightValue()
	) | rpl::on_next([=](QSize size, int titleHeight) {
		_container->setGeometry(QRect(QPoint(), size).marginsRemoved(
			{ 0, titleHeight, 0, 0 }));
	}, _container->lifetime());
	_container->paintRequest() | rpl::on_next([=](QRect clip) {
		QPainter(_container).fillRect(clip, st::windowBg);
	}, _container->lifetime());
	_titleShadow.create(window->body().get());
	updateTitleGeometry(window->body()->width());

	createLayerManager();
	createPreview();
	_container->show();

	window->events() | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Close) {
			close();
		} else if (e->type() == QEvent::KeyPress) {
			const auto event = static_cast<QKeyEvent*>(e.get());
			if (event->key() == Qt::Key_Escape) {
				event->accept();
				close();
			}
		}
	}, window->lifetime());
	base::install_event_filter(window, qApp, [=](not_null<QEvent*> e) {
		if (e->type() != QEvent::ShortcutOverride || !window->isActiveWindow()) {
			return base::EventFilterResult::Continue;
		}
		const auto event = static_cast<QKeyEvent*>(e.get());
		const auto previousAccepted = event->isAccepted();
		ProcessZoomShortcut(_delegate, event);
		return event->isAccepted() && !previousAccepted
			? base::EventFilterResult::Cancel
			: base::EventFilterResult::Continue;
	});

	window->show();
}

std::unique_ptr<Controller> TryOpenLocalFile(
		not_null<Delegate*> delegate,
		const QString &path,
		OpenOptions options) {
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

	if (options.sourceName.isEmpty()) {
		options.sourceName = source.name;
	}
	options.sourcePath = std::move(source.path);
	options.initialFragment = std::move(target.fragment);
	if (options.viewerKind == ViewerKind::Auto) {
		options.viewerKind = ViewerKind::LocalFile;
	}
	return std::make_unique<Controller>(
		delegate,
		std::move(parseResult.document),
		(parseResult.document.title.trimmed().isEmpty()
			? fallbackTitle
			: parseResult.document.title.trimmed()),
		std::move(options));
}

} // namespace Iv::Markdown
