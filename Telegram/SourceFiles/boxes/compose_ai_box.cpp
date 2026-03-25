/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/compose_ai_box.h"

#include "api/api_compose_with_ai.h"
#include "apiwrap.h"
#include "boxes/premium_preview_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/ui_integration.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/session/session_show.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "settings/sections/settings_premium.h"
#include "spellcheck/platform/platform_language.h"
#include "ui/boxes/choose_language_box.h"
#include "ui/controls/send_button.h"
#include "ui/effects/ripple_animation.h"
#include "ui/effects/skeleton_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/custom_emoji_helper.h"
#include "ui/text/custom_emoji_text_badge.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <algorithm>
#include <array>

#include <QtWidgets/QScrollBar>

namespace HistoryView::Controls {
namespace {

enum class ComposeAiMode {
	Translate,
	Style,
	Fix,
};

enum class CardState {
	Waiting,
	Loading,
	Ready,
	Failed,
};

[[nodiscard]] QColor ComposeAiColorWithAlpha(
		const style::color &color,
		float64 alpha) {
	auto result = color->c;
	result.setAlphaF(result.alphaF() * alpha);
	return result;
}

[[nodiscard]] QString ToneTitle(const QString &key) {
	const auto value = Lang::GetNonDefaultValue(
		QByteArray("cloud_lng_ai_compose_style_") + key.toUtf8());
	if (!value.isEmpty()) {
		return value;
	}
	if (key.isEmpty()) {
		return QString();
	}
	auto result = key;
	result[0] = result[0].toUpper();
	return result;
}

[[nodiscard]] TextWithEntities HighlightDiff(TextWithEntities text) {
	return Ui::Text::Colorized(
		Ui::Text::Wrapped(std::move(text), EntityType::Underline), 1);
}

[[nodiscard]] TextWithEntities StrikeOutDiff(TextWithEntities text) {
	return Ui::Text::Colorized(
		Ui::Text::Wrapped(std::move(text), EntityType::StrikeOut), 2);
}

[[nodiscard]] TextWithEntities BuildDiffDisplay(
		const Api::ComposeWithAi::Diff &diff) {
	auto result = TextWithEntities();
	auto entities = diff.entities;
	std::stable_sort(
		entities.begin(),
		entities.end(),
		[](const auto &a, const auto &b) {
			return a.offset < b.offset;
		});
	const auto size = int(diff.text.text.size());
	auto taken = 0;
	for (const auto &entity : entities) {
		const auto offset = std::clamp(entity.offset, 0, size);
		const auto length = std::clamp(entity.length, 0, size - offset);
		if (offset > taken) {
			result.append(Ui::Text::Mid(diff.text, taken, offset - taken));
		}
		auto part = Ui::Text::Mid(diff.text, offset, length);
		switch (entity.type) {
		case Api::ComposeWithAi::DiffEntity::Type::Insert:
			result.append(HighlightDiff(std::move(part)));
			break;
		case Api::ComposeWithAi::DiffEntity::Type::Replace:
			if (!entity.oldText.isEmpty()) {
				result.append(
					StrikeOutDiff(
						TextWithEntities::Simple(entity.oldText)));
			}
			result.append(HighlightDiff(std::move(part)));
			break;
		case Api::ComposeWithAi::DiffEntity::Type::Delete:
			result.append(StrikeOutDiff(std::move(part)));
			break;
		}
		taken = std::max(taken, offset + length);
	}
	if (taken < size) {
		result.append(Ui::Text::Mid(diff.text, taken));
	}
	return result;
}

[[nodiscard]] QString FromTitle(LanguageId id) {
	return tr::lng_ai_compose_original(tr::now);
}

[[nodiscard]] TextWithEntities ToTitle(LanguageId id) {
	return tr::lng_ai_compose_to_language(
		tr::now,
		lt_language,
		tr::link(Ui::LanguageName(id)),
		tr::marked);
}

[[nodiscard]] LanguageId DefaultAiTranslateTo(LanguageId offeredFrom) {
	const auto current = LanguageId{
		QLocale(Lang::LanguageIdOrDefault(Lang::Id())).language()
	};
	if (current && (current != offeredFrom)) {
		return current;
	}
	const auto english = LanguageId{ QLocale::English };
	if (english != offeredFrom) {
		return english;
	}
	return LanguageId{ QLocale::Spanish };
}

[[nodiscard]] const style::icon &ModeIcon(
		ComposeAiMode mode,
		bool active) {
	switch (mode) {
	case ComposeAiMode::Translate:
		return active
			? st::aiComposeTabTranslateIconActive
			: st::aiComposeTabTranslateIcon;
	case ComposeAiMode::Style:
		return active
			? st::aiComposeTabStyleIconActive
			: st::aiComposeTabStyleIcon;
	case ComposeAiMode::Fix:
		return active
			? st::aiComposeTabFixIconActive
			: st::aiComposeTabFixIcon;
	}
	return active
		? st::aiComposeTabTranslateIconActive
		: st::aiComposeTabTranslateIcon;
}

[[nodiscard]] qreal ComposeAiPillRadius(int height) {
	return height / 2.;
}

[[nodiscard]] qreal ComposeAiStyleRadius() {
	return st::aiComposeStyleTabsRadius;
}

[[nodiscard]] QColor ComposeAiActiveBackgroundColor(
		const style::color &color) {
	return ComposeAiColorWithAlpha(
		color,
		st::aiComposeButtonBgActiveOpacity);
}

[[nodiscard]] QColor ComposeAiRippleColor(
		const style::RippleAnimation &ripple,
		float64 opacity) {
	return ComposeAiColorWithAlpha(
		ripple.color,
		opacity);
}

class ComposeAiModeButton final : public Ui::RippleButton {
public:
	ComposeAiModeButton(
		QWidget *parent,
		ComposeAiMode mode,
		QString label);

	void setSelected(bool selected);
	[[nodiscard]] ComposeAiMode mode() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	[[nodiscard]] QImage prepareRippleMask() const override;

private:
	const ComposeAiMode _mode;
	const QString _label;
	bool _selected = false;

};

class ComposeAiModeTabs final : public Ui::RpWidget {
public:
	ComposeAiModeTabs(QWidget *parent);

	void setActive(ComposeAiMode mode);
	void setChangedCallback(Fn<void(ComposeAiMode)> callback);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const not_null<ComposeAiModeButton*> _translate;
	const not_null<ComposeAiModeButton*> _style;
	const not_null<ComposeAiModeButton*> _fix;
	Fn<void(ComposeAiMode)> _changed;
	ComposeAiMode _active = ComposeAiMode::Translate;

};

class ComposeAiStyleButton final : public Ui::RippleButton {
public:
	ComposeAiStyleButton(
		QWidget *parent,
		Main::AppConfig::AiComposeStyle style);

