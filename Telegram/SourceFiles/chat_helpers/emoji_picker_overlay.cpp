/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/emoji_picker_overlay.h"

#include "ui/abstract_button.h"
#include "ui/emoji_config.h"
#include "ui/painter.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "styles/style_chat_helpers.h"

#include <QtCore/QEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>

namespace ChatHelpers {
namespace {

[[nodiscard]] std::vector<EmojiPtr> BuildAllEmojis() {
	using Section = Ui::Emoji::Section;
	auto result = std::vector<EmojiPtr>();
	for (auto i = int(Section::People); i <= int(Section::Symbols); ++i) {
		const auto section = Ui::Emoji::GetSection(Section(i));
		result.reserve(result.size() + section.size());
		for (const auto emoji : section) {
			result.push_back(emoji);
		}
	}
	return result;
}

} // namespace

class EmojiPickerOverlay::Strip final : public Ui::RpWidget {
public:
	Strip(QWidget *parent, not_null<EmojiPickerOverlay*> owner);

	void refresh();

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Cell {
		EmojiPtr emoji = nullptr;
		QRect rect;
	};

	[[nodiscard]] int cellAtPoint(QPoint p) const;
	void updateHover(int index);

	const not_null<EmojiPickerOverlay*> _owner;
	std::vector<Cell> _cells;
	int _hover = -1;
	int _pressed = -1;

};

class EmojiPickerOverlay::Grid final : public Ui::RpWidget {
public:
	Grid(QWidget *parent, not_null<EmojiPickerOverlay*> owner);

	void setEmojis(std::vector<EmojiPtr> emojis);
	int resizeGetHeight(int newWidth) override;
	void refresh();

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Cell {
		EmojiPtr emoji = nullptr;
		QRect rect;
	};

	[[nodiscard]] int cellAtPoint(QPoint p) const;
	void relayoutCells();
	void updateHover(int index);

	const not_null<EmojiPickerOverlay*> _owner;
	std::vector<EmojiPtr> _emojis;
	std::vector<Cell> _cells;
	int _columns = 0;
	int _hover = -1;
	int _pressed = -1;

};

namespace {

void DrawEmojiCell(
		QPainter &p,
		const QRect &cell,
		EmojiPtr emoji,
		bool selected,
		bool hovered) {
	if (selected) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::stickersEmojiPickerSelectedBg);
		p.drawEllipse(cell);
	} else if (hovered) {
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(st::windowSubTextFg->c, 0.12));
		p.drawEllipse(cell);
	}
	if (!emoji) {
		return;
	}
	const auto esize = Ui::Emoji::GetSizeLarge();
	const auto dpr = style::DevicePixelRatio();
	const auto pixelSize = esize / dpr;
	const auto drawSize = std::min(
		pixelSize,
		st::stickersEmojiPickerItemSize - 4);
	const auto x = cell.x() + (cell.width() - drawSize) / 2;
	const auto y = cell.y() + (cell.height() - drawSize) / 2;
	if (drawSize == pixelSize) {
		Ui::Emoji::Draw(p, emoji, esize, x, y);
	} else {
		const auto target = QRect(x, y, drawSize, drawSize);
		auto buffer = QImage(
			QSize(pixelSize, pixelSize) * dpr,
			QImage::Format_ARGB32_Premultiplied);
		buffer.fill(Qt::transparent);
		buffer.setDevicePixelRatio(dpr);
		{
			auto q = QPainter(&buffer);
			Ui::Emoji::Draw(q, emoji, esize, 0, 0);
		}
		auto hq = PainterHighQualityEnabler(p);
		p.drawImage(target, buffer);
	}
}

} // namespace

EmojiPickerOverlay::Strip::Strip(
	QWidget *parent,
	not_null<EmojiPickerOverlay*> owner)
: RpWidget(parent)
, _owner(owner) {
	setMouseTracking(true);
}

