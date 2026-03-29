/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/view/media/history_view_media.h"
#include "ui/effects/animations.h"
#include "data/data_poll.h"
#include "base/weak_ptr.h"
#include "base/timer.h"

namespace Ui {
class RippleAnimation;
class FireworksAnimation;
} // namespace Ui

namespace HistoryView {

class Message;

class Poll final : public Media {
public:
	Poll(
		not_null<Element*> parent,
		not_null<PollData*> poll,
		const TextWithEntities &consumed);
	~Poll();

	void draw(Painter &p, const PaintContext &context) const override;
	TextState textState(QPoint point, StateRequest request) const override;

	bool toggleSelectionByHandlerClick(const ClickHandlerPtr &p) const override {
		return true;
	}
	bool dragItemByHandler(const ClickHandlerPtr &p) const override {
		return true;
	}

	bool needsBubble() const override {
		return true;
	}
	bool customInfoLayout() const override {
		return false;
	}

	[[nodiscard]] TextSelection adjustSelection(
		TextSelection selection,
		TextSelectType type) const override;
	uint16 fullSelectionLength() const override;
	TextForMimeData selectedText(TextSelection selection) const override;

	BubbleRoll bubbleRoll() const override;
	QMargins bubbleRollRepaintMargins() const override;
	void paintBubbleFireworks(
		Painter &p,
		const QRect &bubble,
		crl::time ms) const override;

	void clickHandlerActiveChanged(
		const ClickHandlerPtr &handler,
		bool active) override;
	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) override;

	void unloadHeavyPart() override;
	bool hasHeavyPart() const override;
	void parentTextUpdated() override;

	[[nodiscard]] QRect addOptionRect(int innerWidth) const override;
	void setAddOptionActive(bool active) override;

private:
	struct Part;
	struct Header;
	struct Options;
	struct AddOption;
	struct Footer;

	struct AnswerAnimation;
	struct AnswersAnimation;
	struct SendingAnimation;
	struct Answer;
	struct AttachedMedia;
	struct SolutionMedia;
	struct RecentVoter;

	QSize countOptimalSize() override;
	QSize countCurrentSize(int newWidth) override;

	[[nodiscard]] bool showVotes() const;
	[[nodiscard]] bool canVote() const;
	[[nodiscard]] bool canSendVotes() const;
	[[nodiscard]] bool isAuthorNotVoted() const;
	void updateTexts();
	void updateVotes();
	bool showVotersCount() const;
	bool inlineFooter() const;

	[[nodiscard]] bool canAddOption() const;

	not_null<PollData*> _poll;
	int _pollVersion = 0;
	int _totalVotes = 0;
	bool _voted = false;
	PollData::Flags _flags = PollData::Flags();

	Ui::Text::String _description;
	Ui::Text::String _question;
	Ui::Text::String _subtitle;
	std::unique_ptr<AttachedMedia> _attachedMedia;
	std::unique_ptr<Media> _attachedMediaAttach;
	mutable QImage _attachedMediaCache;
	mutable Ui::BubbleRounding _attachedMediaCacheRounding;
	std::vector<RecentVoter> _recentVoters;
	QImage _recentVotersImage;

	std::vector<Answer> _answers;
	Ui::Text::String _totalVotesLabel;
	Ui::Text::String _adminVotesLabel;
	Ui::Text::String _adminBackVoteLabel;
	ClickHandlerPtr _showResultsLink;
	ClickHandlerPtr _sendVotesLink;
	ClickHandlerPtr _adminVotesLink;
	ClickHandlerPtr _adminBackVoteLink;
	ClickHandlerPtr _addOptionLink;
	ClickHandlerPtr _saveOptionLink;
	mutable ClickHandlerPtr _showSolutionLink;
	mutable std::unique_ptr<Ui::RippleAnimation> _addOptionRipple;
	mutable std::unique_ptr<Ui::RippleAnimation> _linkRipple;
	mutable int _linkRippleShift = 0;

	mutable std::unique_ptr<AnswersAnimation> _answersAnimation;
	mutable std::unique_ptr<SendingAnimation> _sendingAnimation;
	mutable std::unique_ptr<Ui::FireworksAnimation> _fireworksAnimation;
	Ui::Animations::Simple _wrongAnswerAnimation;
	mutable QPoint _lastLinkPoint;
	mutable QImage _userpicCircleCache;
	mutable QImage _fillingIconCache;

	mutable base::Timer _closeTimer;

	mutable Ui::Animations::Simple _solutionButtonAnimation;
	mutable bool _solutionShown = false;
	mutable bool _solutionButtonVisible = false;

	Ui::Text::String _solutionText;
	mutable ClickHandlerPtr _closeSolutionLink;
	std::unique_ptr<SolutionMedia> _solutionMedia;
	std::unique_ptr<Media> _solutionAttach;

	bool _hasSelected = false;
	bool _anyAnswerHasMedia = false;
	bool _votedFromHere = false;
	bool _addOptionActive = false;
	mutable bool _wrongAnswerAnimated = false;
	mutable bool _adminShowResults = false;

	std::unique_ptr<Header> _headerPart;
	std::unique_ptr<Options> _optionsPart;
	std::unique_ptr<AddOption> _addOptionPart;
	std::unique_ptr<Footer> _footerPart;

};

} // namespace HistoryView