	void setSelected(bool selected);
	void setExtraPadding(int extra);
	[[nodiscard]] const Main::AppConfig::AiComposeStyle &style() const;
	
protected:
	void paintEvent(QPaintEvent *e) override;
	[[nodiscard]] QImage prepareRippleMask() const override;

private:
	const Main::AppConfig::AiComposeStyle _style;
	const QString _label;
	bool _selected = false;
	int _extraPadding = 0;

};

class ComposeAiStyleTabs final : public Ui::RpWidget {
public:
	ComposeAiStyleTabs(
		QWidget *parent,
		std::vector<Main::AppConfig::AiComposeStyle> styles);

	void setChangedCallback(Fn<void(int)> callback);
	void setActive(int index);
	void resizeForOuterWidth(int outerWidth);
	[[nodiscard]] QString currentTone() const;
	[[nodiscard]] int buttonCount() const;
	[[nodiscard]] rpl::producer<Ui::ScrollToRequest> requestShown() const;

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	std::vector<not_null<ComposeAiStyleButton*>> _buttons;
	Fn<void(int)> _changed;
	int _active = -1;
	rpl::event_stream<Ui::ScrollToRequest> _requestShown;

};

class ComposeAiStyleScrollTabs final : public Ui::RpWidget {
public:
	ComposeAiStyleScrollTabs(
		QWidget *parent,
		std::vector<Main::AppConfig::AiComposeStyle> styles);

	[[nodiscard]] not_null<ComposeAiStyleTabs*> inner() const;

protected:
	int resizeGetHeight(int newWidth) override;

private:
	void updateFades();
	void scrollToButton(int buttonLeft, int buttonRight);

	const not_null<Ui::ScrollArea*> _scroll;
	const not_null<ComposeAiStyleTabs*> _inner;
	const not_null<Ui::RpWidget*> _fadeLeft;
	const not_null<Ui::RpWidget*> _fadeRight;
	const not_null<Ui::RpWidget*> _cornerLeft;
	const not_null<Ui::RpWidget*> _cornerRight;
	Ui::Animations::Simple _scrollAnimation;

};

class ComposeAiPreviewCard final : public Ui::RpWidget {
public:
	ComposeAiPreviewCard(
		QWidget *parent,
		not_null<Main::Session*> session,
		TextWithEntities original);

	void setResizeCallback(Fn<void()> callback);
	void setChooseCallback(Fn<void()> callback);
	void setCopyCallback(Fn<void()> callback);
	void setEmojifyChangedCallback(Fn<void(bool)> callback);
	void setOriginalTitle(const QString &title);
	void setOriginalVisible(bool visible);
	void setResultTitle(const TextWithEntities &title);
	void setEmojifyVisible(bool visible);
	void setEmojifyChecked(bool checked);
	void setState(CardState state);
	void setResultText(TextWithEntities text);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void refreshGeometry();
	void updateOriginalToggleIcon();

	const Ui::Text::MarkedContext _context;
	const TextWithEntities _original;
	const not_null<Ui::FlatLabel*> _originalTitle;
	const not_null<Ui::FlatLabel*> _originalBody;
	const not_null<Ui::IconButton*> _originalToggle;
	const not_null<Ui::FlatLabel*> _resultTitle;
	const not_null<Ui::FlatLabel*> _resultBody;
	const not_null<Ui::IconButton*> _copy;
	const not_null<Ui::Checkbox*> _emojify;
	Fn<void()> _resized;
	Fn<void()> _chooseCallback;
	Fn<void()> _copyCallback;
	Fn<void(bool)> _emojifyChanged;
	bool _originalExpanded = false;
	bool _originalVisible = true;
	bool _emojifyVisible = false;
	bool _dividerVisible = false;
	int _dividerTop = 0;
	CardState _state = CardState::Waiting;
	Ui::SkeletonAnimation _skeleton;
	std::array<Ui::Text::SpecialColor, 2> _diffColors;

};

class ComposeAiContent final : public Ui::RpWidget {
public:
	ComposeAiContent(
		QWidget *parent,
		not_null<Ui::GenericBox*> box,
		ComposeAiBoxArgs args);
	~ComposeAiContent();

	[[nodiscard]] bool hasResult() const;
	[[nodiscard]] const TextWithEntities &result() const;
	[[nodiscard]] const std::vector<Main::AppConfig::AiComposeStyle> &stylesData() const;
	void setReadyChangedCallback(Fn<void(bool)> callback);
	void setPremiumFloodCallback(Fn<void()> callback);
	void setModeChangedCallback(Fn<void(ComposeAiMode)> callback);
	void setStyleSelectedCallback(Fn<void()> callback);
	[[nodiscard]] ComposeAiMode mode() const;
	[[nodiscard]] bool hasStyleSelection() const;
	void setModeTabs(not_null<ComposeAiModeTabs*> tabs);
	void setStyleTabs(not_null<Ui::SlideWrap<ComposeAiStyleScrollTabs>*> stylesWrap);
	void start();

protected:
	int resizeGetHeight(int newWidth) override;

private:
	void refreshLayout();
	void chooseLanguage();
	void copyResult();
	void setMode(ComposeAiMode mode);
	void updateTitles();
	void updatePinnedTabs(anim::type animated);
	void cancelRequest();
	void request();
	void applyResult(Api::ComposeWithAi::Result &&result);
	void showError(const MTP::Error &error);
	void notifyReadyChanged();

