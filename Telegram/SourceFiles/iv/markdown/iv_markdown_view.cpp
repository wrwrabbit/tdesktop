/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/markdown/iv_markdown_view.h"
#include "base/weak_ptr.h"
#include "core/click_handler_types.h"
#include "core/credits_amount.h"
#include "core/file_utilities.h"
#include "iv/markdown/iv_markdown_article_text.h"
#include "iv/markdown/iv_markdown_view_widget.h"
#include "iv/iv_delegate.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_manager.h"
#include "ui/style/style_core_scale.h"
#include "ui/text/text.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/integration.h"
#include "ui/rect.h"
#include "logs.h"

#include "styles/style_iv.h"
#include "styles/style_layers.h"

#include <QtCore/QElapsedTimer>
#include <QtGui/QScreen>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace Iv::Markdown {
namespace {

[[nodiscard]] QString PrepareTerminalFailureName(
		PrepareTerminalFailure failure) {
	switch (failure) {
	case PrepareTerminalFailure::None:
		return u"none"_q;
	case PrepareTerminalFailure::InvalidRequest:
		return u"invalid-request"_q;
	case PrepareTerminalFailure::InvalidStyle:
		return u"invalid-style"_q;
	case PrepareTerminalFailure::DocumentTooLarge:
		return u"document-too-large"_q;
	case PrepareTerminalFailure::InternalError:
		return u"internal-error"_q;
	}
	return u"unknown"_q;
}

[[nodiscard]] QString PrepareFailureReasonText(
		const PrepareFailureStatus &failure) {
	return !failure.debugReason.isEmpty()
		? failure.debugReason
		: PrepareTerminalFailureName(failure.terminal);
}

[[nodiscard]] QVariant CurrentClickHandlerContext(const OpenOptions &options) {
	return options.clickHandlerContextRef
		? *options.clickHandlerContextRef
		: options.clickHandlerContext;
}

[[nodiscard]] const PreparedFootnote *FindFootnote(
		const std::vector<PreparedFootnote> &footnotes,
		const QString &target) {
	const auto i = std::find_if(
		footnotes.begin(),
		footnotes.end(),
		[&](const PreparedFootnote &footnote) {
			return (footnote.label == target);
		});
	return (i != footnotes.end()) ? &*i : nullptr;
}

[[nodiscard]] int FootnoteLabelContentWidth(
		not_null<Ui::FlatLabel*> label,
		int maxWidth) {
	const auto heightForWidth = [&](int width) {
		label->resizeToWidth(width);
		return label->heightNoMargins();
	};
	const auto natural = label->naturalWidth();
	const auto minWidth = std::min(st::markdownFootnoteLabel.minWidth, maxWidth);
	auto result = std::max(
		(natural >= 0) ? std::min(natural, maxWidth) : maxWidth,
		minWidth);
	if (result >= label->textMaxWidth()) {
		return result;
	}
	auto large = result;
	auto small = std::max(result / 2, minWidth);
	const auto largeHeight = heightForWidth(large);
	while (large - small > 1) {
		const auto middle = (large + small) / 2;
		if (largeHeight == heightForWidth(middle)) {
			large = middle;
		} else {
			small = middle;
		}
	}
	return large;
}

[[nodiscard]] std::optional<EntityLinkData> ExternalEntityLinkData(
		const PreparedLink &link) {
	if (link.kind != PreparedLinkKind::External || link.target.isEmpty()) {
		return std::nullopt;
	}
	switch (link.entityType) {
	case EntityType::Url:
	case EntityType::CustomUrl:
	case EntityType::Email:
		return EntityLinkData{
			.text = !link.copyText.isEmpty() ? link.copyText : link.target,
			.data = link.target,
			.type = link.entityType,
			.shown = link.shown,
		};
	default:
		return std::nullopt;
	}
}

[[nodiscard]] bool ActivateExternalLink(
		const PreparedLink &link,
		Qt::MouseButton button,
		QVariant context) {
	const auto data = ExternalEntityLinkData(link);
	if (!data) {
		return false;
	}
	const auto handler = Ui::Integration::Instance().createLinkHandler(
		*data,
		Ui::Text::MarkedContext());
	if (!handler) {
		return false;
	}
	auto click = ClickContext();
	click.button = button;
	click.other = std::move(context);
	handler->onClick(std::move(click));
	return true;
}

} // namespace

