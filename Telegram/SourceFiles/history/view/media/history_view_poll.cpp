/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_poll.h"

#include "core/click_handler_types.h"
#include "core/ui_integration.h" // TextContext
#include "lang/lang_keys.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_message.h"
#include "history/view/history_view_cursor_state.h"
#include "calls/calls_instance.h"
#include "ui/chat/message_bubble.h"
#include "ui/chat/chat_style.h"
#include "ui/image/image.h"
#include "ui/item_text_options.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/text/format_values.h"
#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/effects/ripple_animation.h"
#include "ui/effects/fireworks_animation.h"
#include "ui/toast/toast.h"
#include "ui/painter.h"
#include "ui/dynamic_image.h"
#include "ui/dynamic_thumbnails.h"
#include "history/view/media/history_view_media_common.h"
#include "data/data_media_types.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_poll.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "base/crc32hash.h"
#include "base/unixtime.h"
#include "base/timer.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "api/api_polls.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"


namespace HistoryView {
namespace {

constexpr auto kShowRecentVotersCount = 3;
constexpr auto kRotateSegments = 8;
constexpr auto kRotateAmplitude = 3.;
constexpr auto kScaleSegments = 2;
constexpr auto kScaleAmplitude = 0.03;
constexpr auto kRollDuration = crl::time(400);
constexpr auto kLargestRadialDuration = 30 * crl::time(1000);
constexpr auto kCriticalCloseDuration = 5 * crl::time(1000);

[[nodiscard]] int PollAnswerMediaSize() {
	return st::historyPollRadio.diameter * 2;
}

[[nodiscard]] int PollAnswerMediaSkip() {
	return st::historyPollPercentSkip * 2;
}

enum class PollThumbnailKind {
	None,
	Photo,
	Document,
	Emoji,
};

struct PercentCounterItem {
	int index = 0;
	int percent = 0;
	int remainder = 0;

	inline bool operator==(const PercentCounterItem &o) const {
		return remainder == o.remainder && percent == o.percent;
	}

	inline bool operator<(const PercentCounterItem &other) const {
		if (remainder > other.remainder) {
			return true;
		} else if (remainder < other.remainder) {
			return false;
		}
		return percent < other.percent;
	}
};

void AdjustPercentCount(gsl::span<PercentCounterItem> items, int left) {
	ranges::sort(items, std::less<>());
	for (auto i = 0, count = int(items.size()); i != count;) {
		const auto &item = items[i];
		auto j = i + 1;
		for (; j != count; ++j) {
			if (items[j].percent != item.percent
				|| items[j].remainder != item.remainder) {
				break;
			}
		}
		if (!items[i].remainder) {
			// If this item has correct value in 'percent' we don't want
			// to increment it to an incorrect one. This fixes a case with
			// four items with three votes for three different items.
			break;
		}
		const auto equal = j - i;
		if (equal <= left) {
			left -= equal;
			for (; i != j; ++i) {
				++items[i].percent;
			}
		} else {
			i = j;
		}
	}
}

void CountNicePercent(
		gsl::span<const int> votes,
		int total,
		gsl::span<int> result) {
	Expects(result.size() >= votes.size());
	Expects(votes.size() <= PollData::kMaxOptions);

	const auto count = size_type(votes.size());
	PercentCounterItem ItemsStorage[PollData::kMaxOptions];
	const auto items = gsl::make_span(ItemsStorage).subspan(0, count);
	auto left = 100;
	auto &&zipped = ranges::views::zip(
		votes,
		items,
		ranges::views::ints(0, int(items.size())));
	for (auto &&[votes, item, index] : zipped) {
		item.index = index;
		item.percent = (votes * 100) / total;
		item.remainder = (votes * 100) - (item.percent * total);
		left -= item.percent;
	}
	if (left > 0 && left <= count) {
		AdjustPercentCount(items, left);
	}
	for (const auto &item : items) {
		result[item.index] = item.percent;
	}
}

[[nodiscard]] uint32 HashPollShuffleValue(
		UserId userId,
		PollId pollId,
		const QByteArray &option) {
	auto hash = QByteArray::number(quint64(userId.bare))
		+ option
		+ QByteArray::number(quint64(pollId));
	return uint32(base::crc32(hash.constData(), hash.size()));
}

struct PollThumbnailData {
	std::shared_ptr<Ui::DynamicImage> thumbnail;
	ClickHandlerPtr handler;
	PollThumbnailKind kind = PollThumbnailKind::None;
	bool rounded = false;
	uint64 id = 0;
};

[[nodiscard]] PollThumbnailData MakePollThumbnail(
		not_null<PollData*> poll,
		const std::optional<MTPInputMedia> &media,
		Window::SessionController::MessageContext messageContext) {
	auto result = PollThumbnailData();
	if (!media) {
		return result;
	}
	media->match([&](const MTPDinputMediaPhoto &media) {
		media.vid().match([&](const MTPDinputPhoto &photo) {
			result.id = uint64(photo.vid().v);
			result.thumbnail = Ui::MakePhotoThumbnailCenterCrop(
				poll->owner().photo(result.id),
				messageContext.id);
			result.rounded = true;
			result.kind = PollThumbnailKind::Photo;
		}, [](const auto &) {
		});
	}, [&](const MTPDinputMediaDocument &media) {
		media.vid().match([&](const MTPDinputDocument &document) {
			result.id = uint64(document.vid().v);
			const auto parsed = poll->owner().document(result.id);
			if (parsed->sticker()) {
				result.thumbnail = Ui::MakeEmojiThumbnail(
					&poll->owner(),
					Data::SerializeCustomEmojiId(parsed));
				result.kind = PollThumbnailKind::Emoji;
			} else {
				result.thumbnail = Ui::MakeDocumentThumbnailCenterCrop(
					parsed,
					messageContext.id);
				result.rounded = true;
				result.kind = PollThumbnailKind::Document;
			}
		}, [](const auto &) {
		});
	}, [](const auto &) {
	});
	if (result.kind == PollThumbnailKind::Photo && result.id) {
		const auto photoId = PhotoId(result.id);
		const auto session = &poll->session();
		result.handler = std::make_shared<LambdaClickHandler>(
			[=](ClickContext context) {
				const auto my = context.other.value<ClickHandlerContext>();
				const auto controller = my.sessionWindow.get();
				if (!controller || (&controller->session() != session)) {
					return;
				}
				controller->openPhoto(
					poll->owner().photo(photoId),
					messageContext);
			});
	} else if (result.kind == PollThumbnailKind::Document && result.id) {
		const auto documentId = DocumentId(result.id);
		const auto session = &poll->session();
		result.handler = std::make_shared<LambdaClickHandler>(
			[=](ClickContext context) {
				const auto my = context.other.value<ClickHandlerContext>();
				const auto controller = my.sessionWindow.get();
				if (!controller || (&controller->session() != session)) {
					return;
				}
				controller->openDocument(
					poll->owner().document(documentId),
					true,
					messageContext);
			});
	} else if (result.kind != PollThumbnailKind::None) {
		result.handler = std::make_shared<LambdaClickHandler>([] {
		});
	}
	return result;
}

} // namespace

struct Poll::AnswerAnimation {
	anim::value percent;
	anim::value filling;
	anim::value opacity;
	bool chosen = false;
	bool correct = false;
};

struct Poll::AnswersAnimation {
	std::vector<AnswerAnimation> data;
	Ui::Animations::Simple progress;
};

struct Poll::SendingAnimation {
	template <typename Callback>
	SendingAnimation(
		const QByteArray &option,
		Callback &&callback);

	QByteArray option;
	Ui::InfiniteRadialAnimation animation;
};

struct Poll::Answer {
	Answer();

	void fillData(
		not_null<PollData*> poll,
		const PollAnswer &original,
		Ui::Text::MarkedContext context);
	void fillMedia(
		not_null<PollData*> poll,
		const PollAnswer &original,
		Window::SessionController::MessageContext messageContext,
		Fn<void()> repaint);

	Ui::Text::String text;
	QByteArray option;
	int votes = 0;
	int votesPercent = 0;
	int votesPercentWidth = 0;
	float64 filling = 0.;
	QString votesPercentString;
	bool chosen = false;
	bool correct = false;
	bool selected = false;
	ClickHandlerPtr handler;
	ClickHandlerPtr mediaHandler;
	Ui::Animations::Simple selectedAnimation;
	std::shared_ptr<Ui::DynamicImage> thumbnail;
	bool thumbnailRounded = false;
	PollThumbnailKind thumbnailKind = PollThumbnailKind::None;
	uint64 thumbnailId = 0;
	mutable std::unique_ptr<Ui::RippleAnimation> ripple;
};

struct Poll::AttachedMedia {
	ClickHandlerPtr handler;
	std::shared_ptr<Ui::DynamicImage> thumbnail;
	PollThumbnailKind kind = PollThumbnailKind::None;
	bool rounded = false;
	uint64 id = 0;
};

struct Poll::SolutionMedia {
	PollThumbnailKind kind = PollThumbnailKind::None;
	uint64 id = 0;
};

struct Poll::CloseInformation {
	CloseInformation(TimeId date, TimeId period, Fn<void()> repaint);