	const not_null<Ui::GenericBox*> _box;
	const not_null<Main::Session*> _session;
	const TextWithEntities _original;
	const LanguageId _detectedFrom;
	LanguageId _to;
	const std::vector<Main::AppConfig::AiComposeStyle> _stylesData;
	QPointer<ComposeAiModeTabs> _tabs;
	QPointer<ComposeAiStyleScrollTabs> _styles;
	QPointer<Ui::SlideWrap<ComposeAiStyleScrollTabs>> _stylesWrap;
	const not_null<ComposeAiPreviewCard*> _preview;
	Fn<void(bool)> _readyChanged;
	Fn<void()> _premiumFlood;
	Fn<void(ComposeAiMode)> _modeChanged;
	Fn<void()> _styleSelected;
	ComposeAiMode _mode = ComposeAiMode::Translate;
	int _styleIndex = -1;
	bool _emojify = false;
	CardState _state = CardState::Waiting;
	mtpRequestId _requestId = 0;
	int _requestToken = 0;
	TextWithEntities _result;

};

// ComposeAiModeButton

ComposeAiModeButton::ComposeAiModeButton(
	QWidget *parent,
	ComposeAiMode mode,
	QString label)
: RippleButton(parent, st::aiComposeButtonRippleInactive)
, _mode(mode)
, _label(std::move(label)) {
	setCursor(style::cur_pointer);
}

void ComposeAiModeButton::setSelected(bool selected) {
	if (_selected == selected) {
		return;
	}
	_selected = selected;
	update();
}

ComposeAiMode ComposeAiModeButton::mode() const {
	return _mode;
}

void ComposeAiModeButton::paintEvent(QPaintEvent *e) {
	Painter p(this);
	PainterHighQualityEnabler hq(p);

	const auto radius = ComposeAiPillRadius(height());
	if (_selected) {
		p.setPen(Qt::NoPen);
		p.setBrush(ComposeAiActiveBackgroundColor(
			st::aiComposeTabButtonBgActive));
		p.drawRoundedRect(
			rect(),
			radius,
			radius);
	}
	const auto ripple = ComposeAiRippleColor(
		_selected
			? st::aiComposeButtonRippleActive
			: st::aiComposeButtonRippleInactive,
		_selected
			? st::aiComposeButtonRippleActiveOpacity
			: st::aiComposeButtonRippleInactiveOpacity);
	paintRipple(p, 0, 0, &ripple);

	const auto &icon = ModeIcon(_mode, _selected);
	const auto iconLeft = (width() - icon.width()) / 2;
	icon.paint(p, iconLeft, st::aiComposeTabIconTop, width());

	p.setPen(_selected
		? st::aiComposeTabLabelFgActive
		: st::aiComposeTabLabelFg);
	p.setFont(st::aiComposeTabLabelFont);
	p.drawText(
		QRect(
			0,
			st::aiComposeTabLabelTop,
			width(),
			height() - st::aiComposeTabLabelTop),
		Qt::AlignHCenter | Qt::AlignTop,
		_label);
}

QImage ComposeAiModeButton::prepareRippleMask() const {
	return Ui::RippleAnimation::MaskByDrawer(size(), false, [&](QPainter &p) {
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		const auto radius = ComposeAiPillRadius(height());
		p.drawRoundedRect(
			rect(),
			radius,
			radius);
	});
}

// ComposeAiModeTabs

ComposeAiModeTabs::ComposeAiModeTabs(QWidget *parent)
: RpWidget(parent)
, _translate(Ui::CreateChild<ComposeAiModeButton>(
	this,
	ComposeAiMode::Translate,
	tr::lng_ai_compose_tab_translate(tr::now)))
, _style(Ui::CreateChild<ComposeAiModeButton>(
	this,
	ComposeAiMode::Style,
	tr::lng_ai_compose_tab_style(tr::now)))
, _fix(Ui::CreateChild<ComposeAiModeButton>(
	this,
	ComposeAiMode::Fix,
	tr::lng_ai_compose_tab_fix(tr::now))) {
	const auto bind = [=](not_null<ComposeAiModeButton*> button) {
		button->setClickedCallback([=] {
			setActive(button->mode());
			if (_changed) {
				_changed(button->mode());
			}
		});
	};
	bind(_translate);
	bind(_style);
	bind(_fix);
	setActive(ComposeAiMode::Translate);
}

void ComposeAiModeTabs::setActive(ComposeAiMode mode) {
	_active = mode;
	_translate->setSelected(mode == ComposeAiMode::Translate);
	_style->setSelected(mode == ComposeAiMode::Style);
	_fix->setSelected(mode == ComposeAiMode::Fix);
}

void ComposeAiModeTabs::setChangedCallback(Fn<void(ComposeAiMode)> callback) {
	_changed = std::move(callback);
}

int ComposeAiModeTabs::resizeGetHeight(int newWidth) {
	const auto padding = st::aiComposeTabsPadding;
	const auto skip = st::aiComposeTabsSkip;
	const auto innerWidth = newWidth - padding.left() - padding.right();
	const auto buttonWidth = (innerWidth - (2 * skip)) / 3;
	const auto buttonHeight = st::aiComposeTabsHeight
		- padding.top()
		- padding.bottom();
	const auto top = padding.top();
	auto left = padding.left();
	for (const auto button : { _translate, _style, _fix }) {
		button->setGeometry(left, top, buttonWidth, buttonHeight);
		left += buttonWidth + skip;
	}
	return st::aiComposeTabsHeight;
}

void ComposeAiModeTabs::paintEvent(QPaintEvent *e) {
	Painter p(this);
	PainterHighQualityEnabler hq(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::aiComposeTabsBg);
	const auto radius = st::aiComposeTabsRadius;
	p.drawRoundedRect(
		rect(),
		radius,
		radius);
}

// ComposeAiStyleButton

ComposeAiStyleButton::ComposeAiStyleButton(
	QWidget *parent,
	Main::AppConfig::AiComposeStyle style)
: RippleButton(parent, st::aiComposeButtonRippleInactive)
, _style(std::move(style))
, _label(ToneTitle(_style.key)) {
	setCursor(style::cur_pointer);
	setNaturalWidth([&] {
		const auto padding = st::aiComposeStyleButtonPadding;
		const auto labelWidth = st::aiComposeStyleLabelFont->width(_label);
		const auto emojiWidth = st::aiComposeStyleEmojiFont->width(_style.emoji);
		return padding.left()
			+ std::max(labelWidth, emojiWidth)
			+ padding.right();
	}());
	setExtraPadding(0);
}

void ComposeAiStyleButton::setSelected(bool selected) {
	if (_selected == selected) {
		return;
	}
	_selected = selected;
	update();
}

void ComposeAiStyleButton::setExtraPadding(int extra) {
	_extraPadding = extra;
	resize(naturalWidth() + 2 * extra, height());
}

const Main::AppConfig::AiComposeStyle &ComposeAiStyleButton::style() const {
	return _style;
}

void ComposeAiStyleButton::paintEvent(QPaintEvent *e) {
	Painter p(this);
	PainterHighQualityEnabler hq(p);

	const auto radius = ComposeAiStyleRadius();
	if (_selected) {
		p.setPen(Qt::NoPen);
		p.setBrush(ComposeAiActiveBackgroundColor(
			st::aiComposeStyleButtonBgActive));
		p.drawRoundedRect(
			rect(),
			radius,
			radius);
	}
	const auto ripple = ComposeAiRippleColor(
		_selected
			? st::aiComposeButtonRippleActive
			: st::aiComposeButtonRippleInactive,
		_selected
			? st::aiComposeButtonRippleActiveOpacity
			: st::aiComposeButtonRippleInactiveOpacity);
	paintRipple(p, 0, 0, &ripple);

	const auto emojiRect = QRect(
		0,
		st::aiComposeStyleEmojiTop,
		width(),
		st::aiComposeStyleEmojiFont->height);
	p.setPen(_selected
		? st::aiComposeStyleLabelFgActive
		: st::aiComposeStyleLabelFg);
	p.setFont(st::aiComposeStyleEmojiFont);
	p.drawText(emojiRect, Qt::AlignHCenter | Qt::AlignTop, _style.emoji);

	p.setPen(_selected
		? st::aiComposeStyleLabelFgActive
		: st::aiComposeStyleLabelFg);
	p.setFont(st::aiComposeStyleLabelFont);
	p.drawText(
		QRect(
			0,
			st::aiComposeStyleLabelTop,
			width(),
			height() - st::aiComposeStyleLabelTop),
		Qt::AlignHCenter | Qt::AlignTop,
		_label);
}

QImage ComposeAiStyleButton::prepareRippleMask() const {
	return Ui::RippleAnimation::MaskByDrawer(size(), false, [&](QPainter &p) {
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		const auto radius = ComposeAiStyleRadius();
		p.drawRoundedRect(
			rect(),
			radius,
			radius);
	});
}

// ComposeAiStyleTabs

ComposeAiStyleTabs::ComposeAiStyleTabs(
	QWidget *parent,
	std::vector<Main::AppConfig::AiComposeStyle> styles)
: RpWidget(parent) {
	_buttons.reserve(styles.size());
	for (auto &style : styles) {
		const auto button = Ui::CreateChild<ComposeAiStyleButton>(
			this,
			std::move(style));
		button->setClickedCallback([=] {
			const auto i = ranges::find(_buttons, not_null(button));
			const auto index = int(i - begin(_buttons));
			if (index == _buttons.size()) {
				return;
			}
			setActive(index);
			_requestShown.fire({
				_buttons[index]->x(),
				_buttons[index]->x() + _buttons[index]->width(),
			});
			if (_changed) {
				_changed(index);
			}
		});
		_buttons.push_back(button);
	}
	setNaturalWidth([&] {
		const auto padding = st::aiComposeStyleTabsPadding;
		const auto skip = st::aiComposeStyleTabsSkip;
		auto total = padding.left();
		for (const auto button : _buttons) {
			total += button->naturalWidth() + skip;
		}
		return total - skip + padding.right();
	}());
	setActive(-1);
}

void ComposeAiStyleTabs::setChangedCallback(Fn<void(int)> callback) {
	_changed = std::move(callback);
}

void ComposeAiStyleTabs::setActive(int index) {
	if (index < -1 || index >= int(_buttons.size())) {
		return;
	}
	_active = index;
	for (auto i = 0; i != int(_buttons.size()); ++i) {
		_buttons[i]->setSelected(i == index);
	}
}

void ComposeAiStyleTabs::resizeForOuterWidth(int outerWidth) {
	const auto count = int(_buttons.size());
	const auto padding = st::aiComposeStyleTabsPadding;
	const auto skip = st::aiComposeStyleTabsSkip;
	const auto bheight = st::aiComposeStyleTabsHeight
		- padding.top()
		- padding.bottom();
	const auto height = st::aiComposeStyleTabsHeight;
	auto left = padding.left();
	const auto guard = gsl::finally([&] {
		resize(left - skip + padding.right(), height);
	});
	if (!count) {
		return;
	}
	const auto setExtraPaddingFor = [&](
			not_null<ComposeAiStyleButton*> button,
			int value) {
		button->setExtraPadding(value);

		const auto w = button->width();
		button->setGeometry(left, padding.top(), w, bheight);
		left += w + skip;
	};
	const auto diff = naturalWidth() - outerWidth;
	if (diff > 0) {
		auto total = left;
		for (auto fit = 0; fit != count;) {
			const auto width = _buttons[fit]->naturalWidth();
			const auto tooLarge = (total + (width / 2) > outerWidth);
			if (!tooLarge) {
				++fit;
				total += width + skip;
			}
			if (tooLarge || (fit == count)) {
				if (fit > 0) {
					const auto width = _buttons[fit - 1]->naturalWidth();
					const auto desired = total - skip - (width / 2);
					const auto add = outerWidth - desired;
					const auto extra = add / ((fit - 1) * 2 + 1);
					for (const auto &button : _buttons) {
						setExtraPaddingFor(button, extra);
					}
				} else {
					for (const auto &button : _buttons) {
						setExtraPaddingFor(button, 0);
					}
				}
				return;
			}
		}
		Unexpected("Tabs width inconsistency.");
	} else {
		const auto add = -diff / 2;
		const auto each = add / _buttons.size();
		const auto more = add - (each * _buttons.size());
		for (auto i = 0; i < more; ++i) {
			setExtraPaddingFor(_buttons[i], each + 1);
		}
		for (auto i = more; i < count; ++i) {
			setExtraPaddingFor(_buttons[i], each);
		}
	}
}

QString ComposeAiStyleTabs::currentTone() const {
	return (_active >= 0 && _active < int(_buttons.size()))
		? _buttons[_active]->style().key
		: QString();
}

int ComposeAiStyleTabs::buttonCount() const {
	return int(_buttons.size());
}

rpl::producer<Ui::ScrollToRequest> ComposeAiStyleTabs::requestShown() const {
	return _requestShown.events();
}

void ComposeAiStyleTabs::paintEvent(QPaintEvent *e) {
	Painter p(this);
	PainterHighQualityEnabler hq(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::aiComposeStyleTabsBg);
	const auto radius = ComposeAiStyleRadius();
	p.drawRoundedRect(
		rect(),
		radius,
		radius);
}

ComposeAiStyleScrollTabs::ComposeAiStyleScrollTabs(
	QWidget *parent,
	std::vector<Main::AppConfig::AiComposeStyle> styles)
: RpWidget(parent)
, _scroll(Ui::CreateChild<Ui::ScrollArea>(
	this,
	st::aiComposeStyleTabsScroll))
, _inner(_scroll->setOwnedWidget(
	object_ptr<ComposeAiStyleTabs>(this, std::move(styles))))
, _fadeLeft(Ui::CreateChild<Ui::RpWidget>(this))
, _fadeRight(Ui::CreateChild<Ui::RpWidget>(this))
, _cornerLeft(Ui::CreateChild<Ui::RpWidget>(this))
, _cornerRight(Ui::CreateChild<Ui::RpWidget>(this)) {
	_scroll->setCustomWheelProcess([=](not_null<QWheelEvent*> e) {
		const auto pixelDelta = e->pixelDelta();
		const auto angleDelta = e->angleDelta();
		if (std::abs(pixelDelta.x()) + std::abs(angleDelta.x())) {
			return false;
		}
		const auto y = pixelDelta.y() ? pixelDelta.y() : angleDelta.y();
		_scrollAnimation.stop();
		_scroll->scrollToX(_scroll->scrollLeft() - y);
		return true;
	});

	const auto setupFade = [&](
			not_null<Ui::RpWidget*> fade,
			bool left) {
		fade->setAttribute(Qt::WA_TransparentForMouseEvents);
		fade->paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(fade);
			const auto w = fade->width();
			const auto bg = st::aiComposeStyleTabsBg->c;
			auto transparent = bg;
			transparent.setAlpha(0);
			auto gradient = QLinearGradient(0, 0, w, 0);
			if (left) {
				gradient.setColorAt(0., bg);
				gradient.setColorAt(1., transparent);
			} else {
				gradient.setColorAt(0., transparent);
				gradient.setColorAt(1., bg);
			}
			p.fillRect(fade->rect(), gradient);
		}, fade->lifetime());
		fade->hide();
	};
	setupFade(_fadeLeft, true);
	setupFade(_fadeRight, false);