class MarkdownPreviewRoot final : public Ui::RpWidget {
public:
	MarkdownPreviewRoot(
		QWidget *parent,
		const PreparedDocument &document,
		Fn<void(Event)> callback,
		const OpenOptions &options);
	MarkdownPreviewRoot(
		QWidget *parent,
		MarkdownArticleContent content,
		std::shared_ptr<MathRenderer> renderer,
	Fn<void(Event)> callback,
	const OpenOptions &options);
	bool scrollToAnchor(const QString &anchorId);
	[[nodiscard]] rpl::producer<int> scrollTopValue() const;

private:
	void setup();
	void prepareArticle();
	void activateLink(const PreparedLink &link, Qt::MouseButton button);
	void showFootnote(const PreparedLink &link, Qt::MouseButton button);
	void fillFootnoteBox(
		not_null<Ui::GenericBox*> box,
		PreparedFootnote footnote);
	void applyPreparedContent(MarkdownArticleContent prepared, int prepareMs);
	void updateBodyVisibleTopBottom();
	void updateChildrenGeometry(QSize size);
	void updateFailureGeometry();
	void logPreparationSummary(
		const PrepareFailureStatus &failure,
		const PrepareDebugStats &debug,
		int prepareMs,
		int layoutMs) const;

	const OpenOptions _options;
	const std::shared_ptr<const PreparedDocument> _document;
	std::optional<MarkdownArticleContent> _preparedContent;
	const Fn<void(Event)> _callback;
	std::vector<PreparedFootnote> _footnotes;
	std::unique_ptr<Ui::LayerManager> _footnoteLayerManager;
	Ui::ScrollArea *_scroll = nullptr;
	MarkdownDocumentWidget *_body = nullptr;
	Ui::FlatLabel *_failure = nullptr;
	Ui::LinkButton *_failureOpen = nullptr;
	const std::shared_ptr<MathRenderer> _renderer;
	std::shared_ptr<MarkdownArticle> _article;
	QString _pendingFragment;
	int _devicePixelRatio = 0;
	rpl::lifetime _channelJoinedLifetime;

};

MarkdownPreviewRoot::MarkdownPreviewRoot(
	QWidget *parent,
	const PreparedDocument &document,
	Fn<void(Event)> callback,
	const OpenOptions &options)
: Ui::RpWidget(parent)
, _options(options)
, _document(std::make_shared<PreparedDocument>(document))
, _callback(std::move(callback))
, _renderer(std::make_shared<MathRenderer>())
, _pendingFragment(options.initialFragment) {
	setup();
}

MarkdownPreviewRoot::MarkdownPreviewRoot(
	QWidget *parent,
	MarkdownArticleContent content,
	std::shared_ptr<MathRenderer> renderer,
	Fn<void(Event)> callback,
	const OpenOptions &options)
: Ui::RpWidget(parent)
, _options(options)
, _preparedContent(std::move(content))
, _callback(std::move(callback))
, _renderer(renderer ? std::move(renderer) : std::make_shared<MathRenderer>())
, _pendingFragment(options.initialFragment) {
	setup();
}

