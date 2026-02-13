/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "boxes/peer_list_controllers.h"
#include "base/flat_map.h"
#include "ui/unread_badge.h"

namespace Ui {
class ChatStyle;
} // namespace Ui

class ParticipantsBoxController;

namespace Window {
class SessionNavigation;
} // namespace Window

namespace Info {
namespace Profile {

class MemberListRow final : public PeerListRowWithLink {
public:
	enum class Rights {
		Normal,
		Admin,
		Creator,
	};
	struct Type {
		Rights rights;
		QString rank;
		not_null<const Ui::ChatStyle*> chatStyle;
		not_null<base::flat_map<QRgb, QImage>*> circleCache;
		bool canAddTag = false;
		bool canRemove = false;
	};

	MemberListRow(not_null<UserData*> user, Type type);
	~MemberListRow();

	void setType(Type type);
	[[nodiscard]] Type type() const;
	bool rightActionDisabled() const override;
	QSize rightActionSize() const override;
	QMargins rightActionMargins() const override;
	void rightActionPaint(
		Painter &p,
		int x,
		int y,
		int outerWidth,
		bool selected,
		bool actionSelected) override;
	void rightActionAddRipple(
		QPoint point,
		Fn<void()> updateCallback) override;
	void rightActionStopLastRipple() override;
	void refreshStatus() override;

	not_null<UserData*> user() const;

private:
	enum class TagMode {
		None,
		Remove,
		AdminPill,
		NormalText,
		AddTag,
	};

	[[nodiscard]] bool canRemove() const;
	[[nodiscard]] int pillHeight() const;
	[[nodiscard]] const QImage &ensurePillCircle(QRgb color) const;
	void paintPill(
		Painter &p,
		int x,
		int y,
		int width,
		QRgb bgColor) const;

	Type _type;
	TagMode _tagMode = TagMode::None;
	QString _actionText;
	int _actionTextWidth = 0;
	std::unique_ptr<Ui::RippleAnimation> _actionRipple;

};

std::unique_ptr<ParticipantsBoxController> CreateMembersController(
	not_null<Window::SessionNavigation*> navigation,
	not_null<PeerData*> peer);

} // namespace Profile
} // namespace Info
