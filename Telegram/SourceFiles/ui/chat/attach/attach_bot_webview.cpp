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
#include "ui/text/format_values.h"
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
#include "styles/style_info.h"
#include "styles/style_payments.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtGui/QWindow>
#include <QtGui/QScreen>
#include <QtGui/qpa/qplatformscreen.h>

#include <algorithm>

namespace Ui::BotWebView {

const char kOptionLinuxExternalBotWebApps[] = "linux-external-bot-webapps";

namespace {

constexpr auto kClipboardReadTimeout = crl::time(10000);
constexpr auto kProgressDuration = crl::time(200);
constexpr auto kProgressOpacity = 0.3;
constexpr auto kLightnessThreshold = 128;
constexpr auto kLightnessDelta = 32;
constexpr auto kExternalShellButtonIconSize = 20;

base::options::toggle OptionLinuxExternalBotWebApps({
	.id = kOptionLinuxExternalBotWebApps,
	.name = "Use external Linux bot web app windows",
	.description = "Open bot web apps in a top-level WebKitGTK window"
		" with an HTML shell instead of embedding the GTK surface.",
	.scope = base::options::linux,
	.restartRequired = true,
});

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

struct ExternalShellColorState {
	bool titleUsesTheme = true;
	bool bodyUsesTheme = true;
	bool bottomUsesTheme = true;
	std::optional<QColor> title;
	std::optional<QColor> body;
	std::optional<QColor> bottom;
};

[[nodiscard]] auto &ExternalShellColorStates() {
	static auto result
		= std::vector<std::pair<const Panel*, ExternalShellColorState>>();
	return result;
}

[[nodiscard]] ExternalShellColorState &ExternalShellColorStateFor(
		const Panel *panel) {
	auto &states = ExternalShellColorStates();
	const auto i = std::find_if(begin(states), end(states), [=](
			const std::pair<const Panel*, ExternalShellColorState> &entry) {
		return (entry.first == panel);
	});
	if (i != end(states)) {
		return i->second;
	}
	states.push_back({ panel, {} });
	return states.back().second;
}

void ForgetExternalShellColorState(const Panel *panel) {
	auto &states = ExternalShellColorStates();
	const auto i = std::find_if(begin(states), end(states), [=](
			const std::pair<const Panel*, ExternalShellColorState> &entry) {
		return (entry.first == panel);
	});
	if (i != end(states)) {
		states.erase(i);
	}
}

void SetExternalShellTitleColor(
		const Panel *panel,
		std::optional<QColor> color) {
	auto &state = ExternalShellColorStateFor(panel);
	state.titleUsesTheme = !color.has_value();
	state.title = std::move(color);
}

void SetExternalShellBodyColor(
		const Panel *panel,
		std::optional<QColor> color) {
	auto &state = ExternalShellColorStateFor(panel);
	state.bodyUsesTheme = !color.has_value();
	state.body = std::move(color);
}

void SetExternalShellBottomColor(
		const Panel *panel,
		std::optional<QColor> color) {
	auto &state = ExternalShellColorStateFor(panel);
	state.bottomUsesTheme = !color.has_value();
	state.bottom = std::move(color);
}

[[nodiscard]] QColor ExternalShellBodyColor(
		const Panel *panel,
		const Webview::ThemeParams &params) {
	const auto &state = ExternalShellColorStateFor(panel);
	return state.bodyUsesTheme
		? (params.bodyBg.alpha() == 255 ? params.bodyBg : st::windowBg->c)
		: state.body.value_or(params.bodyBg);
}

[[nodiscard]] QColor ExternalShellTitleColor(
		const Panel *panel,
		const Webview::ThemeParams &params) {
	const auto &state = ExternalShellColorStateFor(panel);
	return state.titleUsesTheme
		? (params.titleBg.alpha() == 255 ? params.titleBg : st::windowBg->c)
		: state.title.value_or(params.titleBg);
}

[[nodiscard]] QColor ExternalShellBottomBarColor(
		const Panel *panel,
		const Webview::ThemeParams &params) {
	const auto &state = ExternalShellColorStateFor(panel);
	const auto body = ExternalShellBodyColor(panel, params);
	return state.bottomUsesTheme
		? body
		: state.bottom.value_or(body);
}

[[nodiscard]] QJsonObject ExternalShellColorPayload(
		const Panel *panel,
		const Webview::ThemeParams &params) {
	return {
		{ u"bodyBg"_q, ColorValue(ExternalShellBodyColor(panel, params)) },
		{ u"titleBg"_q, ColorValue(ExternalShellTitleColor(panel, params)) },
		{ u"bottomBg"_q, ColorValue(ExternalShellBottomBarColor(panel, params)) },
	};
}

[[nodiscard]] QJsonObject ExternalShellMetrics() {
	const auto &shellPadding = st::botWebViewShellPadding;
	const auto &shadowPadding = st::botWebViewShellShadowPadding;
	const auto &titlePadding = st::botWebViewShellTitlePadding;
	const auto &menuButtonSize = st::botWebViewShellMenuButtonSize;
	const auto fullscreenButtonSize = QSize(
		st::fullScreenPanelClose.width,
		st::fullScreenPanelClose.height);
	const auto fullscreenControlShift
		= st::separatePanelClose.rippleAreaPosition;
	return {
		{ u"shellRadius"_q, st::botWebViewShellRadius },
		{ u"shellPaddingTop"_q, shellPadding.top() },
		{ u"shellPaddingRight"_q, shellPadding.right() },
		{ u"shellPaddingBottom"_q, shellPadding.bottom() },
		{ u"shellPaddingLeft"_q, shellPadding.left() },
		{ u"shadowPaddingTop"_q, shadowPadding.top() },
		{ u"shadowPaddingRight"_q, shadowPadding.right() },
		{ u"shadowPaddingBottom"_q, shadowPadding.bottom() },
		{ u"shadowPaddingLeft"_q, shadowPadding.left() },
		{ u"headerHeight"_q, st::botWebViewShellHeaderHeight },
		{ u"titlePaddingTop"_q, titlePadding.top() },
		{ u"titlePaddingRight"_q, titlePadding.right() },
		{ u"titlePaddingBottom"_q, titlePadding.bottom() },
		{ u"titlePaddingLeft"_q, titlePadding.left() },
		{ u"badgeSkip"_q, st::botWebViewShellBadgeSkip },
		{ u"frameRadius"_q, st::botWebViewShellFrameRadius },
		{ u"controlWidth"_q, menuButtonSize.width() },
		{ u"controlHeight"_q, menuButtonSize.height() },
		{ u"buttonHeight"_q, st::botWebViewBottomButton.height },
		{ u"buttonGapX"_q, st::botWebViewBottomSkip.x() },
		{ u"buttonGapY"_q, st::botWebViewBottomSkip.y() },
		{ u"disclosureSkip"_q, st::botWebViewShellDisclosureSkip },
		{ u"footerButtonSkip"_q, st::botWebViewShellFooterButtonSkip },
		{ u"fullscreenControlWidth"_q, fullscreenButtonSize.width() },
		{ u"fullscreenControlHeight"_q, fullscreenButtonSize.height() },
		{ u"fullscreenControlTop"_q, fullscreenControlShift.y() },
		{ u"fullscreenControlRight"_q, fullscreenControlShift.x() },
		{ u"fullscreenControlGap"_q, fullscreenControlShift.x() },
	};
}

[[nodiscard]] QSize ExternalShellWindowSize(QSize contentSize) {
	const auto &shadowPadding = st::botWebViewShellShadowPadding;
	return contentSize + QSize(
		shadowPadding.left() + shadowPadding.right(),
		shadowPadding.top()
			+ st::botWebViewShellHeaderHeight
			+ shadowPadding.bottom());
}

enum class SharedPanelMenuAction {
	None,
	Settings,
	OpenBot,
	Reload,
	ShareGame,
	Terms,
	Privacy,
	RemoveFromMenu,
	RemoveFromMainMenu,
	DownloadOpen,
	DownloadRetry,
	DownloadCancel,
};

struct SharedPanelMenuItem {
	QString id;
	QString text;
	QString subtitle;
	QString actionLabel;
	QString iconKey;
	const style::icon *icon = nullptr;
	bool isSeparator = false;
	bool isAttention = false;
	bool isEnabled = true;
	std::vector<SharedPanelMenuItem> children;
};

struct SharedPanelMenuBuildArgs {
	const std::vector<DownloadsEntry> *downloads = nullptr;
	bool hasSettings = false;
	MenuButtons buttons = {};
};

struct SharedPanelMenuDispatchArgs {
	Fn<void()> settings;
	Fn<void()> reload;
	Fn<void()> terms;
	Fn<void()> privacy;
	Fn<void(MenuButton)> menuButton;
	Fn<void(uint32, DownloadsAction)> download;
};

[[nodiscard]] QString SharedPanelMenuActionId(SharedPanelMenuAction action) {
	switch (action) {
	case SharedPanelMenuAction::Settings:
		return u"settings"_q;
	case SharedPanelMenuAction::OpenBot:
		return u"open_bot"_q;
	case SharedPanelMenuAction::Reload:
		return u"reload"_q;
	case SharedPanelMenuAction::ShareGame:
		return u"share_game"_q;
	case SharedPanelMenuAction::Terms:
		return u"terms"_q;
	case SharedPanelMenuAction::Privacy:
		return u"privacy"_q;
	case SharedPanelMenuAction::RemoveFromMenu:
		return u"remove_from_menu"_q;
	case SharedPanelMenuAction::RemoveFromMainMenu:
		return u"remove_from_main_menu"_q;
	case SharedPanelMenuAction::None:
	case SharedPanelMenuAction::DownloadOpen:
	case SharedPanelMenuAction::DownloadRetry:
	case SharedPanelMenuAction::DownloadCancel:
		break;
	}
	return QString();
}

[[nodiscard]] QString DownloadPanelMenuActionId(
		uint32 id,
		DownloadsAction action) {
	auto type = QString();
	switch (action) {
	case DownloadsAction::Open:
		type = u"open"_q;
		break;
	case DownloadsAction::Retry:
		type = u"retry"_q;
		break;
	case DownloadsAction::Cancel:
		type = u"cancel"_q;
		break;
	}
	return u"download:%1:%2"_q.arg(id).arg(type);
}

[[nodiscard]] SharedPanelMenuAction ParseSharedPanelMenuActionType(
		const QString &type) {
	if (type == u"open"_q) {
		return SharedPanelMenuAction::DownloadOpen;
	} else if (type == u"retry"_q) {
		return SharedPanelMenuAction::DownloadRetry;
	} else if (type == u"cancel"_q) {
		return SharedPanelMenuAction::DownloadCancel;
	}
	return SharedPanelMenuAction::None;
}

struct ParsedSharedPanelMenuAction {
	SharedPanelMenuAction action = SharedPanelMenuAction::None;
	uint32 downloadId = 0;
};

[[nodiscard]] ParsedSharedPanelMenuAction ParseSharedPanelMenuAction(
		const QString &id) {
	if (id == u"settings"_q) {
		return { SharedPanelMenuAction::Settings };
	} else if (id == u"open_bot"_q) {
		return { SharedPanelMenuAction::OpenBot };
	} else if (id == u"reload"_q) {
		return { SharedPanelMenuAction::Reload };
	} else if (id == u"share_game"_q) {
		return { SharedPanelMenuAction::ShareGame };
	} else if (id == u"terms"_q) {
		return { SharedPanelMenuAction::Terms };
	} else if (id == u"privacy"_q) {
		return { SharedPanelMenuAction::Privacy };
	} else if (id == u"remove_from_menu"_q) {
		return { SharedPanelMenuAction::RemoveFromMenu };
	} else if (id == u"remove_from_main_menu"_q) {
		return { SharedPanelMenuAction::RemoveFromMainMenu };
	} else if (id.startsWith(u"download:"_q)) {
		const auto parts = id.split(u':');
		const auto downloadId = (parts.size() == 3)
			? parts[1].toUInt()
			: 0;
		return {
			.action = (parts.size() == 3)
				? ParseSharedPanelMenuActionType(parts[2])
				: SharedPanelMenuAction::None,
			.downloadId = downloadId,
		};
	}
	return {};
}

void DispatchSharedPanelMenuAction(
		const QString &id,
		const SharedPanelMenuDispatchArgs &dispatch) {
	const auto parsed = ParseSharedPanelMenuAction(id);
	switch (parsed.action) {
	case SharedPanelMenuAction::Settings:
		if (dispatch.settings) {
			dispatch.settings();
		}
		break;
	case SharedPanelMenuAction::OpenBot:
		if (dispatch.menuButton) {
			dispatch.menuButton(MenuButton::OpenBot);
		}
		break;
	case SharedPanelMenuAction::Reload:
		if (dispatch.reload) {
			dispatch.reload();
		}
		break;
	case SharedPanelMenuAction::ShareGame:
		if (dispatch.menuButton) {
			dispatch.menuButton(MenuButton::ShareGame);
		}
		break;
	case SharedPanelMenuAction::Terms:
		if (dispatch.terms) {
			dispatch.terms();
		}
		break;
	case SharedPanelMenuAction::Privacy:
		if (dispatch.privacy) {
			dispatch.privacy();
		}
		break;
	case SharedPanelMenuAction::RemoveFromMenu:
		if (dispatch.menuButton) {
			dispatch.menuButton(MenuButton::RemoveFromMenu);
		}
		break;
	case SharedPanelMenuAction::RemoveFromMainMenu:
		if (dispatch.menuButton) {
			dispatch.menuButton(MenuButton::RemoveFromMainMenu);
		}
		break;
	case SharedPanelMenuAction::DownloadOpen:
		if (dispatch.download) {
			dispatch.download(parsed.downloadId, DownloadsAction::Open);
		}
		break;
	case SharedPanelMenuAction::DownloadRetry:
		if (dispatch.download) {
			dispatch.download(parsed.downloadId, DownloadsAction::Retry);
		}
		break;
	case SharedPanelMenuAction::DownloadCancel:
		if (dispatch.download) {
			dispatch.download(parsed.downloadId, DownloadsAction::Cancel);
		}
		break;
	case SharedPanelMenuAction::None:
		break;
	}
}

[[nodiscard]] SharedPanelMenuItem BuildDownloadPanelMenuItem(
		const DownloadsEntry &entry) {
	auto item = SharedPanelMenuItem();
	item.text = entry.path.section(u'/', -1);
	if (item.text.isEmpty()) {
		item.text = entry.path;
	}
	if (item.text.isEmpty()) {
		item.text = entry.url;
	}
	if (entry.total && (entry.total == entry.ready)) {
		item.id = DownloadPanelMenuActionId(entry.id, DownloadsAction::Open);
		item.subtitle = FormatSizeText(entry.total);
	} else if (entry.loading) {
		item.id = DownloadPanelMenuActionId(entry.id, DownloadsAction::Cancel);
		item.subtitle = entry.total
			? FormatProgressText(entry.ready, entry.total)
			: tr::lng_bot_download_starting(tr::now);
		item.actionLabel = tr::lng_cancel(tr::now);
	} else {
		item.id = DownloadPanelMenuActionId(entry.id, DownloadsAction::Retry);
		item.subtitle = tr::lng_bot_download_failed(
			tr::now,
			lt_retry,
			tr::lng_bot_download_retry(tr::now));
		item.actionLabel = tr::lng_bot_download_retry(tr::now);
	}
	item.isEnabled = !item.id.isEmpty();
	return item;
}

[[nodiscard]] std::vector<SharedPanelMenuItem> BuildSharedPanelMenuItems(
		const SharedPanelMenuBuildArgs &args) {
	auto result = std::vector<SharedPanelMenuItem>();
	if (args.downloads && !args.downloads->empty()) {
		auto children = std::vector<SharedPanelMenuItem>();
		children.reserve(args.downloads->size());
		for (const auto &entry : *args.downloads | ranges::views::reverse) {
			children.push_back(BuildDownloadPanelMenuItem(entry));
		}
		result.push_back({
			.id = u"downloads"_q,
			.text = tr::lng_downloads_section(tr::now),
			.iconKey = u"downloads"_q,
			.icon = &st::menuIconDownload,
			.children = std::move(children),
		});
		result.push_back({
			.isSeparator = true,
		});
	}
	if (args.hasSettings) {
		result.push_back({
			.id = SharedPanelMenuActionId(SharedPanelMenuAction::Settings),
			.text = tr::lng_bot_settings(tr::now),
			.iconKey = u"settings"_q,
			.icon = &st::menuIconSettings,
		});
	}
	if (args.buttons & MenuButton::OpenBot) {
		result.push_back({
			.id = SharedPanelMenuActionId(SharedPanelMenuAction::OpenBot),
			.text = tr::lng_bot_open(tr::now),
			.iconKey = u"open_bot"_q,
			.icon = &st::menuIconLeave,
		});
	}
	result.push_back({
		.id = SharedPanelMenuActionId(SharedPanelMenuAction::Reload),
		.text = tr::lng_bot_reload_page(tr::now),
		.iconKey = u"reload"_q,
		.icon = &st::menuIconRestore,
	});
	if (args.buttons & MenuButton::ShareGame) {
		result.push_back({
			.id = SharedPanelMenuActionId(SharedPanelMenuAction::ShareGame),
			.text = tr::lng_iv_share(tr::now),
			.iconKey = u"share_game"_q,
			.icon = &st::menuIconShare,
		});
	} else {
		result.push_back({
			.id = SharedPanelMenuActionId(SharedPanelMenuAction::Terms),
			.text = tr::lng_bot_terms(tr::now),
			.iconKey = u"terms"_q,
			.icon = &st::menuIconGroupLog,
		});
		result.push_back({
			.id = SharedPanelMenuActionId(SharedPanelMenuAction::Privacy),
			.text = tr::lng_bot_privacy(tr::now),
			.iconKey = u"privacy"_q,
			.icon = &st::menuIconAntispam,
		});
	}
	if (args.buttons & MenuButton::RemoveFromMainMenu) {
		result.push_back({
			.id = SharedPanelMenuActionId(
				SharedPanelMenuAction::RemoveFromMainMenu),
			.text = tr::lng_bot_remove_from_side_menu(tr::now),
			.iconKey = u"remove"_q,
			.icon = &st::menuIconDeleteAttention,
			.isAttention = true,
		});
	} else if (args.buttons & MenuButton::RemoveFromMenu) {
		result.push_back({
			.id = SharedPanelMenuActionId(
				SharedPanelMenuAction::RemoveFromMenu),
			.text = tr::lng_bot_remove_from_menu(tr::now),
			.iconKey = u"remove"_q,
			.icon = &st::menuIconDeleteAttention,
			.isAttention = true,
		});
	}
	return result;
}

void FillNativeSharedPanelMenu(
		const Ui::Menu::MenuCallback &callback,
		const std::vector<SharedPanelMenuItem> &items,
		Fn<rpl::producer<std::vector<DownloadsEntry>>()> makeDownloads,
		const SharedPanelMenuDispatchArgs &dispatch) {
	for (const auto &item : items) {
		if (item.isSeparator) {
			callback({
				.separatorSt = &st::expandedMenuSeparator,
				.isSeparator = true,
			});
		} else if (!item.children.empty()) {
			callback(Ui::Menu::MenuCallback::Args{
				.text = item.text,
				.icon = item.icon,
				.fillSubmenu = FillAttachBotDownloadsSubmenu(
					makeDownloads(),
					[download = dispatch.download](
							uint32 id,
							DownloadsAction type) {
						if (download) {
							download(id, type);
						}
					}),
			});
		} else {
			callback(Ui::Menu::MenuCallback::Args{
				.text = item.text,
				.handler = [=] {
					DispatchSharedPanelMenuAction(item.id, dispatch);
				},
				.icon = item.icon,
				.isAttention = item.isAttention,
			});
		}
	}
}

[[nodiscard]] QImage RasterizeStyleIcon(const style::icon &icon) {
	const auto size = icon.size();
	const auto ratio = style::DevicePixelRatio();
	auto image = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(Qt::transparent);
	auto painter = Painter(&image);
	icon.paintInCenter(painter, QRect(QPoint(), size));
	return image;
}

[[nodiscard]] QImage RasterizeVerifiedBadge() {
	const auto size = st::infoVerifiedStar.size() + QSize(0, st::lineWidth);
	const auto ratio = style::DevicePixelRatio();
	auto image = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(Qt::transparent);
	auto painter = Painter(&image);
	const auto width = size.width();
	st::infoVerifiedStar.paint(painter, st::lineWidth, 0, width);
	st::infoPeerBadge.verifiedCheck.paint(painter, st::lineWidth, 0, width);
	return image;
}

[[nodiscard]] QString PngDataUrl(const QImage &image) {
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "PNG");
	return u"data:image/png;base64,"_q + QString::fromLatin1(bytes.toBase64());
}

[[nodiscard]] QJsonObject SerializeRasterAsset(
		const QImage &image,
		QSize size,
		QString alt = QString()) {
	auto result = QJsonObject{
		{ u"url"_q, PngDataUrl(image) },
		{ u"width"_q, size.width() },
		{ u"height"_q, size.height() },
	};
	if (!alt.isEmpty()) {
		result.insert(u"alt"_q, alt);
	}
	return result;
}

[[nodiscard]] QJsonObject SerializeStyleIconAsset(const style::icon &icon) {
	return SerializeRasterAsset(RasterizeStyleIcon(icon), icon.size());
}

[[nodiscard]] QJsonObject SerializeVerifiedBadgeAsset() {
	const auto size = st::infoVerifiedStar.size() + QSize(0, st::lineWidth);
	return SerializeRasterAsset(
		RasterizeVerifiedBadge(),
		size,
		tr::lng_sr_verified_badge(tr::now));
}

[[nodiscard]] QJsonObject SerializeExternalShellMenuPalette() {
	return {
		{ u"bg"_q, st::windowBg->c.name(QColor::HexRgb) },
		{ u"fg"_q, st::windowFg->c.name(QColor::HexRgb) },
		{ u"hoverBg"_q, st::windowBgOver->c.name(QColor::HexRgb) },
		{ u"separator"_q, st::menuSeparatorFg->c.name(QColor::HexRgb) },
		{ u"attention"_q,
			st::menuIconAttentionColor->c.name(QColor::HexRgb) },
	};
}

void CollectSharedPanelMenuIcons(
		const std::vector<SharedPanelMenuItem> &items,
		QJsonObject &result) {
	for (const auto &item : items) {
		if (!item.iconKey.isEmpty()
			&& item.icon
			&& !result.contains(item.iconKey)) {
			result.insert(item.iconKey, SerializeStyleIconAsset(*item.icon));
		}
		if (!item.children.empty()) {
			CollectSharedPanelMenuIcons(item.children, result);
		}
	}
}

[[nodiscard]] QJsonObject SerializeSharedPanelMenuItem(
		const SharedPanelMenuItem &item) {
	if (item.isSeparator) {
		return { { u"separator"_q, true } };
	}
	auto result = QJsonObject{
		{ u"id"_q, item.id },
		{ u"text"_q, item.text },
		{ u"attention"_q, item.isAttention },
		{ u"enabled"_q, item.isEnabled },
	};
	if (!item.subtitle.isEmpty()) {
		result.insert(u"subtitle"_q, item.subtitle);
	}
	if (!item.actionLabel.isEmpty()) {
		result.insert(u"actionLabel"_q, item.actionLabel);
	}
	if (!item.iconKey.isEmpty()) {
		result.insert(u"icon"_q, item.iconKey);
	}
	if (!item.children.empty()) {
		auto children = QJsonArray();
		for (const auto &child : item.children) {
			children.push_back(SerializeSharedPanelMenuItem(child));
		}
		result.insert(u"children"_q, children);
	}
	return result;
}

[[nodiscard]] QJsonArray SerializeSharedPanelMenu(
		const std::vector<SharedPanelMenuItem> &items) {
	auto result = QJsonArray();
	for (const auto &item : items) {
		result.push_back(SerializeSharedPanelMenuItem(item));
	}
	return result;
}

[[nodiscard]] QByteArray ExternalShellCss() {
	return R"(
html,
body {
	width: 100%;
	height: 100%;
	margin: 0;
	overflow: hidden;
	background: transparent;
}
:root {
	--font-sans: -apple-system, BlinkMacSystemFont, avenir next, avenir, Segoe UI Variable Text, segoe ui, helvetica neue, helvetica, Cantarell, Ubuntu, roboto, noto, tahoma, arial, sans-serif;
}
body {
	font: 14px/1.35 var(--font-sans);
	color: var(--title-fg, #000000);
	-webkit-font-smoothing: antialiased;
	text-rendering: optimizeLegibility;
}
button,
input,
textarea,
select {
	font: inherit;
}
* {
	box-sizing: border-box;
}
#root {
	width: 100%;
	height: 100%;
	position: relative;
	--shell-radius: 7px;
	--shell-pad-top: 12px;
	--shell-pad-right: 12px;
	--shell-pad-bottom: 12px;
	--shell-pad-left: 12px;
	--shadow-pad-top: 12px;
	--shadow-pad-right: 14px;
	--shadow-pad-bottom: 16px;
	--shadow-pad-left: 14px;
	--header-height: 60px;
	--title-pad-top: 0px;
	--title-pad-right: 0px;
	--title-pad-bottom: 0px;
	--title-pad-left: 0px;
	--badge-skip: 4px;
	--frame-radius: 6px;
	--control-width: 36px;
	--control-height: 36px;
	--button-height: 40px;
	--button-gap-x: 12px;
	--button-gap-y: 8px;
	--disclosure-skip: 12px;
	--footer-button-skip: 10px;
	--footer-gap: var(--disclosure-skip);
	--fullscreen-control-width: 44px;
	--fullscreen-control-height: 44px;
	--fullscreen-control-top: 8px;
	--fullscreen-control-right: 8px;
	--fullscreen-control-gap: 8px;
	--body-bg: #ffffff;
	--title-bg: #ffffff;
	--bottom-bg: #ffffff;
	--body-fg: #000000;
	--title-fg: #000000;
	--footer-fg: rgba(0, 0, 0, 0.55);
	--menu-bg: #ffffff;
	--menu-fg: #000000;
	--menu-hover-bg: #f1f1f1;
	--menu-separator: rgba(0, 0, 0, 0.08);
	--menu-attention: #d05c5c;
}
.hidden {
	display: none !important;
}
#scene {
	position: absolute;
	inset: 0;
	padding: var(--shadow-pad-top)
		var(--shadow-pad-right)
		var(--shadow-pad-bottom)
		var(--shadow-pad-left);
}
#shell {
	position: relative;
	width: 100%;
	height: 100%;
	display: flex;
	flex-direction: column;
	border-radius: var(--shell-radius);
	background: var(--body-bg, #ffffff);
	box-shadow:
		0 0 0 1px rgba(0, 0, 0, 0.08),
		0 1px 2px rgba(0, 0, 0, 0.18),
		0 3px 8px -1px rgba(0, 0, 0, 0.16),
		0 8px 14px -5px rgba(0, 0, 0, 0.20);
	overflow: hidden;
}
#header {
	flex: 0 0 auto;
	min-height: var(--header-height);
	display: grid;
	grid-template-columns: auto minmax(0, 1fr) auto auto;
	align-items: center;
	gap: 4px;
	padding: 0 var(--shell-pad-right) 0 var(--shell-pad-left);
	background: var(--title-bg, #ffffff);
	color: var(--title-fg, #000000);
	border-bottom: 1px solid rgba(0, 0, 0, 0.06);
	-webkit-user-select: none;
	user-select: none;
}
#back {
	grid-column: 1;
}
#title-row {
	grid-column: 2;
}
#menu-toggle {
	grid-column: 3;
}
#close {
	grid-column: 4;
}
.title-control {
	width: var(--control-width);
	height: var(--control-height);
	border: 0;
	border-radius: 999px;
	background: transparent;
	color: inherit;
	cursor: default;
	padding: 0;
	display: inline-flex;
	align-items: center;
	justify-content: center;
	transition: background-color 120ms linear, opacity 120ms linear;
}
.title-control svg {
	width: 20px;
	height: 20px;
	stroke: currentColor;
	stroke-width: 2;
	stroke-linecap: round;
	stroke-linejoin: round;
	fill: none;
}
.title-control .fill-icon {
	fill: currentColor;
	stroke: none;
}
.title-control:hover:not(:disabled),
.title-control.active {
	background: rgba(127, 127, 127, 0.14);
}
.title-control:disabled {
	opacity: 0.45;
}
#title-row {
	display: flex;
	align-items: center;
	min-width: 0;
	padding: var(--title-pad-top)
		var(--title-pad-right)
		var(--title-pad-bottom)
		var(--title-pad-left);
	-webkit-user-select: none;
	user-select: none;
}
#title {
	min-width: 0;
	overflow: hidden;
	text-overflow: ellipsis;
	white-space: nowrap;
	font-size: 20px;
	line-height: 24px;
	font-weight: 600;
	-webkit-user-select: none;
	user-select: none;
}
#badge {
	margin-left: var(--badge-skip);
	width: 16px;
	height: 16px;
	flex: 0 0 auto;
	background-position: center;
	background-repeat: no-repeat;
	background-size: contain;
}
#body {
	flex: 1 1 auto;
	min-height: 0;
	display: flex;
	flex-direction: column;
	gap: var(--footer-gap);
	padding: 0;
	background: var(--body-bg, #ffffff);
}
#frame-shell {
	flex: 1 1 auto;
	min-height: 0;
	border-radius: 0;
	overflow: hidden;
	background: var(--body-bg, #ffffff);
	box-shadow: none;
}
#frame-wrap,
#frame-wrap iframe {
	display: block;
	width: 100%;
	height: 100%;
	border: 0;
	background: var(--body-bg, #ffffff);
}
#footer {
	display: none;
	flex: 0 0 auto;
	color: var(--footer-fg);
	padding: var(--shell-pad-top)
		var(--shell-pad-right)
		var(--shell-pad-bottom)
		var(--shell-pad-left);
	background: var(--bottom-bg, var(--body-bg));
}
#footer.visible {
	display: flex;
	flex-direction: column;
}
#disclosure {
	display: none;
	text-align: center;
	font-size: 12px;
	line-height: 16px;
	padding: 0 4px;
	word-break: break-word;
}
#disclosure.visible {
	display: block;
}
#buttons-wrap {
	display: none;
	background: var(--bottom-bg, var(--body-bg));
	border-radius: var(--frame-radius);
	overflow: hidden;
}
#buttons-wrap.visible {
	display: block;
}
#buttons {
	display: grid;
	grid-template-columns: minmax(0, 1fr);
	column-gap: var(--button-gap-x);
	row-gap: var(--button-gap-y);
}
#buttons[data-layout="horizontal"] {
	grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
}
.shell-button {
	min-width: 0;
	height: var(--button-height);
	border: 0;
	border-radius: calc(var(--frame-radius) - 1px);
	padding: 0 14px;
	font-weight: 600;
	display: inline-flex;
	align-items: center;
	justify-content: center;
	gap: 8px;
	cursor: default;
	transition: opacity 120ms linear;
}
.shell-button:disabled {
	opacity: 0.55;
}
.button-icon {
	display: none;
	flex: 0 0 auto;
	width: 20px;
	height: 20px;
	background-position: center;
	background-repeat: no-repeat;
	background-size: contain;
}
.button-icon.visible {
	display: block;
}
.button-icon.pending {
	border-radius: 999px;
	background-color: currentColor;
	opacity: 0.18;
}
.button-label {
	min-width: 0;
	overflow: hidden;
	text-overflow: ellipsis;
	white-space: nowrap;
}
.button-spinner {
	display: none;
	flex: 0 0 auto;
	width: 14px;
	height: 14px;
	border: 2px solid currentColor;
	border-right-color: transparent;
	border-radius: 50%;
	animation: spin 0.8s linear infinite;
}
.button-spinner.visible {
	display: block;
}
#menu {
	position: absolute;
	top: calc(var(--shadow-pad-top) + var(--header-height) - 4px);
	right: var(--shadow-pad-right);
	display: none;
	min-width: 220px;
	max-width: calc(100% - var(--shadow-pad-left) - var(--shadow-pad-right));
	background: var(--menu-bg, #ffffff);
	color: var(--menu-fg, #000000);
	border-radius: 12px;
	box-shadow:
		0 20px 38px rgba(0, 0, 0, 0.22),
		0 4px 12px rgba(0, 0, 0, 0.12);
	overflow: hidden;
	z-index: 12;
}
#root.fullscreen #scene {
	padding: 0;
}
#root.fullscreen #shell {
	border-radius: 0;
	box-shadow: none;
}
#root.fullscreen #header {
	position: absolute;
	top: var(--fullscreen-control-top);
	right: var(--fullscreen-control-right);
	z-index: 13;
	width: auto;
	min-height: var(--fullscreen-control-height);
	display: flex;
	justify-content: flex-end;
	gap: var(--fullscreen-control-gap);
	padding: 0;
	background: transparent;
	border-bottom: 0;
	pointer-events: none;
}
#root.fullscreen #back,
#root.fullscreen #title-row {
	display: none !important;
}
#root.fullscreen .title-control {
	width: var(--fullscreen-control-width);
	height: var(--fullscreen-control-height);
	color: #ffffff;
	background: rgba(0, 0, 0, 0.35);
	pointer-events: auto;
}
#root.fullscreen .title-control:hover:not(:disabled),
#root.fullscreen .title-control.active {
	background: rgba(0, 0, 0, 0.45);
}
#root.fullscreen #body {
	gap: 0;
}
#root.fullscreen #menu {
	top: calc(
		var(--fullscreen-control-top)
			+ var(--fullscreen-control-height)
			+ var(--fullscreen-control-gap));
	right: var(--fullscreen-control-right);
}
#root.fullscreen #resize-handles {
	display: none;
}
#resize-handles {
	position: absolute;
	inset: 0;
	z-index: 11;
	pointer-events: none;
}
.resize-handle {
	position: absolute;
	pointer-events: auto;
}
.resize-handle[data-resize-edge="top"] {
	top: 0;
	left: var(--shadow-pad-left);
	right: var(--shadow-pad-right);
	height: var(--shadow-pad-top);
	cursor: ns-resize;
}
.resize-handle[data-resize-edge="bottom"] {
	bottom: 0;
	left: var(--shadow-pad-left);
	right: var(--shadow-pad-right);
	height: var(--shadow-pad-bottom);
	cursor: ns-resize;
}
.resize-handle[data-resize-edge="left"] {
	top: var(--shadow-pad-top);
	bottom: var(--shadow-pad-bottom);
	left: 0;
	width: var(--shadow-pad-left);
	cursor: ew-resize;
}
.resize-handle[data-resize-edge="right"] {
	top: var(--shadow-pad-top);
	bottom: var(--shadow-pad-bottom);
	right: 0;
	width: var(--shadow-pad-right);
	cursor: ew-resize;
}
.resize-handle[data-resize-edge="top-left"] {
	top: 0;
	left: 0;
	width: var(--shadow-pad-left);
	height: var(--shadow-pad-top);
	cursor: nwse-resize;
}
.resize-handle[data-resize-edge="top-right"] {
	top: 0;
	right: 0;
	width: var(--shadow-pad-right);
	height: var(--shadow-pad-top);
	cursor: nesw-resize;
}
.resize-handle[data-resize-edge="bottom-left"] {
	bottom: 0;
	left: 0;
	width: var(--shadow-pad-left);
	height: var(--shadow-pad-bottom);
	cursor: nesw-resize;
}
.resize-handle[data-resize-edge="bottom-right"] {
	right: 0;
	bottom: 0;
	width: var(--shadow-pad-right);
	height: var(--shadow-pad-bottom);
	cursor: nwse-resize;
}
#menu.visible {
	display: block;
}
#menu-list {
	padding: 6px 0;
}
.menu-separator {
	height: 1px;
	margin: 6px 0;
	background: var(--menu-separator, rgba(0, 0, 0, 0.08));
}
.menu-item,
.menu-download {
	width: 100%;
	border: 0;
	background: transparent;
	color: inherit;
	display: flex;
	align-items: center;
	gap: 12px;
	padding: 10px 14px;
	text-align: left;
	cursor: default;
	transition: background-color 120ms linear, opacity 120ms linear;
}
.menu-download {
	padding-left: 0;
	padding-right: 0;
}
.menu-item:hover,
.menu-download:hover {
	background: var(--menu-hover-bg, rgba(127, 127, 127, 0.14));
}
.menu-item.disabled,
.menu-download.disabled {
	opacity: 0.72;
}
.menu-item.attention,
.menu-download.attention {
	color: var(--menu-attention, #d05c5c);
}
.menu-group {
	padding: 0 14px;
}
.menu-group-title {
	padding-left: 0;
	padding-right: 0;
	cursor: default;
}
.menu-group-children {
	display: flex;
	flex-direction: column;
	gap: 2px;
	padding: 0 0 6px 32px;
}
.menu-item-icon {
	flex: 0 0 20px;
	width: 20px;
	height: 20px;
	background-position: center;
	background-repeat: no-repeat;
	background-size: contain;
}
.menu-item-copy {
	min-width: 0;
	flex: 1 1 auto;
	display: flex;
	flex-direction: column;
	align-items: flex-start;
}
.menu-item-title,
.menu-item-subtitle,
.menu-item-action {
	display: block;
	min-width: 0;
	overflow: hidden;
	text-overflow: ellipsis;
	white-space: nowrap;
}
.menu-item-title {
	font-size: 14px;
	line-height: 18px;
}
.menu-item-subtitle,
.menu-item-action {
	font-size: 12px;
	line-height: 16px;
}
.menu-item-subtitle {
	opacity: 0.64;
}
.menu-item-action {
	flex: 0 0 auto;
	padding-left: 12px;
	opacity: 0.8;
}
#blocker {
	position: absolute;
	inset: 0;
	display: none;
	background: rgba(0, 0, 0, 0.12);
	z-index: 20;
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
	const header = document.getElementById('header');
	const frameShell = document.getElementById('frame-shell');
	const frameWrap = document.getElementById('frame-wrap');
	const disclosure = document.getElementById('disclosure');
	const footer = document.getElementById('footer');
	const buttonsWrap = document.getElementById('buttons-wrap');
	const buttons = document.getElementById('buttons');
	const badge = document.getElementById('badge');
	const menu = document.getElementById('menu');
	const menuList = document.getElementById('menu-list');
	const resizeHandles = Array.prototype.slice.call(
		document.querySelectorAll('.resize-handle'));
	const title = document.getElementById('title');
	const controls = {
		back: document.getElementById('back'),
		menu: document.getElementById('menu-toggle'),
		close: document.getElementById('close')
	};
	const shellState = {
		backVisible: false,
		menuVisible: false,
		badgeVisible: false,
		bottomText: '',
		isFullscreen: false,
		blocked: false,
		menuOpen: false,
		menuItems: [],
		buttons: {
			main: null,
			secondary: null
		}
	};
	const shellAssets = {
		icons: Object.create(null),
		verifiedBadge: null,
		menuPalette: null
	};
	let iframe = null;
	let frameLoaded = false;
	let frameUrl = 'about:blank';
	let reloadSupported = false;
	let reloadTimeout = null;
	let viewportScheduled = false;
	let resizeObserver = null;
	const pendingEvents = [];

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

	function shellPointerPayload(event, extra) {
		const payload = {
			button: event.button,
			x: event.clientX,
			y: event.clientY,
			rootX: event.screenX,
			rootY: event.screenY,
			timeStamp: Math.round(event.timeStamp || 0)
		};
		if (extra && typeof extra === 'object') {
			for (const key in extra) {
				payload[key] = extra[key];
			}
		}
		return payload;
	}

	function beginShellControl(command, event, extra) {
		if (shellState.blocked
			|| shellState.isFullscreen
			|| event.defaultPrevented
			|| event.button !== 0) {
			return;
		}
		closeMenu();
		invoke(command, shellPointerPayload(event, extra));
		event.preventDefault();
	}

	function beginShellMove(event) {
		const target = event.target;
		if (target
			&& target.closest
			&& target.closest('.title-control, #menu')) {
			return;
		}
		beginShellControl('tdesktop_shell_begin_move', event);
	}

	function beginShellResize(edge, event) {
		beginShellControl('tdesktop_shell_begin_resize', event, {
			edge: edge
		});
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
		const height = Math.max(
			0,
			Math.round(frameShell.getBoundingClientRect().height));
		postToFrame('viewport_changed', {
			height: height,
			is_state_stable: true,
			is_expanded: true
		});
	}

	function scheduleViewport() {
		if (viewportScheduled) {
			return;
		}
		viewportScheduled = true;
		window.requestAnimationFrame(function() {
			viewportScheduled = false;
			sendViewportChanged();
		});
	}

	function setMetric(name, value) {
		if (typeof value === 'number' && Number.isFinite(value)) {
			root.style.setProperty(name, String(value) + 'px');
		}
	}

	function applyMetrics(data) {
		if (!data || typeof data !== 'object') {
			return;
		}
		setMetric('--shell-radius', data.shellRadius);
		setMetric('--shell-pad-top', data.shellPaddingTop);
		setMetric('--shell-pad-right', data.shellPaddingRight);
		setMetric('--shell-pad-bottom', data.shellPaddingBottom);
		setMetric('--shell-pad-left', data.shellPaddingLeft);
		setMetric('--shadow-pad-top', data.shadowPaddingTop);
		setMetric('--shadow-pad-right', data.shadowPaddingRight);
		setMetric('--shadow-pad-bottom', data.shadowPaddingBottom);
		setMetric('--shadow-pad-left', data.shadowPaddingLeft);
		setMetric('--header-height', data.headerHeight);
		setMetric('--title-pad-top', data.titlePaddingTop);
		setMetric('--title-pad-right', data.titlePaddingRight);
		setMetric('--title-pad-bottom', data.titlePaddingBottom);
		setMetric('--title-pad-left', data.titlePaddingLeft);
		setMetric('--badge-skip', data.badgeSkip);
		setMetric('--frame-radius', data.frameRadius);
		setMetric('--control-width', data.controlWidth);
		setMetric('--control-height', data.controlHeight);
		setMetric('--button-height', data.buttonHeight);
		setMetric('--button-gap-x', data.buttonGapX);
		setMetric('--button-gap-y', data.buttonGapY);
		setMetric('--disclosure-skip', data.disclosureSkip);
		setMetric('--footer-button-skip', data.footerButtonSkip);
		setMetric('--fullscreen-control-width', data.fullscreenControlWidth);
		setMetric('--fullscreen-control-height', data.fullscreenControlHeight);
		setMetric('--fullscreen-control-top', data.fullscreenControlTop);
		setMetric('--fullscreen-control-right', data.fullscreenControlRight);
		setMetric('--fullscreen-control-gap', data.fullscreenControlGap);
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

	function footerColorForBackground(value) {
		if (!/^#[0-9a-f]{6}$/i.test(value || '')) {
			return null;
		}
		const red = parseInt(value.slice(1, 3), 16) / 255;
		const green = parseInt(value.slice(3, 5), 16) / 255;
		const blue = parseInt(value.slice(5, 7), 16) / 255;
		const luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
		const contrast = 2.5;
		const textLuminance = (luminance > 0.5) ? 0 : 1;
		const adaptiveOpacity = (luminance - textLuminance + contrast) / contrast;
		const opacity = Math.max(0.5, Math.min(0.64, adaptiveOpacity));
		const channel = (luminance > 0.5) ? 0 : 255;
		return 'rgba('
			+ String(channel) + ', '
			+ String(channel) + ', '
			+ String(channel) + ', '
			+ String(opacity) + ')';
	}

	function applyColors(data) {
		const next = (data && data.colors) ? data.colors : data;
		if (!next || typeof next !== 'object') {
			return;
		}
		if (next.bodyBg) {
			root.style.setProperty('--body-bg', next.bodyBg);
			const footerFg = footerColorForBackground(next.bodyBg);
			if (footerFg) {
				root.style.setProperty('--footer-fg', footerFg);
			}
		}
		if (next.titleBg) {
			root.style.setProperty('--title-bg', next.titleBg);
			const titleFg = colorForBackground(next.titleBg);
			if (titleFg) {
				root.style.setProperty('--title-fg', titleFg);
			}
		}
		if (next.bottomBg) {
			root.style.setProperty('--bottom-bg', next.bottomBg);
		}
	}

	function applyAssets(data) {
		if (!data || typeof data !== 'object') {
			return;
		}
		if (data.icons && typeof data.icons === 'object') {
			shellAssets.icons = data.icons;
		}
		if (data.verifiedBadge && typeof data.verifiedBadge === 'object') {
			shellAssets.verifiedBadge = data.verifiedBadge;
		}
		if (data.menuPalette && typeof data.menuPalette === 'object') {
			shellAssets.menuPalette = data.menuPalette;
			if (data.menuPalette.bg) {
				root.style.setProperty('--menu-bg', data.menuPalette.bg);
			}
			if (data.menuPalette.fg) {
				root.style.setProperty('--menu-fg', data.menuPalette.fg);
			}
			if (data.menuPalette.hoverBg) {
				root.style.setProperty(
					'--menu-hover-bg',
					data.menuPalette.hoverBg);
			}
			if (data.menuPalette.separator) {
				root.style.setProperty(
					'--menu-separator',
					data.menuPalette.separator);
			}
			if (data.menuPalette.attention) {
				root.style.setProperty(
					'--menu-attention',
					data.menuPalette.attention);
			}
		}
		if (shellAssets.verifiedBadge && shellAssets.verifiedBadge.url) {
			badge.style.backgroundImage = 'url('
				+ shellAssets.verifiedBadge.url + ')';
			if (shellAssets.verifiedBadge.width) {
				badge.style.width = String(shellAssets.verifiedBadge.width) + 'px';
			}
			if (shellAssets.verifiedBadge.height) {
				badge.style.height = String(shellAssets.verifiedBadge.height) + 'px';
			}
			if (shellAssets.verifiedBadge.alt) {
				badge.setAttribute('aria-label', shellAssets.verifiedBadge.alt);
			}
		} else {
			badge.style.backgroundImage = '';
			badge.removeAttribute('aria-label');
		}
		applyChrome({});
		renderMenu();
	}

	function applyChrome(data) {
		if (!data || typeof data !== 'object') {
			return;
		}
		if (Object.prototype.hasOwnProperty.call(data, 'backVisible')) {
			shellState.backVisible = !!data.backVisible;
		}
		if (Object.prototype.hasOwnProperty.call(data, 'menuVisible')) {
			shellState.menuVisible = !!data.menuVisible;
		}
		if (Object.prototype.hasOwnProperty.call(data, 'badgeVisible')) {
			shellState.badgeVisible = !!data.badgeVisible;
		}
		controls.back.classList.toggle('hidden', !shellState.backVisible);
		controls.menu.classList.toggle('hidden', !shellState.menuVisible);
		controls.menu.disabled = !shellState.menuVisible
			|| !shellState.menuItems.length;
		badge.classList.toggle(
			'hidden',
			!shellState.badgeVisible
				|| !(shellAssets.verifiedBadge && shellAssets.verifiedBadge.url));
		badge.setAttribute(
			'aria-hidden',
			badge.classList.contains('hidden') ? 'true' : 'false');
		if (controls.menu.disabled) {
			closeMenu();
		}
	}

	function visibleButtons() {
		const main = shellState.buttons.main && shellState.buttons.main.visible
			? shellState.buttons.main
			: null;
		const secondary = shellState.buttons.secondary
			&& shellState.buttons.secondary.visible
			? shellState.buttons.secondary
			: null;
		const result = {
			layout: 'single',
			buttons: []
		};
		if (main && secondary) {
			const position = secondary.position || 'left';
			if (position === 'top') {
				result.layout = 'vertical';
				result.buttons = [secondary, main];
			} else if (position === 'bottom') {
				result.layout = 'vertical';
				result.buttons = [main, secondary];
			} else if (position === 'left') {
				result.layout = 'horizontal';
				result.buttons = [secondary, main];
			} else {
				result.layout = 'horizontal';
				result.buttons = [main, secondary];
			}
		} else if (main) {
			result.buttons = [main];
		} else if (secondary) {
			result.buttons = [secondary];
		}
		return result;
	}

	function requestButtonIcon(state) {
		if (!state
			|| !state.visible
			|| !state.iconCustomEmojiId
			|| state.iconResolvedGeneration === state.iconGeneration
			|| state.iconRequestGeneration === state.iconGeneration) {
			return;
		}
		state.iconRequestGeneration = state.iconGeneration;
		invoke('tdesktop_shell_request_button_icon', {
			name: state.name
		});
	}

	function updateFooter() {
		const visible = visibleButtons();
		const hasButtons = !!visible.buttons.length;
		disclosure.textContent = '';
		disclosure.classList.remove('visible');
		buttonsWrap.classList.toggle('visible', hasButtons);
		footer.classList.toggle('visible', hasButtons);
		root.style.setProperty(
			'--footer-gap',
			shellState.isFullscreen
				? '0px'
				: hasButtons
				? 'var(--footer-button-skip)'
				: 'var(--disclosure-skip)');
		scheduleViewport();
	}

	function renderButtons() {
		const visible = visibleButtons();
		buttons.textContent = '';
		buttons.dataset.layout = visible.layout;
		for (const state of visible.buttons) {
			const button = document.createElement('button');
			button.type = 'button';
			button.className = 'shell-button';
			button.disabled = !state.active;
			button.style.background = state.color || '#40a7e3';
			button.style.color = state.textColor || '#ffffff';

			const icon = document.createElement('span');
			icon.className = 'button-icon';
			const iconReady = state.iconResolvedGeneration === state.iconGeneration;
			if (state.iconCustomEmojiId && state.iconUrl) {
				icon.classList.add('visible');
				icon.style.backgroundImage = 'url(' + state.iconUrl + ')';
			} else if (state.iconCustomEmojiId && !iconReady) {
				icon.classList.add('visible', 'pending');
				requestButtonIcon(state);
			}
			button.appendChild(icon);

			const label = document.createElement('span');
			label.className = 'button-label';
			label.textContent = state.text || '';
			button.appendChild(label);

			const spinner = document.createElement('span');
			spinner.className = 'button-spinner';
			if (state.progress) {
				spinner.classList.add('visible');
			}
			button.appendChild(spinner);

			button.addEventListener('click', function() {
				if (!button.disabled) {
					postToFrame(state.name + '_button_pressed', {});
				}
			});
			buttons.appendChild(button);
		}
		updateFooter();
	}

	function menuIconUrl(name) {
		const asset = shellAssets.icons && name ? shellAssets.icons[name] : null;
		return (asset && asset.url) ? asset.url : '';
	}

	function createMenuNode(item, className) {
		const clickable = !!item.id && item.enabled !== false;
		const node = document.createElement(clickable ? 'button' : 'div');
		node.className = className
			+ (item.attention ? ' attention' : '')
			+ (clickable ? '' : ' disabled');
		if (clickable) {
			node.type = 'button';
			node.addEventListener('click', function() {
				if (!shellState.blocked) {
					invoke('tdesktop_shell_menu_action', { id: item.id });
					closeMenu();
				}
			});
		}
		return node;
	}

	function createMenuCopy(item) {
		const copy = document.createElement('span');
		copy.className = 'menu-item-copy';
		const titleNode = document.createElement('span');
		titleNode.className = 'menu-item-title';
		titleNode.textContent = item.text || '';
		copy.appendChild(titleNode);
		if (item.subtitle) {
			const subtitleNode = document.createElement('span');
			subtitleNode.className = 'menu-item-subtitle';
			subtitleNode.textContent = item.subtitle;
			copy.appendChild(subtitleNode);
		}
		return copy;
	}

	function renderMenu() {
		menuList.textContent = '';
		for (const item of shellState.menuItems) {
			if (item.separator) {
				const separator = document.createElement('div');
				separator.className = 'menu-separator';
				menuList.appendChild(separator);
				continue;
			}
			if (Array.isArray(item.children) && item.children.length) {
				const group = document.createElement('div');
				group.className = 'menu-group';

				const header = document.createElement('div');
				header.className = 'menu-item menu-group-title';
				const headerIcon = document.createElement('span');
				headerIcon.className = 'menu-item-icon';
				const headerIconUrl = menuIconUrl(item.icon);
				if (headerIconUrl) {
					headerIcon.style.backgroundImage = 'url('
						+ headerIconUrl + ')';
				}
				header.appendChild(headerIcon);
				header.appendChild(createMenuCopy(item));
				group.appendChild(header);

				const children = document.createElement('div');
				children.className = 'menu-group-children';
				for (const child of item.children) {
					if (child.separator) {
						const separator = document.createElement('div');
						separator.className = 'menu-separator';
						children.appendChild(separator);
						continue;
					}
					const row = createMenuNode(child, 'menu-download');
					row.appendChild(createMenuCopy(child));
					if (child.actionLabel) {
						const action = document.createElement('span');
						action.className = 'menu-item-action';
						action.textContent = child.actionLabel;
						row.appendChild(action);
					}
					children.appendChild(row);
				}
				group.appendChild(children);
				menuList.appendChild(group);
				continue;
			}

			const row = createMenuNode(item, 'menu-item');
			const icon = document.createElement('span');
			icon.className = 'menu-item-icon';
			const iconUrl = menuIconUrl(item.icon);
			if (iconUrl) {
				icon.style.backgroundImage = 'url(' + iconUrl + ')';
			}
			row.appendChild(icon);
			row.appendChild(createMenuCopy(item));
			menuList.appendChild(row);
		}
		menu.classList.toggle(
			'visible',
			shellState.menuOpen
				&& shellState.menuVisible
				&& !!shellState.menuItems.length);
		controls.menu.classList.toggle(
			'active',
			menu.classList.contains('visible'));
	}
	function closeMenu() {
		if (!shellState.menuOpen) {
			return;
		}
		shellState.menuOpen = false;
		renderMenu();
	}

	function toggleMenu() {
		if (shellState.blocked) {
			return;
		}
		if (shellState.menuOpen) {
			closeMenu();
			return;
		}
		invoke('tdesktop_shell_menu_request', {});
		shellState.menuOpen = true;
		renderMenu();
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

	document.addEventListener('mousedown', function(event) {
		if (!shellState.menuOpen) {
			return;
		}
		const target = event.target;
		if (menu.contains(target) || controls.menu.contains(target)) {
			return;
		}
		closeMenu();
	});

	window.addEventListener('keydown', function(event) {
		if (event.key === 'Escape' && shellState.menuOpen) {
			event.preventDefault();
			closeMenu();
		}
	});

	window.addEventListener('resize', scheduleViewport);
	if (window.ResizeObserver) {
		resizeObserver = new window.ResizeObserver(scheduleViewport);
		resizeObserver.observe(frameShell);
		resizeObserver.observe(root);
	}

	controls.close.addEventListener('click', function() {
		invoke('tdesktop_shell_close', {});
	});
	controls.back.addEventListener('click', function() {
		postToFrame('back_button_pressed', {});
	});
	controls.menu.addEventListener('click', toggleMenu);
	header.addEventListener('selectstart', function(event) {
		event.preventDefault();
	});
	header.addEventListener('mousedown', beginShellMove);
	for (const handle of resizeHandles) {
		handle.addEventListener('mousedown', function(event) {
			beginShellResize(handle.getAttribute('data-resize-edge'), event);
		});
	}

	function createIframe(url) {
		closeMenu();
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
			scheduleViewport();
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

	function reloadFrame() {
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
	}

	window.TelegramDesktopShell = {
		bootstrap: function(data) {
			applyMetrics(data && data.metrics);
			applyColors(data && data.colors);
			applyChrome(data || {});
			shellState.bottomText = '';
			title.textContent = (data && data.title) || '';
			document.title = (data && data.title) || 'Telegram';
			frameUrl = (data && data.url) || 'about:blank';
			createIframe(frameUrl);
			renderButtons();
			renderMenu();
		},
		nativeEvent: function(eventType, eventData) {
			if (eventType === 'fullscreen_changed' && eventData) {
				shellState.isFullscreen = !!eventData.is_fullscreen;
				root.classList.toggle('fullscreen', shellState.isFullscreen);
				updateFooter();
			}
			postToFrame(eventType, eventData || {});
		},
		setTitle: function(data) {
			title.textContent = (data && data.title) || '';
			document.title = (data && data.title) || 'Telegram';
		},
		setChrome: function(data) {
			applyChrome(data || {});
		},
		setColors: function(data) {
			applyColors(data || {});
		},
		setAssets: function(data) {
			applyAssets(data || {});
		},
		setMenu: function(data) {
			shellState.menuItems = Array.isArray(data && data.items)
				? data.items
				: [];
			applyChrome({});
			renderMenu();
		},
		setBottomText: function(data) {
			shellState.bottomText = '';
			updateFooter();
		},
		setButton: function(data) {
			if (!data || !data.name) {
				return;
			}
			const previous = shellState.buttons[data.name] || {};
			const next = Object.assign({}, previous, data);
			if (next.iconGeneration !== previous.iconGeneration
				|| next.iconCustomEmojiId !== previous.iconCustomEmojiId) {
				next.iconRequestGeneration = '';
				next.iconResolvedGeneration = '';
				next.iconUrl = '';
			}
			if (!next.iconCustomEmojiId) {
				next.iconRequestGeneration = '';
				next.iconResolvedGeneration = next.iconGeneration || '';
				next.iconUrl = '';
			}
			shellState.buttons[data.name] = next;
			renderButtons();
		},
		setButtonIcon: function(data) {
			if (!data || !data.name) {
				return;
			}
			const state = shellState.buttons[data.name];
			if (!state || state.iconGeneration !== (data.generation || '')) {
				return;
			}
			const icon = (data.icon && typeof data.icon === 'object')
				? data.icon
				: null;
			state.iconRequestGeneration = '';
			state.iconResolvedGeneration = state.iconGeneration;
			state.iconUrl = (icon && icon.url) ? icon.url : '';
			renderButtons();
		},
		setBlocked: function(data) {
			shellState.blocked = !!(data && data.blocked);
			root.classList.toggle('blocked', shellState.blocked);
			if (shellState.blocked) {
				closeMenu();
			}
		},
		setProgress: function(data) {
			root.classList.toggle('loading', !!(data && data.shown));
		},
		reloadFrame: reloadFrame,
		sendViewport: scheduleViewport
	};
})();
)";
}