	const auto setupCorner = [&](
			not_null<Ui::RpWidget*> corner,
			bool left) {
		corner->setAttribute(Qt::WA_TransparentForMouseEvents);
		corner->paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(corner);
			PainterHighQualityEnabler hq(p);
			const auto w = corner->width();
			const auto h = corner->height();
			auto mask = QPainterPath();
			mask.addRect(0, 0, w, h);
			auto rounded = QPainterPath();
			if (left) {
				rounded.addRoundedRect(0, 0, w * 2, h, w, w);
			} else {
				rounded.addRoundedRect(-w, 0, w * 2, h, w, w);
			}
			p.setPen(Qt::NoPen);
			p.setBrush(st::boxDividerBg);
			p.drawPath(mask.subtracted(rounded));
		}, corner->lifetime());
	};
	setupCorner(_cornerLeft, true);
	setupCorner(_cornerRight, false);

	_scroll->scrolls(
	) | rpl::on_next([=] {
		updateFades();
	}, lifetime());

	rpl::combine(
		widthValue(),
		_scroll->widthValue(),
		_inner->widthValue()
	) | rpl::on_next([=] {
		updateFades();
	}, lifetime());

	_inner->requestShown(
	) | rpl::on_next([=](Ui::ScrollToRequest request) {
		scrollToButton(request.ymin, request.ymax);
	}, lifetime());
}

