/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/create_poll_box.h"

#include "base/call_delayed.h"
#include "base/event_filter.h"
#include "base/random.h"
#include "base/unique_qptr.h"
#include "chat_helpers/emoji_suggestions_widget.h"
#include "chat_helpers/message_field.h"
#include "chat_helpers/tabbed_panel.h"
#include "chat_helpers/tabbed_selector.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_document.h"
#include "data/data_poll.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/view/history_view_schedule_box.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "menu/menu_send.h"
#include "settings/detailed_settings_button.h"
#include "settings/settings_common.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/controls/emoji_button.h"
#include "ui/controls/emoji_button_factory.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/dynamic_image.h"
#include "ui/dynamic_thumbnails.h"
#include "ui/effects/radial_animation.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "window/section_widget.h"
#include "window/window_session_controller.h"
#include "apiwrap.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h" // defaultComposeFiles.
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_polls.h"
#include "styles/style_settings.h"

#include <QtCore/QMimeData>

namespace {

constexpr auto kQuestionLimit = 255;
constexpr auto kMaxOptionsCount = PollData::kMaxOptions;
constexpr auto kOptionLimit = 100;
constexpr auto kWarnQuestionLimit = 80;
constexpr auto kWarnOptionLimit = 30;
constexpr auto kSolutionLimit = 200;
constexpr auto kWarnSolutionLimit = 60;
constexpr auto kErrorLimit = 99;

[[nodiscard]] Ui::PreparedList PhotoListFromMimeData(
		not_null<const QMimeData*> data,
		bool premium) {
	using Error = Ui::PreparedList::Error;
	const auto urls = Core::ReadMimeUrls(data);
	auto result = !urls.isEmpty()
		? Storage::PrepareMediaList(
			urls.mid(0, 1),
			st::sendMediaPreviewSize,
			premium)
		: Ui::PreparedList(Error::EmptyFile, QString());
	if (result.error == Error::None) {
		return result;
	} else if (auto read = Core::ReadMimeImage(data)) {
		return Storage::PrepareMediaFromImage(
			std::move(read.image),
			std::move(read.content),
			st::sendMediaPreviewSize);
	}
	return result;
}

struct PollMediaState {
	PollMedia media;
	std::shared_ptr<Ui::DynamicImage> thumbnail;
	bool rounded = false;
	bool uploading = false;
	float64 progress = 0.;
	uint64 uploadDataId = 0;
	uint64 token = 0;
	Fn<void()> update;
};

class LocalImageThumbnail final : public Ui::DynamicImage {
public:
	explicit LocalImageThumbnail(QImage original)
	: _original(std::move(original)) {
	}

	std::shared_ptr<Ui::DynamicImage> clone() override {
		return std::make_shared<LocalImageThumbnail>(_original);
	}

	QImage image(int size) override {
		return _original;
	}

	void subscribeToUpdates(Fn<void()> callback) override {
	}

private:
	QImage _original;

};

class PollMediaButton final : public Ui::RippleButton {
public:
	PollMediaButton(
		not_null<QWidget*> parent,
		const style::IconButton &st,
		std::shared_ptr<PollMediaState> state)
	: Ui::RippleButton(parent, st.ripple)
	, _st(st)
	, _state(std::move(state))
	, _attach(Ui::MakeIconThumbnail(_st.icon))
	, _attachOver(_st.iconOver.empty()
		? _attach
		: Ui::MakeIconThumbnail(_st.iconOver))
	, _radial([=](crl::time now) { radialAnimationCallback(now); }) {
		const auto weak = QPointer<PollMediaButton>(this);
		_state->update = [=] {
			if (weak) {
				weak->updateMediaSubscription();
				weak->update();
			}
		};
		resize(_st.width, _st.height);
		setPointerCursor(true);
		updateMediaSubscription();
	}

	~PollMediaButton() override {
		if (_subscribed) {
			_subscribed->subscribeToUpdates(nullptr);
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		paintRipple(p, _st.rippleAreaPosition);
		if (const auto image = _state->thumbnail
			? _state->thumbnail
			: currentAttachThumbnail()) {
			const auto target = _state->thumbnail
				? rippleRect()
				: iconRect();
			if (_state->thumbnail) {
				paintCover(
					p,
					target,
					image->image(std::max(target.width(), target.height())),
					_state->rounded);
			} else {
				p.drawImage(
					target,
					image->image(std::max(target.width(), target.height())));
			}
		}
		if (_state->uploading && !_radial.animating()) {
			_radial.start(_state->progress);
		}
		if (_state->uploading || _radial.animating()) {
			if (_state->thumbnail) {
				p.save();
				auto path = QPainterPath();
				path.addRoundedRect(
					rippleRect(),
					st::roundRadiusSmall,
					st::roundRadiusSmall);
				p.setClipPath(path);
				p.fillRect(rippleRect(), st::songCoverOverlayFg);
				p.restore();
			}
			const auto line = float64(st::lineWidth * 2);
			const auto margin = float64(st::pollAttachProgressMargin);
			const auto arc = QRectF(rippleRect()) - Margins(margin);
			_radial.draw(p, arc, line, st::historyFileThumbRadialFg);
		}
	}

	void onStateChanged(State was, StateChangeSource source) override {
		RippleButton::onStateChanged(was, source);
		if (!_state->thumbnail) {
			update();
		}
	}

	QImage prepareRippleMask() const override {
		return Ui::RippleAnimation::EllipseMask(QSize(
			_st.rippleAreaSize,
			_st.rippleAreaSize));
	}

	QPoint prepareRippleStartPosition() const override {
		auto result = mapFromGlobal(QCursor::pos())
			- _st.rippleAreaPosition;
		const auto rect = QRect(
			QPoint(),
			QSize(_st.rippleAreaSize, _st.rippleAreaSize));
		return rect.contains(result)
			? result
			: DisabledRippleStartPosition();
	}

private:
	void paintCover(
			Painter &p,
			QRect target,
			QImage image,
			bool rounded) const {
		if (image.isNull() || target.isEmpty()) {
			return;
		}
		const auto source = QRectF(
			0,
			0,
			image.width(),
			image.height());
		const auto kx = target.width() / source.width();
		const auto ky = target.height() / source.height();
		const auto scale = std::max(kx, ky);
		const auto size = QSizeF(
			source.width() * scale,
			source.height() * scale);
		const auto geometry = QRectF(
			target.x() + (target.width() - size.width()) / 2.,
			target.y() + (target.height() - size.height()) / 2.,
			size.width(),
			size.height());
		p.save();
		if (rounded) {
			auto path = QPainterPath();
			path.addRoundedRect(
				target,
				st::roundRadiusSmall,
				st::roundRadiusSmall);
			p.setClipPath(path);
		}
		p.drawImage(geometry, image, source);
		p.restore();
	}

	void radialAnimationCallback(crl::time now) {
		const auto updated = _radial.update(
			_state->progress,
			!_state->uploading,
			now);
		if (!anim::Disabled() || updated || _radial.animating()) {
			update(rippleRect());
		}
	}

	[[nodiscard]] QRect rippleRect() const {
		return QRect(
			_st.rippleAreaPosition,
			QSize(_st.rippleAreaSize, _st.rippleAreaSize));
	}

	[[nodiscard]] QRect iconRect() const {
		const auto over = isOver() || isDown();
		const auto &icon = over && !_st.iconOver.empty()
			? _st.iconOver
			: _st.icon;
		auto position = _st.iconPosition;
		if (position.x() < 0) {
			position.setX((width() - icon.width()) / 2);
		}
		if (position.y() < 0) {
			position.setY((height() - icon.height()) / 2);
		}
		return QRect(position, QSize(icon.width(), icon.height()));
	}

	std::shared_ptr<Ui::DynamicImage> currentAttachThumbnail() const {
		return (isOver() || isDown()) ? _attachOver : _attach;
	}

	void updateMediaSubscription() {
		if (_subscribed == _state->thumbnail) {
			return;
		}
		if (_subscribed) {
			_subscribed->subscribeToUpdates(nullptr);
		}
		_subscribed = _state->thumbnail;
		if (!_subscribed) {
			return;
		}
		const auto weak = QPointer<PollMediaButton>(this);
		_subscribed->subscribeToUpdates([=] {
			if (weak) {
				weak->update();
			}
		});
	}

	const style::IconButton &_st;
	const std::shared_ptr<PollMediaState> _state;
	const std::shared_ptr<Ui::DynamicImage> _attach;
	const std::shared_ptr<Ui::DynamicImage> _attachOver;
	std::shared_ptr<Ui::DynamicImage> _subscribed;
	Ui::RadialAnimation _radial;

};

class PreparePollMediaTask final : public Task {
public:
	PreparePollMediaTask(
		FileLoadTask::Args &&args,
		Fn<void(std::shared_ptr<FilePrepareResult>)> done)
	: _task(std::move(args))
	, _done(std::move(done)) {
	}

