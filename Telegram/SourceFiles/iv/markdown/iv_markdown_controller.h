#pragma once

#include "iv/markdown/iv_markdown_document.h"
#include "iv/markdown/iv_markdown_prepare.h"
#include "ui/widgets/rp_window.h"

#include <optional>

#include <QtCore/QString>

namespace Iv::Markdown {

class Controller final {
public:
	Controller(
		not_null<Delegate*> delegate,
		PreparedDocument document,
		QString title,
		OpenOptions options = {});
	Controller(
		not_null<Delegate*> delegate,
		MarkdownArticleContent content,
		QString title,
		std::shared_ptr<MathRenderer> renderer = nullptr,
		OpenOptions options = {});
	~Controller();

	void activate();
	void update(
		MarkdownArticleContent content,
		QString title,
		OpenOptions options = {});

	[[nodiscard]] bool active() const;
	void minimize();

	[[nodiscard]] rpl::producer<Event> events() const {
		return _events.events();
	}

	[[nodiscard]] rpl::lifetime &lifetime();

private:
	void close();
	void createWindow();
	void createPreview();

	const not_null<Delegate*> _delegate;

	const std::shared_ptr<const PreparedDocument> _document;
	std::optional<MarkdownArticleContent> _preparedContent;
	QString _title;
	const std::shared_ptr<MathRenderer> _renderer;
	OpenOptions _options;
	std::unique_ptr<Ui::RpWindow> _window;
	std::unique_ptr<Ui::RpWidget> _preview;

	rpl::event_stream<Event> _events;

	rpl::lifetime _lifetime;

};

[[nodiscard]] std::unique_ptr<Controller> TryOpenLocalFile(
	not_null<Delegate*> delegate,
	const QString &path);

} // namespace Iv::Markdown