	crl::time start = 0;
	crl::time finish = 0;
	crl::time duration = 0;
	base::Timer timer;
	Ui::Animations::Basic radial;
};

struct Poll::RecentVoter {
	not_null<PeerData*> peer;
	mutable Ui::PeerUserpicView userpic;
};

template <typename Callback>
Poll::SendingAnimation::SendingAnimation(
	const QByteArray &option,
	Callback &&callback)
: option(option)
, animation(
	std::forward<Callback>(callback),
	st::historyPollRadialAnimation) {
}

Poll::Answer::Answer() : text(st::msgMinWidth / 2) {
}

void Poll::Answer::fillData(
		not_null<PollData*> poll,
		const PollAnswer &original,
		Ui::Text::MarkedContext context) {
	chosen = original.chosen;
	correct = poll->quiz() ? original.correct : chosen;
	if (!text.isEmpty() && text.toTextWithEntities() == original.text) {
		return;
	}
	text.setMarkedText(
		st::historyPollAnswerStyle,
		original.text,
		Ui::WebpageTextTitleOptions(),
		context);
}

void Poll::Answer::fillMedia(
		not_null<PollData*> poll,
		const PollAnswer &original,
		Window::SessionController::MessageContext messageContext,
		Fn<void()> repaint) {
	const auto updated = MakePollThumbnail(
		poll,
		original.media,
		messageContext);
	const auto same = (updated.kind == thumbnailKind)
		&& (updated.id == thumbnailId)
		&& (updated.rounded == thumbnailRounded);
	if (same) {
		return;
	}
	if (thumbnail) {
		thumbnail->subscribeToUpdates(nullptr);
	}
	thumbnail = updated.thumbnail;
	mediaHandler = updated.handler;
	thumbnailRounded = updated.rounded;
	thumbnailKind = updated.kind;
	thumbnailId = updated.id;
	if (thumbnail) {
		thumbnail->subscribeToUpdates(std::move(repaint));
	}
}

Poll::CloseInformation::CloseInformation(
	TimeId date,
	TimeId period,
	Fn<void()> repaint)
: duration(period * crl::time(1000))
, timer(std::move(repaint)) {
	const auto left = std::clamp(date - base::unixtime::now(), 0, period);
	finish = crl::now() + left * crl::time(1000);
}

Poll::Poll(
	not_null<Element*> parent,
	not_null<PollData*> poll,
	const TextWithEntities &consumed)
: Media(parent)
, _poll(poll)
, _description(st::msgMinWidth / 2)
, _question(st::msgMinWidth / 2)
, _attachedMedia(std::make_unique<AttachedMedia>())
, _showResultsLink(
	std::make_shared<LambdaClickHandler>(crl::guard(
		this,
		[=] { showResults(); })))
, _sendVotesLink(
	std::make_shared<LambdaClickHandler>(crl::guard(
		this,
		[=] { sendMultiOptions(); }))) {
	if (!consumed.text.isEmpty()) {
		updateDescription();
	}
	history()->owner().registerPollView(_poll, _parent);
}

QSize Poll::countOptimalSize() {
	updateTexts();

	const auto paddings = st::msgPadding.left() + st::msgPadding.right();

	auto maxWidth = st::msgFileMinWidth;
	accumulate_max(maxWidth, paddings + _description.maxWidth());
	accumulate_max(maxWidth, paddings + _question.maxWidth());
	for (const auto &answer : _answers) {
		const auto media = answer.thumbnail
			? (PollAnswerMediaSize() + PollAnswerMediaSkip())
			: 0;
		accumulate_max(
			maxWidth,
			paddings
			+ st::historyPollAnswerPadding.left()
			+ answer.text.maxWidth()
			+ media
			+ st::historyPollAnswerPadding.right());
	}

	const auto answersHeight = ranges::accumulate(ranges::views::all(
		_answers
	) | ranges::views::transform([](const Answer &answer) {
		const auto media = answer.thumbnail ? PollAnswerMediaSize() : 0;
		return st::historyPollAnswerPadding.top()
			+ std::max(answer.text.minHeight(), media)
			+ st::historyPollAnswerPadding.bottom();
	}), 0);

	const auto bottomButtonHeight = inlineFooter()
		? 0
		: st::historyPollBottomButtonSkip;
	auto minHeight = countQuestionTop(maxWidth - paddings)
		+ _question.minHeight()
		+ st::historyPollSubtitleSkip
		+ st::msgDateFont->height
		+ st::historyPollAnswersSkip
		+ answersHeight
		+ st::historyPollTotalVotesSkip
		+ bottomButtonHeight
		+ st::msgDateFont->height
		+ st::msgPadding.bottom();
	return { maxWidth, minHeight };
}

bool Poll::showVotes() const {
	if (_flags & PollData::Flag::HideResultsUntilClose) {
		return (_flags & PollData::Flag::Closed) || _parent->data()->out();
	}
	return _voted || (_flags & PollData::Flag::Closed);
}

bool Poll::canVote() const {
	return !showVotes() && _parent->data()->isRegular();
}

bool Poll::canSendVotes() const {
	return canVote() && _hasSelected;
}

bool Poll::showVotersCount() const {
	return showVotes()
		? (!_totalVotes || !(_flags & PollData::Flag::PublicVotes))
		: !(_flags & PollData::Flag::MultiChoice);
}

bool Poll::inlineFooter() const {
	return !(_flags
		& (PollData::Flag::PublicVotes | PollData::Flag::MultiChoice));
}

int Poll::countAnswerTop(
		const Answer &answer,
		int innerWidth) const {
	auto tshift = countQuestionTop(innerWidth)
		+ _question.countHeight(innerWidth)
		+ st::historyPollSubtitleSkip;
	tshift += st::msgDateFont->height + st::historyPollAnswersSkip;
	const auto i = ranges::find(
		_answers,
		&answer,
		[](const Answer &answer) { return &answer; });
	const auto countHeight = [&](const Answer &answer) {
		return countAnswerHeight(answer, innerWidth);
	};
	tshift += ranges::accumulate(
		begin(_answers),
		i,
		0,
		ranges::plus(),
		countHeight);
	return tshift;
}

int Poll::countAnswerContentWidth(
		const Answer &answer,
		int innerWidth) const {
	const auto answerWidth = innerWidth
		- st::historyPollAnswerPadding.left()
		- st::historyPollAnswerPadding.right();
	const auto mediaWidth = answer.thumbnail
		? (PollAnswerMediaSize() + PollAnswerMediaSkip())
		: 0;
	return std::max(1, answerWidth - mediaWidth);
}

int Poll::countAnswerHeight(
		const Answer &answer,
		int innerWidth) const {
	const auto media = answer.thumbnail ? PollAnswerMediaSize() : 0;
	const auto textWidth = countAnswerContentWidth(answer, innerWidth);
	return st::historyPollAnswerPadding.top()
		+ std::max(answer.text.countHeight(textWidth), media)
		+ st::historyPollAnswerPadding.bottom();
}

QSize Poll::countCurrentSize(int newWidth) {
	accumulate_min(newWidth, maxWidth());
	const auto innerWidth = newWidth
		- st::msgPadding.left()
		- st::msgPadding.right();

	const auto answersHeight = ranges::accumulate(ranges::views::all(
		_answers
	) | ranges::views::transform([&](const Answer &answer) {
		return countAnswerHeight(answer, innerWidth);
	}), 0);

	const auto bottomButtonHeight = inlineFooter()
		? 0
		: st::historyPollBottomButtonSkip;
	auto newHeight = countQuestionTop(innerWidth)
		+ _question.countHeight(innerWidth)
		+ st::historyPollSubtitleSkip
		+ st::msgDateFont->height
		+ st::historyPollAnswersSkip
		+ answersHeight
		+ st::historyPollTotalVotesSkip
		+ bottomButtonHeight
		+ st::msgDateFont->height
		+ st::msgPadding.bottom();
	return { newWidth, newHeight };
}

void Poll::updateTexts() {
	if (_pollVersion == _poll->version) {
		return;
	}
	const auto first = !_pollVersion;
	_pollVersion = _poll->version;

	const auto willStartAnimation = checkAnimationStart();
	const auto voted = _voted;

	updateDescription();
	if (_question.toTextWithEntities() != _poll->question) {
		auto options = Ui::WebpageTextTitleOptions();
		options.maxw = options.maxh = 0;
		_question.setMarkedText(
			st::historyPollQuestionStyle,
			_poll->question,
			options,
			Core::TextContext({
				.session = &_poll->session(),
				.repaint = [=] { repaint(); },
				.customEmojiLoopLimit = 2,
			}));
	}
	if (_flags != _poll->flags() || _subtitle.isEmpty()) {
		using Flag = PollData::Flag;
		_flags = _poll->flags();
		_subtitle.setText(
			st::msgDateTextStyle,
			((_flags & Flag::Closed)
				? tr::lng_polls_closed(tr::now)
				: (_flags & Flag::Quiz)
				? ((_flags & Flag::PublicVotes)
					? tr::lng_polls_public_quiz(tr::now)
					: tr::lng_polls_anonymous_quiz(tr::now))
				: ((_flags & Flag::PublicVotes)
					? tr::lng_polls_public(tr::now)
					: tr::lng_polls_anonymous(tr::now))));
	}
	updateRecentVoters();
	updateAnswers();
	updateAttachedMedia();
	updateSolutionText();
	updateSolutionMedia();
	updateVotes();

	if (willStartAnimation) {
		startAnswersAnimation();
		if (!voted) {
			checkQuizAnswered();
		}
	}
	solutionToggled(
		_solutionShown,
		first ? anim::type::instant : anim::type::normal);
}

void Poll::updateDescription() {
	const auto media = _parent->data()->media();
	const auto consumed = media
		? media->consumedMessageText()
		: TextWithEntities();
	if (consumed.text.isEmpty()) {
		_description = Ui::Text::String(st::msgMinWidth / 2);
		return;
	}
	if (_description.toTextWithEntities() == consumed) {
		return;
	}
	const auto context = Core::TextContext({
		.session = &_poll->session(),
		.repaint = [=] { _parent->customEmojiRepaint(); },
		.customEmojiLoopLimit = 2,
	});
	_description.setMarkedText(
		st::webPageDescriptionStyle,
		consumed,
		Ui::ItemTextOptions(_parent->data()),
		context);
}

void Poll::updateSolutionText() {
	if (_poll->solution.text.isEmpty()) {
		_solutionText = Ui::Text::String();
		return;
	}
	if (_solutionText.toTextWithEntities() == _poll->solution) {
		return;
	}
	_solutionText = Ui::Text::String(st::msgMinWidth);
	_solutionText.setMarkedText(
		st::webPageDescriptionStyle,
		_poll->solution,
		Ui::ItemTextOptions(_parent->data()),
		Core::TextContext({
			.session = &_poll->session(),
			.repaint = [=] { repaint(); },
		}));
}

void Poll::updateSolutionMedia() {
	const auto item = _parent->data();
	const auto messageContext = Window::SessionController::MessageContext{
		.id = item->fullId(),
		.topicRootId = item->topicRootId(),
		.monoforumPeerId = item->sublistPeerId(),
	};
	const auto updated = MakePollThumbnail(
		_poll,
		_poll->solutionMedia,
		messageContext);
	if (!updated.thumbnail) {
		_solutionMedia = nullptr;
		_solutionAttach = nullptr;
		return;
	}
	if (_solutionMedia
		&& _solutionMedia->kind == updated.kind
		&& _solutionMedia->id == updated.id) {
		return;
	}
	if (!_solutionMedia) {
		_solutionMedia = std::make_unique<SolutionMedia>();
	}
	_solutionMedia->kind = updated.kind;
	_solutionMedia->id = updated.id;
	auto photo = (PhotoData*)(nullptr);
	auto document = (DocumentData*)(nullptr);
	if (updated.kind == PollThumbnailKind::Photo && updated.id) {
		photo = _poll->owner().photo(PhotoId(updated.id));
	} else if (updated.kind == PollThumbnailKind::Document && updated.id) {
		document = _poll->owner().document(DocumentId(updated.id));
	}
	_solutionAttach = (photo || document)
		? CreateAttach(_parent, document, photo)
		: nullptr;
}

void Poll::updateAttachedMedia() {
	const auto item = _parent->data();
	const auto messageContext = Window::SessionController::MessageContext{
		.id = item->fullId(),
		.topicRootId = item->topicRootId(),
		.monoforumPeerId = item->sublistPeerId(),
	};
	const auto updated = MakePollThumbnail(
		_poll,
		_poll->attachedMedia,
		messageContext);
	const auto same = (_attachedMedia->kind == updated.kind)
		&& (_attachedMedia->id == updated.id)
		&& (_attachedMedia->rounded == updated.rounded);
	if (same) {
		return;
	}
	if (_attachedMedia->thumbnail) {
		_attachedMedia->thumbnail->subscribeToUpdates(nullptr);
	}
	_attachedMediaCache = QImage();
	_attachedMedia->thumbnail = updated.thumbnail;
	_attachedMedia->handler = updated.handler;
	_attachedMedia->kind = updated.kind;
	_attachedMedia->rounded = updated.rounded;
	_attachedMedia->id = updated.id;
	if (_attachedMedia->thumbnail) {
		_attachedMedia->thumbnail->subscribeToUpdates(
			crl::guard(this, [=] {
				_attachedMediaCache = QImage();
				repaint();
			}));
	}
}

int Poll::countTopContentSkip() const {
	return countTopMediaHeight()
		? st::historyPollMediaTopSkip
		: isBubbleTop()
		? st::historyPollQuestionTop
		: (st::historyPollQuestionTop - st::msgFileTopMinus);
}

int Poll::countTopMediaHeight() const {
	return (_attachedMedia && _attachedMedia->thumbnail)
		? st::historyPollMediaHeight
		: 0;
}

QRect Poll::countTopMediaRect(int top) const {
	const auto sideSkip = st::historyPollMediaSideSkip;
	const auto mediaHeight = countTopMediaHeight();
	return mediaHeight
		? QRect(
			sideSkip,
			top,
			std::max(1, width() - 2 * sideSkip),
			mediaHeight)
		: QRect();
}

Ui::BubbleRounding Poll::topMediaRounding() const {
	using Corner = Ui::BubbleCornerRounding;
	auto result = adjustedBubbleRounding(
		RectPart::BottomLeft | RectPart::BottomRight);
	const auto normalize = [](Corner value) {
		return (value == Corner::Large)
			? Corner::Large
			: (value == Corner::None)
			? Corner::None
			: Corner::Small;
	};
	result.topLeft = normalize(result.topLeft);
	result.topRight = normalize(result.topRight);
	result.bottomLeft = Corner::Small;
	result.bottomRight = Corner::Small;
	return result;
}

void Poll::validateTopMediaCache(QSize size) const {
	if (!_attachedMedia || !_attachedMedia->thumbnail || size.isEmpty()) {
		return;
	}
	const auto ratio = style::DevicePixelRatio();
	const auto rounding = topMediaRounding();
	if ((_attachedMediaCache.size() == (size * ratio))
		&& (_attachedMediaCacheRounding == rounding)) {
		return;
	}
	const auto source = _attachedMedia->thumbnail->image(
		std::max(size.width(), size.height()) * ratio);
	if (source.isNull()) {
		return;
	}
	auto prepared = Images::Prepare(
		source,
		size * ratio,
		{ .outer = size });
	prepared = Images::Round(
		std::move(prepared),
		MediaRoundingMask(rounding));
	prepared.setDevicePixelRatio(ratio);
	_attachedMediaCache = std::move(prepared);
	_attachedMediaCacheRounding = rounding;
}

int Poll::countDescriptionHeight(int innerWidth) const {
	return _description.isEmpty() ? 0 : _description.countHeight(innerWidth);
}

int Poll::countSolutionMediaHeight(int mediaWidth) const {
	if (!_solutionAttach) {
		return 0;
	}
	_solutionAttach->initDimensions();
	return _solutionAttach->resizeGetHeight(mediaWidth);
}

int Poll::countSolutionBlockHeight(int innerWidth) const {
	if (!_solutionShown || !canShowSolution()) {
		return 0;
	}
	const auto &qst = st::historyPagePreview;
	const auto textWidth = innerWidth
		- qst.padding.left()
		- qst.padding.right();
	auto height = qst.padding.top();
	height += st::semiboldFont->height;
	height += st::historyPollExplanationTitleSkip;
	height += _solutionText.countHeight(textWidth);
	if (const auto mediaHeight = countSolutionMediaHeight(textWidth)) {
		height += st::historyPollExplanationMediaSkip + mediaHeight;
	}
	height += qst.padding.bottom();
	return height;
}

int Poll::countQuestionTop(int innerWidth) const {
	auto result = countTopContentSkip();
	if (const auto mediaHeight = countTopMediaHeight()) {
		result += mediaHeight + st::historyPollMediaSkip;
	}
	if (const auto descriptionHeight = countDescriptionHeight(innerWidth)) {
		result += descriptionHeight + st::historyPollDescriptionSkip;
	}
	if (const auto solutionHeight = countSolutionBlockHeight(innerWidth)) {
		result += solutionHeight + st::historyPollExplanationSkip;
	}
	return result;
}

TextSelection Poll::toQuestionSelection(TextSelection selection) const {
	return UnshiftItemSelection(selection, _description);
}

TextSelection Poll::fromQuestionSelection(TextSelection selection) const {
	return ShiftItemSelection(selection, _description);
}

void Poll::checkQuizAnswered() {
	if (!_voted || !_votedFromHere || !_poll->quiz() || anim::Disabled()) {
		return;
	}
	const auto i = ranges::find(_answers, true, &Answer::chosen);
	if (i == end(_answers)) {
		return;
	}
	if (i->correct) {
		_fireworksAnimation = std::make_unique<Ui::FireworksAnimation>(
			[=] { repaint(); });
	} else {
		_wrongAnswerAnimation.start(
			[=] { repaint(); },
			0.,
			1.,
			kRollDuration,
			anim::linear);
		showSolution();
	}
}

void Poll::showSolution() const {
	if (!_poll->solution.text.isEmpty()) {
		solutionToggled(true);
	}
}

void Poll::solutionToggled(
		bool solutionShown,
		anim::type animated) const {
	_solutionShown = solutionShown;
	const auto visible = canShowSolution() && !_solutionShown;
	if (_solutionButtonVisible == visible) {
		if (animated == anim::type::instant
			&& _solutionButtonAnimation.animating()) {
			_solutionButtonAnimation.stop();
			repaint();
		}
		return;
	}
	_solutionButtonVisible = visible;
	if (animated == anim::type::instant) {
		_solutionButtonAnimation.stop();
	} else {
		_solutionButtonAnimation.start(
			[=] { repaint(); },
			visible ? 0. : 1.,
			visible ? 1. : 0.,
			st::fadeWrapDuration);
	}
	history()->owner().requestViewResize(_parent);
}

void Poll::updateRecentVoters() {
	auto &&sliced = ranges::views::all(
		_poll->recentVoters
	) | ranges::views::take(kShowRecentVotersCount);
	const auto changed = !ranges::equal(
		_recentVoters,
		sliced,
		ranges::equal_to(),
		&RecentVoter::peer);
	if (changed) {
		auto updated = ranges::views::all(
			sliced
		) | ranges::views::transform([](not_null<PeerData*> peer) {
			return RecentVoter{ peer };
		}) | ranges::to_vector;
		const auto has = hasHeavyPart();
		if (has) {
			for (auto &voter : updated) {
				const auto i = ranges::find(
					_recentVoters,
					voter.peer,
					&RecentVoter::peer);
				if (i != end(_recentVoters)) {
					voter.userpic = std::move(i->userpic);
				}
			}
		}
		_recentVoters = std::move(updated);
		if (has && !hasHeavyPart()) {
			_parent->checkHeavyPart();
		}
	}
}

void Poll::updateAnswers() {
	const auto context = Core::TextContext({
		.session = &_poll->session(),
		.repaint = [=] { repaint(); },
		.customEmojiLoopLimit = 2,
	});
	const auto repaintThumbnail = crl::guard(this, [=] { repaint(); });
	const auto item = _parent->data();
	const auto messageContext = Window::SessionController::MessageContext{
		.id = item->fullId(),
		.topicRootId = item->topicRootId(),
		.monoforumPeerId = item->sublistPeerId(),
	};
	auto options = ranges::views::all(
		_poll->answers
	) | ranges::views::transform(&PollAnswer::option) | ranges::to_vector;
	if (_flags & PollData::Flag::ShuffleAnswers) {
		const auto userId = _poll->session().userId();
		const auto pollId = _poll->id;
		ranges::sort(options, [&](const QByteArray &a, const QByteArray &b) {
			const auto hashA = HashPollShuffleValue(userId, pollId, a);
			const auto hashB = HashPollShuffleValue(userId, pollId, b);
			return (hashA == hashB) ? (a < b) : (hashA < hashB);
		});
	}
	const auto changed = (_answers.size() != options.size())
		|| !ranges::equal(
			_answers,
			options,
			ranges::equal_to(),
			&Answer::option);
	if (!changed) {
		for (auto &answer : _answers) {
			const auto i = ranges::find(
				_poll->answers,
				answer.option,
				&PollAnswer::option);
			Assert(i != end(_poll->answers));
			answer.fillData(_poll, *i, context);
			answer.fillMedia(_poll, *i, messageContext, repaintThumbnail);
		}
		return;
	}
	_answers = ranges::views::all(options) | ranges::views::transform([&](
			const QByteArray &option) {
		auto result = Answer();
		result.option = option;
		const auto i = ranges::find(
			_poll->answers,
			option,
			&PollAnswer::option);
		Assert(i != end(_poll->answers));
		result.fillData(_poll, *i, context);
		result.fillMedia(_poll, *i, messageContext, repaintThumbnail);
		return result;
	}) | ranges::to_vector;

	if (_flags & PollData::Flag::ShuffleAnswers) {
		const auto visitorId = _poll->session().userId();
		const auto pollId = _poll->id;
		ranges::sort(_answers, [&](const Answer &a, const Answer &b) {
			return HashPollShuffleValue(visitorId, pollId, a.option)
				< HashPollShuffleValue(visitorId, pollId, b.option);
		});
	}

	for (auto &answer : _answers) {
		answer.handler = createAnswerClickHandler(answer);
	}

	resetAnswersAnimation();
}

ClickHandlerPtr Poll::createAnswerClickHandler(
		const Answer &answer) {
	const auto option = answer.option;
	if (_flags & PollData::Flag::MultiChoice) {
		return std::make_shared<LambdaClickHandler>(crl::guard(this, [=] {
			toggleMultiOption(option);
		}));
	}
	return std::make_shared<LambdaClickHandler>(crl::guard(this, [=] {
		_votedFromHere = true;
		history()->session().api().polls().sendVotes(
			_parent->data()->fullId(),
			{ option });
	}));
}

void Poll::toggleMultiOption(const QByteArray &option) {
	const auto i = ranges::find(
		_answers,
		option,
		&Answer::option);
	if (i != end(_answers)) {
		const auto selected = i->selected;
		i->selected = !selected;
		i->selectedAnimation.start(
			[=] { repaint(); },
			selected ? 1. : 0.,
			selected ? 0. : 1.,
			st::defaultCheck.duration);
		if (selected) {
			const auto j = ranges::find(
				_answers,
				true,
				&Answer::selected);
			_hasSelected = (j != end(_answers));
		} else {
			_hasSelected = true;
		}
		repaint();
	}
}

void Poll::sendMultiOptions() {
	auto chosen = _answers | ranges::views::filter(
		&Answer::selected
	) | ranges::views::transform(
		&Answer::option
	) | ranges::to_vector;
	if (!chosen.empty()) {
		_votedFromHere = true;
		history()->session().api().polls().sendVotes(
			_parent->data()->fullId(),
			std::move(chosen));
	}
}

void Poll::showResults() {
	_parent->delegate()->elementShowPollResults(
		_poll,
		_parent->data()->fullId());
}

void Poll::updateVotes() {
	const auto voted = _poll->voted();
	if (_voted != voted) {
		_voted = voted;
		if (_voted) {
			for (auto &answer : _answers) {
				answer.selected = false;
			}
		} else {
			_votedFromHere = false;
		}
	}
	updateAnswerVotes();
	updateTotalVotes();
}

void Poll::checkSendingAnimation() const {
	const auto &sending = _poll->sendingVotes;
	const auto sendingRadial = (sending.size() == 1)
		&& !(_flags & PollData::Flag::MultiChoice);
	if (sendingRadial == (_sendingAnimation != nullptr)) {
		if (_sendingAnimation) {
			_sendingAnimation->option = sending.front();
		}
		return;
	}
	if (!sendingRadial) {
		if (!_answersAnimation) {
			_sendingAnimation = nullptr;
		}
		return;
	}
	_sendingAnimation = std::make_unique<SendingAnimation>(
		sending.front(),
		[=] { radialAnimationCallback(); });
	_sendingAnimation->animation.start();
}

void Poll::updateTotalVotes() {
	if (_totalVotes == _poll->totalVoters && !_totalVotesLabel.isEmpty()) {
		return;
	}
	_totalVotes = _poll->totalVoters;
	const auto quiz = _poll->quiz();
	const auto string = !_totalVotes
		? (quiz
			? tr::lng_polls_answers_none
			: tr::lng_polls_votes_none)(tr::now)
		: (quiz
			? tr::lng_polls_answers_count
			: tr::lng_polls_votes_count)(
				tr::now,
				lt_count_short,
				_totalVotes);
	_totalVotesLabel.setText(st::msgDateTextStyle, string);
}

void Poll::updateAnswerVotesFromOriginal(
		Answer &answer,
		const PollAnswer &original,
		int percent,
		int maxVotes) {
	if (!showVotes()) {
		answer.votesPercent = 0;
		answer.votesPercentString.clear();
		answer.votesPercentWidth = 0;
	} else if (answer.votesPercentString.isEmpty()
		|| answer.votesPercent != percent) {
		answer.votesPercent = percent;
		answer.votesPercentString = QString::number(percent) + '%';
		answer.votesPercentWidth = st::historyPollPercentFont->width(
			answer.votesPercentString);
	}
	answer.votes = original.votes;
	answer.filling = answer.votes / float64(maxVotes);
}

void Poll::updateAnswerVotes() {
	if (_poll->answers.size() != _answers.size()
		|| _poll->answers.empty()) {
		return;
	}
	const auto totalVotes = std::max(1, _poll->totalVoters);
	const auto maxVotes = std::max(1, ranges::max_element(
		_poll->answers,
		ranges::less(),
		&PollAnswer::votes)->votes);

	constexpr auto kMaxCount = PollData::kMaxOptions;
	const auto count = size_type(_poll->answers.size());
	Assert(count <= kMaxCount);
	int PercentsStorage[kMaxCount] = { 0 };
	int VotesStorage[kMaxCount] = { 0 };

	ranges::copy(
		ranges::views::all(
			_poll->answers
		) | ranges::views::transform(&PollAnswer::votes),
		ranges::begin(VotesStorage));

	CountNicePercent(
		gsl::make_span(VotesStorage).subspan(0, count),
		totalVotes,
		gsl::make_span(PercentsStorage).subspan(0, count));

	for (auto &answer : _answers) {
		const auto i = ranges::find(
			_poll->answers,
			answer.option,
			&PollAnswer::option);
		Assert(i != end(_poll->answers));
		const auto index = int(i - begin(_poll->answers));
		updateAnswerVotesFromOriginal(
			answer,
			*i,
			PercentsStorage[index],
			maxVotes);
	}
}

void Poll::draw(Painter &p, const PaintContext &context) const {
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) return;
	auto paintw = width();

