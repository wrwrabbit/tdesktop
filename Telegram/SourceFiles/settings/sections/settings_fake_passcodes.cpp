/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_fake_passcodes.h"

#include "settings/settings_common_session.h"

#include "core/application.h"
#include "fakepasscode/fake_passcode.h"
#include "fakepasscode/ptg.h"
#include "fakepasscode/settings.h"
#include "fakepasscode/ui/fakepasscode_box.h"
#include "fakepasscode/ui/fakepasscode_hwlock_box.h"
#include "fakepasscode/ui/fakepasscodes_list.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "platform/platform_specific.h"
#include "settings/sections/settings_privacy_security.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "storage/storage_domain.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/qt_object_factory.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Builder;

class PasscodesListWidget : public Ui::RpWidget {
public:
	PasscodesListWidget(
		QWidget *parent,
		not_null<Main::Domain*> domain,
		not_null<Window::SessionController*> controller);

private:
	void rebuild(size_t passcodesSize);

	const not_null<Main::Domain*> _domain;
	const not_null<Window::SessionController*> _controller;
	Ui::VerticalLayout *_content = nullptr;
};

PasscodesListWidget::PasscodesListWidget(
	QWidget *parent,
	not_null<Main::Domain*> domain,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _domain(domain)
, _controller(controller) {
	_domain->local().GetFakePasscodesSize(
	) | rpl::on_next([=](size_t value) {
		rebuild(value);
	}, lifetime());
}

void PasscodesListWidget::rebuild(size_t passcodesSize) {
	if (_content) {
		_content->deleteLater();
		_content = nullptr;
	}
	_content = Ui::CreateChild<Ui::VerticalLayout>(this);
	_content->setAutoFillBackground(true);

	for (size_t i = 0; i < passcodesSize; ++i) {
		AddButtonWithIcon(
			_content,
			tr::lng_fakepasscode(lt_caption, _domain->local().GetFakePasscodeName(i)),
			st::settingsButton,
			{ &st::menuIconLock }
		)->addClickHandler([=] {
			_controller->show(
				Box<FakePasscodeContentBox>(_domain, _controller, i),
				Ui::LayerOption::KeepOther);
		});
	}
	AddButtonWithIcon(
		_content,
		tr::lng_add_fakepasscode(),
		st::settingsButtonActive,
		{ &st::settingsIconAdd, IconType::Round, &st::windowBgActive }
	)->addClickHandler([=] {
		_controller->show(
			Box<FakePasscodeBox>(_controller, false, true, 0),
			Ui::LayerOption::KeepOther);
	});
	Ui::AddSkip(_content, st::settingsCheckboxesSkip);

	Ui::ResizeFitChild(this, _content);
}

void BuildFakePasscodesContent(SectionBuilder &builder) {
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"security/ptg"_q,
			.title = tr::lng_show_fakes(tr::now),
			.keywords = { u"partisan"_q, u"fake"_q, u"ptg"_q },
		};
	});

	const auto controller = builder.controller();
	const auto container = builder.container();
	const auto session = builder.session();
	auto *domain = &session->domain();

	if (container) {
		container->add(object_ptr<PasscodesListWidget>(
			container,
			domain,
			controller));
	}

	if (Platform::PTG::IsHWProtectionAvailable()) {
		builder.addSubsectionTitle(tr::lng_hw_lock_title());

		if (container) {
			const auto toggledHWLock = container->lifetime().make_state<rpl::event_stream<bool>>();
			const auto button = AddButtonWithIcon(
				container,
				tr::lng_hw_lock_checkbox(),
				st::settingsButton,
				{ &st::menuIconLock }
			)->toggleOn(
				toggledHWLock->events_starting_with_copy(PTG::IsHWLockEnabled()),
				true);
			button->addClickHandler([=] {
				controller->show(
					Box<FakePasscodeHWLockBox>(controller, toggledHWLock),
					Ui::LayerOption::KeepOther);
			});
		}

		builder.addDividerText(tr::lng_hw_lock_description());
	}

	builder.addSubsectionTitle(tr::lng_da_title());

	const auto chatJoin = builder.addButton({
		.id = u"ptg/da_chat_join"_q,
		.title = tr::lng_da_chat_join_check(),
		.icon = { &st::menuIconSavedMessages },
		.toggled = rpl::single(PTG::DASettings::isChatJoinCheckEnabled()),
	});
	if (chatJoin) {
		chatJoin->toggledValue(
		) | rpl::filter([=](bool v) {
			return v != PTG::DASettings::isChatJoinCheckEnabled();
		}) | rpl::on_next([=](bool v) {
			PTG::DASettings::setChatJoinCheckEnabled(v);
			domain->local().writeAccounts();
		}, chatJoin->lifetime());
	}

	const auto channelJoin = builder.addButton({
		.id = u"ptg/da_channel_join"_q,
		.title = tr::lng_da_channel_join_check(),
		.icon = { &st::menuIconSavedMessages },
		.toggled = rpl::single(PTG::DASettings::isChannelJoinCheckEnabled()),
	});
	if (channelJoin) {
		channelJoin->toggledValue(
		) | rpl::filter([=](bool v) {
			return v != PTG::DASettings::isChannelJoinCheckEnabled();
		}) | rpl::on_next([=](bool v) {
			PTG::DASettings::setChannelJoinCheckEnabled(v);
			domain->local().writeAccounts();
		}, channelJoin->lifetime());
	}

	const auto postComment = builder.addButton({
		.id = u"ptg/da_post_comment"_q,
		.title = tr::lng_da_post_comment_check(),
		.icon = { &st::menuIconSavedMessages },
		.toggled = rpl::single(PTG::DASettings::isPostCommentCheckEnabled()),
	});
	if (postComment) {
		postComment->toggledValue(
		) | rpl::filter([=](bool v) {
			return v != PTG::DASettings::isPostCommentCheckEnabled();
		}) | rpl::on_next([=](bool v) {
			PTG::DASettings::setPostCommentCheckEnabled(v);
			domain->local().writeAccounts();
		}, postComment->lifetime());
	}

	const auto makeReaction = builder.addButton({
		.id = u"ptg/da_make_reaction"_q,
		.title = tr::lng_da_make_reaction_check(),
		.icon = { &st::menuIconSavedMessages },
		.toggled = rpl::single(PTG::DASettings::isMakeReactionCheckEnabled()),
	});
	if (makeReaction) {
		makeReaction->toggledValue(
		) | rpl::filter([=](bool v) {
			return v != PTG::DASettings::isMakeReactionCheckEnabled();
		}) | rpl::on_next([=](bool v) {
			PTG::DASettings::setMakeReactionCheckEnabled(v);
			domain->local().writeAccounts();
		}, makeReaction->lifetime());
	}

	const auto startBot = builder.addButton({
		.id = u"ptg/da_start_bot"_q,
		.title = tr::lng_da_start_bot_check(),
		.icon = { &st::menuIconSavedMessages },
		.toggled = rpl::single(PTG::DASettings::isStartBotCheckEnabled()),
	});
	if (startBot) {
		startBot->toggledValue(
		) | rpl::filter([=](bool v) {
			return v != PTG::DASettings::isStartBotCheckEnabled();
		}) | rpl::on_next([=](bool v) {
			PTG::DASettings::setStartBotCheckEnabled(v);
			domain->local().writeAccounts();
		}, startBot->lifetime());
	}

	builder.addDividerText(tr::lng_da_common());

	builder.addSubsectionTitle(tr::lng_special_actions());

	const auto cacheCleaning = builder.addButton({
		.id = u"ptg/cache_cleaning"_q,
		.title = tr::lng_clear_cache_on_lock(),
		.icon = { &st::menuIconClear },
		.toggled = rpl::single(domain->local().IsCacheCleanedUpOnLock()),
	});
	if (cacheCleaning) {
		cacheCleaning->toggledValue(
		) | rpl::filter([=](bool v) {
			return v != domain->local().IsCacheCleanedUpOnLock();
		}) | rpl::on_next([=](bool v) {
			domain->local().SetCacheCleanedUpOnLock(v);
			domain->local().writeAccounts();
		}, cacheCleaning->lifetime());
	}

	builder.addDividerText(tr::lng_clear_cache_on_lock_help());
	builder.addSkip(st::settingsCheckboxesSkip);

	const auto logging = builder.addButton({
		.id = u"ptg/logging"_q,
		.title = tr::lng_enable_advance_logging(),
		.icon = { &st::menuIconSavedMessages },
		.toggled = rpl::single(domain->local().IsAdvancedLoggingEnabled()),
	});
	if (logging) {
		logging->toggledValue(
		) | rpl::filter([=](bool v) {
			return v != domain->local().IsAdvancedLoggingEnabled();
		}) | rpl::on_next([=](bool v) {
			domain->local().SetAdvancedLoggingEnabled(v);
			domain->local().writeAccounts();
		}, logging->lifetime());
	}

	builder.addDividerText(tr::lng_enable_advance_logging_help());
	builder.addSkip(st::settingsCheckboxesSkip);

	const auto erasing = builder.addButton({
		.id = u"ptg/erasing"_q,
		.title = tr::lng_enable_dod_cleaning(),
		.icon = { &st::menuIconDelete },
		.toggled = rpl::single(domain->local().IsErasingEnabled()),
	});
	if (erasing) {
		erasing->toggledValue(
		) | rpl::filter([=](bool v) {
			return v != domain->local().IsErasingEnabled();
		}) | rpl::on_next([=](bool v) {
			domain->local().SetErasingEnabled(v);
			domain->local().writeAccounts();
		}, erasing->lifetime());
	}

	builder.addDividerText(tr::lng_enable_dod_cleaning_help());
}

class FakePasscodes : public Section<FakePasscodes> {
public:
	FakePasscodes(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
};

const auto kMeta = BuildHelper({
	.id = FakePasscodes::Id(),
	.parentId = PrivacySecurityId(),
	.title = &tr::lng_fakepasscodes_list,
	.icon = &st::menuIconSettings,
}, [](SectionBuilder &builder) {
	BuildFakePasscodesContent(builder);
});

FakePasscodes::FakePasscodes(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> FakePasscodes::title() {
	return tr::lng_fakepasscodes_list();
}

void FakePasscodes::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

} // namespace

Type FakePasscodesId() {
	return FakePasscodes::Id();
}

} // namespace Settings
