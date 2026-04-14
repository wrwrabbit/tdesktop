/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"
#include "ui/emoji_config.h"

namespace Ui {
class AbstractButton;
class FlatLabel;
class ScrollArea;
} // namespace Ui

namespace ChatHelpers {

struct EmojiPickerOverlayDescriptor {
	QString aboutText;
	std::vector<EmojiPtr> recent;
	int maxSelected = 0;
	bool allowExpand = true;
	std::vector<EmojiPtr> initialSelected;
};

class EmojiPickerOverlay final : public Ui::RpWidget {
public:
	EmojiPickerOverlay(
		QWidget *parent,
		EmojiPickerOverlayDescriptor descriptor);
	~EmojiPickerOverlay();

	[[nodiscard]] const std::vector<EmojiPtr> &selected() const;
	[[nodiscard]] rpl::producer<std::vector<EmojiPtr>> selectedValue() const;

	void setExpanded(bool expanded);
	[[nodiscard]] bool expanded() const;
	[[nodiscard]] rpl::producer<bool> expandedValue() const;

	[[nodiscard]] int collapsedHeight() const;
	[[nodiscard]] int expandedHeight() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	class Strip;
	class Grid;

	void buildSections();
	void relayout();
	void toggleEmoji(EmojiPtr emoji);
	void notifySelectionChanged();

	const QString _aboutText;
	const std::vector<EmojiPtr> _recent;
	const int _maxSelected;
	const bool _allowExpand;

	std::vector<EmojiPtr> _allForGrid;

	std::vector<EmojiPtr> _selectedList;
	rpl::variable<std::vector<EmojiPtr>> _selectedVar;
	rpl::variable<bool> _expanded = false;

	std::unique_ptr<Ui::FlatLabel> _about;
	Strip *_strip = nullptr;
	Ui::AbstractButton *_expandButton = nullptr;
	std::unique_ptr<Ui::ScrollArea> _scroll;
	Grid *_grid = nullptr;

};

} // namespace ChatHelpers