not_null<ComposeAiStyleTabs*> ComposeAiStyleScrollTabs::inner() const {
	return _inner;
}

int ComposeAiStyleScrollTabs::resizeGetHeight(int newWidth) {
	_scroll->setGeometry(0, 0, newWidth, st::aiComposeStyleTabsHeight);

	_inner->resizeForOuterWidth(newWidth);

	const auto fadeWidth = st::aiComposeStyleFadeWidth;
	const auto fadeHeight = st::aiComposeStyleTabsHeight;
	_fadeLeft->setGeometry(0, 0, fadeWidth, fadeHeight);
	_fadeRight->setGeometry(
		newWidth - fadeWidth, 0, fadeWidth, fadeHeight);
	_fadeLeft->raise();
	_fadeRight->raise();

	const auto radius = st::aiComposeStyleTabsRadius;
	_cornerLeft->setGeometry(0, 0, radius, fadeHeight);
	_cornerRight->setGeometry(newWidth - radius, 0, radius, fadeHeight);
	_cornerLeft->raise();
	_cornerRight->raise();

	updateFades();
	return st::aiComposeStyleTabsHeight;
}

void ComposeAiStyleScrollTabs::updateFades() {
	const auto scrollLeft = _scroll->scrollLeft();
	const auto scrollMax = _scroll->scrollLeftMax();
	_fadeLeft->setVisible(scrollLeft > 0);
	_fadeRight->setVisible(scrollLeft < scrollMax);
}

void ComposeAiStyleScrollTabs::scrollToButton(
		int buttonLeft,
		int buttonRight) {
	const auto full = _scroll->width();
	const auto tab = buttonRight - buttonLeft;
	if (tab < full) {
		const auto add = std::min(full - tab, tab) / 2;
		buttonRight += add;
		buttonLeft -= add;
	}
	const auto scrollLeft = _scroll->scrollLeft();
	const auto needed = (buttonLeft < scrollLeft)
		|| (buttonRight > scrollLeft + full);
	if (!needed) {
		return;
	}
	const auto target = (buttonLeft < scrollLeft)
		? buttonLeft
		: std::min(buttonLeft, buttonRight - full);
	_scrollAnimation.stop();
	_scrollAnimation.start([=] {
		_scroll->scrollToX(qRound(_scrollAnimation.value(target)));
	}, scrollLeft, target, st::slideDuration, anim::sineInOut);
}

// ComposeAiPreviewCard

ComposeAiPreviewCard::ComposeAiPreviewCard(
	QWidget *parent,
	not_null<Main::Session*> session,
	TextWithEntities original)
: RpWidget(parent)
, _context(Core::TextContext({ .session = session }))
, _original(std::move(original))
, _originalTitle(Ui::CreateChild<Ui::FlatLabel>(
	this,
	st::aiComposeCardTitle))
, _originalBody(Ui::CreateChild<Ui::FlatLabel>(
	this,
	st::aboutLabel))
, _originalToggle(Ui::CreateChild<Ui::IconButton>(
	this,
	st::aiComposeExpandButton))
, _resultTitle(Ui::CreateChild<Ui::FlatLabel>(
	this,
	st::aiComposeCardTitle))
, _resultBody(Ui::CreateChild<Ui::FlatLabel>(
	this,
	st::aboutLabel))
, _copy(Ui::CreateChild<Ui::IconButton>(
	this,
	st::aiComposeCopyButton))
, _emojify(
	Ui::CreateChild<Ui::Checkbox>(
		this,
		tr::lng_ai_compose_emojify(tr::now),
		st::aiComposeEmojifyCheckbox,
		std::make_unique<Ui::RoundCheckView>(st::defaultCheck,false)))
, _skeleton(_resultBody) {
	_originalBody->setSelectable(true);
	_originalBody->setMarkedText(_original, _context);
	_resultTitle->setClickHandlerFilter([=](const auto &...) {
		if (_chooseCallback) {
			_chooseCallback();
		}
		return false;
	});
	_resultBody->setSelectable(true);
	_diffColors[0] = { &st::boxTextFgGood->p, &st::boxTextFgGood->p };
	_diffColors[1] = { &st::attentionButtonFg->p, &st::attentionButtonFg->p };
	_resultBody->setColors(_diffColors);
	_originalToggle->setClickedCallback([=] {
		_originalExpanded = !_originalExpanded;
		updateOriginalToggleIcon();
		if (_resized) {
			_resized();
		}
	});
	_copy->setClickedCallback([=] {
		if (_copyCallback) {
			_copyCallback();
		}
	});
	_emojify->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (_emojifyChanged) {
			_emojifyChanged(checked);
		}
	}, _emojify->lifetime());
	setOriginalTitle(tr::lng_ai_compose_original(tr::now));
	setResultTitle(tr::lng_ai_compose_result(tr::now, tr::marked));
	_resultBody->setMarkedText(_original, _context);
	_copy->setVisible(false);
	updateOriginalToggleIcon();
}

void ComposeAiPreviewCard::setResizeCallback(Fn<void()> callback) {
	_resized = std::move(callback);
}

void ComposeAiPreviewCard::setChooseCallback(Fn<void()> callback) {
	_chooseCallback = std::move(callback);
}

void ComposeAiPreviewCard::setCopyCallback(Fn<void()> callback) {
	_copyCallback = std::move(callback);
}

void ComposeAiPreviewCard::setEmojifyChangedCallback(Fn<void(bool)> callback) {
	_emojifyChanged = std::move(callback);
}

void ComposeAiPreviewCard::setOriginalTitle(const QString &title) {
	_originalTitle->setText(title);
	refreshGeometry();
}

void ComposeAiPreviewCard::setOriginalVisible(bool visible) {
	if (_originalVisible == visible) {
		return;
	}
	_originalVisible = visible;
	_originalTitle->setVisible(visible);
	_originalBody->setVisible(visible);
	_originalToggle->setVisible(false);
	refreshGeometry();
}