[[nodiscard]] QByteArray ExternalShellBodyHtml() {
	auto result = QByteArray();
	result.reserve(4096);
	result += R"(<div id="root">
<div id="scene">
<div id="shell">
<header id="header">
<button id="back" class="title-control hidden" type="button" aria-label="Back">
<svg viewBox="0 0 24 24" aria-hidden="true">
<path d="M14.5 5.5L8 12l6.5 6.5"></path>
</svg>
</button>
<div id="title-row">
<div id="title"></div>
<div id="badge" class="hidden" aria-hidden="true"></div>
</div>
<button id="menu-toggle" class="title-control" type="button" aria-label="Menu">
<svg viewBox="0 0 24 24" aria-hidden="true">
<circle class="fill-icon" cx="12" cy="6.5" r="1.7"></circle>
<circle class="fill-icon" cx="12" cy="12" r="1.7"></circle>
<circle class="fill-icon" cx="12" cy="17.5" r="1.7"></circle>
</svg>
</button>
<button id="close" class="title-control" type="button" aria-label="Close">
<svg viewBox="0 0 24 24" aria-hidden="true">
<path d="M6 6l12 12"></path>
<path d="M18 6L6 18"></path>
</svg>
</button>
</header>
<div id="body">
<div id="frame-shell"><main id="frame-wrap"></main></div>
<footer id="footer">
<div id="disclosure"></div>
<div id="buttons-wrap"><div id="buttons"></div></div>
</footer>
</div>
<div id="menu"><div id="menu-list"></div></div>
<div id="blocker"></div>
</div>
</div>
<div id="resize-handles">
<div class="resize-handle" data-resize-edge="top-left"></div>
<div class="resize-handle" data-resize-edge="top"></div>
<div class="resize-handle" data-resize-edge="top-right"></div>
<div class="resize-handle" data-resize-edge="left"></div>
<div class="resize-handle" data-resize-edge="right"></div>
<div class="resize-handle" data-resize-edge="bottom-left"></div>
<div class="resize-handle" data-resize-edge="bottom"></div>
<div class="resize-handle" data-resize-edge="bottom-right"></div>
</div>
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

	void updateArgs(Panel::ButtonArgs &&args);

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

void Panel::Button::updateArgs(Panel::ButtonArgs &&args) {
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
		if (_externalShell) {
			if (!_webview) {
				return;
			}
			applyExternalShellFullscreen(fullscreen);
			sendFullScreen();
			sendSafeArea();
			sendContentSafeArea();
			sendViewport();
			return;
		}
		_widget->toggleFullScreen(fullscreen);
		layoutButtons();
		sendFullScreen();
		sendSafeArea();
		sendContentSafeArea();
	}, _widget->lifetime());

	_widget->fullScreenValue(
	) | rpl::on_next([=](bool fullscreen) {
		if (!_externalShell) {
			_fullscreen = fullscreen;
		}
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
	_bottomText.value() | rpl::on_next([=](const QString &text) {
		if (_externalShell) {
			return;
		}
	}, _widget->lifetime());
	_externalTitleBadgeVisible = (args.titleBadge != nullptr);
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
	ForgetExternalShellColorState(this);
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
		if (_externalShell && _externalShellBootstrapped) {
			sendExternalShellAssets();
			sendExternalShellMenu();
		}
	}, lifetime());

	if (_externalShell) {
		return true;
	}

	const auto dispatch = SharedPanelMenuDispatchArgs{
		.settings = [=] {
			postEvent("settings_button_pressed");
		},
		.reload = [=] {
			if (_webview && _webview->window.widget()) {
				_webview->window.reload();
			} else if (const auto params = _delegate->botThemeParams()
				; createWebview(params)) {
				showWebviewProgress();
				updateThemeParams(params);
				_webview->window.navigate(url);
			}
		},
		.terms = [=] {
			File::OpenUrl(tr::lng_mini_apps_tos_url(tr::now));
		},
		.privacy = [=] {
			_delegate->botOpenPrivacyPolicy();
		},
		.menuButton = [=](MenuButton button) {
			_delegate->botHandleMenuButton(button);
		},
		.download = [=](uint32 id, DownloadsAction type) {
			_delegate->botDownloadsAction(id, type);
		},
	};
	_widget->setMenuAllowed([=](
			const Ui::Menu::MenuCallback &callback) {
		const auto &downloads = _delegate->botDownloads(true);
		const auto items = BuildSharedPanelMenuItems({
			.downloads = &downloads,
			.hasSettings = _webview
				&& _webview->window.widget()
				&& _hasSettingsButton,
			.buttons = _menuButtons,
		});
		FillNativeSharedPanelMenu(
			callback,
			items,
			[=] {
				return rpl::single(_delegate->botDownloads(true))
					| rpl::then(_downloadsUpdated.events(
					) | rpl::map([=] {
						return _delegate->botDownloads();
					}));
			},
			dispatch);
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
		{ u"metrics"_q, ExternalShellMetrics() },
		{ u"colors"_q, ExternalShellColorPayload(this, params) },
		{ u"bottomText"_q, QString() },
		{ u"backVisible"_q, _externalBackVisible },
		{ u"menuVisible"_q, true },
		{ u"badgeVisible"_q, _externalTitleBadgeVisible },
	});
	sendExternalShellAssets();
	sendExternalShellMenu();
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
	auto &state = (name[0] == 'm')
		? _externalMainButton
		: _externalSecondaryButton;
	const auto text = args["text"].toString();
	const auto trimmed = text.trimmed();
	const auto iconCustomEmojiId
		= args["icon_custom_emoji_id"].toString().toULongLong();
	const auto color = ParseColor(args["color"].toString()).value_or(
		st::windowBgActive->c);
	const auto textColor = ParseColor(args["text_color"].toString()).value_or(
		st::windowFgActive->c);
	const auto iconAffectingChanged
		= (state.args.iconCustomEmojiId != iconCustomEmojiId)
		|| (iconCustomEmojiId && state.textColor != textColor);
	state.args = {
		.isActive = args["is_active"].toBool(),
		.isVisible = args["is_visible"].toBool()
			&& (!trimmed.isEmpty() || iconCustomEmojiId),
		.isProgressVisible = args["is_progress_visible"].toBool(),
		.iconCustomEmojiId = iconCustomEmojiId,
		.text = text,
	};
	state.color = color;
	state.textColor = textColor;
	state.position = args["position"].toString();
	if (iconAffectingChanged) {
		++state.iconGeneration;
	}
	const auto visible = state.args.isVisible;
	sendExternalShellMethod("setButton", {
		{ u"name"_q, QString::fromLatin1(name) },
		{ u"visible"_q, visible },
		{ u"active"_q, state.args.isActive },
		{ u"progress"_q, state.args.isProgressVisible },
		{ u"text"_q, state.args.text },
		{ u"color"_q, state.color.name(QColor::HexRgb) },
		{ u"textColor"_q, state.textColor.name(QColor::HexRgb) },
		{ u"position"_q, state.position },
		{ u"iconCustomEmojiId"_q, state.args.iconCustomEmojiId
			? QString::number(state.args.iconCustomEmojiId)
			: QString() },
		{ u"iconGeneration"_q, QString::number(state.iconGeneration) },
	});
}