	void process() override {
		_task.process({ .generateGoodThumbnail = false });
	}

	void finish() override {
		_done(_task.peekResult());
	}

private:
	FileLoadTask _task;
	Fn<void(std::shared_ptr<FilePrepareResult>)> _done;

};

class Options {
public:
	using AttachCallback = Fn<void(
		not_null<Ui::RpWidget*>,
		std::shared_ptr<PollMediaState>)>;
	using FieldDropCallback = Fn<void(
		not_null<Ui::InputField*>,
		std::shared_ptr<PollMediaState>)>;
	using WidgetDropCallback = Fn<void(
		not_null<QWidget*>,
		std::shared_ptr<PollMediaState>)>;

	Options(
		not_null<Ui::BoxContent*> box,
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		ChatHelpers::TabbedPanel *emojiPanel,
		bool chooseCorrectEnabled,
		AttachCallback attachCallback,
		FieldDropCallback fieldDropCallback,
		WidgetDropCallback widgetDropCallback);

	[[nodiscard]] bool hasOptions() const;
	[[nodiscard]] bool isValid() const;
	[[nodiscard]] bool hasCorrect() const;
	[[nodiscard]] bool hasUploadingMedia() const;
	[[nodiscard]] std::vector<PollAnswer> toPollAnswers() const;
	void focusFirst();

	void enableChooseCorrect(bool enabled);

	[[nodiscard]] rpl::producer<int> usedCount() const;
	[[nodiscard]] rpl::producer<not_null<QWidget*>> scrollToWidget() const;
	[[nodiscard]] rpl::producer<> backspaceInFront() const;
	[[nodiscard]] rpl::producer<> tabbed() const;

private:
	class Option {
	public:
		Option(
			not_null<QWidget*> outer,
			not_null<Ui::VerticalLayout*> container,
			not_null<Main::Session*> session,
			int position,
			std::shared_ptr<Ui::RadiobuttonGroup> group,
			AttachCallback attachCallback,
			FieldDropCallback fieldDropCallback,
			WidgetDropCallback widgetDropCallback);

		Option(const Option &other) = delete;
		Option &operator=(const Option &other) = delete;

		void toggleRemoveAlways(bool toggled);
		void enableChooseCorrect(
			std::shared_ptr<Ui::RadiobuttonGroup> group);

		void show(anim::type animated);
		void destroy(FnMut<void()> done);

		[[nodiscard]] bool hasShadow() const;
		void createShadow();
		void destroyShadow();

		[[nodiscard]] bool isEmpty() const;
		[[nodiscard]] bool isGood() const;
		[[nodiscard]] bool isTooLong() const;
		[[nodiscard]] bool isCorrect() const;
		[[nodiscard]] bool uploadingMedia() const;
		[[nodiscard]] bool hasFocus() const;
		void setFocus() const;
		void clearValue();

		void setPlaceholder() const;
		void removePlaceholder() const;

		[[nodiscard]] not_null<Ui::InputField*> field() const;

		[[nodiscard]] PollAnswer toPollAnswer(int index) const;

		[[nodiscard]] rpl::producer<Qt::MouseButton> removeClicks() const;

	private:
		void createRemove();
		void createAttach();
		void createWarning();
		void toggleCorrectSpace(bool visible);
		void updateFieldGeometry();

		base::unique_qptr<Ui::SlideWrap<Ui::RpWidget>> _wrap;
		not_null<Ui::RpWidget*> _content;
		base::unique_qptr<Ui::FadeWrapScaled<Ui::Radiobutton>> _correct;
		Ui::Animations::Simple _correctShown;
		bool _hasCorrect = false;
		Ui::InputField *_field = nullptr;
		base::unique_qptr<Ui::PlainShadow> _shadow;
		base::unique_qptr<PollMediaButton> _attach;
		base::unique_qptr<Ui::CrossButton> _remove;
		rpl::variable<bool> *_removeAlways = nullptr;
		AttachCallback _attachCallback;
		FieldDropCallback _fieldDropCallback;
		WidgetDropCallback _widgetDropCallback;
		std::shared_ptr<PollMediaState> _media;

	};

	[[nodiscard]] bool full() const;
	[[nodiscard]] bool correctShadows() const;
	void fixShadows();
	void removeEmptyTail();
	void addEmptyOption();
	void checkLastOption();
	void validateState();
	void fixAfterErase();
	void destroy(std::unique_ptr<Option> option);
	void removeDestroyed(not_null<Option*> field);
	int findField(not_null<Ui::InputField*> field) const;
	[[nodiscard]] auto createChooseCorrectGroup()
		-> std::shared_ptr<Ui::RadiobuttonGroup>;