void ComposeAiPreviewCard::setResultTitle(const TextWithEntities &title) {
	_resultTitle->setMarkedText(title);
	refreshGeometry();
}

void ComposeAiPreviewCard::setEmojifyVisible(bool visible) {
	_emojifyVisible = visible;
	_emojify->setVisible(visible);
	refreshGeometry();
}

void ComposeAiPreviewCard::setEmojifyChecked(bool checked) {
	_emojify->setChecked(checked, Ui::Checkbox::NotifyAboutChange::DontNotify);
	refreshGeometry();
}

void ComposeAiPreviewCard::setState(CardState state) {
	if (_state == state) {
		return;
	}
	const auto wasLoading = (_state == CardState::Loading);
	_state = state;
	switch (_state) {
	case CardState::Waiting:
	case CardState::Failed:
		_resultBody->setMarkedText(_original, _context);
		_copy->setVisible(false);
		if (wasLoading) {
			_skeleton.stop();
		}
		break;
	case CardState::Loading:
		_resultBody->setMarkedText(_original, _context);
		_copy->setVisible(false);
		_skeleton.start();
		break;
	case CardState::Ready:
		_copy->setVisible(true);
		if (wasLoading) {
			_skeleton.stop();
		}
		break;
	}
	refreshGeometry();
}

void ComposeAiPreviewCard::setResultText(TextWithEntities text) {
	_resultBody->setMarkedText(std::move(text), _context);
	refreshGeometry();
}

int ComposeAiPreviewCard::resizeGetHeight(int newWidth) {
	const auto padding = st::aiComposeCardPadding;
	const auto contentWidth = newWidth - padding.left() - padding.right();
	auto y = padding.top();

	_dividerVisible = false;
	if (_originalVisible) {
		_originalTitle->show();
		_originalBody->show();
		_originalTitle->resizeToWidth(contentWidth);
		_originalToggle->setVisible(false);
		_originalToggle->moveToRight(
			padding.right(),
			(y + (_originalTitle->height() - _originalToggle->height()) / 2),
			newWidth);
		const auto originalTitleWidth = contentWidth
			- _originalToggle->width()
			- st::aiComposeCardControlSkip;
		_originalTitle->setGeometryToLeft(
			padding.left(),
			y,
			std::max(originalTitleWidth, 0),
			_originalTitle->height(),
			newWidth);
		y += std::max(_originalTitle->height(), _originalToggle->height());
		y += st::aiComposeCardTextSkip;

		_originalBody->resizeToWidth(contentWidth);
		const auto lineHeight = _originalBody->st().style.lineHeight;
		const auto fullOriginalHeight = _originalBody->height();
		const auto originalHeight = _originalExpanded
			? fullOriginalHeight
			: std::min(fullOriginalHeight, lineHeight);
		_originalBody->setGeometryToLeft(
			padding.left(),
			y,
			contentWidth,
			originalHeight,
			newWidth);
		const auto expandable = fullOriginalHeight > lineHeight;
		_originalToggle->setVisible(expandable);
		y += originalHeight + st::aiComposeCardSectionSkip;
		_dividerTop = y;
		_dividerVisible = true;
		y += st::lineWidth + st::aiComposeCardSectionSkip;
	} else {
		_originalTitle->hide();
		_originalBody->hide();
		_originalToggle->hide();
	}

	_resultTitle->show();
	auto controlsWidth = 0;
	if (_emojifyVisible) {
		_emojify->show();
		_emojify->resizeToNaturalWidth(contentWidth);
		controlsWidth += _emojify->width()
			+ st::aiComposeCardControlSkip;
	} else {
		_emojify->hide();
	}
	const auto resultTitleWidth = std::max(
		contentWidth - controlsWidth,
		0);
	_resultTitle->resizeToWidth(resultTitleWidth);
	auto right = padding.right();
	if (_emojifyVisible) {
		_emojify->moveToRight(right, y, newWidth);
		right += _emojify->width() + st::aiComposeCardControlSkip;
	}
	_resultTitle->setGeometryToLeft(
		padding.left(),
		y,
		resultTitleWidth,
		_resultTitle->height(),
		newWidth);
	y += std::max({
		_resultTitle->height(),
		_emojifyVisible ? _emojify->height() : 0,
	});
	y += st::aiComposeCardTextSkip;

	const auto lineHeight = _resultBody->st().style.lineHeight
		? _resultBody->st().style.lineHeight
		: _resultBody->st().style.font->height;
	if (_copy->isVisible()) {
		_resultBody->setSkipBlock(
			_copy->width(),
			lineHeight);
	} else {
		_resultBody->setSkipBlock(0, 0);
	}
	_resultBody->resizeToWidth(contentWidth);
	_resultBody->setGeometryToLeft(
		padding.left(),
		y,
		contentWidth,
		_resultBody->height(),
		newWidth);
	if (_copy->isVisible()) {
		_copy->moveToRight(
			padding.right(),
			y + _resultBody->height() - lineHeight,
			newWidth);
	}
	y += _resultBody->height();

	return y + padding.bottom();
}

void ComposeAiPreviewCard::paintEvent(QPaintEvent *e) {
	Painter p(this);
	PainterHighQualityEnabler hq(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::aiComposeCardBg);
	p.drawRoundedRect(
		rect(),
		st::aiComposeCardRadius,
		st::aiComposeCardRadius);
	if (_dividerVisible) {
		p.setBrush(Qt::NoBrush);
		p.setPen(st::aiComposeCardDivider);
		p.drawLine(
			st::aiComposeCardPadding.left(),
			_dividerTop,
			width() - st::aiComposeCardPadding.right(),
			_dividerTop);
	}
}

void ComposeAiPreviewCard::refreshGeometry() {
	if (width() > 0) {
		resizeToWidth(width());
	}
	if (_resized) {
		_resized();
	}
}

void ComposeAiPreviewCard::updateOriginalToggleIcon() {
	_originalToggle->setIconOverride(
		_originalExpanded ? &st::aiComposeCollapseIcon : nullptr,
		_originalExpanded ? &st::aiComposeCollapseIcon : nullptr);
}

// ComposeAiContent

ComposeAiContent::ComposeAiContent(
	QWidget *parent,
	not_null<Ui::GenericBox*> box,
	ComposeAiBoxArgs args)
: RpWidget(parent)
, _box(box)
, _session(args.session)
, _original(std::move(args.text))
, _detectedFrom(Platform::Language::Recognize(_original.text))
, _to(DefaultAiTranslateTo(_detectedFrom))
, _stylesData(_session->appConfig().aiComposeStyles())
, _preview(Ui::CreateChild<ComposeAiPreviewCard>(this, _session, _original)) {
	_preview->setResizeCallback([=] { refreshLayout(); });
	_preview->setChooseCallback([=] { chooseLanguage(); });
	_preview->setCopyCallback([=] { copyResult(); });
	_preview->setEmojifyChangedCallback([=](bool checked) {
		_emojify = checked;
		if (_mode != ComposeAiMode::Fix) {
			request();
		}
	});
}

