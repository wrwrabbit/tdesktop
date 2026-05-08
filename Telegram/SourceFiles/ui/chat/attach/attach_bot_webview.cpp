/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/chat/attach/attach_bot_webview.h"

#include "core/file_utilities.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_bot_downloads.h"
#include "ui/effects/radial_animation.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/box_content.h"
#include "ui/style/style_core_palette.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/separate_panel.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/integration.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/ui_utility.h"
#include "lang/lang_keys.h"
#include "webview/webview_embed.h"
#include "webview/webview_dialog.h"
#include "webview/webview_interface.h"
#include "base/debug_log.h"
#include "base/invoke_queued.h"
#include "base/options.h"
#include "base/qt_signal_producer.h"
#include "styles/style_chat.h"
#include "styles/style_payments.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtGui/QWindow>
#include <QtGui/QScreen>
#include <QtGui/qpa/qplatformscreen.h>

namespace Ui::BotWebView {

const char kOptionLinuxExternalBotWebApps[] = "linux-external-bot-webapps";

namespace {

constexpr auto kClipboardReadTimeout = crl::time(10000);
constexpr auto kProgressDuration = crl::time(200);
constexpr auto kProgressOpacity = 0.3;
constexpr auto kLightnessThreshold = 128;
constexpr auto kLightnessDelta = 32;

base::options::toggle OptionLinuxExternalBotWebApps({
	.id = kOptionLinuxExternalBotWebApps,
	.name = "Use external Linux bot web app windows",
	.description = "Open bot web apps in a top-level WebKitGTK window"
		" with an HTML shell instead of embedding the GTK surface.",
	.scope = base::options::linux,
	.restartRequired = true,
});

struct ButtonArgs {
	bool isActive = false;
	bool isVisible = false;
	bool isProgressVisible = false;
	uint64 iconCustomEmojiId = 0;
	QString text;
};

[[nodiscard]] RectPart ParsePosition(const QString &position) {
	if (position == u"left"_q) {
		return RectPart::Left;
	} else if (position == u"top"_q) {
		return RectPart::Top;
	} else if (position == u"right"_q) {
		return RectPart::Right;
	} else if (position == u"bottom"_q) {
		return RectPart::Bottom;
	}
	return RectPart::Left;
}

[[nodiscard]] QJsonObject ParseMethodArgs(const QString &json) {
	if (json.isEmpty()) {
		return {};
	}
	auto error = QJsonParseError();
	const auto dictionary = QJsonDocument::fromJson(json.toUtf8(), &error);
	if (error.error != QJsonParseError::NoError) {
		LOG(("BotWebView Error: Could not parse \"%1\".").arg(json));
		return {};
	}
	return dictionary.object();
}

[[nodiscard]] bool UseExternalBotWebApps() {
	static const auto Result = OptionLinuxExternalBotWebApps.relevant()
		&& OptionLinuxExternalBotWebApps.value();
	return Result;
}

[[nodiscard]] QByteArray JsonValue(QJsonValue value) {
	auto array = QJsonArray();
	array.push_back(std::move(value));
	auto result = QJsonDocument(array).toJson(QJsonDocument::Compact);
	return result.mid(1, result.size() - 2);
}

[[nodiscard]] QByteArray JsonObject(QJsonObject object) {
	return QJsonDocument(std::move(object)).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QJsonValue ColorValue(QColor color) {
	return color.isValid()
		? QJsonValue(color.name(QColor::HexRgb))
		: QJsonValue();
}

[[nodiscard]] QByteArray ExternalShellCss() {
	return R"(
html, body, #root {
	width: 100%;
	height: 100%;
	margin: 0;
	overflow: hidden;
}
body {
	font: 14px/1.35 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
	background: var(--body-bg, #ffffff);
	color: var(--fg, #000000);
}
#root {
	display: grid;
	grid-template-rows: auto minmax(0, 1fr) auto;
	position: relative;
}
#header {
	display: grid;
	grid-template-columns: auto minmax(0, 1fr) auto auto auto;
	align-items: center;
	height: 48px;
	background: var(--title-bg, #ffffff);
	color: var(--title-fg, #000000);
	border-bottom: 1px solid rgba(0, 0, 0, 0.08);
}
#title {
	overflow: hidden;
	text-overflow: ellipsis;
	white-space: nowrap;
	font-weight: 600;
	padding: 0 10px;
}
.icon {
	width: 44px;
	height: 44px;
	border: 0;
	background: transparent;
	color: inherit;
	font: inherit;
	font-size: 22px;
	cursor: default;
}
.icon:hover {
	background: rgba(127, 127, 127, 0.14);
}
.hidden {
	display: none !important;
}
#frame-wrap {
	min-height: 0;
}
#frame-wrap iframe {
	display: block;
	width: 100%;
	height: 100%;
	border: 0;
	background: var(--body-bg, #ffffff);
}
#footer {
	background: var(--bottom-bg, var(--body-bg, #ffffff));
	border-top: 1px solid rgba(0, 0, 0, 0.08);
}
#buttons {
	display: grid;
	gap: 8px;
	padding: 10px;
}
.shell-button {
	height: 42px;
	border: 0;
	border-radius: 8px;
	padding: 0 14px;
	font: inherit;
	font-weight: 600;
	overflow: hidden;
	text-overflow: ellipsis;
	white-space: nowrap;
}
.shell-button:disabled {
	opacity: 0.55;
}
.shell-button.progress::after {
	content: "";
	display: inline-block;
	width: 14px;
	height: 14px;
	margin-left: 8px;
	border: 2px solid currentColor;
	border-right-color: transparent;
	border-radius: 50%;
	vertical-align: -2px;
	animation: spin 0.8s linear infinite;
}
#blocker {
	display: none;
	position: absolute;
	inset: 0;
	background: rgba(0, 0, 0, 0.10);
	z-index: 10;
}
#root.blocked #blocker {
	display: block;
}
@keyframes spin {
	to { transform: rotate(360deg); }
}
)";
}

[[nodiscard]] QByteArray ExternalShellJs() {
	return R"(
(function() {
	'use strict';

	const root = document.getElementById('root');
	const frameWrap = document.getElementById('frame-wrap');
	const title = document.getElementById('title');
	const footer = document.getElementById('footer');
	const buttonsWrap = document.getElementById('buttons');
	const controls = {
		back: document.getElementById('back'),
		settings: document.getElementById('settings'),
		reload: document.getElementById('reload'),
		close: document.getElementById('close')
		};
		let iframe = null;
		let frameLoaded = false;
		let frameUrl = 'about:blank';
		let reloadSupported = false;
		let reloadTimeout = null;
		const pendingEvents = [];
		const buttonState = {
			main: null,
			secondary: null
		};

	function invoke(eventType, eventData) {
		if (window.external && window.external.invoke) {
			window.external.invoke(JSON.stringify([
				eventType,
				JSON.stringify(eventData || {})
			]));
			}
		}

		function sendToFrame(eventType, eventData) {
			iframe.contentWindow.postMessage(JSON.stringify({
				eventType: eventType,
				eventData: eventData || {}
			}), '*');
		}

		function postToFrame(eventType, eventData) {
			if (!iframe || !iframe.contentWindow || !frameLoaded) {
				pendingEvents.push({
					eventType: eventType,
					eventData: eventData || {}
				});
				return;
			}
			sendToFrame(eventType, eventData);
		}

	function flushPendingEvents() {
		const pending = pendingEvents.splice(0);
		for (const event of pending) {
			postToFrame(event.eventType, event.eventData);
		}
	}

	function sendViewportChanged() {
		if (!iframe) {
			return;
		}
		const height = Math.max(0, Math.round(iframe.getBoundingClientRect().height));
		postToFrame('viewport_changed', {
			height: height,
			is_state_stable: true,
			is_expanded: true
		});
	}

		function colorForBackground(value) {
			if (!/^#[0-9a-f]{6}$/i.test(value || '')) {
				return null;
			}
			const red = parseInt(value.slice(1, 3), 16) / 255;
			const green = parseInt(value.slice(3, 5), 16) / 255;
			const blue = parseInt(value.slice(5, 7), 16) / 255;
			const luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
			return luminance > 0.5 ? '#000000' : '#ffffff';
		}

		function applyColors(data) {
			if (data.bodyBg) {
				root.style.setProperty('--body-bg', data.bodyBg);
			}
			if (data.titleBg) {
				root.style.setProperty('--title-bg', data.titleBg);
				const titleFg = colorForBackground(data.titleBg);
				if (titleFg) {
					root.style.setProperty('--title-fg', titleFg);
				}
			}
			if (data.bottomBg) {
				root.style.setProperty('--bottom-bg', data.bottomBg);
		}
	}

	function renderButtons() {
		buttonsWrap.textContent = '';
		const visible = [];
		if (buttonState.secondary && buttonState.secondary.visible) {
			visible.push(buttonState.secondary);
		}
		if (buttonState.main && buttonState.main.visible) {
			visible.push(buttonState.main);
		}
		for (const state of visible) {
			const button = document.createElement('button');
			button.type = 'button';
			button.className = 'shell-button';
			if (state.progress) {
				button.classList.add('progress');
			}
			button.textContent = state.text || '';
			button.disabled = !state.active;
			button.style.background = state.color || '#40a7e3';
			button.style.color = state.textColor || '#ffffff';
			button.addEventListener('click', function() {
				if (!button.disabled) {
					postToFrame(state.name + '_button_pressed', {});
				}
			});
			buttonsWrap.appendChild(button);
		}
		footer.classList.toggle('hidden', !visible.length);
		requestAnimationFrame(sendViewportChanged);
	}

		function parseFrameMessage(data) {
			if (typeof data === 'string') {
				try {
					return JSON.parse(data);
				} catch (e) {
				return null;
			}
		}
		return data && typeof data === 'object' ? data : null;
	}

	window.addEventListener('message', function(event) {
		if (!iframe || event.source !== iframe.contentWindow) {
			return;
		}
		const message = parseFrameMessage(event.data);
			if (!message || !message.eventType) {
				return;
			}
			if (message.eventType === 'iframe_ready') {
				reloadSupported = !!(message.eventData && message.eventData.reload_supported);
			} else if (message.eventType === 'iframe_will_reload') {
				if (reloadTimeout) {
					window.clearTimeout(reloadTimeout);
					reloadTimeout = null;
				}
				frameLoaded = false;
			}
			invoke(message.eventType, message.eventData || {});
		});

		function createIframe(url) {
			if (reloadTimeout) {
				window.clearTimeout(reloadTimeout);
				reloadTimeout = null;
			}
			const next = document.createElement('iframe');
			next.setAttribute('allow', 'clipboard-read; clipboard-write; fullscreen');
			next.addEventListener('load', function() {
				if (iframe !== next) {
					return;
				}
				frameLoaded = true;
				flushPendingEvents();
				sendViewportChanged();
			});
			frameLoaded = false;
			reloadSupported = false;
			if (iframe) {
				iframe.remove();
			}
			iframe = next;
			iframe.src = url || 'about:blank';
			frameWrap.appendChild(iframe);
		}

		function fallbackReloadFrame() {
			createIframe(frameUrl || 'about:blank');
		}

		controls.close.addEventListener('click', function() {
			invoke('tdesktop_shell_close', {});
		});
		controls.back.addEventListener('click', function() {
			postToFrame('back_button_pressed', {});
	});
	controls.settings.addEventListener('click', function() {
			postToFrame('settings_button_pressed', {});
		});
		controls.reload.addEventListener('click', function() {
			if (!iframe) {
				return;
			}
			if (reloadSupported && frameLoaded && iframe.contentWindow) {
				sendToFrame('reload_iframe', {});
				if (reloadTimeout) {
					window.clearTimeout(reloadTimeout);
				}
				reloadTimeout = window.setTimeout(function() {
					reloadTimeout = null;
					fallbackReloadFrame();
				}, 500);
				return;
			}
			fallbackReloadFrame();
		});

		window.TelegramDesktopShell = {
			bootstrap: function(data) {
				applyColors(data || {});
				title.textContent = data.title || '';
				document.title = data.title || 'Telegram';
				frameUrl = data.url || 'about:blank';
				createIframe(frameUrl);
				controls.back.classList.toggle('hidden', !data.backVisible);
				controls.settings.classList.toggle('hidden', !data.settingsVisible);
			},
		nativeEvent: function(eventType, eventData) {
			postToFrame(eventType, eventData || {});
		},
			setTitle: function(data) {
				title.textContent = (data && data.title) || '';
				document.title = (data && data.title) || 'Telegram';
			},
		setChrome: function(data) {
			controls.back.classList.toggle('hidden', !(data && data.backVisible));
			controls.settings.classList.toggle('hidden', !(data && data.settingsVisible));
		},
		setColors: function(data) {
			applyColors(data || {});
		},
		setBottomText: function(data) {
			requestAnimationFrame(sendViewportChanged);
		},
		setButton: function(data) {
			if (!data || !data.name) {
				return;
			}
			buttonState[data.name] = data;
			renderButtons();
		},
		setBlocked: function(data) {
			root.classList.toggle('blocked', !!(data && data.blocked));
		},
		setProgress: function(data) {
			root.classList.toggle('loading', !!(data && data.shown));
		},
		sendViewport: sendViewportChanged
	};
})();
)";
}