void Panel::setInitialExternalShellWindowSize() {
	if (!_externalShell || !_webview) {
		return;
	}
	const auto view = _webview->window.widget();
	if (!view) {
		return;
	}
	const auto size = ExternalShellWindowSize(st::botWebViewPanelSize);
	_webview->window.resize(size);
	view->resize(size);
}

void Panel::applyExternalShellFullscreen(bool fullscreen) {
	if (!_externalShell || !_webview) {
		return;
	}
	_webview->window.setFullscreen(fullscreen);
}

void Panel::requestExternalShellButtonEmoji(const QString &name) {
	auto *state = (name == u"main"_q)
		? &_externalMainButton
		: (name == u"secondary"_q)
		? &_externalSecondaryButton
		: nullptr;
	if (!state || !_externalShell) {
		return;
	}
	const auto generation = state->iconGeneration;
	const auto responseName = name;
	const auto weak = base::make_weak(this);
	auto send = [=](QImage image = QImage()) {
		const auto panel = weak.get();
		if (!panel) {
			return;
		}
		const auto *current = (responseName == u"main"_q)
			? &panel->_externalMainButton
			: &panel->_externalSecondaryButton;
		if (current->iconGeneration != generation) {
			return;
		}
		auto data = QJsonObject{
			{ u"name"_q, responseName },
			{ u"generation"_q, QString::number(generation) },
		};
		if (!image.isNull()) {
			data.insert(
				u"icon"_q,
				SerializeRasterAsset(
					image,
					QSize(
						kExternalShellButtonIconSize,
						kExternalShellButtonIconSize)));
		}
		panel->sendExternalShellMethod("setButtonIcon", data);
	};
	if (!state->args.isVisible || !state->args.iconCustomEmojiId) {
		send();
		return;
	}
	_delegate->botResolveButtonEmoji({
		.customEmojiId = state->args.iconCustomEmojiId,
		.textColor = state->textColor,
		.size = kExternalShellButtonIconSize,
		.callback = std::move(send),
	});
}