void EmojiPickerOverlay::Strip::refresh() {
	const auto &sel = _owner->_selectedList;
	const auto &recent = _owner->_recent;
	const auto item = st::stickersEmojiPickerItemSize;
	const auto skip = st::stickersEmojiPickerItemSkip;
	const auto w = width();
	if (w <= 0 || item <= 0) {
		_cells.clear();
		update();
		return;
	}
	const auto capacity = std::max(1, (w + skip) / (item + skip));

	auto order = std::vector<EmojiPtr>();
	order.reserve(sel.size() + recent.size());
	for (const auto emoji : sel) {
		order.push_back(emoji);
	}
	for (const auto emoji : recent) {
		const auto already = std::find(sel.begin(), sel.end(), emoji)
			!= sel.end();
		if (!already) {
			order.push_back(emoji);
		}
	}
	if (int(order.size()) > capacity) {
		order.resize(capacity);
	}

	_cells.clear();
	_cells.reserve(order.size());
	auto x = 0;
	const auto y = (height() - item) / 2;
	for (const auto emoji : order) {
		_cells.push_back({ emoji, QRect(x, y, item, item) });
		x += item + skip;
	}
	_hover = -1;
	_pressed = -1;
	update();
}

int EmojiPickerOverlay::Strip::cellAtPoint(QPoint p) const {
	for (auto i = 0; i != int(_cells.size()); ++i) {
		if (_cells[i].rect.contains(p)) {
			return i;
		}
	}
	return -1;
}

void EmojiPickerOverlay::Strip::updateHover(int index) {
	if (_hover == index) {
		return;
	}
	_hover = index;
	update();
}

void EmojiPickerOverlay::Strip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	for (auto i = 0; i != int(_cells.size()); ++i) {
		const auto &cell = _cells[i];
		const auto selected = _owner->_selectedList.end()
			!= std::find(
				_owner->_selectedList.begin(),
				_owner->_selectedList.end(),
				cell.emoji);
		DrawEmojiCell(p, cell.rect, cell.emoji, selected, i == _hover);
	}
}

void EmojiPickerOverlay::Strip::mouseMoveEvent(QMouseEvent *e) {
	updateHover(cellAtPoint(e->pos()));
}

void EmojiPickerOverlay::Strip::mousePressEvent(QMouseEvent *e) {
	_pressed = cellAtPoint(e->pos());
}

void EmojiPickerOverlay::Strip::mouseReleaseEvent(QMouseEvent *e) {
	const auto released = cellAtPoint(e->pos());
	const auto index = _pressed;
	_pressed = -1;
	if (released == index && index >= 0 && index < int(_cells.size())) {
		_owner->toggleEmoji(_cells[index].emoji, false);
	}
}

void EmojiPickerOverlay::Strip::leaveEventHook(QEvent *e) {
	updateHover(-1);
}

EmojiPickerOverlay::Grid::Grid(
	QWidget *parent,
	not_null<EmojiPickerOverlay*> owner)
: RpWidget(parent)
, _owner(owner) {
	setMouseTracking(true);
}

void EmojiPickerOverlay::Grid::setEmojis(std::vector<EmojiPtr> emojis) {
	_emojis = std::move(emojis);
	relayoutCells();
}

int EmojiPickerOverlay::Grid::resizeGetHeight(int newWidth) {
	resize(newWidth, 0);
	relayoutCells();
	return height();
}

void EmojiPickerOverlay::Grid::refresh() {
	update();
}

void EmojiPickerOverlay::Grid::relayoutCells() {
	const auto item = st::stickersEmojiPickerItemSize;
	const auto skip = st::stickersEmojiPickerItemSkip;
	const auto w = width();
	_columns = std::max(1, (w + skip) / (item + skip));
	_cells.clear();
	_cells.reserve(_emojis.size());
	auto col = 0;
	auto row = 0;
	for (const auto emoji : _emojis) {
		const auto x = col * (item + skip);
		const auto y = row * (item + skip);
		_cells.push_back({ emoji, QRect(x, y, item, item) });
		if (++col >= _columns) {
			col = 0;
			++row;
		}
	}
	const auto fullRows = row + (col > 0 ? 1 : 0);
	const auto h = fullRows > 0
		? (fullRows * item + (fullRows - 1) * skip)
		: 0;
	resize(w, h);
	_hover = -1;
	_pressed = -1;
	update();
}

int EmojiPickerOverlay::Grid::cellAtPoint(QPoint p) const {
	for (auto i = 0; i != int(_cells.size()); ++i) {
		if (_cells[i].rect.contains(p)) {
			return i;
		}
	}
	return -1;
}

void EmojiPickerOverlay::Grid::updateHover(int index) {
	if (_hover == index) {
		return;
	}
	_hover = index;
	update();
}

