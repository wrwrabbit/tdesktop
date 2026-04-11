/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "editor/scene/scene_item_base.h"

#include <QTextDocument>

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Editor {

enum class TextStyle : uchar {
	Framed,
	SemiTransparent,
	Plain,
};

class EmojiDocument final : public QTextDocument {
public:
	explicit EmojiDocument(QObject *parent = nullptr);
	QVariant loadResource(int type, const QUrl &name) override;

private:
	std::map<QUrl, QVariant> _cache;
};

void ReplaceEmoji(QTextDocument *doc);
[[nodiscard]] QString RecoverTextFromDocument(QTextDocument *doc);

class ItemText : public ItemBase {
public:
	enum { Type = ItemBase::Type + 2 };

	ItemText(
		const QString &text,
		const QColor &color,
		float fontSize,
		TextStyle style,
		const QSize &imageSize,
		ItemBase::Data data);

	void paint(
		QPainter *p,
		const QStyleOptionGraphicsItem *option,
		QWidget *widget) override;
	int type() const override;

	[[nodiscard]] const QString &text() const;
	void setText(const QString &text);

	[[nodiscard]] const QColor &color() const;
	void setColor(const QColor &color);

	[[nodiscard]] float fontSize() const;

	[[nodiscard]] TextStyle textStyle() const;
	void setTextStyle(TextStyle style);

	[[nodiscard]] float64 editScale() const;

	[[nodiscard]] static QSize computeContentSize(
		const QString &text,
		float fontSize,
		const QSize &imageSize);

	void save(SaveState state) override;
	void restore(SaveState state) override;
	bool hasState(SaveState state) const override;

protected:
	void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override;
	void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override;
	void performFlip() override;
	std::shared_ptr<ItemBase> duplicate(ItemBase::Data data) const override;

private:
	void renderContent();

	QString _text;
	QColor _color;
	float _fontSize;
	TextStyle _textStyle = TextStyle::Plain;
	QSize _imageSize;
	QPixmap _pixmap;
	base::unique_qptr<Ui::PopupMenu> _contextMenu;

	struct SavedText {
		bool saved = false;
		NumberedItem::Status status = NumberedItem::Status::Normal;
		QString text;
		QColor color;
		float fontSize = 0;
		TextStyle textStyle = TextStyle::Plain;
	};
	SavedText _savedState, _keepedState;
};

} // namespace Editor