	not_null<Ui::BoxContent*> _box;
	not_null<Ui::VerticalLayout*> _container;
	const not_null<Window::SessionController*> _controller;
	ChatHelpers::TabbedPanel * const _emojiPanel;
	const AttachCallback _attachCallback;
	const FieldDropCallback _fieldDropCallback;
	const WidgetDropCallback _widgetDropCallback;
	std::shared_ptr<Ui::RadiobuttonGroup> _chooseCorrectGroup;
	int _position = 0;
	std::vector<std::unique_ptr<Option>> _list;
	std::vector<std::unique_ptr<Option>> _destroyed;
	rpl::variable<int> _usedCount = 0;
	bool _hasOptions = false;
	bool _isValid = false;
	bool _hasCorrect = false;
	rpl::event_stream<not_null<QWidget*>> _scrollToWidget;
	rpl::event_stream<> _backspaceInFront;
	rpl::event_stream<> _tabbed;
	rpl::lifetime _emojiPanelLifetime;

};

void InitField(
		not_null<QWidget*> container,
		not_null<Ui::InputField*> field,
		not_null<Main::Session*> session) {
	field->setInstantReplaces(Ui::InstantReplaces::Default());
	field->setInstantReplacesEnabled(
		Core::App().settings().replaceEmojiValue(),
		Core::App().settings().systemTextReplaceValue());
	auto options = Ui::Emoji::SuggestionsController::Options();
	options.suggestExactFirstWord = false;
	Ui::Emoji::SuggestionsController::Init(
		container,
		field,
		session,
		options);
}

not_null<Ui::FlatLabel*> CreateWarningLabel(
		not_null<QWidget*> parent,
		not_null<Ui::InputField*> field,
		int valueLimit,
		int warnLimit) {
	const auto result = Ui::CreateChild<Ui::FlatLabel>(
		parent.get(),
		QString(),
		st::createPollWarning);
	result->setAttribute(Qt::WA_TransparentForMouseEvents);
	field->changes(
	) | rpl::on_next([=] {
		Ui::PostponeCall(crl::guard(field, [=] {
			const auto length = field->getLastText().size();
			const auto value = valueLimit - length;
			const auto shown = (value < warnLimit)
				&& (field->height() > st::createPollOptionField.heightMin);
			if (value >= 0) {
				result->setText(QString::number(value));
			} else {
				constexpr auto kMinus = QChar(0x2212);
				result->setMarkedText(Ui::Text::Colorized(
					kMinus + QString::number(std::abs(value))));
			}
			result->setVisible(shown);
		}));
	}, field->lifetime());
	return result;
}

void FocusAtEnd(not_null<Ui::InputField*> field) {
	field->setFocus();
	field->setCursorPosition(field->getLastText().size());
	field->ensureCursorVisible();
}

not_null<DetailedSettingsButton*> AddPollToggleButton(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> title,
		rpl::producer<QString> description,
		Settings::IconDescriptor icon,
		rpl::producer<bool> toggled,
		const style::DetailedSettingsButtonStyle &rowStyle) {
	return AddDetailedSettingsButton(
		container,
		std::move(title),
		std::move(description),
		std::move(icon),
		std::move(toggled),
		rowStyle);
}

Options::Option::Option(
	not_null<QWidget*> outer,
	not_null<Ui::VerticalLayout*> container,
	not_null<Main::Session*> session,
	int position,
	std::shared_ptr<Ui::RadiobuttonGroup> group,
	AttachCallback attachCallback,
	FieldDropCallback fieldDropCallback,
	WidgetDropCallback widgetDropCallback)
: _wrap(container->insert(
	position,
	object_ptr<Ui::SlideWrap<Ui::RpWidget>>(
		container,
		object_ptr<Ui::RpWidget>(container))))
, _content(_wrap->entity())
, _field(
	Ui::CreateChild<Ui::InputField>(
		_content.get(),
		session->user()->isPremium()
			? st::createPollOptionFieldPremium
			: st::createPollOptionField,
		Ui::InputField::Mode::NoNewlines,
		tr::lng_polls_create_option_add()))
, _attachCallback(std::move(attachCallback))
, _fieldDropCallback(std::move(fieldDropCallback))
, _widgetDropCallback(std::move(widgetDropCallback))
, _media(std::make_shared<PollMediaState>()) {
	InitField(outer, _field, session);
	_field->setMaxLength(kOptionLimit + kErrorLimit);
	_field->show();
	if (_fieldDropCallback) {
		_fieldDropCallback(_field, _media);
	}

	_wrap->hide(anim::type::instant);

	_content->widthValue(
	) | rpl::on_next([=] {
		updateFieldGeometry();
	}, _field->lifetime());

	_field->heightValue(
	) | rpl::on_next([=](int height) {
		_content->resize(_content->width(), height);
	}, _field->lifetime());

	_field->changes(
	) | rpl::on_next([=] {
		Ui::PostponeCall(crl::guard(_field, [=] {
			if (_hasCorrect) {
				_correct->toggle(isGood(), anim::type::normal);
			}
		}));
	}, _field->lifetime());

	createShadow();
	createRemove();
	createAttach();
	createWarning();
	enableChooseCorrect(group);
	_correctShown.stop();
	if (_correct) {
		_correct->finishAnimating();
	}
	updateFieldGeometry();
}

bool Options::Option::hasShadow() const {
	return (_shadow != nullptr);
}

void Options::Option::createShadow() {
	Expects(_content != nullptr);

	if (_shadow) {
		return;
	}
	_shadow.reset(Ui::CreateChild<Ui::PlainShadow>(field().get()));
	_shadow->show();
	field()->sizeValue(
	) | rpl::on_next([=](QSize size) {
		const auto left = st::createPollFieldPadding.left();
		_shadow->setGeometry(
			left,
			size.height() - st::lineWidth,
			size.width() - left,
			st::lineWidth);
	}, _shadow->lifetime());
}

void Options::Option::destroyShadow() {
	_shadow = nullptr;
}

void Options::Option::createRemove() {
	using namespace rpl::mappers;

	const auto field = this->field();
	auto &lifetime = field->lifetime();

	const auto remove = Ui::CreateChild<Ui::CrossButton>(
		field.get(),
		st::createPollOptionRemove);
	remove->show(anim::type::instant);

	const auto toggle = lifetime.make_state<rpl::variable<bool>>(false);
	_removeAlways = lifetime.make_state<rpl::variable<bool>>(false);

	field->changes(
	) | rpl::on_next([field, toggle] {
		// Don't capture 'this'! Because Option is a value type.
		*toggle = !field->getLastText().isEmpty();
	}, field->lifetime());
#if 0
	rpl::combine(
		toggle->value(),
		_removeAlways->value(),
		_1 || _2
	) | rpl::on_next([=](bool shown) {
		remove->toggle(shown, anim::type::normal);
	}, remove->lifetime());
#endif

	field->widthValue(
	) | rpl::on_next([=](int width) {
		const auto attachSkip = st::pollAttach.width + st::lineWidth * 4;
		remove->moveToRight(
			st::createPollOptionRemovePosition.x() + attachSkip,
			st::createPollOptionRemovePosition.y(),
			width);
	}, remove->lifetime());

	_remove.reset(remove);
}

void Options::Option::createAttach() {
	const auto field = Option::field();
	const auto attach = Ui::CreateChild<PollMediaButton>(
		field.get(),
		st::pollAttach,
		_media);
	attach->show();
	field->sizeValue(
	) | rpl::on_next([=](QSize size) {
		attach->moveToRight(
			st::createPollOptionRemovePosition.x(),
			st::createPollOptionRemovePosition.y() - st::lineWidth * 2,
			size.width());
	}, attach->lifetime());
	attach->clicks(
	) | rpl::on_next([=](Qt::MouseButton button) {
		if (button != Qt::LeftButton) {
			return;
		}
		if (_attachCallback) {
			_attachCallback(not_null<Ui::RpWidget*>(attach), _media);
		}
	}, attach->lifetime());
	if (_widgetDropCallback) {
		_widgetDropCallback(attach, _media);
	}
	_attach.reset(attach);
}

void Options::Option::createWarning() {
	using namespace rpl::mappers;

	const auto field = this->field();
	const auto warning = CreateWarningLabel(
		field,
		field,
		kOptionLimit,
		kWarnOptionLimit);
	rpl::combine(
		field->sizeValue(),
		warning->sizeValue()
	) | rpl::on_next([=](QSize size, QSize label) {
		warning->moveToLeft(
			(size.width()
				- label.width()
				- st::createPollWarningPosition.x()),
			(size.height()
				- label.height()
				- st::createPollWarningPosition.y()),
			size.width());
	}, warning->lifetime());
}

bool Options::Option::isEmpty() const {
	return field()->getLastText().trimmed().isEmpty();
}

bool Options::Option::isGood() const {
	return !field()->getLastText().trimmed().isEmpty() && !isTooLong();
}

bool Options::Option::isTooLong() const {
	return (field()->getLastText().size() > kOptionLimit);
}

bool Options::Option::isCorrect() const {
	return isGood() && _correct && _correct->entity()->Checkbox::checked();
}

bool Options::Option::uploadingMedia() const {
	return _media->uploading;
}

bool Options::Option::hasFocus() const {
	return field()->hasFocus();
}

void Options::Option::setFocus() const {
	FocusAtEnd(field());
}

void Options::Option::clearValue() {
	field()->setText(QString());
}

void Options::Option::setPlaceholder() const {
	field()->setPlaceholder(tr::lng_polls_create_option_add());
}

void Options::Option::toggleRemoveAlways(bool toggled) {
	*_removeAlways = toggled;
}

void Options::Option::enableChooseCorrect(
		std::shared_ptr<Ui::RadiobuttonGroup> group) {
	if (!group) {
		if (_correct) {
			_hasCorrect = false;
			_correct->hide(anim::type::normal);
			toggleCorrectSpace(false);
		}
		return;
	}
	static auto Index = 0;
	const auto button = Ui::CreateChild<Ui::FadeWrapScaled<Ui::Radiobutton>>(
		_content.get(),
		object_ptr<Ui::Radiobutton>(
			_content.get(),
			group,
			++Index,
			QString(),
			st::defaultCheckbox));
	button->entity()->resize(
		button->entity()->height(),
		button->entity()->height());
	button->hide(anim::type::instant);
	_content->sizeValue(
	) | rpl::on_next([=](QSize size) {
		const auto left = st::createPollFieldPadding.left();
		button->moveToLeft(
			left,
			(size.height() - button->heightNoMargins()) / 2);
	}, button->lifetime());
	_correct.reset(button);
	_hasCorrect = true;
	if (isGood()) {
		_correct->show(anim::type::normal);
	} else {
		_correct->hide(anim::type::instant);
	}
	toggleCorrectSpace(true);
}

void Options::Option::toggleCorrectSpace(bool visible) {
	_correctShown.start(
		[=] { updateFieldGeometry(); },
		visible ? 0. : 1.,
		visible ? 1. : 0.,
		st::fadeWrapDuration);
}

void Options::Option::updateFieldGeometry() {
	const auto shown = _correctShown.value(_hasCorrect ? 1. : 0.);
	const auto skip = st::defaultRadio.diameter
		+ st::defaultCheckbox.textPosition.x();
	const auto left = anim::interpolate(0, skip, shown);
	_field->resizeToWidth(_content->width() - left);
	_field->moveToLeft(left, 0);
}

not_null<Ui::InputField*> Options::Option::field() const {
	return _field;
}

void Options::Option::removePlaceholder() const {
	field()->setPlaceholder(rpl::single(QString()));
}

PollAnswer Options::Option::toPollAnswer(int index) const {
	Expects(index >= 0 && index < kMaxOptionsCount);

	const auto text = field()->getTextWithTags();

	auto result = PollAnswer{
		TextWithEntities{
			.text = text.text,
			.entities = TextUtilities::ConvertTextTagsToEntities(text.tags),
		},
		QByteArray(1, ('0' + index)),
	};
	result.media = _media->media;
	TextUtilities::Trim(result.text);
	result.correct = _correct ? _correct->entity()->Checkbox::checked() : false;
	return result;
}

rpl::producer<Qt::MouseButton> Options::Option::removeClicks() const {
	return _remove->clicks();
}

Options::Options(
	not_null<Ui::BoxContent*> box,
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller,
	ChatHelpers::TabbedPanel *emojiPanel,
	bool chooseCorrectEnabled,
	AttachCallback attachCallback,
	FieldDropCallback fieldDropCallback,
	WidgetDropCallback widgetDropCallback)
: _box(box)
, _container(container)
, _controller(controller)
, _emojiPanel(emojiPanel)
, _attachCallback(std::move(attachCallback))
, _fieldDropCallback(std::move(fieldDropCallback))
, _widgetDropCallback(std::move(widgetDropCallback))
, _chooseCorrectGroup(chooseCorrectEnabled
	? createChooseCorrectGroup()
	: nullptr)
, _position(_container->count()) {
	checkLastOption();
}

bool Options::full() const {
	const auto limit = _controller->session().appConfig().pollOptionsLimit();
	return (_list.size() >= limit);
}

bool Options::hasOptions() const {
	return _hasOptions;
}

bool Options::isValid() const {
	return _isValid;
}

bool Options::hasCorrect() const {
	return _hasCorrect;
}

bool Options::hasUploadingMedia() const {
	return ranges::any_of(_list, &Option::uploadingMedia);
}

rpl::producer<int> Options::usedCount() const {
	return _usedCount.value();
}

rpl::producer<not_null<QWidget*>> Options::scrollToWidget() const {
	return _scrollToWidget.events();
}

rpl::producer<> Options::backspaceInFront() const {
	return _backspaceInFront.events();
}

rpl::producer<> Options::tabbed() const {
	return _tabbed.events();
}

void Options::Option::show(anim::type animated) {
	_wrap->show(animated);
}

void Options::Option::destroy(FnMut<void()> done) {
	if (anim::Disabled() || _wrap->isHidden()) {
		Ui::PostponeCall(std::move(done));
		return;
	}
	_wrap->hide(anim::type::normal);
	base::call_delayed(
		st::slideWrapDuration * 2,
		_content.get(),
		std::move(done));
}

std::vector<PollAnswer> Options::toPollAnswers() const {
	auto result = std::vector<PollAnswer>();
	result.reserve(_list.size());
	auto counter = int(0);
	const auto makeAnswer = [&](const std::unique_ptr<Option> &option) {
		return option->toPollAnswer(counter++);
	};
	ranges::copy(
		_list
		| ranges::views::filter(&Option::isGood)
		| ranges::views::transform(makeAnswer),
		ranges::back_inserter(result));
	return result;
}

void Options::focusFirst() {
	Expects(!_list.empty());

	_list.front()->setFocus();
}

std::shared_ptr<Ui::RadiobuttonGroup> Options::createChooseCorrectGroup() {
	auto result = std::make_shared<Ui::RadiobuttonGroup>(0);
	result->setChangedCallback([=](int) {
		validateState();
	});
	return result;
}

void Options::enableChooseCorrect(bool enabled) {
	_chooseCorrectGroup = enabled
		? createChooseCorrectGroup()
		: nullptr;
	for (auto &option : _list) {
		option->enableChooseCorrect(_chooseCorrectGroup);
	}
	validateState();
}

bool Options::correctShadows() const {
	// Last one should be without shadow.
	const auto noShadow = ranges::find(
		_list,
		true,
		ranges::not_fn(&Option::hasShadow));
	return (noShadow == end(_list) - 1);
}

void Options::fixShadows() {
	if (correctShadows()) {
		return;
	}
	for (auto &option : _list) {
		option->createShadow();
	}
	_list.back()->destroyShadow();
}

void Options::removeEmptyTail() {
	// Only one option at the end of options list can be empty.
	// Remove all other trailing empty options.
	// Only last empty and previous option have non-empty placeholders.
	const auto focused = ranges::find_if(
		_list,
		&Option::hasFocus);
	const auto end = _list.end();
	const auto reversed = ranges::views::reverse(_list);
	const auto emptyItem = ranges::find_if(
		reversed,
		ranges::not_fn(&Option::isEmpty)).base();
	const auto focusLast = (focused > emptyItem) && (focused < end);
	if (emptyItem == end) {
		return;
	}
	if (focusLast) {
		(*emptyItem)->setFocus();
	}
	for (auto i = emptyItem + 1; i != end; ++i) {
		destroy(std::move(*i));
	}
	_list.erase(emptyItem + 1, end);
	fixAfterErase();
}

void Options::destroy(std::unique_ptr<Option> option) {
	const auto value = option.get();
	option->destroy([=] { removeDestroyed(value); });
	_destroyed.push_back(std::move(option));
}

void Options::fixAfterErase() {
	Expects(!_list.empty());

	const auto last = _list.end() - 1;
	(*last)->setPlaceholder();
	(*last)->toggleRemoveAlways(false);
	if (last != begin(_list)) {
		(*(last - 1))->setPlaceholder();
		(*(last - 1))->toggleRemoveAlways(false);
	}
	fixShadows();
}

void Options::addEmptyOption() {
	if (full()) {
		return;
	} else if (!_list.empty() && _list.back()->isEmpty()) {
		return;
	}
	if (_list.size() > 1) {
		(*(_list.end() - 2))->removePlaceholder();
		(*(_list.end() - 2))->toggleRemoveAlways(true);
	}
	_list.push_back(std::make_unique<Option>(
		_box,
		_container,
		&_controller->session(),
		_position + _list.size() + _destroyed.size(),
		_chooseCorrectGroup,
		_attachCallback,
		_fieldDropCallback,
		_widgetDropCallback));
	const auto field = _list.back()->field();
	if (const auto emojiPanel = _emojiPanel) {
		const auto emojiToggle = Ui::AddEmojiToggleToField(
			field,
			_box,
			_controller,
			emojiPanel,
			QPoint(
				-st::createPollOptionFieldPremium.textMargins.right(),
				st::createPollOptionEmojiPositionSkip));
		emojiToggle->shownValue() | rpl::on_next([=](bool shown) {
			if (!shown) {
				return;
			}
			_emojiPanelLifetime.destroy();
			emojiPanel->selector()->emojiChosen(
			) | rpl::on_next([=](ChatHelpers::EmojiChosen data) {
				if (field->hasFocus()) {
					Ui::InsertEmojiAtCursor(field->textCursor(), data.emoji);
				}
			}, _emojiPanelLifetime);
			emojiPanel->selector()->customEmojiChosen(
			) | rpl::on_next([=](ChatHelpers::FileChosen data) {
				if (field->hasFocus()) {
					Data::InsertCustomEmoji(field, data.document);
				}
			}, _emojiPanelLifetime);
		}, emojiToggle->lifetime());
	}
	field->submits(
	) | rpl::on_next([=] {
		const auto index = findField(field);
		if (_list[index]->isGood() && index + 1 < _list.size()) {
			_list[index + 1]->setFocus();
		}
	}, field->lifetime());
	field->changes(
	) | rpl::on_next([=] {
		Ui::PostponeCall(crl::guard(field, [=] {
			validateState();
		}));
	}, field->lifetime());
	field->focusedChanges(
	) | rpl::filter(rpl::mappers::_1) | rpl::on_next([=] {
		_scrollToWidget.fire_copy(field);
	}, field->lifetime());
	field->tabbed(
	) | rpl::on_next([=](not_null<bool*> handled) {
		const auto index = findField(field);
		if (index + 1 < _list.size()) {
			_list[index + 1]->setFocus();
		} else {
			_tabbed.fire({});
		}
		*handled = true;
	}, field->lifetime());
	base::install_event_filter(field, [=](not_null<QEvent*> event) {
		if (event->type() != QEvent::KeyPress
			|| !field->getLastText().isEmpty()) {
			return base::EventFilterResult::Continue;
		}
		const auto key = static_cast<QKeyEvent*>(event.get())->key();
		if (key != Qt::Key_Backspace) {
			return base::EventFilterResult::Continue;
		}

		const auto index = findField(field);
		if (index > 0) {
			_list[index - 1]->setFocus();
		} else {
			_backspaceInFront.fire({});
		}
		return base::EventFilterResult::Cancel;
	});

	_list.back()->removeClicks(
	) | rpl::on_next([=] {
		Ui::PostponeCall(crl::guard(field, [=] {
			Expects(!_list.empty());

			const auto item = begin(_list) + findField(field);
			if (item == _list.end() - 1) {
				(*item)->clearValue();
				return;
			}
			if ((*item)->hasFocus()) {
				(*(item + 1))->setFocus();
			}
			destroy(std::move(*item));
			_list.erase(item);
			fixAfterErase();
			validateState();
		}));
	}, field->lifetime());

	_list.back()->show((_list.size() == 1)
		? anim::type::instant
		: anim::type::normal);
	fixShadows();
}

void Options::removeDestroyed(not_null<Option*> option) {
	const auto i = ranges::find(
		_destroyed,
		option.get(),
		&std::unique_ptr<Option>::get);
	Assert(i != end(_destroyed));
	_destroyed.erase(i);
}

void Options::validateState() {
	checkLastOption();
	_hasOptions = (ranges::count_if(_list, &Option::isGood) > 1);
	_isValid = _hasOptions && ranges::none_of(_list, &Option::isTooLong);
	_hasCorrect = ranges::any_of(_list, &Option::isCorrect);

	const auto lastEmpty = !_list.empty() && _list.back()->isEmpty();
	_usedCount = _list.size() - (lastEmpty ? 1 : 0);
}

int Options::findField(not_null<Ui::InputField*> field) const {
	const auto result = ranges::find(
		_list,
		field,
		&Option::field) - begin(_list);

	Ensures(result >= 0 && result < _list.size());
	return result;
}

void Options::checkLastOption() {
	removeEmptyTail();
	addEmptyOption();
}

} // namespace

CreatePollBox::CreatePollBox(
	QWidget*,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	PollData::Flags chosen,
	PollData::Flags disabled,
	rpl::producer<int> starsRequired,
	Api::SendType sendType,
	SendMenu::Details sendMenuDetails)
: _controller(controller)
, _peer(peer)
, _chosen(chosen)
, _disabled(disabled)
, _sendType(sendType)
, _sendMenuDetails([result = sendMenuDetails] { return result; })
, _starsRequired(std::move(starsRequired)) {
}

rpl::producer<CreatePollBox::Result> CreatePollBox::submitRequests() const {
	return _submitRequests.events();
}

void CreatePollBox::setInnerFocus() {
	_setInnerFocus();
}

void CreatePollBox::submitFailed(const QString &error) {
	showToast(error);
}

not_null<Ui::InputField*> CreatePollBox::setupQuestion(
		not_null<Ui::VerticalLayout*> container) {
	using namespace Settings;

	const auto session = &_controller->session();
	const auto isPremium = session->user()->isPremium();
	Ui::AddSubsectionTitle(container, tr::lng_polls_create_question());

	const auto question = container->add(
		object_ptr<Ui::InputField>(
			container,
			st::createPollField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_polls_create_question_placeholder()),
		st::createPollFieldPadding
			+ (isPremium
				? QMargins(0, 0, st::defaultComposeFiles.emoji.inner.width, 0)
				: QMargins()));
	InitField(getDelegate()->outerContainer(), question, session);
	question->setMaxLength(kQuestionLimit + kErrorLimit);
	question->setSubmitSettings(Ui::InputField::SubmitSettings::Both);

	if (isPremium) {
		using Selector = ChatHelpers::TabbedSelector;
		const auto outer = getDelegate()->outerContainer();
		_emojiPanel = base::make_unique_q<ChatHelpers::TabbedPanel>(
			outer,
			_controller,
			object_ptr<Selector>(
				nullptr,
				_controller->uiShow(),
				Window::GifPauseReason::Layer,
				Selector::Mode::EmojiOnly));
		const auto emojiPanel = _emojiPanel.get();
		emojiPanel->setDesiredHeightValues(
			1.,
			st::emojiPanMinHeight / 2,
			st::emojiPanMinHeight);
		emojiPanel->hide();
		emojiPanel->selector()->setCurrentPeer(session->user());

		const auto emojiToggle = Ui::AddEmojiToggleToField(
			question,
			this,
			_controller,
			emojiPanel,
			st::createPollOptionFieldPremiumEmojiPosition);
		emojiPanel->selector()->emojiChosen(
		) | rpl::on_next([=](ChatHelpers::EmojiChosen data) {
			if (question->hasFocus()) {
				Ui::InsertEmojiAtCursor(question->textCursor(), data.emoji);
			}
		}, emojiToggle->lifetime());
		emojiPanel->selector()->customEmojiChosen(
		) | rpl::on_next([=](ChatHelpers::FileChosen data) {
			if (question->hasFocus()) {
				Data::InsertCustomEmoji(question, data.document);
			}
		}, emojiToggle->lifetime());
	}

	const auto warning = CreateWarningLabel(
		container,
		question,
		kQuestionLimit,
		kWarnQuestionLimit);
	rpl::combine(
		question->geometryValue(),
		warning->sizeValue()
	) | rpl::on_next([=](QRect geometry, QSize label) {
		warning->moveToLeft(
			(container->width()
				- label.width()
				- st::createPollWarningPosition.x()),
			(geometry.y()
				- st::createPollFieldPadding.top()
				- st::defaultSubsectionTitlePadding.bottom()
				- st::defaultSubsectionTitle.style.font->height
				+ st::defaultSubsectionTitle.style.font->ascent
				- st::createPollWarning.style.font->ascent),
			geometry.width());
	}, warning->lifetime());

	return question;
}

not_null<Ui::InputField*> CreatePollBox::setupDescription(
		not_null<Ui::VerticalLayout*> container) {
	const auto session = &_controller->session();
	const auto description = container->add(
		object_ptr<Ui::InputField>(
			container,
			st::pollDescriptionField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_polls_create_description_placeholder()),
		st::pollDescriptionFieldPadding);
	InitField(getDelegate()->outerContainer(), description, session);
	description->setSubmitSettings(Ui::InputField::SubmitSettings::Both);
	return description;
}

not_null<Ui::InputField*> CreatePollBox::setupSolution(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<bool> shown) {
	using namespace Settings;

	const auto outer = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container))
	)->setDuration(0)->toggleOn(std::move(shown));
	const auto inner = outer->entity();

	const auto session = &_controller->session();
	Ui::AddSkip(inner);
	Ui::AddSubsectionTitle(inner, tr::lng_polls_solution_title());
	const auto solution = inner->add(
		object_ptr<Ui::InputField>(
			inner,
			st::pollMediaField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_polls_solution_placeholder()),
		st::createPollFieldPadding);
	InitField(getDelegate()->outerContainer(), solution, session);
	solution->setMaxLength(kSolutionLimit + kErrorLimit);
	solution->setInstantReplaces(Ui::InstantReplaces::Default());
	solution->setInstantReplacesEnabled(
		Core::App().settings().replaceEmojiValue(),
		Core::App().settings().systemTextReplaceValue());
	solution->setMarkdownReplacesEnabled(rpl::single(
		Ui::MarkdownEnabledState{ Ui::MarkdownEnabled{ {
			Ui::InputField::kTagBold,
			Ui::InputField::kTagItalic,
			Ui::InputField::kTagUnderline,
			Ui::InputField::kTagStrikeOut,
			Ui::InputField::kTagCode,
			Ui::InputField::kTagSpoiler,
		} } }
	));
	solution->setEditLinkCallback(
		DefaultEditLinkCallback(_controller->uiShow(), solution));

	const auto warning = CreateWarningLabel(
		inner,
		solution,
		kSolutionLimit,
		kWarnSolutionLimit);
	rpl::combine(
		solution->geometryValue(),
		warning->sizeValue()
	) | rpl::on_next([=](QRect geometry, QSize label) {
		warning->moveToLeft(
			(inner->width()
				- label.width()
				- st::createPollWarningPosition.x()),
			(geometry.y()
				- st::createPollFieldPadding.top()
				- st::defaultSubsectionTitlePadding.bottom()
				- st::defaultSubsectionTitle.style.font->height
				+ st::defaultSubsectionTitle.style.font->ascent
				- st::createPollWarning.style.font->ascent),
			geometry.width());
	}, warning->lifetime());

	inner->add(
		object_ptr<Ui::FlatLabel>(
			inner,
			tr::lng_polls_solution_about(),
			st::boxDividerLabel),
		st::createPollFieldTitlePadding);

	return solution;
}