	checkSendingAnimation();
	if (_poll->checkResultsReload(context.now)) {
		history()->session().api().polls().reloadResults(_parent->data());
	}

	const auto stm = context.messageStyle();
	const auto padding = st::msgPadding;
	auto tshift = countTopContentSkip();
	paintw -= padding.left() + padding.right();

	if (const auto mediaHeight = countTopMediaHeight()) {
		const auto target = countTopMediaRect(tshift);
		p.setPen(Qt::NoPen);
		p.setBrush(stm->msgFileBg);
		PainterHighQualityEnabler hq(p);
		if (_attachedMedia->kind == PollThumbnailKind::Emoji) {
			p.drawRoundedRect(
				target,
				st::roundRadiusLarge,
				st::roundRadiusLarge);
			const auto image = _attachedMedia->thumbnail->image(
				std::max(target.width(), target.height()));
			if (!image.isNull()) {
				const auto source = QRectF(QPointF(), QSizeF(image.size()));
				const auto kx = target.width() / source.width();
				const auto ky = target.height() / source.height();
				const auto scale = std::min(kx, ky);
				const auto imageSize = QSizeF(
					source.width() * scale,
					source.height() * scale);
				const auto geometry = QRectF(
					target.x() + (target.width() - imageSize.width()) / 2.,
					target.y() + (target.height() - imageSize.height()) / 2.,
					imageSize.width(),
					imageSize.height());
				p.save();
				auto path = QPainterPath();
				path.addRoundedRect(
					target,
					st::roundRadiusLarge,
					st::roundRadiusLarge);
				p.setClipPath(path);
				p.drawImage(geometry, image, source);
				p.restore();
			}
		} else {
			validateTopMediaCache(target.size());
			if (!_attachedMediaCache.isNull()) {
				p.drawImage(target.topLeft(), _attachedMediaCache);
			}
		}
		tshift += mediaHeight + st::historyPollMediaSkip;
	}

