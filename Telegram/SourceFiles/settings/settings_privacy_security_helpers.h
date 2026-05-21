/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_authorizations.h"
#include "api/api_user_privacy.h"

namespace Ui {
class SettingsButton;
class RpWidget;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

[[nodiscard]] bool IsPrivacyKeySecure(
	Api::UserPrivacy::Key key,
	const Api::UserPrivacy::Rule &rule);

[[nodiscard]] Api::UserPrivacy::Option SecureTargetOption(
	Api::UserPrivacy::Key key);

[[nodiscard]] rpl::producer<int> InsecurePrivacyCount(
	not_null<Main::Session*> session);

void ApplyMaxPrivacy(not_null<Main::Session*> session);

[[nodiscard]] rpl::producer<bool> HasSessionAnomaly(
	not_null<Main::Session*> session);

[[nodiscard]] base::flat_set<uint64> NewCountrySessionHashes(
	const Api::Authorizations::List &list);

void RunBackgroundSessionCheck(not_null<Main::Session*> session);

void AttachPrivacyCountBadge(
	not_null<Ui::RpWidget*> button,
	not_null<Main::Session*> session);

[[nodiscard]] rpl::event_stream<> &PrivacyReviewAccepted();
void MarkPrivacyReviewed(int insecureCount);

} // namespace Settings