void Panel::sendExternalShellMenu() {
	const auto &downloads = _delegate->botDownloads(true);
	const auto items = BuildSharedPanelMenuItems({
		.downloads = &downloads,
		.hasSettings = _webview && _webview->window.widget() && _hasSettingsButton,
		.buttons = _menuButtons,
	});
	sendExternalShellMethod("setMenu", {
		{ u"items"_q, SerializeSharedPanelMenu(items) },
	});
}

void Panel::sendExternalShellAssets() {
	const auto &downloads = _delegate->botDownloads(true);
	const auto items = BuildSharedPanelMenuItems({
		.downloads = &downloads,
		.hasSettings = _webview && _webview->window.widget() && _hasSettingsButton,
		.buttons = _menuButtons,
	});
	auto icons = QJsonObject();
	CollectSharedPanelMenuIcons(items, icons);
	sendExternalShellMethod("setAssets", {
		{ u"icons"_q, icons },
		{ u"verifiedBadge"_q, SerializeVerifiedBadgeAsset() },
		{ u"menuPalette"_q, SerializeExternalShellMenuPalette() },
	});
}

void Panel::handleExternalShellMenuAction(const QString &id) {
	DispatchSharedPanelMenuAction(id, {
		.settings = [=] {
			postEvent("settings_button_pressed");
		},
		.reload = [=] {
			if (_webview && _webview->window.widget()) {
				sendExternalShellMethod("reloadFrame", {});
			} else if (const auto params = _delegate->botThemeParams()
				; createWebview(params)) {
				showWebviewProgress();
				updateThemeParams(params);
				_externalShellBootstrapped = false;
				_webview->window.navigate(
					u"https://web.telegram.org/blank.html"_q);
			}
		},
		.terms = [=] {
			File::OpenUrl(tr::lng_mini_apps_tos_url(tr::now));
		},
		.privacy = [=] {
			_delegate->botOpenPrivacyPolicy();
		},
		.menuButton = [=](MenuButton button) {
			_delegate->botHandleMenuButton(button);
		},
		.download = [=](uint32 downloadId, DownloadsAction type) {
			_delegate->botDownloadsAction(downloadId, type);
		},
	});
}