	if (const auto descriptionHeight = countDescriptionHeight(paintw)) {
		p.setPen(stm->historyTextFg);
		_parent->prepareCustomEmojiPaint(p, context, _description);
		_description.draw(p, {
			.position = { padding.left(), tshift },
			.outerWidth = width(),
			.availableWidth = paintw,
			.spoiler = Ui::Text::DefaultSpoilerCache(),
			.now = context.now,
			.pausedEmoji = context.paused,
			.pausedSpoiler = context.paused,
			.selection = context.selection,
			.useFullWidth = true,
		});
		tshift += descriptionHeight + st::historyPollDescriptionSkip;
	}

	if (const auto solutionHeight = countSolutionBlockHeight(paintw)) {
		paintSolutionBlock(p, padding.left(), tshift, paintw, context);
		tshift += solutionHeight + st::historyPollExplanationSkip;
	}

	p.setPen(stm->historyTextFg);
	_question.drawLeft(
		p,
		padding.left(),
		tshift,
		paintw,
		width(),
		style::al_left,
		0,
		-1,
		toQuestionSelection(context.selection));
	tshift += _question.countHeight(paintw) + st::historyPollSubtitleSkip;

	p.setPen(stm->msgDateFg);
	_subtitle.drawLeftElided(p, padding.left(), tshift, paintw, width());
	paintRecentVoters(p, padding.left() + _subtitle.maxWidth(), tshift, context);
	paintCloseByTimer(p, padding.left() + paintw, tshift, context);
	paintShowSolution(p, padding.left() + paintw, tshift, context);
	tshift += st::msgDateFont->height + st::historyPollAnswersSkip;