[[nodiscard]] QByteArray ExternalShellBodyHtml() {
	auto result = QByteArray();
	result.reserve(1024);
	result += R"(<div id="root">
<header id="header">
<button id="back" class="icon hidden" type="button">&lsaquo;</button>
<div id="title"></div>
<button id="settings" class="icon hidden" type="button">&#9881;</button>
<button id="reload" class="icon" type="button">&#8635;</button>
<button id="close" class="icon" type="button">&times;</button>
</header>
<main id="frame-wrap"></main>
<footer id="footer" class="hidden">
<div id="buttons"></div>
</footer>
<div id="blocker"></div>
</div>)";
	return result;
}

[[nodiscard]] QByteArray ExternalShellInstallScript() {
	const auto css = QString::fromUtf8(ExternalShellCss());
	const auto body = QString::fromUtf8(ExternalShellBodyHtml());
	auto script = QByteArray();
	script += "if (window === window.top"
		" && !window.TelegramDesktopShell"
		" && !window.TelegramDesktopShellInstalling) {"
		"window.TelegramDesktopShellInstalling = true;"
		"try {"
		"if (!document.head) {"
		"document.documentElement.insertBefore("
		"document.createElement('head'),"
		"document.documentElement.firstChild);"
		"}"
		"if (!document.body) {"
		"document.documentElement.appendChild(document.createElement('body'));"
		"}"
		"document.title = 'Telegram';"
		"const metaRobots = document.createElement('meta');"
		"metaRobots.name = 'robots';"
		"metaRobots.content = 'noindex, nofollow';"
		"document.head.appendChild(metaRobots);"
		"const metaViewport = document.createElement('meta');"
		"metaViewport.name = 'viewport';"
		"metaViewport.content = 'width=device-width, initial-scale=1.0';"
		"document.head.appendChild(metaViewport);"
		"const style = document.createElement('style');"
		"style.textContent = ";
	script += JsonValue(css);
	script += ";"
		"document.head.appendChild(style);"
		"document.body.insertAdjacentHTML('beforeend', ";
	script += JsonValue(body);
	script += ");";
	script += ExternalShellJs();
	script += "} finally {"
		"window.TelegramDesktopShellInstalling = false;"
		"}"
		"}";
	return script;
}

[[nodiscard]] std::optional<QColor> ParseColor(const QString &text) {
	if (!text.startsWith('#') || text.size() != 7) {
		return {};
	}
	const auto data = text.data() + 1;
	const auto hex = [&](int from) -> std::optional<int> {
		const auto parse = [](QChar ch) -> std::optional<int> {
			const auto code = ch.unicode();
			return (code >= 'a' && code <= 'f')
				? std::make_optional(10 + (code - 'a'))
				: (code >= 'A' && code <= 'F')
				? std::make_optional(10 + (code - 'A'))
				: (code >= '0' && code <= '9')
				? std::make_optional(code - '0')
				: std::nullopt;
		};
		const auto h = parse(data[from]), l = parse(data[from + 1]);
		return (h && l) ? std::make_optional(*h * 16 + *l) : std::nullopt;
	};
	const auto r = hex(0), g = hex(2), b = hex(4);
	return (r && g && b) ? QColor(*r, *g, *b) : std::optional<QColor>();
}

[[nodiscard]] QColor ResolveRipple(QColor background) {
	auto hue = 0;
	auto saturation = 0;
	auto lightness = 0;
	auto alpha = 0;
	background.getHsv(&hue, &saturation, &lightness, &alpha);
	return QColor::fromHsv(
		hue,
		saturation,
		lightness - (lightness > kLightnessThreshold
			? kLightnessDelta
			: -kLightnessDelta),
		alpha);
}

[[nodiscard]] const style::color *LookupNamedColor(const QString &key) {
	if (key == u"secondary_bg_color"_q) {
		return &st::boxDividerBg;
	} else if (key == u"bottom_bar_bg_color"_q) {
		return &st::windowBg;
	}
	return nullptr;
}

} // namespace

class Panel::Button final : public RippleButton {
public:
	Button(
		QWidget *parent,
		const style::RoundButton &st,
		Text::MarkedContext textContext);
	~Button();

	void updateBg(QColor bg);
	void updateBg(not_null<const style::color*> paletteBg);
	void updateFg(QColor fg);
	void updateFg(not_null<const style::color*> paletteFg);

	void updateArgs(ButtonArgs &&args);

private:
	void paintEvent(QPaintEvent *e) override;

	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;

	void toggleProgress(bool shown);
	void setupProgressGeometry();
	void updateText(uint64 iconCustomEmojiId, const QString &text);

	std::unique_ptr<Progress> _progress;
	Ui::Text::String _text;

	Text::MarkedContext _textContext;
	const style::RoundButton &_st;
	QColor _fg;
	style::owned_color _bg;
	RoundRect _roundRect;

	rpl::lifetime _bgLifetime;
	rpl::lifetime _fgLifetime;

};

struct Panel::Progress {
	Progress(QWidget *parent, Fn<QRect()> rect);

	RpWidget widget;
	InfiniteRadialAnimation animation;
	Animations::Simple shownAnimation;
	bool shown = true;
	rpl::lifetime geometryLifetime;
};

struct Panel::WebviewWithLifetime {
	WebviewWithLifetime(
		QWidget *parent = nullptr,
		Webview::WindowConfig config = Webview::WindowConfig());

	Webview::Window window;
	std::vector<QPointer<RpWidget>> boxes;
	rpl::lifetime boxesLifetime;
	rpl::lifetime lifetime;
};

Panel::Button::Button(
	QWidget *parent,
	const style::RoundButton &st,
	Text::MarkedContext textContext)
: RippleButton(parent, st.ripple)
, _textContext(std::move(textContext))
, _st(st)
, _bg(st::windowBgActive->c)
, _roundRect(st::callRadius, st::windowBgActive) {
	resize(
		_st.padding.left() + _text.maxWidth() + _st.padding.right(),
		_st.padding.top() + _st.height + _st.padding.bottom());
}

Panel::Button::~Button() = default;

void Panel::Button::updateText(
		uint64 iconCustomEmojiId,
		const QString &text) {
	if (iconCustomEmojiId) {
		auto result = Text::SingleCustomEmoji(
			QString::number(iconCustomEmojiId));
		if (!text.isEmpty()) {
			result.append(' ').append(text);
		}
		auto context = _textContext;
		context.repaint = [=] { update(); };
		_text.setMarkedText(
			st::semiboldTextStyle,
			result,
			kMarkupTextOptions,
			context);
	} else {
		_text.setText(st::semiboldTextStyle, text);
	}
}

void Panel::Button::updateBg(QColor bg) {
	_bg.update(bg);
	_roundRect.setColor(_bg.color());
	_bgLifetime.destroy();
	update();
}

void Panel::Button::updateBg(not_null<const style::color*> paletteBg) {
	updateBg((*paletteBg)->c);
	_bgLifetime = style::PaletteChanged(
	) | rpl::on_next([=] {
		updateBg((*paletteBg)->c);
	});
}

void Panel::Button::updateFg(QColor fg) {
	_fg = fg;
	_fgLifetime.destroy();
	update();
}

void Panel::Button::updateFg(not_null<const style::color*> paletteFg) {
	updateFg((*paletteFg)->c);
	_fgLifetime = style::PaletteChanged(
	) | rpl::on_next([=] {
		updateFg((*paletteFg)->c);
	});
}

void Panel::Button::updateArgs(ButtonArgs &&args) {
	updateText(args.iconCustomEmojiId, args.text);
	setDisabled(!args.isActive);
	setPointerCursor(false);
	setCursor(args.isActive ? style::cur_pointer : Qt::ForbiddenCursor);
	setVisible(args.isVisible);
	toggleProgress(args.isProgressVisible);
	update();
}

void Panel::Button::toggleProgress(bool shown) {
	if (!_progress) {
		if (!shown) {
			return;
		}
		_progress = std::make_unique<Progress>(
			this,
			[=] { return _progress->widget.rect(); });
		_progress->widget.paintRequest(
		) | rpl::on_next([=](QRect clip) {
			auto p = QPainter(&_progress->widget);
			p.setOpacity(
				_progress->shownAnimation.value(_progress->shown ? 1. : 0.));
			auto thickness = st::paymentsLoading.thickness;
			const auto rect = _progress->widget.rect().marginsRemoved(
				{ thickness, thickness, thickness, thickness });
			InfiniteRadialAnimation::Draw(
				p,
				_progress->animation.computeState(),
				rect.topLeft(),
				rect.size() - QSize(),
				_progress->widget.width(),
				_fg,
				thickness);
		}, _progress->widget.lifetime());
		_progress->widget.show();
		_progress->animation.start();
	} else if (_progress->shown == shown) {
		return;
	}
	const auto callback = [=] {
		if (!_progress->shownAnimation.animating() && !_progress->shown) {
			_progress = nullptr;
		} else {
			_progress->widget.update();
		}
	};
	_progress->shown = shown;
	_progress->shownAnimation.start(
		callback,
		shown ? 0. : 1.,
		shown ? 1. : 0.,
		kProgressDuration);
	if (shown) {
		setupProgressGeometry();
	}
}

void Panel::Button::setupProgressGeometry() {
	if (!_progress || !_progress->shown) {
		return;
	}
	_progress->geometryLifetime.destroy();
	sizeValue(
	) | rpl::on_next([=](QSize outer) {
		const auto height = outer.height();
		const auto size = st::paymentsLoading.size;
		const auto skip = (height - size.height()) / 2;
		const auto right = outer.width();
		const auto top = outer.height() - height;
		_progress->widget.setGeometry(QRect{
			QPoint(right - skip - size.width(), top + skip),
			size });
	}, _progress->geometryLifetime);

	_progress->widget.show();
	_progress->widget.raise();
	if (_progress->shown
		&& Ui::AppInFocus()
		&& Ui::InFocusChain(_progress->widget.window())) {
		_progress->widget.setFocus();
	}
}

