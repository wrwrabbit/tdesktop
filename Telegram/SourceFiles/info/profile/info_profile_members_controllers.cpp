/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_members_controllers.h"

#include "boxes/peers/edit_participants_box.h"
#include "info/profile/info_profile_values.h"
#include "data/data_chat.h"
#include "data/data_user.h"
#include "ui/unread_badge.h"
#include "lang/lang_keys.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "styles/style_info.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_dialogs.h"
#include "styles/style_widgets.h"

namespace Info {
namespace Profile {

MemberListRow::MemberListRow(
	not_null<UserData*> user,
	Type type)
: PeerListRowWithLink(user)
, _type(type) {
	setType(type);
}

MemberListRow::~MemberListRow() = default;

void MemberListRow::setType(Type type) {
	_type = type;
	_actionRipple = nullptr;
	if (_type.canRemove) {
		_tagMode = TagMode::Remove;
		_actionText = tr::lng_profile_delete_removed(tr::now);
	} else if (_type.canAddTag) {
		_tagMode = TagMode::AddTag;
		_actionText = tr::lng_context_add_my_tag(tr::now);
	} else if (!_type.rank.isEmpty()) {
		_tagMode = (_type.rights == Rights::Admin
			|| _type.rights == Rights::Creator)
			? TagMode::AdminPill
			: TagMode::NormalText;
		_actionText = _type.rank;
	} else if (_type.rights == Rights::Creator) {
		_tagMode = TagMode::AdminPill;
		_actionText = tr::lng_owner_badge(tr::now);
	} else if (_type.rights == Rights::Admin) {
		_tagMode = TagMode::AdminPill;
		_actionText = tr::lng_admin_badge(tr::now);
	} else {
		_tagMode = TagMode::None;
		_actionText = QString();
	}
	_actionTextWidth = _actionText.isEmpty()
		? 0
		: st::normalFont->width(_actionText);
}

MemberListRow::Type MemberListRow::type() const {
	return _type;
}

bool MemberListRow::rightActionDisabled() const {
	if (_tagMode == TagMode::AddTag || _tagMode == TagMode::Remove) {
		return false;
	}
	return !canRemove();
}

QSize MemberListRow::rightActionSize() const {
	if (_actionTextWidth == 0) {
		return QSize();
	}
	switch (_tagMode) {
	case TagMode::Remove:
	case TagMode::AdminPill:
	case TagMode::AddTag: {
		const auto &p = st::memberTagPillPadding;
		const auto h = p.top() + st::normalFont->height + p.bottom();
		const auto w = p.left() + _actionTextWidth + p.right();
		return QSize(std::max(w, h), h);
	}
	case TagMode::NormalText:
		return QSize(_actionTextWidth, st::normalFont->height);
	case TagMode::None:
		return QSize();
	}
	return QSize();
}

QMargins MemberListRow::rightActionMargins() const {
	const auto skip = st::contactsCheckPosition.x();
	const auto &st = st::defaultPeerListItem;
	const auto size = rightActionSize();
	if (size.isEmpty()) {
		return QMargins();
	}
	return QMargins(
		skip,
		(st.height - size.height()) / 2,
		st.photoPosition.x() + skip,
		0);
}

not_null<UserData*> MemberListRow::user() const {
	return peer()->asUser();
}

void MemberListRow::refreshStatus() {
	if (user()->isBot()) {
		const auto seesAllMessages = (user()->botInfo->readsAllHistory
			|| _type.rights != Rights::Normal);
		setCustomStatus(seesAllMessages
			? tr::lng_status_bot_reads_all(tr::now)
			: tr::lng_status_bot_not_reads_all(tr::now));
	} else {
		PeerListRow::refreshStatus();
	}
}

bool MemberListRow::canRemove() const {
	return _type.canRemove;
}

int MemberListRow::pillHeight() const {
	const auto &p = st::memberTagPillPadding;
	return p.top() + st::normalFont->height + p.bottom();
}

const QImage &MemberListRow::ensurePillCircle(QRgb color) const {
	auto &cache = *_type.circleCache;
	const auto it = cache.find(color);
	if (it != end(cache)) {
		return it->second;
	}
	const auto h = pillHeight();
	const auto ratio = style::DevicePixelRatio();
	auto image = QImage(
		QSize(h, h) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(Qt::transparent);
	{
		auto p = QPainter(&image);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor::fromRgba(color));
		p.drawEllipse(0, 0, h, h);
	}
	return cache.emplace(color, std::move(image)).first->second;
}

void MemberListRow::paintPill(
		Painter &p,
		int x,
		int y,
		int width,
		QRgb bgColor) const {
	const auto h = pillHeight();
	const auto &circle = ensurePillCircle(bgColor);
	const auto ratio = style::DevicePixelRatio();
	const auto half = h / 2;
	const auto otherHalf = h - half;
	p.drawImage(
		QRect(x, y, half, h),
		circle,
		QRect(0, 0, half * ratio, h * ratio));
	if (width > h) {
		p.fillRect(
			x + half,
			y,
			width - h,
			h,
			QColor::fromRgba(bgColor));
	}
	p.drawImage(
		QRect(x + width - otherHalf, y, otherHalf, h),
		circle,
		QRect(half * ratio, 0, otherHalf * ratio, h * ratio));
}

void MemberListRow::rightActionPaint(
		Painter &p,
		int x,
		int y,
		int outerWidth,
		bool selected,
		bool actionSelected) {
	if (_actionTextWidth == 0) {
		return;
	}
	const auto &pad = st::memberTagPillPadding;
	switch (_tagMode) {
	case TagMode::AdminPill: {
		const auto nameColor = (_type.rights == Rights::Creator)
			? st::rankOwnerFg->c
			: st::rankAdminFg->c;
		auto bgColor = nameColor;
		bgColor.setAlphaF(0.15);
		const auto h = pillHeight();
		const auto cw = pad.left() + _actionTextWidth + pad.right();
		const auto w = std::max(cw, h);
		paintPill(p, x, y, w, bgColor.rgba());
		p.setFont(st::normalFont);
		p.setPen(nameColor);
		p.drawTextLeft(
			x + (w - _actionTextWidth) / 2,
			y + pad.top(),
			outerWidth,
			_actionText,
			_actionTextWidth);
	} break;
	case TagMode::NormalText: {
		p.setFont(st::normalFont);
		p.setPen(st::rankUserFg);
		p.drawTextLeft(
			x, y, outerWidth, _actionText, _actionTextWidth);
	} break;
	case TagMode::Remove:
	case TagMode::AddTag: {
		const auto h = pillHeight();
		const auto cw = pad.left() + _actionTextWidth + pad.right();
		const auto w = std::max(cw, h);
		if (actionSelected) {
			paintPill(p, x, y, w, st::lightButtonBgOver->c.rgba());
		}
		if (_actionRipple) {
			const auto color = st::lightButtonBgRipple->c;
			_actionRipple->paint(p, x, y, outerWidth, &color);
			if (_actionRipple->empty()) {
				_actionRipple.reset();
			}
		}
		p.setFont(st::normalFont);
		p.setPen(actionSelected
			? st::lightButtonFgOver
			: st::lightButtonFg);
		p.drawTextLeft(
			x + (w - _actionTextWidth) / 2,
			y + pad.top(),
			outerWidth,
			_actionText,
			_actionTextWidth);
	} break;
	case TagMode::None:
		break;
	}
}

void MemberListRow::rightActionAddRipple(
		QPoint point,
		Fn<void()> updateCallback) {
	if (_tagMode != TagMode::AddTag && _tagMode != TagMode::Remove) {
		return;
	}
	if (!_actionRipple) {
		const auto size = rightActionSize();
		const auto radius = size.height() / 2;
		auto mask = Ui::RippleAnimation::RoundRectMask(size, radius);
		_actionRipple = std::make_unique<Ui::RippleAnimation>(
			st::defaultLightButton.ripple,
			std::move(mask),
			std::move(updateCallback));
	}
	_actionRipple->add(point);
}

void MemberListRow::rightActionStopLastRipple() {
	if (_actionRipple) {
		_actionRipple->lastStop();
	}
}

std::unique_ptr<ParticipantsBoxController> CreateMembersController(
		not_null<Window::SessionNavigation*> navigation,
		not_null<PeerData*> peer) {
	return std::make_unique<ParticipantsBoxController>(
		navigation,
		peer,
		ParticipantsBoxController::Role::Profile);
}

} // namespace Profile
} // namespace Info