	const auto progress = _answersAnimation
		? _answersAnimation->progress.value(1.)
		: 1.;
	if (progress == 1.) {
		resetAnswersAnimation();
	}

	auto &&answers = ranges::views::zip(
		_answers,
		ranges::views::ints(0, int(_answers.size())));
	for (const auto &[answer, index] : answers) {
		const auto animation = _answersAnimation
			? &_answersAnimation->data[index]
			: nullptr;
		if (animation) {
			animation->percent.update(progress, anim::linear);
			animation->filling.update(
				progress,
				showVotes() ? anim::easeOutCirc : anim::linear);
			animation->opacity.update(progress, anim::linear);
		}
		const auto height = paintAnswer(
			p,
			answer,
			animation,
			padding.left(),
			tshift,
			paintw,
			width(),
			context);
		tshift += height;
	}
	if (!inlineFooter()) {
		paintBottom(p, padding.left(), tshift, paintw, context);
	} else if (!_totalVotesLabel.isEmpty()) {
		tshift += st::msgPadding.bottom();
		paintInlineFooter(p, padding.left(), tshift, paintw, context);
	}
}

void Poll::paintInlineFooter(
		Painter &p,
		int left,
		int top,
		int paintw,
		const PaintContext &context) const {
	const auto stm = context.messageStyle();
	p.setPen(stm->msgDateFg);
	_totalVotesLabel.drawLeftElided(
		p,
		left,
		top,
		_parent->data()->reactions().empty()
			? std::min(
				_totalVotesLabel.maxWidth(),
				paintw - _parent->bottomInfoFirstLineWidth())
			: _totalVotesLabel.maxWidth(),
		width());
}

void Poll::paintBottom(
		Painter &p,
		int left,
		int top,
		int paintw,
		const PaintContext &context) const {
	const auto stringtop = top
		+ st::msgPadding.bottom()
		+ st::historyPollBottomButtonTop;
	const auto stm = context.messageStyle();
	if (showVotersCount()) {
		p.setPen(stm->msgDateFg);
		_totalVotesLabel.draw(p, left, stringtop, paintw, style::al_top);
	} else {
		const auto link = showVotes()
			? _showResultsLink
			: canSendVotes()
			? _sendVotesLink
			: nullptr;
		if (_linkRipple) {
			const auto linkHeight = bottomButtonHeight();
			p.setOpacity(st::historyPollRippleOpacity);
			_linkRipple->paint(
				p,
				left - st::msgPadding.left() - _linkRippleShift,
				height() - linkHeight,
				width(),
				&stm->msgWaveformInactive->c);
			if (_linkRipple->empty()) {
				_linkRipple.reset();
			}
			p.setOpacity(1.);
		}
		p.setFont(st::semiboldFont);
		p.setPen(link ? stm->msgFileThumbLinkFg : stm->msgDateFg);
		const auto string = showVotes()
			? tr::lng_polls_view_results(tr::now, tr::upper)
			: tr::lng_polls_submit_votes(tr::now, tr::upper);
		const auto stringw = st::semiboldFont->width(string);
		p.drawTextLeft(
			left + (paintw - stringw) / 2,
			stringtop,
			width(),
			string,
			stringw);
	}
}

void Poll::resetAnswersAnimation() const {
	_answersAnimation = nullptr;
	if (_poll->sendingVotes.size() != 1
		|| (_flags & PollData::Flag::MultiChoice)) {
		_sendingAnimation = nullptr;
	}
}

void Poll::radialAnimationCallback() const {
	if (!anim::Disabled()) {
		repaint();
	}
}

void Poll::paintRecentVoters(
		Painter &p,
		int left,
		int top,
		const PaintContext &context) const {
	const auto count = int(_recentVoters.size());
	if (!count) {
		return;
	}
	auto x = left
		+ st::historyPollRecentVotersSkip
		+ (count - 1) * st::historyPollRecentVoterSkip;
	auto y = top;
	const auto size = st::historyPollRecentVoterSize;
	const auto stm = context.messageStyle();
	auto pen = stm->msgBg->p;
	pen.setWidth(st::lineWidth);

	auto created = false;
	for (const auto &recent : ranges::views::reverse(_recentVoters)) {
		const auto was = !recent.userpic.null();
		recent.peer->paintUserpic(p, recent.userpic, x, y, size);
		if (!was && !recent.userpic.null()) {
			created = true;
		}
		const auto paintContent = [&](QPainter &p) {
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(x, y, size, size);
		};
		if (usesBubblePattern(context)) {
			const auto add = st::lineWidth * 2;
			const auto target = QRect(x, y, size, size).marginsAdded(
				{ add, add, add, add });
			Ui::PaintPatternBubblePart(
				p,
				context.viewport,
				context.bubblesPattern->pixmap,
				target,
				paintContent,
				_userpicCircleCache);
		} else {
			paintContent(p);
		}
		x -= st::historyPollRecentVoterSkip;
	}
	if (created) {
		history()->owner().registerHeavyViewPart(_parent);
	}
}