void Panel::Button::paintEvent(QPaintEvent *e) {
	Painter p(this);

	_roundRect.paint(p, rect());

	if (!isDisabled()) {
		const auto ripple = ResolveRipple(_bg.color()->c);
		paintRipple(p, rect().topLeft(), &ripple);
	}

	p.setFont(_st.style.font);

	const auto height = rect().height();
	const auto progress = st::paymentsLoading.size;
	const auto minPad = (height - progress.height()) / 2;
	const auto rightReserved = _progress
		? (minPad + progress.width() + minPad)
		: minPad;
	const auto maxTextEnd = width() - rightReserved;
	const auto maxSpace = std::max(maxTextEnd - minPad, 0);
	const auto textWidth = std::min(_text.maxWidth(), maxSpace);
	const auto centered = (width() - textWidth) / 2;
	const auto textLeft = std::max(
		std::min(centered, maxTextEnd - textWidth),
		minPad);
	const auto textTop = _st.padding.top() + _st.textTop;
	p.setPen(_fg);
	_text.drawLeftElided(p, textLeft, textTop, textWidth, width());
}

QImage Panel::Button::prepareRippleMask() const {
	return RippleAnimation::RoundRectMask(size(), st::callRadius);
}

QPoint Panel::Button::prepareRippleStartPosition() const {
	return mapFromGlobal(QCursor::pos())
		- QPoint(_st.padding.left(), _st.padding.top());
}

Panel::WebviewWithLifetime::WebviewWithLifetime(
	QWidget *parent,
	Webview::WindowConfig config)
: window(parent, std::move(config)) {
}

Panel::Progress::Progress(QWidget *parent, Fn<QRect()> rect)
: widget(parent)
, animation(
	[=] { if (!anim::Disabled()) widget.update(rect()); },
	st::paymentsLoading) {
}

Panel::Panel(Args &&args)
: _storageId(args.storageId)
, _delegate(args.delegate)
, _externalShell(UseExternalBotWebApps())
, _menuButtons(args.menuButtons)
, _externalPanelParent(_externalShell ? std::make_unique<RpWidget>() : nullptr)
, _widget(std::make_unique<SeparatePanel>(Ui::SeparatePanelArgs{
			.parent = _externalPanelParent.get(),
			.menuSt = &st::botWebViewMenu,
		}))
, _fullscreen(args.fullscreen)
, _allowClipboardRead(args.allowClipboardRead) {
	if (_externalShell) {
		_widget->setAttribute(Qt::WA_DontShowOnScreen);
	}
	_widget->setWindowFlag(Qt::WindowStaysOnTopHint, false);
	_widget->setInnerSize(st::botWebViewPanelSize, true);

	const auto panel = _widget.get();
	rpl::duplicate(
		args.title
	) | rpl::on_next([=](const QString &title) {
		const auto value = tr::lng_credits_box_history_entry_miniapp(tr::now)
			+ u": "_q
			+ title;
		panel->window()->setWindowTitle(value);
	}, panel->lifetime());

	const auto params = _delegate->botThemeParams();
	updateColorOverrides(params);

	_fullscreen.value(
	) | rpl::on_next([=](bool fullscreen) {
		_widget->toggleFullScreen(fullscreen);
		layoutButtons();
		sendFullScreen();
		sendSafeArea();
		sendContentSafeArea();
	}, _widget->lifetime());

	_widget->fullScreenValue(
	) | rpl::on_next([=](bool fullscreen) {
		_fullscreen = fullscreen;
	}, _widget->lifetime());

	_widget->closeRequests(
	) | rpl::on_next([=] {
		if (_closeNeedConfirmation) {
			scheduleCloseWithConfirmation();
		} else {
			_delegate->botClose();
		}
	}, _widget->lifetime());

	_widget->closeEvents(
	) | rpl::filter([=] {
		return !_hiddenForPayment;
	}) | rpl::on_next([=] {
		_delegate->botClose();
	}, _widget->lifetime());

	_widget->backRequests(
	) | rpl::on_next([=] {
		postEvent("back_button_pressed");
	}, _widget->lifetime());

	rpl::merge(
		style::PaletteChanged(),
		_themeUpdateForced.events()
	) | rpl::filter([=] {
		return !_themeUpdateScheduled;
	}) | rpl::on_next([=] {
		_themeUpdateScheduled = true;
		crl::on_main(_widget.get(), [=] {
			_themeUpdateScheduled = false;
			updateThemeParams(_delegate->botThemeParams());
		});
	}, _widget->lifetime());

	setTitle(std::move(args.title));
	_widget->setTitleBadge(std::move(args.titleBadge));

	if (!showWebview(std::move(args), params)) {
		_externalShell = false;
		const auto available = Webview::Availability();
		if (available.error != Webview::Available::Error::None) {
			showWebviewError(tr::lng_bot_no_webview(tr::now), available);
		} else {
			showCriticalError({ "Error: Could not initialize WebView." });
		}
	}
}

Panel::~Panel() {
	base::take(_webview);
	_progress = nullptr;
	_externalWebviewParent = nullptr;
	_widget = nullptr;
	_externalPanelParent = nullptr;
}

void Panel::setupDownloadsProgress(
		not_null<RpWidget*> button,
		rpl::producer<DownloadsProgress> progress,
		bool fullscreen) {
	const auto widget = Ui::CreateChild<RpWidget>(button.get());
	widget->show();
	widget->setAttribute(Qt::WA_TransparentForMouseEvents);

	button->sizeValue() | rpl::on_next([=](QSize size) {
		widget->setGeometry(QRect(QPoint(), size));
	}, widget->lifetime());

	struct State {
		State(QWidget *parent)
		: animation([=](crl::time now) {
			const auto total = progress.total;
			const auto current = total
				? (progress.ready / float64(total))
				: 0.;
			const auto updated = animation.update(current, false, now);
			if (!anim::Disabled() || updated) {
				parent->update();
			}
		}) {
		}

		DownloadsProgress progress;
		RadialAnimation animation;
		Animations::Simple fade;
		bool shown = false;
	};
	const auto state = widget->lifetime().make_state<State>(widget);
	std::move(
		progress
	) | rpl::on_next([=](DownloadsProgress progress) {
		const auto toggle = [&](bool shown) {
			if (state->shown == shown) {
				return;
			}
			state->shown = shown;
			if (shown && !state->fade.animating()) {
				return;
			}
			state->fade.start([=] {
				widget->update();
				if (!state->shown
					&& !state->fade.animating()
					&& (!state->progress.total
						|| (state->progress.ready
							== state->progress.total))) {
					state->animation.stop();
				}
			}, shown ? 0. : 2., shown ? 2. : 0., st::radialDuration * 2);
		};
		if (!state->shown && progress.loading) {
			if (!state->animation.animating()) {
				state->animation.start(0.);
			}
			toggle(true);
		} else if ((state->progress.total && !progress.total)
			|| (state->progress.ready < state->progress.total
				&& progress.ready == progress.total)) {
			state->animation.update(1., false, crl::now());
			toggle(false);
		}
		state->progress = progress;
	}, widget->lifetime());

	widget->paintRequest() | rpl::on_next([=] {
		const auto opacity = std::clamp(
			state->fade.value(state->shown ? 2. : 0.) - 1.,
			0.,
			1.);
		if (!opacity) {
			return;
		}
		auto p = QPainter(widget);
		p.setOpacity(opacity);
		const auto palette = _widget->titleOverridePalette();
		const auto color = fullscreen
			? st::radialFg
			: palette
			? palette->boxTitleCloseFg()
			: st::paymentsLoading.color;
		const auto &st = fullscreen
			? st::fullScreenPanelMenu
			: st::separatePanelMenu;
		const auto size = st.rippleAreaSize;
		const auto rect = QRect(st.rippleAreaPosition, QSize(size, size));
		const auto stroke = st::botWebViewRadialStroke;
		const auto shift = stroke * 1.5;
		const auto inner = QRectF(rect).marginsRemoved(
			QMarginsF{ shift, shift, shift, shift });
		state->animation.draw(p, inner, stroke, color);
	}, widget->lifetime());
}

void Panel::requestActivate() {
	if (_externalShell) {
		if (_webview) {
			_webview->window.focus();
		}
		return;
	}
	_widget->showAndActivate();
	if (const auto widget = _webview ? _webview->window.widget() : nullptr) {
		InvokeQueued(widget, [=] {
			if (widget->isVisible()) {
				_webview->window.focus();
			}
		});
	}
}

void Panel::toggleProgress(bool shown) {
	if (_externalShell) {
		sendExternalShellMethod("setProgress", { { u"shown"_q, shown } });
		return;
	}
	if (!_progress) {
		if (!shown) {
			return;
		}
		_progress = std::make_unique<Progress>(
			_widget.get(),
			[=] { return progressRect(); });
		_progress->widget.paintRequest(
		) | rpl::on_next([=](QRect clip) {
			auto p = QPainter(&_progress->widget);
			p.setOpacity(
				_progress->shownAnimation.value(_progress->shown ? 1. : 0.));
			const auto thickness = st::paymentsLoading.thickness;
			if (progressWithBackground()) {
				auto color = st::windowBg->c;
				color.setAlphaF(kProgressOpacity);
				p.fillRect(clip, color);
			}
			const auto rect = progressRect() - Margins(thickness);
			InfiniteRadialAnimation::Draw(
				p,
				_progress->animation.computeState(),
				rect.topLeft(),
				rect.size() - QSize(),
				_progress->widget.width(),
				st::paymentsLoading.color,
				anim::Disabled() ? (thickness / 2.) : thickness);
		}, _progress->widget.lifetime());
		_progress->widget.show();
		_progress->animation.start();
	} else if (_progress->shown == shown) {
		return;
	}
	const auto callback = [=] {
		if (!_progress->shownAnimation.animating() && !_progress->shown) {
			_progress = nullptr;
		} else {
			_progress->widget.update();
		}
	};
	_progress->shown = shown;
	_progress->shownAnimation.start(
		callback,
		shown ? 0. : 1.,
		shown ? 1. : 0.,
		kProgressDuration);
	if (shown) {
		setupProgressGeometry();
	}
}

bool Panel::progressWithBackground() const {
	return (_progress->widget.width() == _widget->innerGeometry().width());
}

QRect Panel::progressRect() const {
	const auto rect = _progress->widget.rect();
	if (!progressWithBackground()) {
		return rect;
	}
	const auto size = st::defaultBoxButton.height;
	return QRect(
		rect.x() + (rect.width() - size) / 2,
		rect.y() + (rect.height() - size) / 2,
		size,
		size);
}

void Panel::setupProgressGeometry() {
	if (!_progress || !_progress->shown) {
		return;
	}
	_progress->geometryLifetime.destroy();
	if (_webviewBottom) {
		_webviewBottom->geometryValue(
		) | rpl::on_next([=](QRect bottom) {
			const auto height = bottom.height();
			const auto size = st::paymentsLoading.size;
			const auto skip = (height - size.height()) / 2;
			const auto inner = _widget->innerGeometry();
			const auto right = inner.x() + inner.width();
			const auto top = inner.y() + inner.height() - height;
			// This doesn't work, because first we get the correct bottom
			// geometry and after that we get the previous event (which
			// triggered the 'fire' of correct geometry before getting here).
			//const auto right = bottom.x() + bottom.width();
			//const auto top = bottom.y();
			_progress->widget.setGeometry(QRect{
				QPoint(right - skip - size.width(), top + skip),
				size });
		}, _progress->geometryLifetime);
	}
	_progress->widget.show();
	_progress->widget.raise();
	if (_progress->shown) {
		_progress->widget.setFocus();
	}
}

