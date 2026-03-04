/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/mac/translate_provider_mac.h"

#include "spellcheck/platform/platform_language.h"

#include <QtCore/QStandardPaths>
#include <QtCore/QProcess>
#include <QtGui/QTextDocument>

namespace Platform {
namespace {

[[nodiscard]] QString Command() {
	const auto commands = {
		u"crow"_q,
		u"org.kde.CrowTranslate"_q,
	};
	const auto it = ranges::find_if(commands, [](const auto &command) {
		return !QStandardPaths::findExecutable(command).isEmpty();
	});
	return it != end(commands) ? *it : QString();
}

class TranslateProvider final : public Ui::TranslateProvider {
public:
	[[nodiscard]] bool supportsMessageId() const override {
		return false;
	}

	void request(
			Ui::TranslateProviderRequest request,
			LanguageId to,
			Fn<void(Ui::TranslateProviderResult)> done) override {
		const auto from = Platform::Language::Recognize(request.text.text);
		_process.setProgram(Command());
		_process.setArguments(
			QStringList{ u"-i"_q, u"-b"_q, u"-t"_q, to.twoLetterCode() }
				+ (from.known()
					? QStringList{ u"-s"_q, from.twoLetterCode() }
					: QStringList()));
		QObject::connect(&_process, &QProcess::finished, [=] {
			_document.setHtml(_process.readAllStandardOutput());
			const auto text = _document.toPlainText();
			done(!text.isEmpty()
				? Ui::TranslateProviderResult{
					.text = TextWithEntities{ .text = text },
				}
				: Ui::TranslateProviderResult{
					.error = Ui::TranslateProviderError::Unknown,
				}
			);
		});
		_process.start();
		_process.write(request.text.text.toUtf8());
		_process.closeWriteChannel();
	}

private:
	QProcess _process;
	QTextDocument _document;
};

} // namespace

std::unique_ptr<Ui::TranslateProvider> CreateTranslateProvider() {
	return std::make_unique<TranslateProvider>();
}

bool IsTranslateProviderAvailable() {
	return !Command().isEmpty();
}

} // namespace Platform
