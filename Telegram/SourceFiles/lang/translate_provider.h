/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "spellcheck/platform/platform_language.h"
#include "ui/text/text_entity.h"

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {

struct TranslateProviderRequest {
	not_null<PeerData*> peer;
	MsgId msgId = 0;
	TextWithEntities text;
};

class TranslateProvider {
public:
	virtual ~TranslateProvider() = default;
	[[nodiscard]] virtual bool supportsMessageId() const = 0;
	virtual void request(
		TranslateProviderRequest request,
		LanguageId to,
		Fn<void(std::optional<TextWithEntities>)> done) = 0;
};

[[nodiscard]] std::unique_ptr<TranslateProvider> CreateTranslateProvider(
	not_null<Main::Session*> session);

[[nodiscard]] TranslateProviderRequest PrepareTranslateProviderRequest(
	not_null<TranslateProvider*> provider,
	not_null<PeerData*> peer,
	MsgId msgId,
	TextWithEntities text);

} // namespace Ui