ComposeAiContent::~ComposeAiContent() {
	cancelRequest();
}

bool ComposeAiContent::hasResult() const {
	return _state == CardState::Ready;
}

const TextWithEntities &ComposeAiContent::result() const {
	return _result;
}

const std::vector<Main::AppConfig::AiComposeStyle> &ComposeAiContent::stylesData() const {
	return _stylesData;
}

void ComposeAiContent::setReadyChangedCallback(Fn<void(bool)> callback) {
	_readyChanged = std::move(callback);
}

void ComposeAiContent::setModeTabs(not_null<ComposeAiModeTabs*> tabs) {
	_tabs = tabs;
	_tabs->setChangedCallback([=](ComposeAiMode mode) {
		setMode(mode);
	});
	_tabs->setActive(_mode);
}

void ComposeAiContent::setStyleTabs(
		not_null<Ui::SlideWrap<ComposeAiStyleScrollTabs>*> stylesWrap) {
	_stylesWrap = stylesWrap;
	_stylesWrap->setDuration(0);
	_styles = stylesWrap->entity();
	_styles->inner()->setChangedCallback([=](int index) {
		if (index >= 0 && index < int(_stylesData.size())) {
			const auto wasNoSelection = (_styleIndex < 0);
			_styleIndex = index;
			updateTitles();
			if (_mode == ComposeAiMode::Style) {
				request();
				if (wasNoSelection && _styleSelected) {
					_styleSelected();
				}
			}
		}
	});
	_styles->inner()->setActive(_styleIndex);
	_stylesWrap->toggle(_mode == ComposeAiMode::Style, anim::type::instant);
}

void ComposeAiContent::start() {
	updatePinnedTabs(anim::type::instant);
	updateTitles();
	request();
}

int ComposeAiContent::resizeGetHeight(int newWidth) {
	_preview->resizeToWidth(newWidth);
	_preview->moveToLeft(0, 0, newWidth);
	return _preview->height();
}

void ComposeAiContent::refreshLayout() {
	if (width() > 0) {
		resizeToWidth(width());
	}
}

void ComposeAiContent::chooseLanguage() {
	if (_mode != ComposeAiMode::Translate) {
		return;
	}
	const auto weak = QPointer<ComposeAiContent>(this);
	_box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		Ui::ChooseLanguageBox(
			box,
			tr::lng_languages(),
			[=](std::vector<LanguageId> ids) {
				if (!weak || ids.empty()) {
					return;
				}
				weak->_to = ids.front();
				weak->updateTitles();
				weak->request();
			},
			{ _to },
			false,
			nullptr);
	}));
}

void ComposeAiContent::copyResult() {
	if (_state != CardState::Ready) {
		return;
	}
	TextUtilities::SetClipboardText(
		TextForMimeData::WithExpandedLinks(_result));
}

void ComposeAiContent::setMode(ComposeAiMode mode) {
	if (_mode == mode) {
		return;
	}
	_mode = mode;
	_state = CardState::Waiting;
	_preview->setState(CardState::Waiting);
	if (_modeChanged) {
		_modeChanged(_mode);
	}
	updatePinnedTabs(anim::type::normal);
	updateTitles();
	refreshLayout();
	request();
}

void ComposeAiContent::updateTitles() {
	const auto hasResult = (_state == CardState::Loading)
		|| (_state == CardState::Ready);
	_preview->setOriginalVisible(hasResult);
	_preview->setOriginalTitle(
		(_mode == ComposeAiMode::Translate)
			? FromTitle(_detectedFrom)
			: tr::lng_ai_compose_original(tr::now));
	_preview->setResultTitle(
		hasResult
			? ((_mode == ComposeAiMode::Translate)
				? ToTitle(_to)
				: tr::lng_ai_compose_result(tr::now, tr::marked))
			: tr::lng_ai_compose_original(tr::now, tr::marked));
	_preview->setEmojifyVisible(
		hasResult && (_mode != ComposeAiMode::Fix));
	_preview->setEmojifyChecked(_emojify);
}

void ComposeAiContent::updatePinnedTabs(anim::type animated) {
	if (_tabs) {
		_tabs->setActive(_mode);
	}
	if (_styles) {
		_styles->inner()->setActive(_styleIndex);
	}
	if (_stylesWrap) {
		_stylesWrap->toggle(_mode == ComposeAiMode::Style, animated);
	}
}

void ComposeAiContent::cancelRequest() {
	++_requestToken;
	if (_requestId) {
		_session->api().composeWithAi().cancel(_requestId);
		_requestId = 0;
	}
}

void ComposeAiContent::request() {
	if (_mode == ComposeAiMode::Style && _styleIndex < 0) {
		return;
	}
	cancelRequest();
	_state = CardState::Loading;
	_result = {};
	_preview->setState(CardState::Loading);
	updateTitles();
	notifyReadyChanged();

	auto request = Api::ComposeWithAi::Request{
		.text = _original,
		.emojify = (_mode != ComposeAiMode::Fix) && _emojify,
	};
	switch (_mode) {
	case ComposeAiMode::Translate:
		request.translateToLang = _to.twoLetterCode();
		break;
	case ComposeAiMode::Style:
		request.changeTone = _stylesData[_styleIndex].key;
		break;
	case ComposeAiMode::Fix:
		request.proofread = true;
		break;
	}

	const auto token = ++_requestToken;
	const auto weak = QPointer<ComposeAiContent>(this);
	_requestId = _session->api().composeWithAi().request(
		std::move(request),
		[=](Api::ComposeWithAi::Result &&result) {
			if (!weak || weak->_requestToken != token) {
				return;
			}
			weak->_requestId = 0;
			weak->applyResult(std::move(result));
		},
		[=](const MTP::Error &error) {
			if (!weak || weak->_requestToken != token) {
				return;
			}
			weak->_requestId = 0;
			weak->showError(error);
		});
}

void ComposeAiContent::applyResult(Api::ComposeWithAi::Result &&result) {
	_result = std::move(result.resultText);
	auto display = (_mode == ComposeAiMode::Fix && result.diffText)
		? BuildDiffDisplay(*result.diffText)
		: _result;
	_state = _result.text.isEmpty() ? CardState::Failed : CardState::Ready;
	_preview->setState(_state);
	if (_state == CardState::Ready) {
		_preview->setResultText(std::move(display));
	}
	updateTitles();
	notifyReadyChanged();
	refreshLayout();
}