void Panel::showWebviewProgress() {
	if (_webviewProgress && _progress && _progress->shown) {
		return;
	}
	_webviewProgress = true;
	toggleProgress(true);
}

void Panel::hideWebviewProgress() {
	if (!_webviewProgress) {
		return;
	}
	_webviewProgress = false;
	toggleProgress(false);
}

bool Panel::showWebview(Args &&args, const Webview::ThemeParams &params) {
	_bottomText = std::move(args.bottom);
	_externalUrl = args.url;
	if (!_webview && !createWebview(params)) {
		return false;
	}
	const auto allowBack = false;
	showWebviewProgress();
	if (!_externalShell) {
		_widget->hideLayer(anim::type::instant);
	}
	updateThemeParams(params);
	const auto url = args.url;
	if (_externalShell) {
		_externalShellBootstrapped = false;
		_webview->window.navigate(
			u"https://web.telegram.org/blank.html"_q);
	} else {
		_webview->window.navigate(url);
		_widget->setBackAllowed(allowBack);
	}

	rpl::duplicate(args.downloadsProgress) | rpl::on_next([=] {
		_downloadsUpdated.fire({});
	}, lifetime());

	if (_externalShell) {
		return true;
	}

	_widget->setMenuAllowed([=](
			const Ui::Menu::MenuCallback &callback) {
		auto list = _delegate->botDownloads(true);
		if (!list.empty()) {
			auto value = rpl::single(
				std::move(list)
			) | rpl::then(_downloadsUpdated.events(
			) | rpl::map([=] {
				return _delegate->botDownloads();
			}));
			const auto action = [=](uint32 id, DownloadsAction type) {
				_delegate->botDownloadsAction(id, type);
			};
			callback(Ui::Menu::MenuCallback::Args{
				.text = tr::lng_downloads_section(tr::now),
				.icon = &st::menuIconDownload,
				.fillSubmenu = FillAttachBotDownloadsSubmenu(
					std::move(value),
					action),
			});
			callback({
				.separatorSt = &st::expandedMenuSeparator,
				.isSeparator = true,
			});
		}
		if (_webview && _webview->window.widget() && _hasSettingsButton) {
			callback(tr::lng_bot_settings(tr::now), [=] {
				postEvent("settings_button_pressed");
			}, &st::menuIconSettings);
		}
		if (_menuButtons & MenuButton::OpenBot) {
			callback(tr::lng_bot_open(tr::now), [=] {
				_delegate->botHandleMenuButton(MenuButton::OpenBot);
			}, &st::menuIconLeave);
		}
		callback(tr::lng_bot_reload_page(tr::now), [=] {
			if (_webview && _webview->window.widget()) {
				_webview->window.reload();
			} else if (const auto params = _delegate->botThemeParams()
				; createWebview(params)) {
				showWebviewProgress();
				updateThemeParams(params);
				_webview->window.navigate(url);
			}
		}, &st::menuIconRestore);
		if (_menuButtons & MenuButton::ShareGame) {
			callback(tr::lng_iv_share(tr::now), [=] {
				_delegate->botHandleMenuButton(MenuButton::ShareGame);
			}, &st::menuIconShare);
		} else {
			callback(tr::lng_bot_terms(tr::now), [=] {
				File::OpenUrl(tr::lng_mini_apps_tos_url(tr::now));
			}, &st::menuIconGroupLog);
			callback(tr::lng_bot_privacy(tr::now), [=] {
				_delegate->botOpenPrivacyPolicy();
			}, &st::menuIconAntispam);
		}
		const auto main = (_menuButtons & MenuButton::RemoveFromMainMenu);
		if (main || (_menuButtons & MenuButton::RemoveFromMenu)) {
			const auto handler = [=] {
				_delegate->botHandleMenuButton(main
					? MenuButton::RemoveFromMainMenu
					: MenuButton::RemoveFromMenu);
			};
			callback({
				.text = (main
					? tr::lng_bot_remove_from_side_menu
					: tr::lng_bot_remove_from_menu)(tr::now),
				.handler = handler,
				.icon = &st::menuIconDeleteAttention,
				.isAttention = true,
			});
		}
	}, [=, progress = std::move(args.downloadsProgress)](
			not_null<RpWidget*> button,
			bool fullscreen) {
		setupDownloadsProgress(
			button,
			rpl::duplicate(progress),
			fullscreen);
	});

	return true;
}

void Panel::installExternalShellDocument() {
	if (!_webview) {
		return;
	}
	_webview->window.eval(ExternalShellInstallScript());
}

void Panel::sendExternalShellBootstrap() {
	const auto params = _delegate->botThemeParams();
	sendExternalShellMethod("bootstrap", {
		{ u"url"_q, _externalUrl },
		{ u"title"_q, _externalTitle },
		{ u"backVisible"_q, _externalBackVisible },
		{ u"settingsVisible"_q, _hasSettingsButton },
		{ u"bodyBg"_q, ColorValue(params.bodyBg) },
		{ u"titleBg"_q, ColorValue(params.titleBg) },
		{ u"bottomBg"_q, _bottomBarColor
			? QJsonValue(_bottomBarColor->name(QColor::HexRgb))
			: ColorValue(params.bodyBg) },
	});
	postEvent("theme_changed", "{\"theme_params\": " + params.json + "}");
	sendFullScreen();
	sendSafeArea();
	sendContentSafeArea();
}

void Panel::sendExternalShellMethod(
		const QByteArray &method,
		const QJsonObject &data) {
	if (!_webview) {
		return;
	}
	const auto payload = JsonObject(data);
	auto script = QByteArray();
	script.reserve(method.size() * 2 + payload.size() + 96);
	script += "if (window.TelegramDesktopShell"
		" && window.TelegramDesktopShell.";
	script += method;
	script += ") { window.TelegramDesktopShell.";
	script += method;
	script += "(";
	script += payload;
	script += "); }";
	_webview->window.eval(script);
}

void Panel::sendExternalShellEvent(
		const QString &event,
		const QByteArray &data) {
	if (!_webview) {
		return;
	}
	auto script = QByteArray();
	script += "if (window.TelegramDesktopShell) {"
		"window.TelegramDesktopShell.nativeEvent(";
	script += JsonValue(event);
	script += ", ";
	script += (data.isEmpty() ? QByteArray("{}") : data);
	script += "); }";
	_webview->window.eval(script);
}

void Panel::sendExternalShellButton(
		const char *name,
		const QJsonObject &args) {
	const auto text = args["text"].toString().trimmed();
	const auto visible = args["is_visible"].toBool() && !text.isEmpty();
	sendExternalShellMethod("setButton", {
		{ u"name"_q, QString::fromLatin1(name) },
		{ u"visible"_q, visible },
		{ u"active"_q, args["is_active"].toBool() },
		{ u"progress"_q, args["is_progress_visible"].toBool() },
		{ u"text"_q, args["text"].toString() },
		{ u"color"_q, args["color"].toString() },
		{ u"textColor"_q, args["text_color"].toString() },
		{ u"position"_q, args["position"].toString() },
	});
}

void Panel::sendExternalShellChrome() {
	sendExternalShellMethod("setChrome", {
		{ u"backVisible"_q, _externalBackVisible },
		{ u"settingsVisible"_q, _hasSettingsButton },
	});
}

void Panel::setExternalShellBlocked(bool blocked) {
	if (!_externalShell) {
		return;
	}
	const auto was = (_externalBlockCount > 0);
	if (blocked) {
		++_externalBlockCount;
	} else if (_externalBlockCount > 0) {
		--_externalBlockCount;
	}
	const auto now = (_externalBlockCount > 0);
	if (was != now) {
		sendExternalShellMethod("setBlocked", { { u"blocked"_q, now } });
	}
}

Webview::PopupResult Panel::showBlockingPopup(Webview::PopupArgs &&args) {
	if (!_externalShell) {
		return Webview::ShowBlockingPopup(std::move(args));
	}
	args.parent = nullptr;
	setExternalShellBlocked(true);
	const auto weak = base::make_weak(this);
	const auto guard = gsl::finally([=] {
		if (weak) {
			weak->setExternalShellBlocked(false);
		}
	});
	return Webview::ShowBlockingPopup(std::move(args));
}

void Panel::createWebviewBottom() {
	_webviewBottom = std::make_unique<RpWidget>(_widget.get());
	const auto bottom = _webviewBottom.get();
	bottom->setVisible(!_fullscreen.current());

	const auto &padding = st::paymentsPanelPadding;
	const auto label = CreateChild<FlatLabel>(
		_webviewBottom.get(),
		_bottomText.value(),
		st::paymentsWebviewBottom);
	_webviewBottomLabel = label;

	const auto height = padding.top()
		+ label->heightNoMargins()
		+ padding.bottom();
	rpl::combine(
		_webviewBottom->widthValue(),
		label->widthValue()
	) | rpl::on_next([=](int outerWidth, int width) {
		label->move((outerWidth - width) / 2, padding.top());
	}, label->lifetime());
	label->show();
	_webviewBottom->resize(_webviewBottom->width(), height);

	rpl::combine(
		_webviewParent->geometryValue() | rpl::map([=] {
			return _widget->innerGeometry();
		}),
		bottom->heightValue()
	) | rpl::on_next([=](QRect inner, int height) {
		bottom->move(inner.x(), inner.y() + inner.height() - height);
		bottom->resizeToWidth(inner.width());
		layoutButtons();
	}, bottom->lifetime());
}