void EmojiPickerOverlay::Grid::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto clip = e->rect();
	for (auto i = 0; i != int(_cells.size()); ++i) {
		const auto &cell = _cells[i];
		if (!cell.rect.intersects(clip)) {
			continue;
		}
		const auto selected = _owner->_selectedList.end()
			!= std::find(
				_owner->_selectedList.begin(),
				_owner->_selectedList.end(),
				cell.emoji);
		DrawEmojiCell(p, cell.rect, cell.emoji, selected, i == _hover);
	}
}

void EmojiPickerOverlay::Grid::mouseMoveEvent(QMouseEvent *e) {
	updateHover(cellAtPoint(e->pos()));
}

void EmojiPickerOverlay::Grid::mousePressEvent(QMouseEvent *e) {
	_pressed = cellAtPoint(e->pos());
}

void EmojiPickerOverlay::Grid::mouseReleaseEvent(QMouseEvent *e) {
	const auto released = cellAtPoint(e->pos());
	const auto index = _pressed;
	_pressed = -1;
	if (released == index && index >= 0 && index < int(_cells.size())) {
		_owner->toggleEmoji(_cells[index].emoji, true);
	}
}

void EmojiPickerOverlay::Grid::leaveEventHook(QEvent *e) {
	updateHover(-1);
}

EmojiPickerOverlay::EmojiPickerOverlay(
	QWidget *parent,
	EmojiPickerOverlayDescriptor descriptor)
: RpWidget(parent)
, _aboutText(std::move(descriptor.aboutText))
, _recent(descriptor.recent.empty()
	? std::vector<EmojiPtr>(
		Ui::Emoji::GetDefaultRecent().begin(),
		Ui::Emoji::GetDefaultRecent().end())
	: std::move(descriptor.recent))
, _maxSelected(descriptor.maxSelected)
, _allowExpand(descriptor.allowExpand)
, _selectedList(std::move(descriptor.initialSelected)) {
	_allForGrid = BuildAllEmojis();

	_about = std::make_unique<Ui::FlatLabel>(
		this,
		_aboutText,
		st::stickersEmojiPickerAbout);

	_strip = Ui::CreateChild<Strip>(this, this);

	if (_allowExpand) {
		_expandButton = Ui::CreateChild<Ui::AbstractButton>(this);
		_expandButton->resize(
			st::stickersEmojiPickerExpandSize,
			st::stickersEmojiPickerExpandSize);
		_expandButton->setClickedCallback([=] {
			setExpanded(!_expanded.current());
		});
		_expandButton->paintRequest(
		) | rpl::on_next([=](const QRect &clip) {
			auto p = QPainter(_expandButton);
			const auto &icon = _expanded.current()
				? st::stickersEmojiPickerCollapseIcon
				: st::stickersEmojiPickerExpandIcon;
			const auto x = (_expandButton->width() - icon.width()) / 2;
			const auto y = (_expandButton->height() - icon.height()) / 2;
			icon.paint(p, x, y, _expandButton->width());
		}, _expandButton->lifetime());

		_scroll = std::make_unique<Ui::ScrollArea>(this);
		_scroll->setFrameStyle(QFrame::NoFrame);
		_scroll->hide();
		const auto gridPtr = _scroll->setOwnedWidget(
			object_ptr<Grid>(_scroll.get(), this));
		_grid = gridPtr.data();
		_grid->setEmojis(_allForGrid);
	}

	_selectedVar = _selectedList;
	resize(width(), expandedHeight());
}

EmojiPickerOverlay::~EmojiPickerOverlay() = default;

const std::vector<EmojiPtr> &EmojiPickerOverlay::selected() const {
	return _selectedList;
}

rpl::producer<std::vector<EmojiPtr>>
EmojiPickerOverlay::selectedValue() const {
	return _selectedVar.value();
}

void EmojiPickerOverlay::setExpanded(bool expanded) {
	if (!_allowExpand || _expanded.current() == expanded) {
		return;
	}
	startExpandAnimation(expanded);
	_expanded = expanded;
	if (_expandButton) {
		_expandButton->update();
	}
}

void EmojiPickerOverlay::startExpandAnimation(bool expanded) {
	const auto from = _expandAnim.value(expanded ? 0. : 1.);
	const auto to = expanded ? 1. : 0.;
	_expandAnim.start(
		[=] { applyExpandProgress(); },
		from,
		to,
		st::slideWrapDuration,
		anim::easeOutCirc);
	applyExpandProgress();
}