void MarkdownPreviewRoot::setup() {
	_footnoteLayerManager = std::make_unique<Ui::LayerManager>(not_null{ this });
	_footnoteLayerManager->setHideByBackgroundClick(true);

	_scroll = Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll);
	_body = _scroll->setOwnedWidget(object_ptr<MarkdownDocumentWidget>(_scroll));
	_failure = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_markdown_preview_cant(tr::now),
		st::defaultMarkdown.failure.label);
	_failureOpen = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_markdown_preview_open_file(tr::now));

	_scroll->hide();
	if (_body) {
		_body->hide();
		_body->setClickHandlerContext(
			CurrentClickHandlerContext(_options),
			_options.clickHandlerContextRef);
		_body->setLinkActivationCallback([=](
				const PreparedLink &link,
				Qt::MouseButton button) {
			activateLink(link, button);
		});
		_body->setMediaActivationCallback([=](
				const MediaActivation &activation,
				Qt::MouseButton button) {
			return _options.activateMedia
				? _options.activateMedia(activation, button)
				: false;
		});
		if (_options.delegate) {
			_body->setZoom(_options.delegate->ivZoom());
		}
	}
	_failure->hide();
	_failureOpen->hide();
	_failureOpen->setClickedCallback([=] {
		if (!_options.sourcePath.isEmpty()) {
			File::Launch(_options.sourcePath);
		}
	});

	sizeValue() | rpl::on_next([=](QSize size) {
		updateChildrenGeometry(size);
	}, lifetime());

	rpl::combine(
		_scroll->scrollTopValue(),
		_scroll->heightValue()
	) | rpl::on_next([=](int, int) {
		updateBodyVisibleTopBottom();
	}, lifetime());

	style::PaletteChanged() | rpl::on_next([=] {
		if (_body && !_body->isHidden()) {
			_body->refreshPalette();
		}
	}, lifetime());

	screenValue() | rpl::on_next([=](not_null<QScreen*>) {
		const auto devicePixelRatio = style::DevicePixelRatio();
		if (devicePixelRatio == _devicePixelRatio) {
			return;
		}
		_devicePixelRatio = devicePixelRatio;
		if (_body && !_body->isHidden()) {
			_body->invalidateRasterCache();
		}
	}, lifetime());

	if (_options.delegate) {
		_options.delegate->ivZoomValue(
		) | rpl::on_next([=](int value) {
			if (_body) {
				_body->setZoom(value);
			}
		}, lifetime());
	}

	_devicePixelRatio = style::DevicePixelRatio();
	prepareArticle();
}

void MarkdownPreviewRoot::prepareArticle() {
	if (_preparedContent) {
		applyPreparedContent(std::move(*_preparedContent), 0);
		_preparedContent.reset();
		return;
	}
	if (_renderer) {
		_renderer->resetDebugCounters();
	}
	auto timer = QElapsedTimer();
	timer.start();
	auto prepared = PrepareSynchronously({
		.document = _document,
		.renderer = _renderer,
		.dimensions = CaptureMarkdownPrepareDimensions(),
		.sourcePath = _options.sourcePath,
	});
	applyPreparedContent(std::move(prepared), int(timer.elapsed()));
}

void MarkdownPreviewRoot::activateLink(
		const PreparedLink &link,
		Qt::MouseButton button) {
	if (button != Qt::LeftButton && button != Qt::MiddleButton) {
		return;
	}
	switch (link.kind) {
	case PreparedLinkKind::External:
		if (!ActivateExternalLink(
				link,
				button,
				CurrentClickHandlerContext(_options))) {
			DEBUG_LOG(("Native Markdown IV: failed external link activation: %1").arg(
				link.target));
		}
		break;
	case PreparedLinkKind::Anchor:
	case PreparedLinkKind::FootnoteBacklink:
		if (!scrollToAnchor(link.target)) {
			DEBUG_LOG(("Native Markdown IV: unresolved anchor: %1").arg(
				link.target));
		}
		break;
	case PreparedLinkKind::Footnote:
		showFootnote(link, button);
		break;
	case PreparedLinkKind::LocalFile: {
		auto target = link.target;
		if (!link.fragment.isEmpty()) {
			target += u"#"_q + link.fragment;
		}
		_callback({
			.type = Event::Type::OpenFile,
			.url = std::move(target),
			.context = CurrentClickHandlerContext(_options),
		});
	} break;
	case PreparedLinkKind::RejectedRelative:
		DEBUG_LOG(("Native Markdown IV: "
			"rejected relative markdown link: %1").arg(
			link.target));
		break;
	case PreparedLinkKind::ToggleDetails:
		if (_body && !_body->toggleDetails(link.target)) {
			DEBUG_LOG(("Native Markdown IV: failed details toggle: %1").arg(
				link.target));
		}
		break;
	}
}

