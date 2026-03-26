/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_privacy_security_helpers.h"

#include "api/api_authorizations.h"
#include "api/api_user_privacy.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "core/core_settings.h"
#include "core/application.h"
#include "fakepasscode/settings.h"
#include "storage/storage_domain.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using Key = Api::UserPrivacy::Key;
using Option = Api::UserPrivacy::Option;
using Rule = Api::UserPrivacy::Rule;

constexpr auto kAllPrivacyKeys = std::array{
	Key::PhoneNumber,
	Key::LastSeen,
	Key::ProfilePhoto,
	Key::Forwards,
	Key::Calls,
	Key::CallsPeer2Peer,
	Key::Voices,
	Key::About,
	Key::Birthday,
	Key::GiftsAutoSave,
	Key::SavedMusic,
	Key::Invites,
};

constexpr auto kSessionAnomalyMaxTotal = 5;
constexpr auto kSessionAnomalyRecentDays = 7;
constexpr auto kBackgroundCheckInterval = TimeId(24 * 60 * 60);

} // namespace

bool IsPrivacyKeySecure(Key key, const Rule &rule) {
	switch (key) {
	case Key::PhoneNumber:
	case Key::LastSeen:
	case Key::Forwards:
	case Key::Birthday:
	case Key::GiftsAutoSave:
	case Key::CallsPeer2Peer:
		return rule.option == Option::Nobody;
	case Key::ProfilePhoto:
	case Key::Calls:
	case Key::Voices:
	case Key::About:
	case Key::SavedMusic:
	case Key::Invites:
		return (rule.option == Option::Contacts)
			|| (rule.option == Option::Nobody);
	default:
		return true;
	}
}

Option SecureTargetOption(Key key) {
	switch (key) {
	case Key::PhoneNumber:
	case Key::LastSeen:
	case Key::Forwards:
	case Key::Birthday:
	case Key::GiftsAutoSave:
	case Key::CallsPeer2Peer:
		return Option::Nobody;
	case Key::ProfilePhoto:
	case Key::Calls:
	case Key::Voices:
	case Key::About:
	case Key::SavedMusic:
	case Key::Invites:
		return Option::Contacts;
	default:
		return Option::Nobody;
	}
}

rpl::producer<int> InsecurePrivacyCount(not_null<Main::Session*> session) {
	auto producers = std::vector<rpl::producer<Rule>>();
	producers.reserve(kAllPrivacyKeys.size());
	for (const auto key : kAllPrivacyKeys) {
		session->api().userPrivacy().reload(key);
		producers.push_back(session->api().userPrivacy().value(key));
	}
	return rpl::combine(
		std::move(producers)
	) | rpl::map([](const std::vector<Rule> &rules) {
		auto count = 0;
		for (auto i = 0, n = int(kAllPrivacyKeys.size()); i != n; ++i) {
			if (!IsPrivacyKeySecure(kAllPrivacyKeys[i], rules[i])) {
				++count;
			}
		}
		return count;
	});
}

void ApplyMaxPrivacy(not_null<Main::Session*> session) {
	for (const auto key : kAllPrivacyKeys) {
		session->api().userPrivacy().reload(key);
	}
	auto producers = std::vector<rpl::producer<Rule>>();
	producers.reserve(kAllPrivacyKeys.size());
	for (const auto key : kAllPrivacyKeys) {
		producers.push_back(
			session->api().userPrivacy().value(key) | rpl::take(1));
	}
	rpl::combine(
		std::move(producers)
	) | rpl::take(
		1
	) | rpl::on_next([session](const std::vector<Rule> &rules) {
		for (auto i = 0, n = int(kAllPrivacyKeys.size()); i != n; ++i) {
			const auto key = kAllPrivacyKeys[i];
			if (!IsPrivacyKeySecure(key, rules[i])) {
				auto tightened = rules[i];
				tightened.option = SecureTargetOption(key);
				session->api().userPrivacy().save(key, tightened);
			}
		}
	}, session->lifetime());
}

rpl::producer<bool> HasSessionAnomaly(not_null<Main::Session*> session) {
	return session->api().authorizations().listValue(
	) | rpl::map([](const Api::Authorizations::List &list) {
		if (int(list.size()) > kSessionAnomalyMaxTotal) {
			return true;
		}
		const auto now = base::unixtime::now();
		const auto recentThreshold = now - kSessionAnomalyRecentDays * 86400;
		auto currentCountry = QString();
		for (const auto &entry : list) {
			if (entry.hash == 0) {
				currentCountry = entry.location;
				break;
			}
		}
		if (currentCountry.isEmpty()) {
			return false;
		}
		for (const auto &entry : list) {
			if (entry.hash == 0) {
				continue;
			}
			if (entry.activeTime >= recentThreshold
				&& entry.location != currentCountry) {
				return true;
			}
		}
		return false;
	});
}

void RunBackgroundSessionCheck(not_null<Main::Session*> session) {
	const auto now = base::unixtime::now();
	const auto last = PTG::GetLastSessionCheckTime();
	if (now - last < kBackgroundCheckInterval) {
		return;
	}
	PTG::SetLastSessionCheckTime(now);
	Core::App().domain().local().writeAccounts();

	session->api().authorizations().reload();

	HasSessionAnomaly(
		session
	) | rpl::take(
		1
	) | rpl::on_next([](bool anomaly) {
		PTG::SetSessionAnomalyPending(anomaly);
		Core::App().domain().local().writeAccounts();
	}, session->lifetime());
}

void AttachPrivacyCountBadge(
		not_null<Ui::RpWidget*> button,
		not_null<Main::Session*> session) {
	struct State {
		int count = 0;
	};
	const auto badge = Ui::CreateChild<Ui::RpWidget>(button.get());
	const auto state = badge->lifetime().make_state<State>();

	badge->paintRequest(
	) | rpl::on_next([badge, state] {
		if (state->count <= 0) {
			return;
		}
		auto p = QPainter(badge);
		auto font = st::semiboldFont;
		p.setFont(font);

		const auto text = QString::number(state->count);
		const auto textWidth = font->width(text);
		const auto badgeWidth = std::max(
			textWidth + st::settingsPrivacyBadgePadding.left()
				+ st::settingsPrivacyBadgePadding.right(),
			badge->height());

		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::attentionButtonFg);
		p.drawRoundedRect(
			0,
			0,
			badgeWidth,
			badge->height(),
			badge->height() / 2,
			badge->height() / 2);
		p.setPen(st::windowFgActive);
		p.drawText(
			(badgeWidth - textWidth) / 2,
			st::settingsPrivacyBadgePadding.top() + font->ascent,
			text);
	}, badge->lifetime());

	rpl::combine(
		button->sizeValue(),
		InsecurePrivacyCount(session)
	) | rpl::on_next([badge, state](QSize size, int count) {
		state->count = count;
		const auto badgeHeight = st::settingsPrivacyBadgeSize;
		badge->resize(
			std::max(badgeHeight, badge->width()),
			badgeHeight);
		badge->moveToRight(
			st::settingsButtonRightSkip,
			(size.height() - badgeHeight) / 2);
		badge->setVisible(count > 0);
		if (count > 0) {
			badge->update();
		}
	}, badge->lifetime());
}

} // namespace Settings