bool Panel::createWebview(const Webview::ThemeParams &params) {
	RpWidget *container = nullptr;
	if (_externalShell) {
		_externalWebviewParent = std::make_unique<RpWidget>();
		container = _externalWebviewParent.get();
	} else {
		auto outer = base::make_unique_q<RpWidget>(_widget.get());
		container = outer.get();
		_widget->showInner(std::move(outer));
	}
	_webviewParent = container;

	_headerColorReceived = false;
	_bodyColorReceived = false;
	_bottomColorReceived = false;
	updateColorOverrides(params);
	if (!_externalShell) {
		createWebviewBottom();
	}

	if (!_externalShell) {
		container->show();
	}
	_webview = std::make_unique<WebviewWithLifetime>(
		container,
		Webview::WindowConfig{
			.opaqueBg = params.bodyBg,
			.storageId = _storageId,
			.mode = _externalShell
				? Webview::WindowMode::External
				: Webview::WindowMode::Embedded,
		});
	const auto raw = &_webview->window;

	const auto bottom = _webviewBottom.get();
	QObject::connect(container, &QObject::destroyed, [=] {
		if (_webview && &_webview->window == raw) {
			base::take(_webview);
			if (_webviewProgress) {
				hideWebviewProgress();
				if (_progress && !_progress->shown) {
					_progress = nullptr;
				}
			}
		}
		if (_webviewBottom.get() == bottom) {
			_webviewBottomLabel = nullptr;
			_webviewBottom = nullptr;
			_secondaryButton = nullptr;
			_mainButton = nullptr;
			_bottomButtonsBg = nullptr;
		}
	});
	if (!raw->widget()) {
		return false;
	}

#if !defined Q_OS_WIN && !defined Q_OS_MAC
	if (!_externalShell) {
		_widget->allowChildFullScreenControls(
			!raw->widget()->inherits("QWindowContainer"));
	}
#endif // !Q_OS_WIN && !Q_OS_MAC

	raw->setInteractionHandler([=] {
		_lastWebviewInteraction = crl::now();
	});

	QObject::connect(raw->widget(), &QObject::destroyed, [=] {
		const auto parent = _webviewParent.data();
		if (!_webview
			|| &_webview->window != raw
			|| (!_externalShell
				&& (!parent || _widget->inner() != parent))) {
			// If we destroyed _webview ourselves,
			// or if we changed _widget->inner ourselves,
			// we don't show any message, nothing crashed.
			return;
		}
		crl::on_main(this, [=] {
			if (_externalShell) {
				_delegate->botClose();
			} else {
				showCriticalError({ "Error: WebView has crashed." });
			}
		});
	});

	if (_externalShell) {
		raw->resize(st::botWebViewPanelSize);
		raw->widget()->resize(st::botWebViewPanelSize);
	} else {
		rpl::combine(
			container->geometryValue(),
			_footerHeight.value()
		) | rpl::on_next([=](QRect geometry, int footer) {
			if (const auto view = raw->widget()) {
				view->setGeometry(geometry.marginsRemoved({ 0, 0, 0, footer }));
				crl::on_main(view, [=] {
					sendViewport();
					InvokeQueued(view, [=] { sendViewport(); });
				});
			}
		}, _webview->lifetime);
	}

	raw->setMessageHandler([=](const QJsonDocument &message) {
		if (!message.isArray()) {
			LOG(("BotWebView Error: "
				"Not an array received in buy_callback arguments."));
			return;
		}
		const auto list = message.array();
		const auto command = list.at(0).toString();
		const auto arguments = ParseMethodArgs(list.at(1).toString());
		if (command == "web_app_close") {
			_delegate->botClose();
		} else if (command == "tdesktop_shell_close") {
			if (_closeNeedConfirmation) {
				scheduleCloseWithConfirmation();
			} else {
				_delegate->botClose();
			}
		} else if (command == "web_app_data_send") {
			sendDataMessage(arguments);
		} else if (command == "web_app_switch_inline_query") {
			switchInlineQueryMessage(arguments);
		} else if (command == "web_app_setup_main_button") {
			processButtonMessage(_mainButton, arguments);
		} else if (command == "web_app_setup_secondary_button") {
			processButtonMessage(_secondaryButton, arguments);
		} else if (command == "web_app_setup_back_button") {
			processBackButtonMessage(arguments);
		} else if (command == "web_app_setup_settings_button") {
			processSettingsButtonMessage(arguments);
		} else if (command == "web_app_request_theme") {
			_themeUpdateForced.fire({});
		} else if (command == "web_app_request_viewport") {
			sendViewport();
		} else if (command == "web_app_request_safe_area") {
			sendSafeArea();
		} else if (command == "web_app_request_content_safe_area") {
			sendContentSafeArea();
		} else if (command == "web_app_request_fullscreen") {
			if (!_fullscreen.current()) {
				_fullscreen = true;
			} else {
				sendFullScreen();
			}
		} else if (command == "web_app_request_file_download") {
			processDownloadRequest(arguments);
		} else if (command == "web_app_exit_fullscreen") {
			if (_fullscreen.current()) {
				_fullscreen = false;
			} else {
				sendFullScreen();
			}
		} else if (command == "web_app_check_home_screen") {
			postEvent("home_screen_checked", "{ status: \"unsupported\" }");
		} else if (command == "web_app_start_accelerometer") {
			postEvent("accelerometer_failed", "{ error: \"UNSUPPORTED\" }");
		} else if (command == "web_app_start_device_orientation") {
			postEvent(
				"device_orientation_failed",
				"{ error: \"UNSUPPORTED\" }");
		} else if (command == "web_app_start_gyroscope") {
			postEvent("gyroscope_failed", "{ error: \"UNSUPPORTED\" }");
		} else if (command == "web_app_check_location") {
			postEvent("location_checked", "{ available: false }");
		} else if (command == "web_app_request_location") {
			postEvent("location_requested", "{ available: false }");
		} else if (command == "web_app_biometry_get_info") {
			postEvent("biometry_info_received", "{ available: false }");
		} else if (command == "web_app_open_tg_link") {
			openTgLink(arguments);
		} else if (command == "web_app_open_link") {
			openExternalLink(arguments);
		} else if (command == "web_app_open_invoice") {
			openInvoice(arguments);
		} else if (command == "web_app_open_popup") {
			openPopup(arguments);
		} else if (command == "web_app_open_scan_qr_popup") {
			openScanQrPopup(arguments);
		} else if (command == "web_app_share_to_story") {
			openShareStory(arguments);
		} else if (command == "web_app_request_write_access") {
			requestWriteAccess();
		} else if (command == "web_app_request_phone") {
			requestPhone();
		} else if (command == "web_app_invoke_custom_method") {
			invokeCustomMethod(arguments);
		} else if (command == "web_app_setup_closing_behavior") {
			setupClosingBehaviour(arguments);
		} else if (command == "web_app_read_text_from_clipboard") {
			requestClipboardText(arguments);
		} else if (command == "web_app_set_header_color") {
			processHeaderColor(arguments);
		} else if (command == "web_app_set_background_color") {
			processBackgroundColor(arguments);
		} else if (command == "web_app_set_bottom_bar_color") {
			processBottomBarColor(arguments);
		} else if (command == "web_app_send_prepared_message") {
			processSendMessageRequest(arguments);
		} else if (command == "web_app_request_chat") {
			processRequestChat(arguments);
		} else if (command == "web_app_set_emoji_status") {
			processEmojiStatusRequest(arguments);
		} else if (command == "web_app_request_emoji_status_access") {
			processEmojiStatusAccessRequest();
		} else if (command == "web_app_device_storage_save_key") {
			processStorageSaveKey(arguments);
		} else if (command == "web_app_device_storage_get_key") {
			processStorageGetKey(arguments);
		} else if (command == "web_app_device_storage_clear") {
			processStorageClear(arguments);
		} else if (command == "web_app_secure_storage_save_key") {
			secureStorageFailed(arguments);
		} else if (command == "web_app_secure_storage_get_key") {
			secureStorageFailed(arguments);
		} else if (command == "web_app_secure_storage_restore_key") {
			secureStorageFailed(arguments);
		} else if (command == "web_app_secure_storage_clear") {
			secureStorageFailed(arguments);
		} else if (command == "web_app_verify_age") {
			const auto passed = arguments["passed"];
			const auto detected = arguments["age"];
			const auto valid = passed.isBool()
				&& passed.toBool()
				&& detected.isDouble();
			const auto age = valid
				? int(std::floor(detected.toDouble()))
				: 0;
			_delegate->botVerifyAge(age);
		} else if (command == "share_score") {
			_delegate->botHandleMenuButton(MenuButton::ShareGame);
		}
	});

	raw->setNavigationStartHandler([=](const QString &uri, bool newWindow) {
		if (_delegate->botHandleLocalUri(uri, false)) {
			return false;
		} else if (newWindow) {
			return true;
		}
		showWebviewProgress();
		return true;
	});
	raw->setNavigationDoneHandler([=](bool success) {
		hideWebviewProgress();
		if (_externalShell && !_externalShellBootstrapped) {
			if (!success) {
				_externalShell = false;
				showCriticalError({ "Error: Could not initialize WebView." });
				_widget->showAndActivate();
				return;
			}
			_externalShellBootstrapped = true;
			installExternalShellDocument();
			sendExternalShellBootstrap();
		}
	});
	if (_externalShell) {
		raw->setDialogHandler([=](Webview::DialogArgs args) {
			args.parent = nullptr;
			setExternalShellBlocked(true);
			const auto weak = base::make_weak(this);
			const auto guard = gsl::finally([=] {
				if (weak) {
					weak->setExternalShellBlocked(false);
				}
			});
			return Webview::DefaultDialogHandler(std::move(args));
		});
	}

	auto initScript = QByteArray(R"(
window.TelegramWebviewProxy = {
postEvent: function(eventType, eventData) {
	if (window.external && window.external.invoke) {
		window.external.invoke(JSON.stringify([eventType, eventData]));
	}
}
};)");
		raw->init(initScript);

	if (!_webview) {
		return false;
	}

	if (!_externalShell) {
		layoutButtons();
		setupProgressGeometry();
	}

	base::qt_signal_producer(
		qApp,
		&QGuiApplication::focusWindowChanged
	) | rpl::filter([=](QWindow *focused) {
		const auto handle = _widget->window()->windowHandle();
		const auto widget = _webview ? _webview->window.widget() : nullptr;
		return widget
			&& !widget->isHidden()
			&& handle
			&& (focused == handle);
	}) | rpl::on_next([=] {
		_webview->window.focus();
	}, _webview->lifetime);

	return true;
}

void Panel::sendViewport() {
	if (_externalShell) {
		sendExternalShellMethod("sendViewport", {});
		return;
	}
	postEvent("viewport_changed", "{ "
		"height: window.innerHeight, "
		"is_state_stable: true, "
		"is_expanded: true }");
}

void Panel::sendFullScreen() {
	postEvent("fullscreen_changed", _fullscreen.current()
		? "{ is_fullscreen: true }"
		: "{ is_fullscreen: false }");
}

void Panel::sendSafeArea() {
	postEvent("safe_area_changed",
		"{ top: 0, right: 0, bottom: 0, left: 0 }");
}

void Panel::sendContentSafeArea() {
	const auto shift = st::separatePanelClose.rippleAreaPosition.y();
	const auto top = _fullscreen.current()
		? (shift + st::fullScreenPanelClose.height + (shift / 2))
		: 0;
	const auto scaled = top * style::DevicePixelRatio();
	auto report = 0;
	if (const auto screen = QGuiApplication::primaryScreen()) {
		const auto dpi = screen->logicalDotsPerInch();
		const auto ratio = screen->devicePixelRatio();
		const auto basePair = screen->handle()->logicalBaseDpi();
		const auto base = (basePair.first + basePair.second) * 0.5;
		const auto systemScreenScale = dpi * ratio / base;
		report = int(base::SafeRound(scaled / systemScreenScale));
	}
	postEvent("content_safe_area_changed",
		u"{ top: %1, right: 0, bottom: 0, left: 0 }"_q.arg(report));
}

void Panel::setTitle(rpl::producer<QString> title) {
	if (!_externalShell) {
		_widget->setTitle(std::move(title));
		return;
	}
	std::move(title) | rpl::on_next([=](const QString &title) {
		_externalTitle = title;
		sendExternalShellMethod("setTitle", { { u"title"_q, title } });
	}, _widget->lifetime());
}

void Panel::sendDataMessage(const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	}
	const auto data = args["data"].toString();
	if (data.isEmpty()) {
		LOG(("BotWebView Error: Bad 'data' in sendDataMessage."));
		_delegate->botClose();
		return;
	}
	_delegate->botSendData(data.toUtf8());
}

void Panel::switchInlineQueryMessage(const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	} else if (!args.contains("query")) {
		LOG(("BotWebView Error: No 'query' in switchInlineQueryMessage."));
		_delegate->botClose();
		return;
	}
	const auto query = args["query"].toString();
	const auto valid = base::flat_set<QString>{
		u"users"_q,
		u"bots"_q,
		u"groups"_q,
		u"channels"_q,
	};
	const auto typeArray = args["chat_types"].toArray();
	auto types = std::vector<QString>();
	for (const auto &value : typeArray) {
		const auto type = value.toString();
		if (valid.contains(type)) {
			types.push_back(type);
		} else {
			LOG(("BotWebView Error: "
				"Bad chat type in switchInlineQueryMessage: %1.").arg(type));
			types.clear();
			break;
		}
	}
	_delegate->botSwitchInlineQuery(types, query);
}

void Panel::processSendMessageRequest(const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	}
	const auto id = args["id"].toString();
	auto callback = crl::guard(this, [=](QString error) {
		if (error.isEmpty()) {
			postEvent("prepared_message_sent");
		} else {
			postEvent(
				"prepared_message_failed",
				u"{ error: \"%1\" }"_q.arg(error));
		}
	});
	_delegate->botSendPreparedMessage({
		.id = id,
		.callback = std::move(callback),
	});
}