void Panel::sendExternalShellChrome() {
	sendExternalShellMethod("setChrome", {
		{ u"backVisible"_q, _externalBackVisible },
		{ u"menuVisible"_q, true },
		{ u"badgeVisible"_q, _externalTitleBadgeVisible },
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
		setInitialExternalShellWindowSize();
		applyExternalShellFullscreen(_fullscreen.current());
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
		} else if (command == "tdesktop_shell_menu_request") {
			sendExternalShellAssets();
			sendExternalShellMenu();
		} else if (command == "tdesktop_shell_menu_action") {
			handleExternalShellMenuAction(arguments["id"].toString());
		} else if (command == "tdesktop_shell_request_button_icon") {
			requestExternalShellButtonEmoji(arguments["name"].toString());
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
		sendExternalShellAssets();
		sendExternalShellMenu();
	}
}

void Panel::processHeaderColor(const QJsonObject &args) {
	_headerColorReceived = true;
	const auto apply = [&](std::optional<QColor> color) {
		if (_externalShell) {
			SetExternalShellTitleColor(this, color);
			sendExternalShellMethod(
				"setColors",
				ExternalShellColorPayload(this, _delegate->botThemeParams()));
		} else {
			_widget->overrideTitleColor(color);
		}
	};
	if (const auto color = ParseColor(args["color"].toString())) {
		apply(*color);
		_headerColorLifetime.destroy();
	} else if (const auto color = LookupNamedColor(
			args["color_key"].toString())) {
		apply((*color)->c);
		_headerColorLifetime = style::PaletteChanged(
		) | rpl::on_next([=] {
			apply((*color)->c);
		});
	} else {
		apply(std::nullopt);
		_headerColorLifetime.destroy();
	}
}

void Panel::overrideBodyColor(std::optional<QColor> color) {
	if (_externalShell) {
		if (_bodyColorReceived) {
			SetExternalShellBodyColor(this, color);
		}
		sendExternalShellMethod(
			"setColors",
			ExternalShellColorPayload(this, _delegate->botThemeParams()));
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
	const auto apply = [&](std::optional<QColor> color) {
		_bottomBarColor = color;
		if (_externalShell) {
			SetExternalShellBottomColor(this, color);
			sendExternalShellMethod(
				"setColors",
				ExternalShellColorPayload(this, _delegate->botThemeParams()));
		} else {
			_widget->overrideBottomBarColor(color);
		}
	};
	if (const auto color = ParseColor(args["color"].toString())) {
		apply(*color);
		_bottomBarColorLifetime.destroy();
	} else if (const auto color = LookupNamedColor(
			args["color_key"].toString())) {
		apply((*color)->c);
		_bottomBarColorLifetime = style::PaletteChanged(
		) | rpl::on_next([=] {
			apply((*color)->c);
		});
	} else {
		apply(std::nullopt);
		_bottomBarColorLifetime.destroy();
	}
	if (_externalShell) {
		return;
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
		sendExternalShellMethod(
			"setColors",
			ExternalShellColorPayload(this, params));
		sendExternalShellAssets();
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
				ExternalShellColorPayload(this, params));
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