void MarkdownPreviewRoot::showFootnote(
		const PreparedLink &link,
		Qt::MouseButton button) {
	Q_UNUSED(button);

	const auto found = FindFootnote(_footnotes, link.target);
	if (!found) {
		DEBUG_LOG(("Native Markdown IV: unresolved footnote: %1").arg(
			link.target));
		return;
	}
	if (!_footnoteLayerManager) {
		DEBUG_LOG(("Native Markdown IV: missing footnote layer manager: %1").arg(
			link.target));
		return;
	}

	const auto footnote = *found;
	_footnoteLayerManager->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		fillFootnoteBox(box, footnote);
	}));
}

void MarkdownPreviewRoot::fillFootnoteBox(
		not_null<Ui::GenericBox*> box,
		PreparedFootnote footnote) {
	box->setStyle(st::markdownFootnoteBox);
	box->setNoContentMargin(true);
	box->setCloseByOutsideClick(true);
	box->clearButtons();

	auto label = object_ptr<Ui::FlatLabel>(box, st::markdownFootnoteLabel);
	label->setMarkedText(footnote.text);
	label->setTryMakeSimilarLines(true);
	for (const auto &link : footnote.links) {
		label->setLink(link.index, CreatePreparedLinkHandler(link));
	}
	label->setClickHandlerFilter([=](
			const ClickHandlerPtr &handler,
			Qt::MouseButton button) {
		if (const auto prepared = ExtractPreparedLink(handler)) {
			activateLink(*prepared, button);
			return false;
		}
		return true;
	});

	const auto skips = rect::m::sum::h(st::markdownFootnotePadding);
	const auto contentWidth = FootnoteLabelContentWidth(
		label.get(),
		std::max(
			st::boxWideWidth - skips,
			st::markdownFootnoteLabel.minWidth));
	label->resizeToWidth(contentWidth);
	box->setWidth(contentWidth + skips);
	box->addRow(std::move(label), st::markdownFootnotePadding);
}

void MarkdownPreviewRoot::applyPreparedContent(
		MarkdownArticleContent prepared,
		int prepareMs) {
	const auto failure = prepared.failure;
	const auto debug = prepared.debug;
	_channelJoinedLifetime.destroy();
	if (failure.failed()) {
		_article = nullptr;
		_footnotes.clear();
		_scroll->hide();
		if (_body) {
			_body->hide();
		}
		_failure->show();
		if (_options.sourcePath.isEmpty()) {
			_failureOpen->hide();
		} else {
			_failureOpen->show();
		}
		_failure->raise();
		_failureOpen->raise();
		updateFailureGeometry();
		logPreparationSummary(failure, debug, prepareMs, 0);
		return;
	}

	_footnotes = prepared.footnotes;

	if (!_body) {
		_article = nullptr;
		logPreparationSummary(failure, debug, prepareMs, 0);
		return;
	}

	if (prepared.mediaRuntime) {
		prepared.mediaRuntime->channelJoinedChanges(
		) | rpl::on_next([=](uint64) {
			if (_body && !_body->isHidden() && _article) {
				_article->invalidateLayout();
				updateChildrenGeometry(size());
				_body->update();
			}
		}, _channelJoinedLifetime);
	}

	auto article = std::make_shared<MarkdownArticle>(_renderer);
	article->setContent(std::move(prepared));
	_article = article;
	updateChildrenGeometry(size());
	_body->setArticle(article);
	if (_options.delegate) {
		_body->setZoom(_options.delegate->ivZoom());
	}
	updateBodyVisibleTopBottom();
	_scroll->show();
	_body->show();
	_failure->hide();
	_failureOpen->hide();
	if (!_pendingFragment.isEmpty()) {
		const auto scrolled = scrollToAnchor(_pendingFragment);
		static_cast<void>(scrolled);
		_pendingFragment.clear();
	}
	logPreparationSummary(
		failure,
		debug,
		prepareMs,
		_body->lastRelayoutMs());
}

bool MarkdownPreviewRoot::scrollToAnchor(const QString &anchorId) {
	if (!_body || !_scroll || anchorId.isEmpty()) {
		return false;
	}
	const auto top = _body->anchorTop(anchorId);
	if (top < 0) {
		return false;
	}
	_scroll->scrollToY(top);
	return true;
}

rpl::producer<int> MarkdownPreviewRoot::scrollTopValue() const {
	return _scroll
		? _scroll->scrollTopValue()
		: rpl::single(0);
}