void Panel::processRequestChat(const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	}
	const auto requestId = args["req_id"].toString();
	if (requestId.isEmpty()) {
		return;
	}
	auto callback = crl::guard(this, [=](QString error) {
		if (error.isEmpty()) {
			postEvent(
				"requested_chat_sent",
				u"{ req_id: \"%1\" }"_q.arg(requestId));
		} else {
			postEvent(
				"requested_chat_failed",
				u"{ req_id: \"%1\", error: \"%2\" }"_q.arg(
					requestId,
					error));
		}
	});
	_delegate->botRequestChat({
		.requestId = requestId,
		.callback = std::move(callback),
	});
}

void Panel::processEmojiStatusRequest(const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	}
	const auto emojiId = args["custom_emoji_id"].toString().toULongLong();
	const auto duration = TimeId(base::SafeRound(
		args["duration"].toDouble()));
	if (!emojiId) {
		postEvent(
			"emoji_status_failed",
			"{ error: \"SUGGESTED_EMOJI_INVALID\" }");
		return;
	} else if (duration < 0) {
		postEvent(
			"emoji_status_failed",
			"{ error: \"DURATION_INVALID\" }");
		return;
	}
	auto callback = crl::guard(this, [=](QString error) {
		if (error.isEmpty()) {
			postEvent("emoji_status_set");
		} else {
			postEvent(
				"emoji_status_failed",
				u"{ error: \"%1\" }"_q.arg(error));
		}
	});
	_delegate->botSetEmojiStatus({
		.customEmojiId = emojiId,
		.duration = duration,
		.callback = std::move(callback),
	});
}

void Panel::processEmojiStatusAccessRequest() {
	auto callback = crl::guard(this, [=](bool allowed) {
		postEvent("emoji_status_access_requested", allowed
			? "{ status: \"allowed\" }"
			: "{ status: \"cancelled\" }");
	});
	_delegate->botRequestEmojiStatusAccess(std::move(callback));
}

void Panel::processStorageSaveKey(const QJsonObject &args) {
	const auto keyObject = args["key"];
	const auto valueObject = args["value"];
	const auto key = keyObject.toString();
	if (!keyObject.isString()) {
		deviceStorageFailed(args, u"KEY_INVALID"_q);
	} else if (valueObject.isNull()) {
		_delegate->botStorageWrite(key, std::nullopt);
		replyDeviceStorage(args, u"device_storage_key_saved"_q, {});
	} else if (!valueObject.isString()) {
		deviceStorageFailed(args, u"VALUE_INVALID"_q);
	} else if (_delegate->botStorageWrite(key, valueObject.toString())) {
		replyDeviceStorage(args, u"device_storage_key_saved"_q, {});
	} else {
		deviceStorageFailed(args, u"QUOTA_EXCEEDED"_q);
	}
}

void Panel::processStorageGetKey(const QJsonObject &args) {
	const auto keyObject = args["key"];
	const auto key = keyObject.toString();
	if (!keyObject.isString()) {
		deviceStorageFailed(args, u"KEY_INVALID"_q);
	} else {
		const auto value = _delegate->botStorageRead(key);
		replyDeviceStorage(args, u"device_storage_key_received"_q, {
			{ u"value"_q, value ? QJsonValue(*value) : QJsonValue::Null },
		});
	}
}

void Panel::processStorageClear(const QJsonObject &args) {
	_delegate->botStorageClear();
	replyDeviceStorage(args, u"device_storage_cleared"_q, {});
}

void Panel::replyDeviceStorage(
		const QJsonObject &args,
		const QString &event,
		QJsonObject response) {
	response[u"req_id"_q] = args[u"req_id"_q];
	postEvent(event, response);
}

void Panel::deviceStorageFailed(const QJsonObject &args, QString error) {
	replyDeviceStorage(args, u"device_storage_failed"_q, {
		{ u"error"_q, error },
	});
}

void Panel::secureStorageFailed(const QJsonObject &args) {
	postEvent(u"secure_storage_failed"_q, QJsonObject{
		{ u"req_id"_q, args["req_id"] },
		{ u"error"_q, u"UNSUPPORTED"_q },
	});
}

void Panel::openTgLink(const QJsonObject &args) {
	if (args.isEmpty()) {
		LOG(("BotWebView Error: Bad arguments in 'web_app_open_tg_link'."));
		_delegate->botClose();
		return;
	}
	const auto path = args["path_full"].toString();
	if (path.isEmpty()) {
		LOG(("BotWebView Error: Bad 'path_full' in 'web_app_open_tg_link'."));
		_delegate->botClose();
		return;
	}
	_delegate->botHandleLocalUri("https://t.me" + path, true);
}

void Panel::openExternalLink(const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	}
	const auto iv = args["try_instant_view"].toBool();
	const auto url = args["url"].toString();
	if (!_delegate->botValidateExternalLink(url)) {
		LOG(("BotWebView Error: Bad url in openExternalLink: %1").arg(url));
		_delegate->botClose();
		return;
	} else if (!allowOpenLink()) {
		return;
	} else if (iv) {
		_delegate->botOpenIvLink(url);
	} else {
		File::OpenUrl(url);
	}
}

void Panel::openInvoice(const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	}
	const auto slug = args["slug"].toString();
	if (slug.isEmpty()) {
		LOG(("BotWebView Error: Bad 'slug' in openInvoice."));
		_delegate->botClose();
		return;
	}
	_delegate->botHandleInvoice(slug);
}

void Panel::openPopup(const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	}
	using Button = Webview::PopupArgs::Button;
	using Type = Button::Type;
	const auto message = args["message"].toString();
	const auto types = base::flat_map<QString, Button::Type>{
		{ "default", Type::Default },
		{ "ok", Type::Ok },
		{ "close", Type::Close },
		{ "cancel", Type::Cancel },
		{ "destructive", Type::Destructive },
	};
	const auto buttonArray = args["buttons"].toArray();
	auto buttons = std::vector<Webview::PopupArgs::Button>();
	for (const auto button : buttonArray) {
		const auto fields = button.toObject();
		const auto i = types.find(fields["type"].toString());
		if (i == end(types)) {
			LOG(("BotWebView Error: Bad 'type' in openPopup buttons."));
			_delegate->botClose();
			return;
		}
		buttons.push_back({
			.id = fields["id"].toString(),
			.text = fields["text"].toString(),
			.type = i->second,
		});
	}
	if (message.isEmpty()) {
		LOG(("BotWebView Error: Bad 'message' in openPopup."));
		_delegate->botClose();
		return;
	} else if (buttons.empty()) {
		LOG(("BotWebView Error: Bad 'buttons' in openPopup."));
		_delegate->botClose();
		return;
	}
	const auto widget = _webview->window.widget();
	const auto weak = base::make_weak(this);
	const auto result = showBlockingPopup({
		.parent = widget ? widget->window() : nullptr,
		.title = args["title"].toString(),
		.text = message,
		.buttons = std::move(buttons),
	});
	if (weak) {
		postEvent("popup_closed", result.id
			? QJsonObject{ { u"button_id"_q, *result.id } }
			: EventData());
	}
}

void Panel::openScanQrPopup(const QJsonObject &args) {
	const auto widget = _webview->window.widget();
	[[maybe_unused]] const auto ok = showBlockingPopup({
		.parent = widget ? widget->window() : nullptr,
		.text = tr::lng_bot_no_scan_qr(tr::now),
		.buttons = { {
			.id = "ok",
			.text = tr::lng_box_ok(tr::now),
			.type = Webview::PopupArgs::Button::Type::Ok,
		}},
	});
}

void Panel::openShareStory(const QJsonObject &args) {
	const auto widget = _webview->window.widget();
	[[maybe_unused]] const auto ok = showBlockingPopup({
		.parent = widget ? widget->window() : nullptr,
		.text = tr::lng_bot_no_share_story(tr::now),
		.buttons = { {
			.id = "ok",
			.text = tr::lng_box_ok(tr::now),
			.type = Webview::PopupArgs::Button::Type::Ok,
		}},
	});
}

void Panel::requestWriteAccess() {
	if (_inBlockingRequest) {
		replyRequestWriteAccess(false);
		return;
	}
	_inBlockingRequest = true;
	const auto finish = [=](bool allowed) {
		_inBlockingRequest = false;
		replyRequestWriteAccess(allowed);
	};
	const auto weak = base::make_weak(this);
	_delegate->botCheckWriteAccess([=](bool allowed) {
		if (!weak) {
			return;
		} else if (allowed) {
			finish(true);
			return;
		}
		using Button = Webview::PopupArgs::Button;
		const auto widget = _webview->window.widget();
		const auto integration = &Ui::Integration::Instance();
		const auto result = showBlockingPopup({
			.parent = widget ? widget->window() : nullptr,
			.title = integration->phraseBotAllowWriteTitle(),
			.text = integration->phraseBotAllowWrite(),
			.buttons = {
				{
					.id = "allow",
					.text = integration->phraseBotAllowWriteConfirm(),
				},
				{ .id = "cancel", .type = Button::Type::Cancel },
			},
		});
		if (!weak) {
			return;
		} else if (result.id == "allow") {
			_delegate->botAllowWriteAccess(crl::guard(this, finish));
		} else {
			finish(false);
		}
	});
}

void Panel::replyRequestWriteAccess(bool allowed) {
	postEvent("write_access_requested", QJsonObject{
		{ u"status"_q, allowed ? u"allowed"_q : u"cancelled"_q }
	});
}

void Panel::requestPhone() {
	if (_inBlockingRequest) {
		replyRequestPhone(false);
		return;
	}
	_inBlockingRequest = true;
	const auto finish = [=](bool shared) {
		_inBlockingRequest = false;
		replyRequestPhone(shared);
	};
	using Button = Webview::PopupArgs::Button;
	const auto widget = _webview->window.widget();
	const auto weak = base::make_weak(this);
	const auto integration = &Ui::Integration::Instance();
	const auto result = showBlockingPopup({
		.parent = widget ? widget->window() : nullptr,
		.title = integration->phraseBotSharePhoneTitle(),
		.text = integration->phraseBotSharePhone(),
		.buttons = {
			{
				.id = "share",
				.text = integration->phraseBotSharePhoneConfirm(),
			},
			{ .id = "cancel", .type = Button::Type::Cancel },
		},
	});
	if (!weak) {
		return;
	} else if (result.id == "share") {
		_delegate->botSharePhone(crl::guard(this, finish));
	} else {
		finish(false);
	}
}

void Panel::replyRequestPhone(bool shared) {
	postEvent("phone_requested", QJsonObject{
		{ u"status"_q, shared ? u"sent"_q : u"cancelled"_q }
	});
}

void Panel::invokeCustomMethod(const QJsonObject &args) {
	const auto requestId = args["req_id"];
	if (requestId.isUndefined()) {
		return;
	}
	const auto finish = [=](QJsonObject response) {
		replyCustomMethod(requestId, std::move(response));
	};
	auto callback = crl::guard(this, [=](CustomMethodResult result) {
		if (result) {
			auto error = QJsonParseError();
			const auto parsed = QJsonDocument::fromJson(
				"{ \"result\": " + *result + '}',
				&error);
			if (error.error != QJsonParseError::NoError
				|| !parsed.isObject()
				|| parsed.object().size() != 1) {
				finish({ { u"error"_q, u"Could not parse response."_q } });
			} else {
				finish(parsed.object());
			}
		} else {
			finish({ { u"error"_q, result.error() } });
		}
	});
	const auto params = QJsonDocument(
		args["params"].toObject()
	).toJson(QJsonDocument::Compact);
	_delegate->botInvokeCustomMethod({
		.method = args["method"].toString(),
		.params = params,
		.callback = std::move(callback),
	});
}