void Poll::paintCloseByTimer(
		Painter &p,
		int right,
		int top,
		const PaintContext &context) const {
	if (!canVote() || _poll->closeDate <= 0 || _poll->closePeriod <= 0) {
		_close = nullptr;
		return;
	}
	if (!_close) {
		_close = std::make_unique<CloseInformation>(
			_poll->closeDate,
			_poll->closePeriod,
			[=] { repaint(); });
	}
	const auto now = crl::now();
	const auto left = std::max(_close->finish - now, crl::time(0));
	const auto radial = std::min(_close->duration, kLargestRadialDuration);
	if (!left) {
		_close->radial.stop();
	} else if (left < radial && !anim::Disabled()) {
		if (!_close->radial.animating()) {
			_close->radial.init([=] {
				repaint();
			});
			_close->radial.start();
		}
	} else {
		_close->radial.stop();
	}
	const auto time = Ui::FormatDurationText(int(std::ceil(left / 1000.)));
	const auto st = context.st;
	const auto stm = context.messageStyle();
	const auto &icon = stm->historyQuizTimer;
	const auto x = right - icon.width();
	const auto y = top
		+ (st::normalFont->height - icon.height()) / 2
		- st::lineWidth;
	const auto &regular = (left < kCriticalCloseDuration)
		? st->boxTextFgError()
		: stm->msgDateFg;
	p.setPen(regular);
	const auto timeWidth = st::normalFont->width(time);
	p.drawTextLeft(x - timeWidth, top, width(), time, timeWidth);
	if (left < radial) {
		auto hq = PainterHighQualityEnabler(p);
		const auto part = std::max(
			left / float64(radial),
			1. / arc::kFullLength);
		const auto length = int(base::SafeRound(arc::kFullLength * part));
		auto pen = regular->p;
		pen.setWidth(st::historyPollRadio.thickness);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		const auto size = icon.width() / 2;
		const auto left = (x + (icon.width() - size) / 2);
		const auto top = (y + (icon.height() - size) / 2) + st::lineWidth;
		p.drawArc(left, top, size, size, (arc::kFullLength / 4), length);
	} else {
		icon.paint(p, x, y, width());
	}

	if (left > (anim::Disabled() ? 0 : (radial - 1))) {
		const auto next = (left % 1000);
		_close->timer.callOnce((next ? next : 1000) + 1);
	}
}

void Poll::paintShowSolution(
		Painter &p,
		int right,
		int top,
		const PaintContext &context) const {
	const auto shown = _solutionButtonAnimation.value(
		_solutionButtonVisible ? 1. : 0.);
	if (!shown) {
		return;
	}
	if (!_showSolutionLink) {
		_showSolutionLink = std::make_shared<LambdaClickHandler>(
			crl::guard(this, [=] { showSolution(); }));
	}
	const auto stm = context.messageStyle();
	const auto &icon = stm->historyQuizExplain;
	const auto x = right - icon.width();
	const auto y = top + (st::normalFont->height - icon.height()) / 2;
	if (shown == 1.) {
		icon.paint(p, x, y, width());
	} else {
		p.save();
		p.translate(x + icon.width() / 2, y + icon.height() / 2);
		p.scale(shown, shown);
		p.setOpacity(shown);
		icon.paint(p, -icon.width() / 2, -icon.height() / 2, width());
		p.restore();
	}
}

void Poll::paintSolutionBlock(
		Painter &p,
		int left,
		int top,
		int paintw,
		const PaintContext &context) const {
	if (!_solutionShown || !canShowSolution()) {
		return;
	}
	if (!_closeSolutionLink) {
		_closeSolutionLink = std::make_shared<LambdaClickHandler>(
			crl::guard(this, [=] { solutionToggled(false); }));
	}

	const auto &qst = st::historyPagePreview;
	const auto blockHeight = countSolutionBlockHeight(paintw);
	const auto outer = QRect(left, top, paintw, blockHeight);

	const auto stm = context.messageStyle();
	const auto view = _parent;
	const auto selected = context.selected();
	const auto colorIndex = view->contentColorIndex();
	const auto &chatSt = *context.st;
	const auto colorPattern = chatSt.colorPatternIndex(colorIndex);
	const auto useColorIndex = !context.outbg;
	const auto cache = useColorIndex
		? chatSt.coloredReplyCache(selected, colorIndex).get()
		: stm->replyCache[colorPattern].get();

	Ui::Text::ValidateQuotePaintCache(*cache, qst);
	Ui::Text::FillQuotePaint(p, outer, *cache, qst);

	const auto innerLeft = left + qst.padding.left();
	const auto innerRight = left + paintw - qst.padding.right();
	const auto textWidth = innerRight - innerLeft;
	auto yshift = top + qst.padding.top();

	p.setPen(cache->outlines[0]);
	p.setFont(st::semiboldFont);
	const auto closeArea = st::historyPollExplanationCloseSize;
	p.drawTextLeft(
		innerLeft,
		yshift,
		width(),
		tr::lng_polls_solution_title(tr::now),
		textWidth - closeArea);

	{
		const auto iconSize = st::historyPollExplanationCloseIconSize;
		const auto centerX = innerRight - closeArea / 2;
		const auto centerY = yshift + st::semiboldFont->height / 2;
		const auto half = iconSize / 2;
		auto pen = QPen(cache->outlines[0]);
		pen.setWidthF(st::historyPollExplanationCloseStroke * 1.);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.setRenderHint(QPainter::Antialiasing);
		p.drawLine(
			centerX - half, centerY - half,
			centerX + half, centerY + half);
		p.drawLine(
			centerX + half, centerY - half,
			centerX - half, centerY + half);
		p.setRenderHint(QPainter::Antialiasing, false);
	}

	yshift += st::semiboldFont->height + st::historyPollExplanationTitleSkip;

	p.setPen(stm->historyTextFg);
	_solutionText.draw(p, {
		.position = { innerLeft, yshift },
		.outerWidth = width(),
		.availableWidth = textWidth,
		.now = context.now,
		.pausedEmoji = context.paused,
		.pausedSpoiler = context.paused,
	});

	if (countSolutionMediaHeight(textWidth)) {
		yshift += _solutionText.countHeight(textWidth)
			+ st::historyPollExplanationMediaSkip;
		const auto shift = st::msgFileLayout.padding.left();
		const auto attachLeft = rtl()
			? (width() - innerLeft + shift - _solutionAttach->width())
			: (innerLeft - shift);
		p.translate(attachLeft, yshift);
		_solutionAttach->draw(
			p,
			context.translated(-attachLeft, -yshift)
				.withSelection(TextSelection()));
		p.translate(-attachLeft, -yshift);
	}
}