void ComposeAiContent::showError(const MTP::Error &error) {
	_state = CardState::Failed;
	_preview->setState(CardState::Failed);
	updateTitles();
	notifyReadyChanged();
	refreshLayout();
	if (error.type() == u"AICOMPOSE_FLOOD_PREMIUM"_q) {
		const auto show = Main::MakeSessionShow(
			_box->uiShow(),
			_session);
		Settings::ShowPremiumPromoToast(
			show,
			ChatHelpers::ResolveWindowDefault(),
			tr::lng_ai_compose_flood_text(
				tr::now,
				lt_link,
				tr::link(tr::lng_ai_compose_flood_link(tr::now)),
				tr::rich),
			u"ai_compose"_q);
		if (_premiumFlood) {
			_premiumFlood();
		}
		return;
	}
	_box->showToast(error.type().isEmpty()
		? tr::lng_ai_compose_error(tr::now)
		: error.type());
}

void ComposeAiContent::notifyReadyChanged() {
	if (_readyChanged) {
		_readyChanged(_state == CardState::Ready);
	}
}

void ComposeAiContent::setPremiumFloodCallback(Fn<void()> callback) {
	_premiumFlood = std::move(callback);
}

void ComposeAiContent::setModeChangedCallback(
		Fn<void(ComposeAiMode)> callback) {
	_modeChanged = std::move(callback);
}

void ComposeAiContent::setStyleSelectedCallback(Fn<void()> callback) {
	_styleSelected = std::move(callback);
}

ComposeAiMode ComposeAiContent::mode() const {
	return _mode;
}

bool ComposeAiContent::hasStyleSelection() const {
	return _styleIndex >= 0;
}

} // namespace

void ComposeAiBox(not_null<Ui::GenericBox*> box, ComposeAiBoxArgs &&args) {
	box->setStyle(st::aiComposeBox);
	box->setNoContentMargin(true);
	box->setWidth(st::boxWideWidth);
	box->setTitle(tr::lng_ai_compose_title());
	box->addTopButton(st::boxTitleClose, [=] {
		box->closeBox();
	});

	const auto session = args.session;
	const auto body = box->verticalLayout();
	const auto tabsSkip = QMargins(0, 0, 0, st::aiComposeBoxStyleTabsSkip);
	const auto pinnedToTop = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));
	const auto tabs = pinnedToTop->add(
		object_ptr<ComposeAiModeTabs>(pinnedToTop),
		st::aiComposeContentMargin + tabsSkip);
	const auto content = body->add(
		object_ptr<ComposeAiContent>(box, box, args),
		st::aiComposeContentMargin);
	const auto stylesWrap = pinnedToTop->add(
		object_ptr<Ui::SlideWrap<ComposeAiStyleScrollTabs>>(
			pinnedToTop,
			object_ptr<ComposeAiStyleScrollTabs>(
				pinnedToTop,
				content->stylesData()),
			tabsSkip),
		st::aiComposeContentMargin);
	stylesWrap->hide(anim::type::instant);
	content->setModeTabs(tabs);
	content->setStyleTabs(stylesWrap);

	auto premiumFlooded = std::make_shared<bool>(false);
	auto applyButton = std::make_shared<QPointer<Ui::RoundButton>>();
	auto sendButton = std::make_shared<QPointer<Ui::SendButton>>();

	const auto applyAndClose = [=] {
		if (!content->hasResult()) {
			return;
		}
		args.apply(TextWithEntities(content->result()));
		box->closeBox();
	};

	const auto rebuildButtons = [=] {
		if (*sendButton) {
			delete sendButton->data();
		}
		*sendButton = nullptr;
		box->clearButtons();
		box->addTopButton(st::boxTitleClose, [=] {
			box->closeBox();
		});
		*applyButton = nullptr;

		if (*premiumFlooded) {
			box->setStyle(st::aiComposeBox);
			auto helper = Ui::Text::CustomEmojiHelper();
			const auto badge = helper.paletteDependent(
				Ui::Text::CustomEmojiTextBadge(
					u"x50"_q,
					st::aiComposeBadge,
					st::aiComposeBadgeMargin));
			const auto btn = box->addButton(
				tr::lng_ai_compose_increase_limit(), nullptr);
			btn->setFullRadius(true);
			btn->setContext(helper.context());
			btn->setText(rpl::single(
				tr::lng_ai_compose_increase_limit(tr::now, tr::marked)
					.append(' ')
					.append(badge)));
			const auto resolve = ChatHelpers::ResolveWindowDefault();
			const auto close = crl::guard(box, [=] {
				box->closeBox();
			});
			btn->setClickedCallback([=] {
				if (const auto controller = resolve(session)) {
					ShowPremiumPreviewBox(
						controller,
						PremiumFeature::AiCompose);
				}
				close();
			});
		} else if (content->mode() == ComposeAiMode::Style
				&& !content->hasStyleSelection()) {
			box->setStyle(st::aiComposeBox);
			const auto btn = box->addButton(
				tr::lng_ai_compose_select_style(), nullptr);
			btn->setFullRadius(true);
			btn->setDisabled(true);
			btn->setTextFgOverride(
				anim::color(st::activeButtonBg, st::activeButtonFg, 0.5));
		} else if (content->hasResult()) {
			box->setStyle(st::aiComposeBoxWithSend);
			const auto isStyle =
				(content->mode() == ComposeAiMode::Style);
			const auto btn = box->addButton(
				isStyle
					? tr::lng_ai_compose_apply_style()
					: tr::lng_ai_compose_apply(),
				applyAndClose);
			btn->setFullRadius(true);
			*applyButton = btn;

			const auto send = Ui::CreateChild<Ui::SendButton>(
				btn->parentWidget(),
				st::aiComposeSendButton);
			send->setState({ .type = Ui::SendButton::Type::Send });
			send->show();
			btn->geometryValue(
			) | rpl::on_next([=](QRect geometry) {
				const auto size = st::aiComposeBoxWithSend.buttonHeight;
				send->resize(size, size);
				send->moveToLeft(
					geometry.x() + geometry.width()
						+ st::aiComposeSendButtonSkip,
					geometry.y() + (geometry.height() - size) / 2);
			}, send->lifetime());
			send->setClickedCallback(applyAndClose);
			*sendButton = send;
		} else {
			box->setStyle(st::aiComposeBox);
			const auto isStyle =
				(content->mode() == ComposeAiMode::Style);
			const auto btn = box->addButton(
				isStyle
					? tr::lng_ai_compose_apply_style()
					: tr::lng_ai_compose_apply(),
				nullptr);
			btn->setFullRadius(true);
			btn->setDisabled(true);
			*applyButton = btn;
		}
	};

	content->setReadyChangedCallback([=](bool) {
		rebuildButtons();
	});
	content->setPremiumFloodCallback([=] {
		*premiumFlooded = true;
		rebuildButtons();
	});
	content->setModeChangedCallback([=](ComposeAiMode) {
		rebuildButtons();
	});
	content->setStyleSelectedCallback([=] {
		rebuildButtons();
	});

	rebuildButtons();
	content->start();
}

void ShowComposeAiBox(
		std::shared_ptr<Ui::Show> show,
		ComposeAiBoxArgs &&args) {
	show->show(Box(ComposeAiBox, std::move(args)));
}

} // namespace HistoryView::Controls