void Panel::replyCustomMethod(QJsonValue requestId, QJsonObject response) {
	response["req_id"] = requestId;
	postEvent(u"custom_method_invoked"_q, response);
}

void Panel::requestClipboardText(const QJsonObject &args) {
	const auto requestId = args["req_id"];
	if (requestId.isUndefined()) {
		return;
	}
	auto result = QJsonObject();
	result["req_id"] = requestId;
	if (allowClipboardQuery()) {
		result["data"] = QGuiApplication::clipboard()->text();
	}
	postEvent(u"clipboard_text_received"_q, result);
}

bool Panel::allowOpenLink() const {
	//const auto now = crl::now();
	//if (_mainButtonLastClick
	//	&& _mainButtonLastClick + kProcessClickTimeout >= now) {
	//	_mainButtonLastClick = 0;
	//	return true;
	//}
	return true;
}

bool Panel::allowClipboardQuery() const {
	if (!_allowClipboardRead) {
		return false;
	}
	const auto now = crl::now();
	return _lastWebviewInteraction
		&& (_lastWebviewInteraction + kClipboardReadTimeout >= now);
}

void Panel::scheduleCloseWithConfirmation() {
	if (!_closeWithConfirmationScheduled) {
		_closeWithConfirmationScheduled = true;
		InvokeQueued(_widget.get(), [=] { closeWithConfirmation(); });
	}
}

void Panel::closeWithConfirmation() {
	using Button = Webview::PopupArgs::Button;
	const auto widget = _webview->window.widget();
	const auto weak = base::make_weak(this);
	const auto integration = &Ui::Integration::Instance();
	const auto result = showBlockingPopup({
		.parent = widget ? widget->window() : nullptr,
		.title = integration->phrasePanelCloseWarning(),
		.text = integration->phrasePanelCloseUnsaved(),
		.buttons = {
			{
				.id = "close",
				.text = integration->phrasePanelCloseAnyway(),
				.type = Button::Type::Destructive,
			},
			{ .id = "cancel", .type = Button::Type::Cancel },
		},
		.ignoreFloodCheck = true,
	});
	if (!weak) {
		return;
	} else if (result.id == "close") {
		_delegate->botClose();
	} else {
		_closeWithConfirmationScheduled = false;
	}
}

void Panel::setupClosingBehaviour(const QJsonObject &args) {
	_closeNeedConfirmation = args["need_confirmation"].toBool();
}

void Panel::processButtonMessage(
		std::unique_ptr<Button> &button,
		const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	}
	if (_externalShell) {
		sendExternalShellButton(
			(&button == &_mainButton) ? "main" : "secondary",
			args);
		sendViewport();
		return;
	}

	const auto shown = [&] {
		return button && !button->isHidden();
	};
	const auto wasShown = shown();
	const auto guard = gsl::finally([&] {
		if (shown() != wasShown) {
			crl::on_main(this, [=] {
				sendViewport();
			});
		}
	});

	const auto text = args["text"].toString().trimmed();
	const auto iconCustomEmojiId
		= args["icon_custom_emoji_id"].toString().toULongLong();
	const auto visible = args["is_visible"].toBool()
		&& (!text.isEmpty() || iconCustomEmojiId);
	if (!button) {
		if (visible) {
			createButton(button);
			_bottomButtonsBg->show();
		} else {
			return;
		}
	}

	if (const auto bg = ParseColor(args["color"].toString())) {
		button->updateBg(*bg);
	} else {
		button->updateBg(&st::windowBgActive);
	}

	if (const auto fg = ParseColor(args["text_color"].toString())) {
		button->updateFg(*fg);
	} else {
		button->updateFg(&st::windowFgActive);
	}

	button->updateArgs({
		.isActive = args["is_active"].toBool(),
		.isVisible = visible,
		.isProgressVisible = args["is_progress_visible"].toBool(),
		.iconCustomEmojiId = iconCustomEmojiId,
		.text = args["text"].toString(),
	});
	if (button.get() == _secondaryButton.get()) {
		const auto position = ParsePosition(args["position"].toString());
		if (_secondaryPosition != position) {
			_secondaryPosition = position;
			layoutButtons();
		}
	}
}

void Panel::processBackButtonMessage(const QJsonObject &args) {
	if (_externalShell) {
		_externalBackVisible = args["is_visible"].toBool();
		sendExternalShellChrome();
		return;
	}
	_widget->setBackAllowed(args["is_visible"].toBool());
}

void Panel::processSettingsButtonMessage(const QJsonObject &args) {
	_hasSettingsButton = args["is_visible"].toBool();
	if (_externalShell) {
		sendExternalShellChrome();
	}
}

void Panel::processHeaderColor(const QJsonObject &args) {
	_headerColorReceived = true;
	if (_externalShell) {
		if (const auto color = ParseColor(args["color"].toString())) {
			sendExternalShellMethod(
				"setColors",
				{ { u"titleBg"_q, color->name(QColor::HexRgb) } });
		} else if (const auto color = LookupNamedColor(
				args["color_key"].toString())) {
			sendExternalShellMethod(
				"setColors",
				{ { u"titleBg"_q, (*color)->c.name(QColor::HexRgb) } });
		}
		return;
	}
	if (const auto color = ParseColor(args["color"].toString())) {
		_widget->overrideTitleColor(color);
		_headerColorLifetime.destroy();
	} else if (const auto color = LookupNamedColor(
			args["color_key"].toString())) {
		_widget->overrideTitleColor((*color)->c);
		_headerColorLifetime = style::PaletteChanged(
		) | rpl::on_next([=] {
			_widget->overrideTitleColor((*color)->c);
		});
	} else {
		_widget->overrideTitleColor(std::nullopt);
		_headerColorLifetime.destroy();
	}
}

void Panel::overrideBodyColor(std::optional<QColor> color) {
	if (_externalShell) {
		if (color) {
			sendExternalShellMethod(
				"setColors",
				{ { u"bodyBg"_q, color->name(QColor::HexRgb) } });
		}
		return;
	}
	_widget->overrideBodyColor(color);
	const auto raw = _webviewBottomLabel.data();
	if (!raw) {
		return;
	} else if (!color) {
		raw->setTextColorOverride(std::nullopt);
		return;
	}
	const auto contrast = 2.5;
	const auto luminance = 0.2126 * color->redF()
		+ 0.7152 * color->greenF()
		+ 0.0722 * color->blueF();
	const auto textColor = (luminance > 0.5)
		? QColor(0, 0, 0)
		: QColor(255, 255, 255);
	const auto textLuminance = (luminance > 0.5) ? 0 : 1;
	const auto adaptiveOpacity = (luminance - textLuminance + contrast)
		/ contrast;
	const auto opacity = std::clamp(adaptiveOpacity, 0.5, 0.64);
	auto buttonColor = textColor;
	buttonColor.setAlphaF(opacity);
	raw->setTextColorOverride(buttonColor);
}

void Panel::processBackgroundColor(const QJsonObject &args) {
	_bodyColorReceived = true;
	if (const auto color = ParseColor(args["color"].toString())) {
		overrideBodyColor(*color);
		_bodyColorLifetime.destroy();
	} else if (const auto color = LookupNamedColor(
			args["color_key"].toString())) {
		overrideBodyColor((*color)->c);
		_bodyColorLifetime = style::PaletteChanged(
		) | rpl::on_next([=] {
			overrideBodyColor((*color)->c);
		});
	} else {
		overrideBodyColor(std::nullopt);
		_bodyColorLifetime.destroy();
	}
	if (const auto raw = _bottomButtonsBg.get()) {
		raw->update();
	}
	if (const auto raw = _webviewBottom.get()) {
		raw->update();
	}
}

void Panel::processBottomBarColor(const QJsonObject &args) {
	_bottomColorReceived = true;
	if (_externalShell) {
		if (const auto color = ParseColor(args["color"].toString())) {
			_bottomBarColor = color;
			sendExternalShellMethod(
				"setColors",
				{ { u"bottomBg"_q, color->name(QColor::HexRgb) } });
		} else if (const auto color = LookupNamedColor(
				args["color_key"].toString())) {
			_bottomBarColor = (*color)->c;
			sendExternalShellMethod(
				"setColors",
				{ { u"bottomBg"_q, (*color)->c.name(QColor::HexRgb) } });
		} else {
			_bottomBarColor = std::nullopt;
		}
		return;
	}
	if (const auto color = ParseColor(args["color"].toString())) {
		_widget->overrideBottomBarColor(color);
		_bottomBarColor = color;
		_bottomBarColorLifetime.destroy();
	} else if (const auto color = LookupNamedColor(
			args["color_key"].toString())) {
		_widget->overrideBottomBarColor((*color)->c);
		_bottomBarColor = (*color)->c;
		_bottomBarColorLifetime = style::PaletteChanged(
		) | rpl::on_next([=] {
			_widget->overrideBottomBarColor((*color)->c);
			_bottomBarColor = (*color)->c;
		});
	} else {
		_widget->overrideBottomBarColor(std::nullopt);
		_bottomBarColor = std::nullopt;
		_bottomBarColorLifetime.destroy();
	}
	if (const auto raw = _bottomButtonsBg.get()) {
		raw->update();
	}
}

void Panel::processDownloadRequest(const QJsonObject &args) {
	if (args.isEmpty()) {
		_delegate->botClose();
		return;
	}
	const auto url = args["url"].toString();
	const auto name = args["file_name"].toString();
	if (url.isEmpty()) {
		LOG(("BotWebView Error: Bad 'url' in download request."));
		_delegate->botClose();
		return;
	} else if (name.isEmpty()) {
		LOG(("BotWebView Error: Bad 'file_name' in download request."));
		_delegate->botClose();
		return;
	}
	const auto done = crl::guard(this, [=](bool started) {
		postEvent("file_download_requested", started
			? "{ status: \"downloading\" }"
			: "{ status: \"cancelled\" }");
	});
	_delegate->botDownloadFile({
		.url = url,
		.name = name,
		.callback = done,
	});
}

void Panel::createButton(std::unique_ptr<Button> &button) {
	if (!_bottomButtonsBg) {
		_bottomButtonsBg = std::make_unique<RpWidget>(_widget.get());

		const auto raw = _bottomButtonsBg.get();
		raw->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(raw);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(_bottomBarColor.value_or(st::windowBg->c));
			p.drawRoundedRect(
				raw->rect().marginsAdded({ 0, 2 * st::callRadius, 0, 0 }),
				st::callRadius,
				st::callRadius);
		}, raw->lifetime());
	}
	button = std::make_unique<Button>(
		_bottomButtonsBg.get(),
		st::botWebViewBottomButton,
		_delegate->botTextContext());
	const auto raw = button.get();

	raw->setClickedCallback([=] {
		if (!raw->isDisabled()) {
			if (raw == _mainButton.get()) {
				postEvent("main_button_pressed");
			} else if (raw == _secondaryButton.get()) {
				postEvent("secondary_button_pressed");
			}
		}
	});
	raw->hide();

	rpl::combine(
		raw->shownValue(),
		raw->heightValue()
	) | rpl::on_next([=] {
		layoutButtons();
	}, raw->lifetime());
}