int Poll::paintAnswer(
		Painter &p,
		const Answer &answer,
		const AnswerAnimation *animation,
		int left,
		int top,
		int width,
		int outerWidth,
		const PaintContext &context) const {
	const auto height = countAnswerHeight(answer, width);
	const auto stm = context.messageStyle();
	const auto aleft = left + st::historyPollAnswerPadding.left();
	const auto awidth = width
		- st::historyPollAnswerPadding.left()
		- st::historyPollAnswerPadding.right();
	const auto media = answer.thumbnail ? PollAnswerMediaSize() : 0;
	const auto textWidth = countAnswerContentWidth(answer, width);

	if (answer.ripple) {
		p.setOpacity(st::historyPollRippleOpacity);
		answer.ripple->paint(
			p,
			left - st::msgPadding.left(),
			top,
			outerWidth,
			&stm->msgWaveformInactive->c);
		if (answer.ripple->empty()) {
			answer.ripple.reset();
		}
		p.setOpacity(1.);
	}

	if (animation) {
		const auto opacity = animation->opacity.current();
		if (opacity < 1.) {
			p.setOpacity(1. - opacity);
			paintRadio(p, answer, left, top, context);
		}
		if (opacity > 0.) {
			const auto percent = QString::number(
				int(base::SafeRound(animation->percent.current()))) + '%';
			const auto percentWidth = st::historyPollPercentFont->width(
				percent);
			p.setOpacity(opacity);
			paintPercent(
				p,
				percent,
				percentWidth,
				left,
				top,
				outerWidth,
				context);
			p.setOpacity(sqrt(opacity));
			paintFilling(
				p,
				animation->chosen,
				animation->correct,
				animation->filling.current(),
				left,
				top,
				width,
				textWidth,
				height,
				context);
			p.setOpacity(1.);
		}
	} else if (!showVotes()) {
		paintRadio(p, answer, left, top, context);
	} else {
		paintPercent(
			p,
			answer.votesPercentString,
			answer.votesPercentWidth,
			left,
			top,
			outerWidth,
			context);
		paintFilling(
			p,
			answer.chosen,
			answer.correct,
			answer.filling,
			left,
			top,
			width,
			textWidth,
			height,
			context);
	}

	top += st::historyPollAnswerPadding.top();
	if (answer.thumbnail) {
		const auto target = QRect(
			aleft + awidth - media,
			top,
			media,
			media);
		if (!target.isEmpty()) {
			const auto image = answer.thumbnail->image(media);
			if (!image.isNull()) {
				const auto source = QRectF(QPointF(), QSizeF(image.size()));
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
				if (answer.thumbnailRounded) {
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
		}
	}
	p.setPen(stm->historyTextFg);
	answer.text.drawLeft(p, aleft, top, textWidth, outerWidth);

	return height;
}

void Poll::paintRadio(
		Painter &p,
		const Answer &answer,
		int left,
		int top,
		const PaintContext &context) const {
	top += st::historyPollAnswerPadding.top();

	const auto stm = context.messageStyle();

	PainterHighQualityEnabler hq(p);
	const auto &radio = st::historyPollRadio;
	const auto over = ClickHandler::showAsActive(answer.handler);
	const auto &regular = stm->msgDateFg;

	const auto checkmark = answer.selectedAnimation.value(answer.selected ? 1. : 0.);

	const auto o = p.opacity();
	if (checkmark < 1.) {
		p.setBrush(Qt::NoBrush);
		p.setOpacity(o * (over ? st::historyPollRadioOpacityOver : st::historyPollRadioOpacity));
	}

	const auto rect = QRectF(left, top, radio.diameter, radio.diameter).marginsRemoved(QMarginsF(radio.thickness / 2., radio.thickness / 2., radio.thickness / 2., radio.thickness / 2.));
	if (_sendingAnimation && _sendingAnimation->option == answer.option) {
		const auto &active = stm->msgServiceFg;
		if (anim::Disabled()) {
			anim::DrawStaticLoading(p, rect, radio.thickness, active);
		} else {
			const auto state = _sendingAnimation->animation.computeState();
			auto pen = anim::pen(regular, active, state.shown);
			pen.setWidth(radio.thickness);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			p.drawArc(
				rect,
				state.arcFrom,
				state.arcLength);
		}
	} else {
		if (checkmark < 1.) {
			auto pen = regular->p;
			pen.setWidth(radio.thickness);
			p.setPen(pen);
			p.drawEllipse(rect);
		}
		if (checkmark > 0.) {
			const auto removeFull = (radio.diameter / 2 - radio.thickness);
			const auto removeNow = removeFull * (1. - checkmark);
			const auto color = stm->msgFileThumbLinkFg;
			auto pen = color->p;
			pen.setWidth(radio.thickness);
			p.setPen(pen);
			p.setBrush(color);
			p.drawEllipse(rect.marginsRemoved({ removeNow, removeNow, removeNow, removeNow }));
			const auto &icon = stm->historyPollChosen;
			icon.paint(p, left + (radio.diameter - icon.width()) / 2, top + (radio.diameter - icon.height()) / 2, width());
		}
	}

	p.setOpacity(o);
}

void Poll::paintPercent(
		Painter &p,
		const QString &percent,
		int percentWidth,
		int left,
		int top,
		int outerWidth,
		const PaintContext &context) const {
	const auto stm = context.messageStyle();
	const auto aleft = left + st::historyPollAnswerPadding.left();

	top += st::historyPollAnswerPadding.top();

	p.setFont(st::historyPollPercentFont);
	p.setPen(stm->historyTextFg);
	const auto pleft = aleft - percentWidth - st::historyPollPercentSkip;
	p.drawTextLeft(pleft, top + st::historyPollPercentTop, outerWidth, percent, percentWidth);
}

void Poll::paintFilling(
		Painter &p,
		bool chosen,
		bool correct,
		float64 filling,
		int left,
		int top,
		int width,
		int contentWidth,
		int height,
		const PaintContext &context) const {
	const auto bottom = top + height;
	const auto st = context.st;
	const auto stm = context.messageStyle();
	const auto aleft = left + st::historyPollAnswerPadding.left();

	top += st::historyPollAnswerPadding.top();

	const auto thickness = st::historyPollFillingHeight;
	const auto max = contentWidth - st::historyPollFillingRight;
	const auto size = anim::interpolate(st::historyPollFillingMin, max, filling);
	const auto radius = st::historyPollFillingRadius;
	const auto ftop = bottom - st::historyPollFillingBottom - thickness;

	enum class Style {
		Incorrect,
		Correct,
		Default,
	};
	const auto style = [&] {
		if (chosen && !correct) {
			return Style::Incorrect;
		} else if (chosen && correct && _poll->quiz() && !context.outbg) {
			return Style::Correct;
		} else {
			return Style::Default;
		}
	}();
	auto barleft = aleft;
	auto barwidth = size;
	const auto &color = (style == Style::Incorrect)
		? st->boxTextFgError()
		: (style == Style::Correct)
		? st->boxTextFgGood()
		: stm->msgFileBg;
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	PainterHighQualityEnabler hq(p);
	if (chosen || correct) {
		const auto &icon = (style == Style::Incorrect)
			? st->historyPollChoiceWrong()
			: (style == Style::Correct)
			? st->historyPollChoiceRight()
			: stm->historyPollChoiceRight;
		const auto cleft = aleft - st::historyPollPercentSkip - icon.width();
		const auto ctop = ftop - (icon.height() - thickness) / 2;
		p.drawEllipse(cleft, ctop, icon.width(), icon.height());

		const auto paintContent = [&](QPainter &p) {
			icon.paint(p, cleft, ctop, width);
		};
		if (style == Style::Default && usesBubblePattern(context)) {
			const auto add = st::lineWidth * 2;
			const auto target = QRect(
				cleft,
				ctop,
				icon.width(),
				icon.height()
			).marginsAdded({ add, add, add, add });
			Ui::PaintPatternBubblePart(
				p,
				context.viewport,
				context.bubblesPattern->pixmap,
				target,
				paintContent,
				_fillingIconCache);
		} else {
			paintContent(p);
		}
		//barleft += icon.width() - radius;
		//barwidth -= icon.width() - radius;
	}
	if (barwidth > 0) {
		p.drawRoundedRect(barleft, ftop, barwidth, thickness, radius, radius);
	}
}

bool Poll::answerVotesChanged() const {
	if (_poll->answers.size() != _answers.size()
		|| _poll->answers.empty()) {
		return false;
	}
	for (const auto &answer : _answers) {
		const auto i = ranges::find(
			_poll->answers,
			answer.option,
			&PollAnswer::option);
		if (i == end(_poll->answers)) {
			return false;
		} else if (answer.votes != i->votes) {
			return true;
		}
	}
	return false;
}

void Poll::saveStateInAnimation() const {
	if (_answersAnimation) {
		return;
	}
	const auto show = showVotes();
	_answersAnimation = std::make_unique<AnswersAnimation>();
	_answersAnimation->data.reserve(_answers.size());
	const auto convert = [&](const Answer &answer) {
		auto result = AnswerAnimation();
		result.percent = show ? float64(answer.votesPercent) : 0.;
		result.filling = show ? answer.filling : 0.;
		result.opacity = show ? 1. : 0.;
		result.chosen = answer.chosen;
		result.correct = answer.correct;
		return result;
	};
	ranges::transform(
		_answers,
		ranges::back_inserter(_answersAnimation->data),
		convert);
}

bool Poll::checkAnimationStart() const {
	if (_poll->answers.size() != _answers.size()) {
		// Skip initial changes.
		return false;
	}
	const auto result = (showVotes() != (_poll->voted() || _poll->closed()))
		|| answerVotesChanged();
	if (result) {
		saveStateInAnimation();
	}
	return result;
}

void Poll::startAnswersAnimation() const {
	if (!_answersAnimation) {
		return;
	}

	const auto show = showVotes();
	auto &&both = ranges::views::zip(_answers, _answersAnimation->data);
	for (auto &&[answer, data] : both) {
		data.percent.start(show ? float64(answer.votesPercent) : 0.);
		data.filling.start(show ? answer.filling : 0.);
		data.opacity.start(show ? 1. : 0.);
		data.chosen = data.chosen || answer.chosen;
		data.correct = data.correct || answer.correct;
	}
	_answersAnimation->progress.start(
		[=] { repaint(); },
		0.,
		1.,
		st::historyPollDuration);
}

TextSelection Poll::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	if (_description.isEmpty()) {
		return _question.adjustSelection(selection, type);
	} else if (selection.to <= _description.length()) {
		return _description.adjustSelection(selection, type);
	}
	const auto questionSelection = _question.adjustSelection(
		toQuestionSelection(selection),
		type);
	if (selection.from >= _description.length()) {
		return fromQuestionSelection(questionSelection);
	}
	const auto descriptionSelection = _description.adjustSelection(
		selection,
		type);
	return {
		descriptionSelection.from,
		fromQuestionSelection(questionSelection).to
	};
}

uint16 Poll::fullSelectionLength() const {
	return _description.length() + _question.length();
}

TextForMimeData Poll::selectedText(TextSelection selection) const {
	auto description = _description.toTextForMimeData(selection);
	auto question = _question.toTextForMimeData(
		toQuestionSelection(selection));
	if (description.empty()) {
		return question;
	} else if (question.empty()) {
		return description;
	}
	return description.append('\n').append(std::move(question));
}

TextState Poll::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);
	if (!_poll->sendingVotes.empty()) {
		return result;
	}

	const auto can = canVote();
	const auto show = showVotes();
	const auto padding = st::msgPadding;
	auto paintw = width();
	auto tshift = countTopContentSkip();
	paintw -= padding.left() + padding.right();

	if (const auto mediaHeight = countTopMediaHeight()) {
		if (_attachedMedia->handler
			&& countTopMediaRect(tshift).contains(point)) {
			result.link = _attachedMedia->handler;
			return result;
		}
		tshift += mediaHeight + st::historyPollMediaSkip;
	}

	auto symbolAdd = 0;
	if (const auto descriptionHeight = countDescriptionHeight(paintw)) {
		if (QRect(
			padding.left(),
			tshift,
			paintw,
			descriptionHeight).contains(point)) {
			result = TextState(_parent, _description.getStateLeft(
				point - QPoint(padding.left(), tshift),
				paintw,
				width(),
				request.forText()));
			return result;
		}
		if (point.y() >= tshift + descriptionHeight) {
			symbolAdd += _description.length();
		}
		tshift += descriptionHeight + st::historyPollDescriptionSkip;
	}

	if (const auto solutionHeight = countSolutionBlockHeight(paintw)) {
		if (QRect(
			padding.left(),
			tshift,
			paintw,
			solutionHeight).contains(point)) {
			const auto &qst = st::historyPagePreview;
			const auto innerLeft = padding.left() + qst.padding.left();
			const auto innerRight = padding.left()
				+ paintw
				- qst.padding.right();
			const auto closeArea = st::historyPollExplanationCloseSize;
			const auto closeLeft = innerRight - closeArea;
			const auto closeTop = tshift + qst.padding.top();
			if (QRect(
				closeLeft,
				closeTop,
				closeArea,
				st::semiboldFont->height).contains(point)) {
				result.link = _closeSolutionLink;
				return result;
			}
			const auto textTop = tshift
				+ qst.padding.top()
				+ st::semiboldFont->height
				+ st::historyPollExplanationTitleSkip;
			const auto textWidth = innerRight - innerLeft;
			const auto textHeight = _solutionText.countHeight(textWidth);
			if (QRect(
				innerLeft,
				textTop,
				textWidth,
				textHeight).contains(point)) {
				auto textResult = _solutionText.getStateLeft(
					point - QPoint(innerLeft, textTop),
					textWidth,
					width(),
					request.forText());
				if (textResult.link) {
					result.link = textResult.link;
				}
				return result;
			}
			if (const auto mh = countSolutionMediaHeight(textWidth)) {
				const auto mediaTop = textTop
					+ textHeight
					+ st::historyPollExplanationMediaSkip;
				const auto shift = st::msgFileLayout.padding.left();
				const auto mediaLeft = innerLeft - shift;
				if (_solutionAttach
					&& QRect(
						mediaLeft,
						mediaTop,
						_solutionAttach->width(),
						mh).contains(point)) {
					const auto attachLeft = rtl()
						? (width() - innerLeft - _solutionAttach->width())
						: innerLeft;
					result = _solutionAttach->textState(
						point - QPoint(attachLeft, mediaTop),
						request);
				}
			}
			return result;
		}
		tshift += solutionHeight + st::historyPollExplanationSkip;
	}

	const auto questionH = _question.countHeight(paintw);
	if (QRect(padding.left(), tshift, paintw, questionH).contains(point)) {
		result = TextState(_parent, _question.getState(
			point - QPoint(padding.left(), tshift),
			paintw,
			request.forText()));
		result.symbol += symbolAdd;
		return result;
	}
	if (point.y() >= tshift + questionH) {
		symbolAdd += _question.length();
	}
	tshift += questionH + st::historyPollSubtitleSkip;
	if (inShowSolution(point, padding.left() + paintw, tshift)) {
		result.link = _showSolutionLink;
		return result;
	}
	tshift += st::msgDateFont->height + st::historyPollAnswersSkip;
	for (const auto &answer : _answers) {
		const auto height = countAnswerHeight(answer, paintw);
		if (point.y() >= tshift && point.y() < tshift + height) {
			const auto media = answer.thumbnail ? PollAnswerMediaSize() : 0;
			if (media
				&& answer.mediaHandler
				&& QRect(
					padding.left()
						+ paintw
						- st::historyPollAnswerPadding.right()
						- media,
					tshift + st::historyPollAnswerPadding.top(),
					media,
					media).contains(point)) {
				result.link = answer.mediaHandler;
			} else if (can) {
				_lastLinkPoint = point;
				result.link = answer.handler;
			} else if (show) {
				result.customTooltip = true;
				using Flag = Ui::Text::StateRequest::Flag;
				if (request.flags & Flag::LookupCustomTooltip) {
					const auto quiz = _poll->quiz();
					result.customTooltipText = answer.votes
						? (quiz
							? tr::lng_polls_answers_count
							: tr::lng_polls_votes_count)(
								tr::now,
								lt_count_decimal,
								answer.votes)
						: (quiz
							? tr::lng_polls_answers_none
							: tr::lng_polls_votes_none)(tr::now);
				}
			}
			return result;
		}
		tshift += height;
	}
	if (!showVotersCount()) {
		const auto link = showVotes()
			? _showResultsLink
			: canSendVotes()
			? _sendVotesLink
			: nullptr;
		if (link) {
			const auto linkHeight = bottomButtonHeight();
			const auto linkTop = height() - linkHeight;
			if (QRect(0, linkTop, width(), linkHeight).contains(point)) {
				_lastLinkPoint = point;
				result.link = link;
				return result;
			}
		}
	}
	return result;
}

