#pragma once

#include "iv/markdown/iv_markdown_document.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QString>

namespace Iv::Markdown {

class Controller final {
public:
	Controller(
		not_null<Delegate*> delegate,
		PreparedDocument document,
		QString title,
		QString sourcePath,
		QString initialFragment);
	~Controller();

	void activate();

	[[nodiscard]] bool active() const;
	void minimize();

	[[nodiscard]] rpl::producer<Event> events() const {
		return _events.events();
	}

	[[nodiscard]] rpl::lifetime &lifetime();

private:
	void close();
	void createWindow();

	const not_null<Delegate*> _delegate;

	PreparedDocument _document;
	QString _title;
	QString _sourcePath;
	QString _initialFragment;
	std::unique_ptr<Ui::RpWindow> _window;
	std::unique_ptr<Ui::RpWidget> _preview;

	rpl::event_stream<Event> _events;

	rpl::lifetime _lifetime;

};

[[nodiscard]] std::unique_ptr<Controller> TryOpenLocalFile(
	not_null<Delegate*> delegate,
	const QString &path);

} // namespace Iv::Markdown