void Panel::layoutButtons() {
	if (!_webviewBottom) {
		return;
	}
	const auto inner = _widget->innerGeometry();
	const auto shown = [](std::unique_ptr<Button> &button) {
		return button && !button->isHidden();
	};
	const auto any = shown(_mainButton) || shown(_secondaryButton);
	_webviewBottom->setVisible(!any
		&& !_fullscreen.current()
		&& !_layerShown);
	if (any) {
		_bottomButtonsBg->setVisible(!_layerShown);

		const auto one = shown(_mainButton)
			? _mainButton.get()
			: _secondaryButton.get();
		const auto both = shown(_mainButton) && shown(_secondaryButton);
		const auto vertical = both
			&& ((_secondaryPosition == RectPart::Top)
				|| (_secondaryPosition == RectPart::Bottom));
		const auto padding = st::botWebViewBottomPadding;
		const auto height = padding.top()
			+ (vertical
				? (_mainButton->height()
					+ st::botWebViewBottomSkip.y()
					+ _secondaryButton->height())
				: one->height())
			+ padding.bottom();
		_bottomButtonsBg->setGeometry(
			inner.x(),
			inner.y() + inner.height() - height,
			inner.width(),
			height);
		auto left = padding.left();
		auto bottom = height - padding.bottom();
		auto available = inner.width() - padding.left() - padding.right();
		if (!both) {
			one->resizeToWidth(available);
			one->move(left, bottom - one->height());
		} else if (_secondaryPosition == RectPart::Top) {
			_mainButton->resizeToWidth(available);
			bottom -= _mainButton->height();
			_mainButton->move(left, bottom);
			bottom -= st::botWebViewBottomSkip.y();
			_secondaryButton->resizeToWidth(available);
			bottom -= _secondaryButton->height();
			_secondaryButton->move(left, bottom);
		} else if (_secondaryPosition == RectPart::Bottom) {
			_secondaryButton->resizeToWidth(available);
			bottom -= _secondaryButton->height();
			_secondaryButton->move(left, bottom);
			bottom -= st::botWebViewBottomSkip.y();
			_mainButton->resizeToWidth(available);
			bottom -= _mainButton->height();
			_mainButton->move(left, bottom);
		} else if (_secondaryPosition == RectPart::Left) {
			available = (available - st::botWebViewBottomSkip.x()) / 2;
			_secondaryButton->resizeToWidth(available);
			bottom -= _secondaryButton->height();
			_secondaryButton->move(left, bottom);
			_mainButton->resizeToWidth(available);
			_mainButton->move(
				inner.width() - padding.right() - available,
				bottom);
		} else {
			available = (available - st::botWebViewBottomSkip.x()) / 2;
			_mainButton->resizeToWidth(available);
			bottom -= _mainButton->height();
			_mainButton->move(left, bottom);
			_secondaryButton->resizeToWidth(available);
			_secondaryButton->move(
				inner.width() - padding.right() - available,
				bottom);
		}
	} else if (_bottomButtonsBg) {
		_bottomButtonsBg->hide();
	}
	const auto footer = _layerShown
		? 0
		: any
		? _bottomButtonsBg->height()
		: _fullscreen.current()
		? 0
		: _webviewBottom->height();
	_widget->setBottomBarHeight((!_layerShown && any) ? footer : 0);
	_footerHeight = footer;
}

void Panel::showBox(object_ptr<BoxContent> box) {
	showBox(std::move(box), LayerOption::KeepOther, anim::type::normal);
}

void Panel::showBox(
		object_ptr<BoxContent> box,
		LayerOptions options,
		anim::type animated) {
	if (_externalShell) {
		setExternalShellBlocked(true);
		auto panel = std::make_unique<SeparatePanel>();
		panel->setWindowFlag(Qt::WindowStaysOnTopHint, false);
		panel->setInnerSize(st::botWebViewPanelSize, true);
		const auto rawPanel = panel.get();
		const auto rawBox = box.data();
		rawBox->boxClosing(
		) | rpl::on_next([=] {
			finishExternalBox(rawPanel);
		}, rawPanel->lifetime());
		rawPanel->closeEvents(
		) | rpl::on_next([=] {
			finishExternalBox(rawPanel);
		}, rawPanel->lifetime());
		rawPanel->showBox(std::move(box), options, animated);
		rawPanel->showAndActivate();
		_externalBoxes.push_back(std::move(panel));
		return;
	}
	if (const auto widget = _webview ? _webview->window.widget() : nullptr) {
		_layerShown = true;
		const auto hideNow = !widget->isHidden();
		const auto raw = box.data();
		_webview->boxes.push_back(raw);
		raw->boxClosing(
		) | rpl::filter([=] {
			return _webview != nullptr;
		}) | rpl::on_next([=] {
			auto &list = _webview->boxes;
			list.erase(ranges::remove_if(list, [&](QPointer<RpWidget> b) {
				return !b || (b == raw);
			}), end(list));
			if (list.empty()) {
				_webview->boxesLifetime.destroy();
				_layerShown = false;
				const auto widget = _webview
					? _webview->window.widget()
					: nullptr;
				if (widget && widget->isHidden()) {
					widget->show();
					layoutButtons();
				}
			}
		}, _webview->boxesLifetime);

		if (hideNow) {
			widget->hide();
			layoutButtons();
		}
	}
	const auto raw = box.data();

	InvokeQueued(raw, [=] {
		if (raw->window()->isActiveWindow()) {
			// In case focus is somewhat in a native child window,
			// like a webview, Qt glitches here with input fields showing
			// focused state, but not receiving any keyboard input:
			//
			// window()->windowHandle()->isActive() == false.
			//
			// Steps were: SeparatePanel with a WebView2 child,
			// some interaction with mouse inside the WebView2,
			// so that WebView2 gets focus and active window state,
			// then we call setSearchAllowed() and after animation
			// is finished try typing -> nothing happens.
			//
			// With this workaround it works fine.
			_widget->activateWindow();
		}
	});

	_widget->showBox(
		std::move(box),
		LayerOption::KeepOther,
		anim::type::normal);
}

void Panel::finishExternalBox(not_null<SeparatePanel*> panel) {
	const auto i = ranges::find_if(_externalBoxes, [&](const auto &entry) {
		return entry.get() == panel.get();
	});
	if (i == end(_externalBoxes)) {
		return;
	}
	panel->hideGetDuration();
	_externalBoxes.erase(i);
	setExternalShellBlocked(false);
}

void Panel::showToast(TextWithEntities &&text) {
	_widget->showToast(std::move(text));
}

not_null<QWidget*> Panel::toastParent() const {
	return _widget->uiShow()->toastParent();
}

void Panel::hideLayer(anim::type animated) {
	_widget->hideLayer(animated);
}

void Panel::showCriticalError(const TextWithEntities &text) {
	_progress = nullptr;
	_webviewProgress = false;
	auto wrap = base::make_unique_q<RpWidget>(_widget.get());
	const auto raw = wrap.get();

	const auto error = CreateChild<PaddingWrap<FlatLabel>>(
		raw,
		object_ptr<FlatLabel>(
			raw,
			rpl::single(text),
			st::paymentsCriticalError),
		st::paymentsCriticalErrorPadding);
	error->entity()->setClickHandlerFilter([=](
			const ClickHandlerPtr &handler,
			Qt::MouseButton) {
		const auto entity = handler->getTextEntity();
		if (entity.type != EntityType::CustomUrl) {
			return true;
		}
		File::OpenUrl(entity.data);
		return false;
	});

	raw->widthValue() | rpl::on_next([=](int width) {
		error->resizeToWidth(width);
		raw->resize(width, error->height());
	}, raw->lifetime());

	_widget->showInner(std::move(wrap));
}

void Panel::updateThemeParams(const Webview::ThemeParams &params) {
	updateColorOverrides(params);
	if (!_webview || !_webview->window.widget()) {
		return;
	}
	if (_externalShell) {
		_webview->window.updateTheme(
			params.bodyBg,
			params.scrollBg,
			params.scrollBgOver,
			params.scrollBarBg,
			params.scrollBarBgOver);
		sendExternalShellMethod("setColors", {
			{ u"bodyBg"_q, ColorValue(params.bodyBg) },
			{ u"titleBg"_q, ColorValue(params.titleBg) },
			{ u"bottomBg"_q, _bottomBarColor
				? QJsonValue(_bottomBarColor->name(QColor::HexRgb))
				: ColorValue(params.bodyBg) },
		});
		postEvent("theme_changed", "{\"theme_params\": " + params.json + "}");
		return;
	}
	_webview->window.updateTheme(
		params.bodyBg,
		params.scrollBg,
		params.scrollBgOver,
		params.scrollBarBg,
		params.scrollBarBgOver);
	postEvent("theme_changed", "{\"theme_params\": " + params.json + "}");
}

void Panel::updateColorOverrides(const Webview::ThemeParams &params) {
	if (!_headerColorReceived && params.titleBg.alpha() == 255) {
		if (_externalShell) {
			sendExternalShellMethod(
				"setColors",
				{ { u"titleBg"_q, ColorValue(params.titleBg) } });
		} else {
			_widget->overrideTitleColor(params.titleBg);
		}
	}
	if (!_bodyColorReceived && params.bodyBg.alpha() == 255) {
		overrideBodyColor(params.bodyBg);
	}
}

void Panel::invoiceClosed(const QString &slug, const QString &status) {
	if (!_webview || !_webview->window.widget()) {
		return;
	}
	postEvent("invoice_closed", QJsonObject{
		{ u"slug"_q, slug },
		{ u"status"_q, status },
	});
	if (_hiddenForPayment) {
		_hiddenForPayment = false;
		if (_externalShell) {
			setExternalShellBlocked(false);
		} else {
			_widget->showAndActivate();
		}
	}
}

void Panel::hideForPayment() {
	_hiddenForPayment = true;
	if (_externalShell) {
		setExternalShellBlocked(true);
	} else {
		_widget->hideGetDuration();
	}
}

void Panel::postEvent(const QString &event) {
	postEvent(event, {});
}

void Panel::postEvent(const QString &event, EventData data) {
	if (!_webview) {
		LOG(("BotWebView Error: Post event \"%1\" on crashed webview."
			).arg(event));
		return;
	}
	auto written = v::is<QString>(data)
		? v::get<QString>(data).toUtf8()
		: QJsonDocument(
			v::get<QJsonObject>(data)).toJson(QJsonDocument::Compact);
	if (_externalShell) {
		sendExternalShellEvent(event, written);
		return;
	}
	_webview->window.eval(R"(
if (window.TelegramGameProxy) {
window.TelegramGameProxy.receiveEvent(
		")"
		+ event.toUtf8()
		+ '"' + (written.isEmpty() ? QByteArray() : ", " + written)
		+ R"();
}
)");
}

TextWithEntities ErrorText(const Webview::Available &info) {
	Expects(info.error != Webview::Available::Error::None);

	using Error = Webview::Available::Error;
	switch (info.error) {
	case Error::NoWebview2:
		return tr::lng_payments_webview_install_edge(
			tr::now,
			lt_link,
			Text::Link(
				"Microsoft Edge WebView2 Runtime",
				"https://go.microsoft.com/fwlink/p/?LinkId=2124703"),
			tr::marked);
	case Error::NoWebKitGTK:
		return { tr::lng_payments_webview_install_webkit(tr::now) };
	case Error::OldWindows:
		return { tr::lng_payments_webview_update_windows(tr::now) };
	default:
		return { QString::fromStdString(info.details) };
	}
}

void Panel::showWebviewError(
		const QString &text,
		const Webview::Available &information) {
	showCriticalError(TextWithEntities{ text }.append(
		"\n\n"
	).append(ErrorText(information)));
}

rpl::lifetime &Panel::lifetime() {
	return _widget->lifetime();
}

std::unique_ptr<Panel> Show(Args &&args) {
	return std::make_unique<Panel>(std::move(args));
}

} // namespace Ui::BotWebView