bool ScrollMarkdownPreviewToAnchor(
		Ui::RpWidget *preview,
		const QString &anchorId) {
	const auto root = dynamic_cast<MarkdownPreviewRoot*>(preview);
	return root ? root->scrollToAnchor(anchorId) : false;
}

rpl::producer<int> MarkdownPreviewScrollTopValue(Ui::RpWidget *preview) {
	const auto root = dynamic_cast<MarkdownPreviewRoot*>(preview);
	return root ? root->scrollTopValue() : rpl::single(0);
}

void MarkdownPreviewRoot::updateBodyVisibleTopBottom() {
	if (_body) {
		const auto scrollTop = _scroll->scrollTop();
		_body->setVisibleTopBottom(scrollTop, scrollTop + _scroll->height());
	}
}

void MarkdownPreviewRoot::updateChildrenGeometry(QSize size) {
	_scroll->setGeometry(QRect(QPoint(), size));
	if (_body) {
		_body->resizeToWidth(_scroll->width());
		updateBodyVisibleTopBottom();
	}
	updateFailureGeometry();
}

void MarkdownPreviewRoot::updateFailureGeometry() {
	const auto availableWidth = std::max(width(), 1);
	const auto failureWidth = std::min(
		availableWidth,
		st::defaultMarkdown.failure.width);
	_failure->resizeToWidth(failureWidth);
	_failureOpen->resizeToNaturalWidth(failureWidth);
	const auto openVisible = !_failureOpen->isHidden();
	const auto totalHeight = _failure->height()
		+ (openVisible
			? (st::defaultMarkdown.failure.skip + _failureOpen->height())
			: 0);
	const auto top = std::max((height() - totalHeight) / 2, 0);
	_failure->moveToLeft(
		(availableWidth - failureWidth) / 2,
		top,
		availableWidth);
	if (openVisible) {
		_failureOpen->moveToLeft(
			(availableWidth - _failureOpen->width()) / 2,
			top + _failure->height() + st::defaultMarkdown.failure.skip,
			availableWidth);
	}
}

void MarkdownPreviewRoot::logPreparationSummary(
		const PrepareFailureStatus &failure,
		const PrepareDebugStats &debug,
		int prepareMs,
		int layoutMs) const {
#ifndef NDEBUG
	const auto counters = _renderer ? _renderer->debugCounters() : FormulaDebugCounters();
	const auto reason = PrepareFailureReasonText(failure);
	DEBUG_LOG((
		failure.failed()
			? "Native Markdown IV: unexpected preview prepare failure (%1 ms prepare, %2 ms layout, %3 ms formulas, cache hits=%4 misses=%5 bytes=%6, terminal=%7): %8"
			: "Native Markdown IV: preview prepare success (%1 ms prepare, %2 ms layout, %3 ms formulas, cache hits=%4 misses=%5 bytes=%6, terminal=%7): %8"
		).arg(prepareMs
		).arg(layoutMs
		).arg(debug.formulaMeasureMs
		).arg(counters.hits
		).arg(counters.misses
		).arg(qlonglong(counters.cacheBytes)
		).arg(reason
		).arg(_options.sourcePath));
#else
	Q_UNUSED(failure);
	Q_UNUSED(debug);
	Q_UNUSED(prepareMs);
	Q_UNUSED(layoutMs);
#endif
}

std::unique_ptr<Ui::RpWidget> CreateMarkdownPreviewWidget(
		QWidget *parent,
		const PreparedDocument &document,
		Fn<void(Event)> callback,
		const OpenOptions &options) {
	return std::make_unique<MarkdownPreviewRoot>(
		parent,
		document,
		std::move(callback),
		options);
}

std::unique_ptr<Ui::RpWidget> CreateMarkdownPreviewWidget(
		QWidget *parent,
		MarkdownArticleContent content,
		std::shared_ptr<MathRenderer> renderer,
		Fn<void(Event)> callback,
		const OpenOptions &options) {
	return std::make_unique<MarkdownPreviewRoot>(
		parent,
		std::move(content),
		std::move(renderer),
		std::move(callback),
		options);
}

} // namespace Iv::Markdown