object_ptr<Ui::RpWidget> CreatePollBox::setupContent() {
	using namespace Settings;

	const auto id = base::RandomValue<uint64>();
	struct UploadContext {
		std::weak_ptr<PollMediaState> media;
		uint64 token = 0;
	};
	struct State final {
		Errors error = Error::Question;
		std::unique_ptr<Options> options;
		rpl::event_stream<bool> multipleForceOff;
		rpl::event_stream<bool> addOptionsForceOff;
		rpl::event_stream<bool> revotingForceOff;
		rpl::event_stream<bool> quizForceOff;
		std::shared_ptr<PollMediaState> descriptionMedia
			= std::make_shared<PollMediaState>();
		std::shared_ptr<PollMediaState> solutionMedia
			= std::make_shared<PollMediaState>();
		std::weak_ptr<PollMediaState> stickerTarget;
		base::flat_map<FullMsgId, UploadContext> uploads;
		base::unique_qptr<Ui::PopupMenu> mediaMenu;
		base::unique_qptr<ChatHelpers::TabbedPanel> stickerPanel;
		std::unique_ptr<TaskQueue> prepareQueue;
	};
	const auto state = lifetime().make_state<State>();
	state->prepareQueue = std::make_unique<TaskQueue>();

	auto result = object_ptr<Ui::VerticalLayout>(this);
	const auto container = result.data();

	const auto updateMedia = [=](
			const std::shared_ptr<PollMediaState> &media) {
		if (media->update) {
			media->update();
		}
	};
	const auto setMedia = [=](
			const std::shared_ptr<PollMediaState> &media,
			PollMedia value,
			std::shared_ptr<Ui::DynamicImage> thumbnail,
			bool rounded) {
		media->token++;
		media->media = value;
		media->thumbnail = std::move(thumbnail);
		media->rounded = rounded;
		media->progress = (media->uploading && media->media)
			? 1.
			: 0.;
		media->uploadDataId = 0;
		media->uploading = false;
		updateMedia(media);
	};
	struct UploadedMedia final {
		PollMedia input;
		std::shared_ptr<Ui::DynamicImage> thumbnail;
	};
	const auto parseUploaded = [=](
			const MTPMessageMedia &result,
			FullMsgId fullId) {
		auto parsed = UploadedMedia();
		auto &owner = _controller->session().data();
		result.match([&](const MTPDmessageMediaPhoto &media) {
			if (const auto photo = media.vphoto()) {
				photo->match([&](const MTPDphoto &) {
					parsed.input.photo = owner.processPhoto(*photo);
					parsed.thumbnail = Ui::MakePhotoThumbnail(
						parsed.input.photo,
						fullId);
				}, [](const auto &) {
				});
			}
		}, [&](const MTPDmessageMediaDocument &media) {
			if (const auto document = media.vdocument()) {
				document->match([&](const MTPDdocument &) {
					parsed.input.document = owner.processDocument(
						*document);
					parsed.thumbnail
						= Ui::MakeDocumentThumbnail(
							parsed.input.document,
							fullId);
				}, [](const auto &) {
				});
			}
		}, [](const auto &) {
		});
		return parsed;
	};
	const auto applyUploaded = [=](
			const std::shared_ptr<PollMediaState> &media,
			uint64 token,
			FullMsgId fullId,
			const MTPInputFile &file) {
		const auto uploaded = MTP_inputMediaUploadedPhoto(
			MTP_flags(0),
			file,
			MTP_vector<MTPInputDocument>(QVector<MTPInputDocument>()),
			MTPint(),
			MTPInputDocument());
		_controller->session().api().request(MTPmessages_UploadMedia(
			MTP_flags(0),
			MTPstring(),
			_peer->input(),
			uploaded
		)).done([=](const MTPMessageMedia &result) {
			if (media->token != token) {
				return;
			}
			auto parsed = parseUploaded(result, fullId);
			if (!parsed.input) {
				setMedia(media, PollMedia(), nullptr, false);
				showToast(tr::lng_attach_failed(tr::now));
				return;
			}
			setMedia(
				media,
				parsed.input,
				media->thumbnail
					? media->thumbnail
					: std::move(parsed.thumbnail),
				true);
		}).fail([=](const MTP::Error &) {
			if (media->token != token) {
				return;
			}
			setMedia(media, PollMedia(), nullptr, false);
			showToast(tr::lng_attach_failed(tr::now));
		}).send();
	};
	_controller->session().uploader().photoReady(
	) | rpl::on_next([=](const Storage::UploadedMedia &data) {
		const auto context = state->uploads.take(data.fullId);
		if (!context) {
			return;
		}
		const auto media = context->media.lock();
		if (!media || (media->token != context->token)) {
			return;
		}
		applyUploaded(media, context->token, data.fullId, data.info.file);
	}, lifetime());
	_controller->session().uploader().photoProgress(
	) | rpl::on_next([=](const FullMsgId &id) {
		const auto i = state->uploads.find(id);
		if (i == state->uploads.end()) {
			return;
		}
		const auto &context = i->second;
		const auto media = context.media.lock();
		if (!media
			|| (media->token != context.token)
			|| !media->uploadDataId) {
			return;
		}
		media->progress = _controller->session().data().photo(
			media->uploadDataId)->progress();
		updateMedia(media);
	}, lifetime());
	_controller->session().uploader().photoFailed(
	) | rpl::on_next([=](const FullMsgId &id) {
		const auto context = state->uploads.take(id);
		if (!context) {
			return;
		}
		const auto media = context->media.lock();
		if (!media || (media->token != context->token)) {
			return;
		}
		setMedia(media, PollMedia(), nullptr, false);
		showToast(tr::lng_attach_failed(tr::now));
	}, lifetime());
	const auto emojiPaused = [=] {
		using namespace Window;
		return _controller->isGifPausedAtLeastFor(GifPauseReason::Any);
	};
	const auto updateStickerPanelGeometry = [=] {
		if (!state->stickerPanel) {
			return;
		}
		const auto panel = state->stickerPanel.get();
		const auto parent = panel->parentWidget();
		const auto left = std::max(
			(parent->width() - panel->width()) / 2,
			0);
		const auto top = std::max(
			(parent->height() - panel->height()) / 2,
			0);
		panel->moveTopRight(top, left + panel->width());
	};
	const auto showStickerPanel = [=](
			not_null<Ui::RpWidget*>,
			std::shared_ptr<PollMediaState> media) {
		using Selector = ChatHelpers::TabbedSelector;
		using Descriptor = ChatHelpers::TabbedSelectorDescriptor;
		using Mode = ChatHelpers::TabbedSelector::Mode;
		using Panel = ChatHelpers::TabbedPanel;
		if (!state->stickerPanel) {
			const auto body = getDelegate()->outerContainer();
			state->stickerPanel = base::make_unique_q<Panel>(
				body,
				_controller,
				object_ptr<Selector>(
					nullptr,
					Descriptor{
						.show = _controller->uiShow(),
						.st = st::backgroundEmojiPan,
						.level = Window::GifPauseReason::Layer,
						.mode = Mode::StickersOnly,
						.features = {
							.megagroupSet = false,
							.stickersSettings = false,
							.openStickerSets = false,
						},
					}));
			state->stickerPanel->setDropDown(true);
			state->stickerPanel->setDesiredHeightValues(
				0.,
				st::emojiPanMinHeight,
				st::emojiPanMinHeight);
			state->stickerPanel->hide();
			base::install_event_filter(
				body,
				[=](not_null<QEvent*> event) {
					const auto type = event->type();
					if (type == QEvent::Move || type == QEvent::Resize) {
						crl::on_main(this, updateStickerPanelGeometry);
					}
			return base::EventFilterResult::Continue;
		},
		lifetime());
			state->stickerPanel->selector()->fileChosen(
			) | rpl::on_next([=](ChatHelpers::FileChosen data) {
				if (Window::ShowSendPremiumError(
						_controller,
						data.document)) {
					return;
				}
				const auto target = state->stickerTarget.lock();
				if (!target) {
					return;
				}
				setMedia(
					target,
					PollMedia{ .document = data.document },
					Ui::MakeEmojiThumbnail(
						&_controller->session().data(),
						Data::SerializeCustomEmojiId(data.document),
						emojiPaused),
					false);
				state->stickerPanel->hideAnimated();
			}, state->stickerPanel->lifetime());
		}
		state->stickerTarget = media;
		const auto panel = state->stickerPanel.get();
		updateStickerPanelGeometry();
		panel->toggleAnimated();
	};
	const auto startPreparedPhotoUpload = [=](
			std::shared_ptr<PollMediaState> media,
			Ui::PreparedFile file) {
		const auto token = ++media->token;
		media->media = PollMedia();
		media->thumbnail = std::make_shared<LocalImageThumbnail>(
			std::move(file.preview));
		media->rounded = true;
		media->uploading = true;
		media->progress = 0.;
		media->uploadDataId = 0;
		updateMedia(media);
		using PreparePoll = PreparePollMediaTask;
		state->prepareQueue->addTask(std::make_unique<PreparePoll>(
			FileLoadTask::Args{
				.session = &_controller->session(),
				.filepath = file.path,
				.content = file.content,
				.information = std::move(file.information),
				.videoCover = nullptr,
				.type = SendMediaType::Photo,
				.to = FileLoadTo(
					_peer->id,
					Api::SendOptions(),
					FullReplyTo(),
					MsgId()),
				.caption = TextWithTags(),
				.spoiler = false,
				.album = nullptr,
				.forceFile = false,
				.idOverride = 0,
				.displayName = file.displayName,
			},
			[=](std::shared_ptr<FilePrepareResult> prepared) {
				if ((media->token != token)
					|| !prepared
					|| (prepared->type != SendMediaType::Photo)) {
					if (media->token == token) {
						setMedia(media, PollMedia(), nullptr, false);
						showToast(tr::lng_attach_failed(tr::now));
					}
					return;
				}
				const auto uploadId = FullMsgId(
					_peer->id,
					_controller->session().data().nextLocalMessageId());
				state->uploads.emplace(uploadId, UploadContext{
					.media = media,
					.token = token,
				});
				media->uploadDataId = prepared->id;
				_controller->session().uploader().upload(
					uploadId,
					prepared);
			}));
	};
	const auto applyPreparedPhotoList = [=](
			std::shared_ptr<PollMediaState> media,
			Ui::PreparedList &&list) {
		if (list.error != Ui::PreparedList::Error::None
			|| (list.files.size() != 1)
			|| (list.files.front().type != Ui::PreparedFile::Type::Photo)) {
			return false;
		}
		startPreparedPhotoUpload(media, std::move(list.files.front()));
		return true;
	};
	const auto applyPhotoDrop = [=](
			std::shared_ptr<PollMediaState> media,
			not_null<const QMimeData*> data) {
		return applyPreparedPhotoList(
			media,
			PhotoListFromMimeData(
				data,
				_controller->session().premium()));
	};
	const auto installPhotoDropToWidget = [=](
			not_null<QWidget*> widget,
			std::shared_ptr<PollMediaState> media) {
		widget->setAcceptDrops(true);
		base::install_event_filter(widget, [=](not_null<QEvent*> event) {
			const auto type = event->type();
			if (type != QEvent::DragEnter
				&& type != QEvent::DragMove
				&& type != QEvent::Drop) {
				return base::EventFilterResult::Continue;
			}
			const auto drop = static_cast<QDropEvent*>(event.get());
			const auto data = drop->mimeData();
			if (!data || !Storage::ValidatePhotoEditorMediaDragData(data)) {
				return base::EventFilterResult::Continue;
			}
			if (type == QEvent::Drop && !applyPhotoDrop(media, data)) {
				return base::EventFilterResult::Continue;
			}
			drop->acceptProposedAction();
			return base::EventFilterResult::Cancel;
		});
	};
	const auto installPhotoDropToField = [=](
			not_null<Ui::InputField*> field,
			std::shared_ptr<PollMediaState> media) {
		field->setMimeDataHook([=](
				not_null<const QMimeData*> data,
				Ui::InputField::MimeAction action) {
			if (action == Ui::InputField::MimeAction::Check) {
				return Storage::ValidatePhotoEditorMediaDragData(data);
			} else if (action == Ui::InputField::MimeAction::Insert) {
				return applyPhotoDrop(media, data);
			}
			Unexpected("Polls: action in MimeData hook.");
		});
	};
	const auto choosePhoto = [=](std::shared_ptr<PollMediaState> media) {
		const auto callback = crl::guard(this, [=](
				FileDialog::OpenResult &&result) {
			const auto checkResult = [&](const Ui::PreparedList &list) {
				using namespace Ui;
				return (list.files.size() == 1)
					&& (list.files.front().type == PreparedFile::Type::Photo);
			};
			const auto showError = [=](tr::phrase<> text) {
				showToast(text(tr::now));
			};
			auto list = Storage::PreparedFileFromFilesDialog(
				std::move(result),
				checkResult,
				showError,
				st::sendMediaPreviewSize,
				_controller->session().premium());
			if (list) {
				applyPreparedPhotoList(media, std::move(*list));
			}
		});
		FileDialog::GetOpenPath(
			this,
			tr::lng_attach_photo(tr::now),
			FileDialog::ImagesFilter(),
			callback);
	};
	const auto clearMedia = [=](std::shared_ptr<PollMediaState> media) {
		auto toCancel = std::vector<FullMsgId>();
		for (auto i = state->uploads.begin(); i != state->uploads.end();) {
			if (i->second.media.lock() == media) {
				toCancel.push_back(i->first);
				i = state->uploads.erase(i);
			} else {
				++i;
			}
		}
		for (const auto &id : toCancel) {
			_controller->session().uploader().cancel(id);
		}
		setMedia(media, PollMedia(), nullptr, false);
	};
	const auto showMediaMenu = [=](
			not_null<Ui::RpWidget*> button,
			std::shared_ptr<PollMediaState> media,
			bool allowStickers = true) {
		state->mediaMenu = base::make_unique_q<Ui::PopupMenu>(
			button,
			st::popupMenuWithIcons);
		state->mediaMenu->setForcedOrigin(
			Ui::PanelAnimation::Origin::TopRight);
		state->mediaMenu->addAction(
			tr::lng_attach_photo(tr::now),
			[=] { choosePhoto(media); },
			&st::menuIconPhoto);
		if (allowStickers) {
			state->mediaMenu->addAction(
				tr::lng_chat_intro_choose_sticker(tr::now),
				[=] { showStickerPanel(button, media); },
				&st::menuIconStickers);
		}
		if (media->media || media->uploading) {
			state->mediaMenu->addAction(
				tr::lng_box_remove(tr::now),
				[=] { clearMedia(media); },
				&st::menuIconDelete);
		}
		state->mediaMenu->popup(QCursor::pos());
	};
	const auto addMediaButton = [=](
			not_null<Ui::InputField*> field,
			std::shared_ptr<PollMediaState> media) {
		const auto button = Ui::CreateChild<PollMediaButton>(
			field,
			st::pollAttach,
			media);
		button->show();
		installPhotoDropToField(field, media);
		installPhotoDropToWidget(button, media);
		field->sizeValue(
		) | rpl::on_next([=](QSize size) {
			button->moveToRight(
				st::createPollOptionRemovePosition.x() + st::pollAttachShift.x(),
				((size.height() - button->height()) / 2)
					+ st::pollAttachShift.y(),
				size.width());
		}, button->lifetime());
		button->clicks(
		) | rpl::on_next([=](Qt::MouseButton buttonType) {
			if (buttonType != Qt::LeftButton) {
				return;
			}
			showMediaMenu(button, media, false);
		}, button->lifetime());
	};

	const auto question = setupQuestion(container);
	const auto description = setupDescription(container);
	addMediaButton(description, state->descriptionMedia);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_polls_create_options(),
			st::defaultSubsectionTitle),
		st::createPollFieldTitlePadding);
	state->options = std::make_unique<Options>(
		this,
		container,
		_controller,
		_emojiPanel ? _emojiPanel.get() : nullptr,
		(_chosen & PollData::Flag::Quiz),
		showMediaMenu,
		installPhotoDropToField,
		installPhotoDropToWidget);
	const auto options = state->options.get();
	auto limit = options->usedCount() | rpl::after_next([=](int count) {
		setCloseByEscape(!count);
		setCloseByOutsideClick(!count);
	}) | rpl::map([=](int count) {
		const auto appConfig = &_controller->session().appConfig();
		const auto max = appConfig->pollOptionsLimit();
		return (count < max)
			? tr::lng_polls_create_limit(tr::now, lt_count, max - count)
			: tr::lng_polls_create_maximum(tr::now);
	}) | rpl::after_next([=] {
		container->resizeToWidth(container->widthNoMargins());
	});
	container->add(
		object_ptr<Ui::DividerLabel>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				std::move(limit),
				st::boxDividerLabel),
			st::createPollLimitPadding));

	question->tabbed(
	) | rpl::on_next([=](not_null<bool*> handled) {
		description->setFocus();
		*handled = true;
	}, question->lifetime());

	description->tabbed(
	) | rpl::on_next([=](not_null<bool*> handled) {
		options->focusFirst();
		*handled = true;
	}, description->lifetime());

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_polls_create_settings());

	const auto showWhoVoted = (!(_disabled & PollData::Flag::PublicVotes))
		? AddPollToggleButton(
			container,
			tr::lng_polls_create_show_who_voted(),
			tr::lng_polls_create_show_who_voted_about(),
			{
				.icon = &st::pollBoxFilledPollViewIcon,
				.background = &st::settingsIconBg4,
			},
			rpl::single(!!(_chosen & PollData::Flag::PublicVotes)),
			st::detailedSettingsButtonStyle)
		: nullptr;
	const auto multiple = AddPollToggleButton(
		container,
		tr::lng_polls_create_allow_multiple_answers(),
		tr::lng_polls_create_allow_multiple_answers_about(),
		{
			.icon = &st::pollBoxFilledPollMultipleIcon,
			.background = &st::settingsIconBg3,
		},
		rpl::single(!!(_chosen & PollData::Flag::MultiChoice))
			| rpl::then(state->multipleForceOff.events()),
		st::detailedSettingsButtonStyle);
	const auto addOptions = (!(_disabled & PollData::Flag::OpenAnswers))
		? AddPollToggleButton(
			container,
			tr::lng_polls_create_allow_adding_options(),
			tr::lng_polls_create_allow_adding_options_about(),
			{
				.icon = &st::pollBoxFilledPollAddIcon,
				.background = &st::settingsIconBg4,
			},
			rpl::single(!!(_chosen & PollData::Flag::OpenAnswers))
				| rpl::then(state->addOptionsForceOff.events()),
			st::detailedSettingsButtonStyle)
		: nullptr;
	const auto revoting = AddPollToggleButton(
		container,
		tr::lng_polls_create_allow_revoting(),
		tr::lng_polls_create_allow_revoting_about(),
		{
			.icon = &st::pollBoxFilledPollRevoteIcon,
			.background = &st::settingsIconBg6,
		},
		rpl::single(!(_chosen & PollData::Flag::RevotingDisabled))
			| rpl::then(state->revotingForceOff.events()),
		st::detailedSettingsButtonStyle);
	const auto shuffle = AddPollToggleButton(
		container,
		tr::lng_polls_create_shuffle_options(),
		tr::lng_polls_create_shuffle_options_about(),
		{
			.icon = &st::pollBoxFilledPollShuffleIcon,
			.background = &st::settingsIconBg8,
		},
		rpl::single(!!(_chosen & PollData::Flag::ShuffleAnswers)),
		st::detailedSettingsButtonStyle);
	const auto quiz = AddPollToggleButton(
		container,
		tr::lng_polls_create_set_correct_answer(),
		tr::lng_polls_create_set_correct_answer_about(),
		{
			.icon = &st::pollBoxFilledPollCorrectIcon,
			.background = &st::settingsIconBg2,
		},
		rpl::single(!!(_chosen & PollData::Flag::Quiz))
			| rpl::then(state->quizForceOff.events()),
		st::detailedSettingsButtonStyle);

	const auto solution = setupSolution(
		container,
		rpl::single(quiz->toggled()) | rpl::then(quiz->toggledChanges()));
	addMediaButton(solution, state->solutionMedia);

	options->tabbed(
	) | rpl::on_next([=] {
		if (quiz->toggled()) {
			solution->setFocus();
		} else {
			question->setFocus();
		}
	}, question->lifetime());

	solution->tabbed(
	) | rpl::on_next([=](not_null<bool*> handled) {
		question->setFocus();
		*handled = true;
	}, solution->lifetime());

	const auto updateQuizDependentLocks = [=](bool checked) {
		if (addOptions) {
			addOptions->setToggleLocked(
				(_disabled & PollData::Flag::OpenAnswers) || checked);
		}
		revoting->setToggleLocked(
			(_disabled & PollData::Flag::RevotingDisabled) || checked);
		multiple->setToggleLocked(
			(_disabled & PollData::Flag::MultiChoice) || checked);
	};
	quiz->setToggleLocked(_disabled & PollData::Flag::Quiz);
	shuffle->setToggleLocked(_disabled & PollData::Flag::ShuffleAnswers);
	updateQuizDependentLocks(quiz->toggled());

	using namespace rpl::mappers;
	quiz->toggledChanges(
	) | rpl::on_next([=](bool checked) {
		if (checked && (_disabled & PollData::Flag::Quiz)) {
			state->quizForceOff.fire(false);
			return;
		}
		if (checked) {
			state->multipleForceOff.fire(false);
			state->addOptionsForceOff.fire(false);
			state->revotingForceOff.fire(false);
		}
		updateQuizDependentLocks(checked);
		options->enableChooseCorrect(checked);
	}, quiz->lifetime());

	const auto isValidQuestion = [=] {
		const auto text = question->getLastText().trimmed();
		return !text.isEmpty() && (text.size() <= kQuestionLimit);
	};
	question->submits(
	) | rpl::on_next([=] {
		if (isValidQuestion()) {
			description->setFocus();
		}
	}, question->lifetime());

	description->submits(
	) | rpl::on_next([=] {
		options->focusFirst();
	}, description->lifetime());

	_setInnerFocus = [=] {
		question->setFocusFast();
	};

	const auto collectResult = [=] {
		const auto textWithTags = question->getTextWithTags();
		const auto descriptionWithTags = description->getTextWithTags();
		using Flag = PollData::Flag;
		auto result = PollData(&_controller->session().data(), id);
		result.question.text = textWithTags.text;
		result.question.entities = TextUtilities::ConvertTextTagsToEntities(
			textWithTags.tags);
		TextUtilities::Trim(result.question);
		result.answers = options->toPollAnswers();
		const auto solutionWithTags = quiz->toggled()
			? solution->getTextWithAppliedMarkdown()
			: TextWithTags();
		result.solution = TextWithEntities{
			solutionWithTags.text,
			TextUtilities::ConvertTextTagsToEntities(solutionWithTags.tags)
		};
		result.attachedMedia = state->descriptionMedia->media;
		if (quiz->toggled()) {
			result.solutionMedia = state->solutionMedia->media;
		}
		const auto publicVotes = (showWhoVoted && showWhoVoted->toggled());
		const auto multiChoice = multiple->toggled();
		result.setFlags(Flag(0)
			| (publicVotes ? Flag::PublicVotes : Flag(0))
			| (multiChoice ? Flag::MultiChoice : Flag(0))
			| ((addOptions && addOptions->toggled()) ? Flag::OpenAnswers : Flag(0))
			| (!revoting->toggled() ? Flag::RevotingDisabled : Flag(0))
			| (shuffle->toggled() ? Flag::ShuffleAnswers : Flag(0))
			| (quiz->toggled() ? Flag::Quiz : Flag(0)));
		auto text = TextWithEntities{
			descriptionWithTags.text,
			TextUtilities::ConvertTextTagsToEntities(
				descriptionWithTags.tags),
		};
		TextUtilities::Trim(text);
		return Result{
			std::move(result),
			std::move(text),
			Api::SendOptions(),
		};
	};
	const auto collectError = [=] {
		if (isValidQuestion()) {
			state->error &= ~Error::Question;
		} else {
			state->error |= Error::Question;
		}
		if (!options->hasOptions()) {
			state->error |= Error::Options;
		} else if (!options->isValid()) {
			state->error |= Error::Other;
		} else {
			state->error &= ~(Error::Options | Error::Other);
		}
		if (quiz->toggled() && !options->hasCorrect()) {
			state->error |= Error::Correct;
		} else {
			state->error &= ~Error::Correct;
		}
		if (quiz->toggled()
			&& solution->getLastText().trimmed().size() > kSolutionLimit) {
			state->error |= Error::Solution;
		} else {
			state->error &= ~Error::Solution;
		}
		if (state->descriptionMedia->uploading
			|| (quiz->toggled() && state->solutionMedia->uploading)
			|| options->hasUploadingMedia()) {
			state->error |= Error::Media;
		} else {
			state->error &= ~Error::Media;
		}
	};
	const auto showError = [show = uiShow()](
			tr::phrase<> text) {
		show->showToast(text(tr::now));
	};
	const auto send = [=](Api::SendOptions sendOptions) {
		collectError();
		if (state->error & Error::Question) {
			showError(tr::lng_polls_choose_question);
			question->setFocus();
		} else if (state->error & Error::Options) {
			showError(tr::lng_polls_choose_answers);
			options->focusFirst();
		} else if (state->error & Error::Correct) {
			showError(tr::lng_polls_choose_correct);
		} else if (state->error & Error::Solution) {
			solution->showError();
		} else if (state->error & Error::Media) {
			showError(tr::lng_polls_media_uploading);
		} else if (!state->error) {
			auto result = collectResult();
			result.options = sendOptions;
			_submitRequests.fire(std::move(result));
		}
	};
	const auto sendAction = SendMenu::DefaultCallback(
		_controller->uiShow(),
		crl::guard(this, send));

	options->scrollToWidget(
	) | rpl::on_next([=](not_null<QWidget*> widget) {
		scrollToWidget(widget);
	}, lifetime());

	options->backspaceInFront(
	) | rpl::on_next([=] {
		FocusAtEnd(description);
	}, lifetime());

	const auto isNormal = (_sendType == Api::SendType::Normal);
	const auto schedule = [=] {
		sendAction(
			{ .type = SendMenu::ActionType::Schedule },
			_sendMenuDetails());
	};
	const auto submit = addButton(
		tr::lng_polls_create_button(),
		[=] { isNormal ? send({}) : schedule(); });
	submit->setText(PaidSendButtonText(_starsRequired.value(), isNormal
		? tr::lng_polls_create_button()
		: tr::lng_schedule_button()));
	const auto sendMenuDetails = [=] {
		collectError();
		return (state->error) ? SendMenu::Details() : _sendMenuDetails();
	};
	SendMenu::SetupMenuAndShortcuts(
		submit.data(),
		_controller->uiShow(),
		sendMenuDetails,
		sendAction);
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	return result;
}

void CreatePollBox::prepare() {
	setTitle(tr::lng_polls_create_title());

	const auto inner = setInnerWidget(setupContent());

	setDimensionsToContent(st::boxWideWidth, inner);
}