float64 EmojiPickerOverlay::currentExpandValue() const {
	return _expandAnim.value(_expanded.current() ? 1. : 0.);
}

int EmojiPickerOverlay::currentShownHeight() const {
	const auto progress = currentExpandValue();
	return anim::interpolate(
		collapsedHeight(),
		expandedHeight(),
		progress);
}

void EmojiPickerOverlay::applyExpandProgress() {
	const auto h = currentShownHeight();
	setMask(QRegion(0, 0, width(), h));
	if (_scroll) {
		const auto progress = currentExpandValue();
		_scroll->setVisible(progress > 0.);
	}
	relayout();
	update();
}

bool EmojiPickerOverlay::expanded() const {
	return _expanded.current();
}

rpl::producer<bool> EmojiPickerOverlay::expandedValue() const {
	return _expanded.value();
}

int EmojiPickerOverlay::collapsedHeight() const {
	const auto &pad = st::stickersEmojiPickerPadding;
	const auto aboutH = _about ? _about->height() : 0;
	return pad.top()
		+ aboutH
		+ st::stickersEmojiPickerStripHeight
		+ pad.bottom();
}

int EmojiPickerOverlay::expandedHeight() const {
	return collapsedHeight() + st::stickersEmojiPickerExpandedHeight;
}

void EmojiPickerOverlay::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::stickersEmojiPickerBg);
	const auto radius = st::stickersEmojiPickerRadius;
	const auto h = currentShownHeight();
	p.drawRoundedRect(QRect(0, 0, width(), h), radius, radius);
}

void EmojiPickerOverlay::resizeEvent(QResizeEvent *e) {
	setMask(QRegion(0, 0, width(), currentShownHeight()));
	relayout();
}

void EmojiPickerOverlay::mousePressEvent(QMouseEvent *e) {
	if (e->pos().y() > currentShownHeight()) {
		e->ignore();
	}
}

void EmojiPickerOverlay::relayout() {
	const auto &pad = st::stickersEmojiPickerPadding;
	if (_about) {
		_about->resizeToWidth(width() - pad.left() - pad.right());
		_about->moveToLeft(pad.left(), pad.top());
	}
	const auto aboutBottom = _about
		? (_about->y() + _about->height())
		: pad.top();

	const auto stripTop = aboutBottom;
	const auto stripH = st::stickersEmojiPickerStripHeight;
	const auto expandSize = _expandButton
		? _expandButton->width()
		: 0;
	const auto expandGap = _expandButton
		? st::stickersEmojiPickerItemSkip
		: 0;
	const auto stripW = width()
		- pad.left()
		- pad.right()
		- expandSize
		- expandGap;
	_strip->setGeometry(pad.left(), stripTop, stripW, stripH);
	_strip->refresh();

	if (_expandButton) {
		const auto bx = width() - pad.right() - expandSize;
		const auto by = stripTop + (stripH - expandSize) / 2;
		_expandButton->moveToLeft(bx, by);
	}

	if (_scroll) {
		const auto scrollTop = stripTop + stripH;
		const auto scrollH = std::max(
			0,
			height() - scrollTop - pad.bottom());
		_scroll->setGeometry(
			pad.left(),
			scrollTop,
			width() - pad.left() - pad.right(),
			scrollH);
		if (_grid) {
			_grid->resizeGetHeight(_scroll->width());
		}
	}
}

void EmojiPickerOverlay::toggleEmoji(EmojiPtr emoji, bool fromGrid) {
	if (!emoji) {
		return;
	}
	const auto it = std::find(
		_selectedList.begin(),
		_selectedList.end(),
		emoji);
	if (it != _selectedList.end()) {
		_selectedList.erase(it);
	} else {
		if (_maxSelected > 0 && int(_selectedList.size()) >= _maxSelected) {
			return;
		}
		_selectedList.push_back(emoji);
	}
	notifySelectionChanged();
	if (fromGrid) {
		setExpanded(false);
	}
}

void EmojiPickerOverlay::notifySelectionChanged() {
	_selectedVar = _selectedList;
	if (_strip) {
		_strip->refresh();
	}
	if (_grid) {
		_grid->refresh();
	}
}

} // namespace ChatHelpers