void Poll::parentTextUpdated() {
	updateDescription();
	history()->owner().requestViewResize(_parent);
}

auto Poll::bubbleRoll() const -> BubbleRoll {
	const auto value = _wrongAnswerAnimation.value(1.);
	_wrongAnswerAnimated = (value < 1.);
	if (!_wrongAnswerAnimated) {
		return BubbleRoll();
	}
	const auto progress = [](float64 full) {
		const auto lower = std::floor(full);
		const auto shift = (full - lower);
		switch (int(lower) % 4) {
		case 0: return -shift;
		case 1: return (shift - 1.);
		case 2: return shift;
		case 3: return (1. - shift);
		}
		Unexpected("Value in Poll::getBubbleRollDegrees.");
	};
	return {
		.rotate = progress(value * kRotateSegments) * kRotateAmplitude,
		.scale = 1. + progress(value * kScaleSegments) * kScaleAmplitude
	};
}

QMargins Poll::bubbleRollRepaintMargins() const {
	if (!_wrongAnswerAnimated) {
		return QMargins();
	}
	static const auto kAdd = int(std::ceil(
		st::msgMaxWidth * std::sin(kRotateAmplitude * M_PI / 180.)));
	return QMargins(kAdd, kAdd, kAdd, kAdd);
}

void Poll::paintBubbleFireworks(
		Painter &p,
		const QRect &bubble,
		crl::time ms) const {
	if (!_fireworksAnimation || _fireworksAnimation->paint(p, bubble)) {
		return;
	}
	_fireworksAnimation = nullptr;
}

void Poll::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (!handler) return;

	const auto i = ranges::find(
		_answers,
		handler,
		&Answer::handler);
	if (i != end(_answers)) {
		toggleRipple(*i, pressed);
	} else if (handler == _sendVotesLink || handler == _showResultsLink) {
		toggleLinkRipple(pressed);
	}
}

void Poll::unloadHeavyPart() {
	for (auto &recent : _recentVoters) {
		recent.userpic = {};
	}
}

bool Poll::hasHeavyPart() const {
	for (auto &recent : _recentVoters) {
		if (!recent.userpic.null()) {
			return true;
		}
	}
	return false;
}

void Poll::toggleRipple(Answer &answer, bool pressed) {
	if (pressed) {
		const auto outerWidth = width();
		const auto innerWidth = outerWidth
			- st::msgPadding.left()
			- st::msgPadding.right();
		if (!answer.ripple) {
			auto mask = Ui::RippleAnimation::RectMask(QSize(
				outerWidth,
				countAnswerHeight(answer, innerWidth)));
			answer.ripple = std::make_unique<Ui::RippleAnimation>(
				st::defaultRippleAnimation,
				std::move(mask),
				[=] { repaint(); });
		}
		const auto top = countAnswerTop(answer, innerWidth);
		answer.ripple->add(_lastLinkPoint - QPoint(0, top));
	} else if (answer.ripple) {
		answer.ripple->lastStop();
	}
}

bool Poll::canShowSolution() const {
	return showVotes() && !_poll->solution.text.isEmpty();
}

bool Poll::inShowSolution(
		QPoint point,
		int right,
		int top) const {
	if (!canShowSolution() || !_solutionButtonVisible) {
		return false;
	}
	const auto &icon = st::historyQuizExplainIn;
	const auto x = right - icon.width();
	const auto y = top + (st::normalFont->height - icon.height()) / 2;
	return QRect(x, y, icon.width(), icon.height()).contains(point);
}

int Poll::bottomButtonHeight() const {
	const auto skip = st::historyPollChoiceRight.height()
		- st::historyPollFillingBottom
		- st::historyPollFillingHeight
		- (st::historyPollChoiceRight.height() - st::historyPollFillingHeight) / 2;
	return st::historyPollTotalVotesSkip
		- skip
		+ st::historyPollBottomButtonSkip
		+ st::msgDateFont->height
		+ st::msgPadding.bottom();
}

void Poll::toggleLinkRipple(bool pressed) {
	if (pressed) {
		const auto linkWidth = width();
		const auto linkHeight = bottomButtonHeight();
		if (!_linkRipple) {
			auto mask = isRoundedInBubbleBottom()
				? static_cast<Message*>(_parent.get())->bottomRippleMask(
					bottomButtonHeight())
				: BottomRippleMask{
					Ui::RippleAnimation::RectMask({ linkWidth, linkHeight }),
				};
			_linkRipple = std::make_unique<Ui::RippleAnimation>(
				st::defaultRippleAnimation,
				std::move(mask.image),
				[=] { repaint(); });
			_linkRippleShift = mask.shift;
		}
		_linkRipple->add(_lastLinkPoint
			+ QPoint(_linkRippleShift, linkHeight - height()));
	} else if (_linkRipple) {
		_linkRipple->lastStop();
	}
}

Poll::~Poll() {
	history()->owner().unregisterPollView(_poll, _parent);
	if (hasHeavyPart()) {
		unloadHeavyPart();
		_parent->checkHeavyPart();
	}
}

} // namespace HistoryView
