/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_local_storage.h"

#include "settings.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "statistics/view/stack_linear_chart_common.h"
#include "storage/storage_account.h"
#include "storage/cache/storage_cache_database.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/emoji_config.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/widgets/tooltip.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QStorageInfo>

namespace Settings {
namespace {

constexpr auto kMegabyte = int64(1024 * 1024);
constexpr auto kTotalSizeLimitsCount = 18;
constexpr auto kMediaSizeLimitsCount = 18;
constexpr auto kMinimalSizeLimit = 100 * kMegabyte;
constexpr auto kTimeLimitsCount = 16;
constexpr auto kMaxTimeLimitValue = std::numeric_limits<size_type>::max();
constexpr auto kFakeMediaCacheTag = uint16(0xFFFF);

int64 TotalSizeLimitInMB(int index) {
	if (index < 8) {
		return int64(index + 2) * 100;
	}
	return int64(index - 7) * 1024;
}

int64 TotalSizeLimit(int index) {
	return TotalSizeLimitInMB(index) * kMegabyte;
}

int64 MediaSizeLimitInMB(int index) {
	if (index < 9) {
		return int64(index + 1) * 100;
	}
	return int64(index - 8) * 1024;
}

int64 MediaSizeLimit(int index) {
	return MediaSizeLimitInMB(index) * kMegabyte;
}

QString SizeLimitText(int64 limit) {
	const auto mb = (limit / (1024 * 1024));
	const auto gb = (mb / 1024);
	return (gb > 0)
		? (QString::number(gb) + " GB")
		: (QString::number(mb) + " MB");
}

size_type TimeLimitInDays(int index) {
	if (index < 3) {
		const auto weeks = (index + 1);
		return size_type(weeks) * 7;
	} else if (index < 15) {
		const auto month = (index - 2);
		return (size_type(month) * 30)
			+ ((month >= 12) ? 5 :
				(month >= 10) ? 4 :
				(month >= 8) ? 3 :
				(month >= 7) ? 2 :
				(month >= 5) ? 1 :
				(month >= 3) ? 0 :
				(month >= 2) ? -1 :
				(month >= 1) ? 1 : 0);
	}
	return 0;
}

size_type TimeLimit(int index) {
	const auto days = TimeLimitInDays(index);
	return days
		? (days * 24 * 60 * 60)
		: kMaxTimeLimitValue;
}

QString TimeLimitText(size_type limit) {
	const auto days = (limit / (24 * 60 * 60));
	const auto weeks = (days / 7);
	const auto months = (days / 29);
	return (months > 0)
		? tr::lng_months(tr::now, lt_count, months)
		: (limit > 0)
		? tr::lng_weeks(tr::now, lt_count, weeks)
		: tr::lng_local_storage_limit_never(tr::now);
}

size_type LimitToValue(size_type timeLimit) {
	return timeLimit ? timeLimit : kMaxTimeLimitValue;
}

size_type ValueToLimit(size_type timeLimit) {
	return (timeLimit != kMaxTimeLimitValue) ? timeLimit : 0;
}

constexpr auto kChartPartsCount = 6;
constexpr auto kChartSeparator = 2.;
constexpr auto kChartMinFraction = 0.02;
constexpr auto kChartMorphDuration = crl::time(650);
constexpr auto kChartAppearDuration = crl::time(750);
constexpr auto kChartSelectDuration = crl::time(200);
constexpr auto kChartPercentFadeDuration = crl::time(200);

QColor PartColor(int index) {
	switch (index) {
	case 0: return st::statisticsChartLineLightblue->c;
	case 1: return st::statisticsChartLineOrange->c;
	case 2: return st::statisticsChartLineLightgreen->c;
	case 3: return st::statisticsChartLineGreen->c;
	case 4: return st::statisticsChartLinePurple->c;
	case 5: return st::statisticsChartLineBlue->c;
	}
	return st::statisticsChartLineCyan->c;
}

const style::icon &ParticleIcon(int index) {
	switch (index) {
	case 0: return st::localStorageParticlePhotos;
	case 1: return st::localStorageParticleStickers;
	case 2: return st::localStorageParticleMusic;
	case 3: return st::localStorageParticleVideos;
	case 4: return st::localStorageParticleVideos;
	case 5: return st::localStorageParticleDocuments;
	}
	return st::localStorageParticleDocuments;
}

const QImage &ParticleImage(int index) {
	static auto images = std::array<QImage, kChartPartsCount>();
	auto &image = images[index];
	if (image.isNull()) {
		image = ParticleIcon(index).instance(Qt::white);
	}
	return image;
}

[[nodiscard]] QString FormatStoragePercent(int64 part, int64 whole) {
	if (whole <= 0) {
		return u"0%"_q;
	}
	const auto ratio = part / float64(whole);
	if (ratio < 0.001) {
		return u"<0.1%"_q;
	}
	const auto percent = int(base::SafeRound(ratio * 100.));
	return (percent <= 0)
		? u"<1%"_q
		: (QString::number(percent) + '%');
}

[[nodiscard]] QString FormatStorageSize(int64 size) {
	constexpr auto kGigabyte = int64(1024) * 1024 * 1024;
	if (size < kGigabyte) {
		return Ui::FormatSizeText(size);
	}
	const auto tenthGb = size * 10 / kGigabyte;
	return QString::number(tenthGb / 10)
		+ '.'
		+ QString::number(tenthGb % 10)
		+ u" GB"_q;
}

} // namespace

class LocalStorage::Chart final : public Ui::RpWidget {
public:
	explicit Chart(QWidget *parent);

	void setParts(std::array<int64, kChartPartsCount> sizes, int64 total);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Geometry {
		float64 center = 0.;
		float64 half = 0.;
	};

	void computeTargets();
	[[nodiscard]] std::array<Geometry, kChartPartsCount> shown() const;
	void setSelected(int index);
	[[nodiscard]] QPointF radialPoint(
		QPointF center,
		float64 radius,
		float64 angle) const;
	void drawParticles(
		QPainter &p,
		QPointF center,
		int index,
		float64 from,
		float64 to,
		float64 innerRadius,
		float64 outerRadius,
		float64 alpha);

	std::array<int64, kChartPartsCount> _sizes = { { 0 } };
	std::array<Geometry, kChartPartsCount> _from;
	std::array<Geometry, kChartPartsCount> _to;
	std::array<int, kChartPartsCount> _percent = { { 0 } };
	std::array<bool, kChartPartsCount> _showPercent = { { false } };
	std::array<float64, kChartPartsCount> _percentTarget = { { 0. } };
	std::array<Ui::Animations::Simple, kChartPartsCount> _percentAlpha;
	int64 _total = 0;

	Ui::Animations::Simple _morph;
	Ui::Animations::Simple _appear;
	bool _appeared = false;

	int _selected = -1;
	std::array<Ui::Animations::Simple, kChartPartsCount> _selectAnimations;

	Ui::Animations::Basic _particles;
	crl::time _particlesStart = 0;

};

LocalStorage::Chart::Chart(QWidget *parent) : RpWidget(parent) {
	setMouseTracking(true);

	if (!anim::Disabled()) {
		_particlesStart = crl::now();
		_particles.init([=](crl::time) {
			update();
			return true;
		});
		shownValue() | rpl::on_next([=](bool shown) {
			if (shown) {
				_particles.start();
			} else {
				_particles.stop();
			}
		}, lifetime());
	}
}

void LocalStorage::Chart::drawParticles(
		QPainter &p,
		QPointF center,
		int index,
		float64 from,
		float64 to,
		float64 innerRadius,
		float64 outerRadius,
		float64 alpha) {
	if (alpha <= 0. || anim::Disabled()) {
		return;
	}
	const auto &image = ParticleImage(index);
	const auto size = float64(st::localStorageChartParticle);
	const auto sqrt2 = std::sqrt(2.);
	const auto time = (crl::now() - _particlesStart) / 10000.;
	const auto step = 7.;
	const auto fromStep = int(std::floor(from / step));
	const auto toStep = int(std::ceil(to / step));
	const auto rFrom = innerRadius - size * sqrt2;
	const auto rTo = outerRadius + size * sqrt2;
	for (auto k = fromStep; k <= toStep; ++k) {
		const auto angle = k * step;
		const auto t = std::fmod(
			(time + 100.) * (1. + (std::sin(angle * 2000.) + 1.) * 0.25),
			1.);
		const auto radius = rFrom + (rTo - rFrom) * t;
		const auto point = radialPoint(center, radius, angle);
		const auto distance = std::hypot(
			point.x() - center.x(),
			point.y() - center.y());
		const auto centerFade = std::min(distance / 64., 1.);
		auto particleAlpha = 0.65
			* alpha
			* (-1.75 * std::abs(t - 0.5) + 1.)
			* (0.25 * (std::sin(t * M_PI) - 1.) + 1.)
			* centerFade;
		particleAlpha = std::clamp(particleAlpha, 0., 1.);
		if (particleAlpha <= 0.) {
			continue;
		}
		const auto scale = 0.75
			* (0.25 * (std::sin(t * M_PI) - 1.) + 1.)
			* (0.8 + (std::sin(angle) + 1.) * 0.25);
		const auto side = size * scale;
		p.setOpacity(particleAlpha);
		p.drawImage(
			Rect(point.x() - side / 2., point.y() - side / 2., Size(side)),
			image);
	}
}

void LocalStorage::Chart::setParts(
		std::array<int64, kChartPartsCount> sizes,
		int64 total) {
	if (_sizes == sizes && _total == total) {
		return;
	}
	_from = shown();
	_sizes = sizes;
	_total = total;
	computeTargets();

	for (auto i = 0; i != kChartPartsCount; ++i) {
		const auto target = _showPercent[i] ? 1. : 0.;
		if (_percentTarget[i] != target) {
			const auto from = _percentAlpha[i].value(_percentTarget[i]);
			_percentTarget[i] = target;
			_percentAlpha[i].start(
				[=] { update(); },
				from,
				target,
				kChartPercentFadeDuration);
		}
	}

	_morph.stop();
	_morph.start(
		[=] { update(); },
		0.,
		1.,
		kChartMorphDuration,
		anim::easeOutQuint);
	if (!_appeared && total > 0) {
		_appeared = true;
		_appear.start(
			[=] { update(); },
			0.,
			1.,
			kChartAppearDuration,
			anim::easeOutQuint);
	}
	update();
}

void LocalStorage::Chart::computeTargets() {
	auto sum = int64();
	auto count = 0;
	for (auto i = 0; i != kChartPartsCount; ++i) {
		if (_sizes[i] > 0) {
			sum += _sizes[i];
			++count;
		}
	}
	_to = std::array<Geometry, kChartPartsCount>();
	_percent = { { 0 } };
	_showPercent = { { false } };
	if (sum <= 0) {
		for (auto i = 0; i != kChartPartsCount; ++i) {
			_to[i].center = _from[i].center;
		}
		return;
	}

	auto under = 0;
	auto minus = 0.;
	for (auto i = 0; i != kChartPartsCount; ++i) {
		const auto fraction = _sizes[i] / float64(sum);
		if (fraction > 0. && fraction < kChartMinFraction) {
			++under;
			minus += fraction;
		}
	}

	auto values = std::vector<float64>(kChartPartsCount);
	for (auto i = 0; i != kChartPartsCount; ++i) {
		values[i] = float64(_sizes[i]);
	}
	const auto percentage = Statistic::PiePartsPercentage(
		values,
		float64(sum),
		true);

	const auto separators = (count >= 2) ? count : 0;
	const auto sweep = 360. - kChartSeparator * separators;
	auto start = 0.;
	auto drawn = 0;
	for (auto i = 0; i != kChartPartsCount; ++i) {
		const auto from = start + drawn * kChartSeparator;
		const auto fraction = _sizes[i] / float64(sum);
		if (fraction <= 0.) {
			_to[i].center = from;
			_to[i].half = 0.;
			continue;
		}
		_percent[i] = int(base::SafeRound(
			percentage.parts[i].roundedPercentage * 100.));
		_showPercent[i] = (fraction > 0.05) && (fraction < 1.);
		auto adjusted = fraction;
		if (adjusted < kChartMinFraction) {
			adjusted = kChartMinFraction;
		} else {
			adjusted *= 1. - (kChartMinFraction * under - minus);
		}
		const auto to = from + adjusted * sweep;
		_to[i].center = (from + to) / 2.;
		_to[i].half = (to - from) / 2.;
		start += adjusted * sweep;
		++drawn;
	}
}

auto LocalStorage::Chart::shown() const
-> std::array<Geometry, kChartPartsCount> {
	const auto progress = _morph.value(1.);
	auto result = std::array<Geometry, kChartPartsCount>();
	for (auto i = 0; i != kChartPartsCount; ++i) {
		result[i].center = _from[i].center
			+ (_to[i].center - _from[i].center) * progress;
		result[i].half = _from[i].half
			+ (_to[i].half - _from[i].half) * progress;
	}
	return result;
}

QPointF LocalStorage::Chart::radialPoint(
		QPointF center,
		float64 radius,
		float64 angle) const {
	const auto radians = angle * M_PI / 180.;
	return QPointF(
		center.x() + radius * std::sin(radians),
		center.y() - radius * std::cos(radians));
}

void LocalStorage::Chart::setSelected(int index) {
	if (_selected == index) {
		return;
	}
	const auto previous = _selected;
	_selected = index;
	const auto animate = [&](int i, float64 to) {
		_selectAnimations[i].start(
			[=] { update(); },
			1. - to,
			to,
			kChartSelectDuration,
			anim::easeOutQuint);
	};
	if (previous >= 0) {
		animate(previous, 0.);
	}
	if (index >= 0) {
		animate(index, 1.);
	}
	setCursor(index >= 0 ? style::cur_pointer : style::cur_default);
	update();
}

int LocalStorage::Chart::resizeGetHeight(int newWidth) {
	return st::localStorageChartHeight;
}

void LocalStorage::Chart::mouseMoveEvent(QMouseEvent *e) {
	const auto center = QPointF(width() / 2., height() / 2.);
	const auto outer = st::localStorageChartDiameter / 2.;
	const auto thickness = st::localStorageChartThickness;
	const auto inner = outer - thickness;
	const auto dx = e->pos().x() - center.x();
	const auto dy = e->pos().y() - center.y();
	const auto distance = std::hypot(dx, dy);
	auto found = -1;
	if (distance >= inner - st::localStorageChartSelectGrow
		&& distance <= outer + st::localStorageChartSelectGrow) {
		auto angle = std::atan2(dx, -dy) * 180. / M_PI;
		if (angle < 0.) {
			angle += 360.;
		}
		const auto geometry = shown();
		for (auto i = 0; i != kChartPartsCount; ++i) {
			if (geometry[i].half <= 0.) {
				continue;
			}
			auto delta = std::fmod(
				std::abs(angle - geometry[i].center),
				360.);
			if (delta > 180.) {
				delta = 360. - delta;
			}
			if (delta <= geometry[i].half) {
				found = i;
				break;
			}
		}
	}
	setSelected(found);
}

void LocalStorage::Chart::leaveEventHook(QEvent *e) {
	setSelected(-1);
}

void LocalStorage::Chart::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto appear = (_total <= 0)
		? 1.
		: (_appeared ? _appear.value(1.) : 0.);
	const auto center = QPointF(width() / 2., height() / 2.);
	const auto outer = st::localStorageChartDiameter / 2.;
	const auto thin = float64(st::localStorageChartThicknessThin);
	const auto full = float64(st::localStorageChartThickness);
	const auto thickness = thin + (full - thin) * appear;
	const auto rotation = (1. - appear) * -120.;
	const auto midRadius = outer - thickness / 2.;

	p.setOpacity(appear);

	auto background = QPen(st::windowBgOver->c);
	background.setWidthF(thickness);
	background.setCapStyle(Qt::FlatCap);
	p.setPen(background);
	p.setBrush(Qt::NoBrush);
	p.drawEllipse(center, midRadius, midRadius);

	const auto geometry = shown();
	for (auto i = 0; i != kChartPartsCount; ++i) {
		if (geometry[i].half <= 0.1) {
			continue;
		}
		const auto selected = _selectAnimations[i].value(
			(_selected == i) ? 1. : 0.);
		const auto grow = selected * st::localStorageChartSelectGrow;
		const auto segmentThickness = thickness + grow;
		const auto segmentMid = midRadius + grow / 2.;
		const auto segmentOuter = outer + grow;
		const auto base = PartColor(i);

		auto gradient = QRadialGradient(center, segmentOuter);
		gradient.setColorAt(
			0.3,
			anim::color(base, QColor(255, 255, 255), 0.30));
		gradient.setColorAt(1., base);

		auto pen = QPen(QBrush(gradient), segmentThickness);
		pen.setCapStyle(Qt::FlatCap);
		p.setPen(pen);

		const auto from = geometry[i].center - geometry[i].half + rotation;
		const auto to = geometry[i].center + geometry[i].half + rotation;
		const auto rect = Rect(
			center.x() - segmentMid,
			center.y() - segmentMid,
			Size(segmentMid * 2.));
		const auto startAngle = qRound((90. - from) * 16.);
		const auto spanAngle = -qRound((to - from) * 16.);
		p.drawArc(rect, startAngle, spanAngle);

		const auto innerRadius = segmentOuter - segmentThickness;
		const auto outerRect = Rect(
			center.x() - segmentOuter,
			center.y() - segmentOuter,
			Size(segmentOuter * 2.));
		const auto innerRect = Rect(
			center.x() - innerRadius,
			center.y() - innerRadius,
			Size(innerRadius * 2.));
		auto sector = QPainterPath();
		sector.arcMoveTo(outerRect, startAngle / 16.);
		sector.arcTo(outerRect, startAngle / 16., spanAngle / 16.);
		sector.arcTo(
			innerRect,
			(startAngle + spanAngle) / 16.,
			-spanAngle / 16.);
		sector.closeSubpath();
		p.save();
		p.setClipPath(sector);
		drawParticles(
			p,
			center,
			i,
			from,
			to,
			innerRadius,
			segmentOuter,
			appear);
		p.restore();
		p.setOpacity(appear);
	}

	p.setFont(st::localStorageChartPercentFont);
	const auto percentFont = st::localStorageChartPercentFont;
	for (auto i = 0; i != kChartPartsCount; ++i) {
		const auto labelAlpha = _percentAlpha[i].value(_percentTarget[i]);
		if (labelAlpha <= 0. || geometry[i].half <= 0.1) {
			continue;
		}
		const auto selected = _selectAnimations[i].value(
			(_selected == i) ? 1. : 0.);
		const auto point = radialPoint(
			center,
			midRadius + selected * st::localStorageChartSelectGrow / 2.,
			geometry[i].center + rotation);
		const auto text = QString::number(_percent[i]) + '%';
		const auto twidth = percentFont->width(text);
		p.setOpacity(appear * labelAlpha);
		p.setPen(QColor(255, 255, 255));
		p.save();
		p.translate(point);
		const auto scale = 1. + selected * 0.1;
		p.scale(scale, scale);
		p.drawText(
			QRectF(
				-twidth,
				-percentFont->height,
				twidth * 2.,
				percentFont->height * 2.),
			text,
			QTextOption(Qt::AlignCenter));
		p.restore();
	}
	p.setOpacity(1.);

	const auto number = QString::number(
		(_total + kMegabyte - 1) / kMegabyte);
	const auto unit = u"MB"_q;
	const auto sizeFont = st::localStorageChartSizeFont;
	const auto unitFont = st::localStorageChartUnitFont;
	const auto blockHeight = sizeFont->height + unitFont->height;
	const auto top = (st::localStorageChartHeight - blockHeight) / 2;

	p.setOpacity(appear);
	p.save();
	const auto scale = 0.6 + 0.4 * appear;
	p.translate(center);
	p.scale(scale, scale);
	p.translate(-center);
	p.setFont(sizeFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		QRect(0, top, width(), sizeFont->height),
		number,
		style::al_top);
	p.setFont(unitFont);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		QRect(0, top + sizeFont->height, width(), unitFont->height),
		unit,
		style::al_top);
	p.restore();
	p.setOpacity(1.);
}

class LocalStorage::DeviceBar final : public Ui::RpWidget {
public:
	explicit DeviceBar(QWidget *parent);

	void setCache(int64 cache);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	void refresh();
	void apply();
	[[nodiscard]] QRect barRect() const;
	[[nodiscard]] TextWithEntities tooltipText() const;
	void showTooltip();
	void hideTooltip();

	object_ptr<Ui::FlatLabel> _subtitle;
	std::unique_ptr<Ui::ImportantTooltip> _tooltip;
	int64 _reported = 0;
	int64 _cache = 0;
	int64 _total = 0;
	int64 _free = 0;

};

LocalStorage::DeviceBar::DeviceBar(QWidget *parent)
: RpWidget(parent)
, _subtitle(this, QString(), st::localStorageUsageSubtitle) {
	_subtitle->setAttribute(Qt::WA_TransparentForMouseEvents);
	setMouseTracking(true);
	refresh();
}

void LocalStorage::DeviceBar::refresh() {
	const auto weak = base::make_weak(this);
	const auto dir = cWorkingDir();
	crl::async([=] {
		const auto info = QStorageInfo(dir);
		const auto total = info.isValid() ? info.bytesTotal() : int64();
		const auto free = info.isValid() ? info.bytesAvailable() : int64();
		crl::on_main(weak, [=] {
			_total = total;
			_free = (free > total) ? total : free;
			apply();
		});
	});
}

void LocalStorage::DeviceBar::apply() {
	const auto used = std::max(_total - _free, int64());
	_cache = std::min(_reported, used);
	_subtitle->setText(tr::lng_local_storage_device_usage(
		tr::now,
		lt_percent,
		FormatStoragePercent(_cache, _total)));
	resizeToWidth(width());
	update();
}

void LocalStorage::DeviceBar::setCache(int64 cache) {
	_reported = cache;
	apply();
	refresh();
}

QRect LocalStorage::DeviceBar::barRect() const {
	const auto width = std::min(st::localStorageUsageBarWidth, QWidget::width());
	const auto top = st::localStorageUsageTopSkip
		+ _subtitle->height()
		+ st::localStorageUsageBarSkip;
	return QRect(
		(QWidget::width() - width) / 2,
		top,
		width,
		st::localStorageUsageBarHeight);
}

int LocalStorage::DeviceBar::resizeGetHeight(int newWidth) {
	_subtitle->resizeToWidth(newWidth);
	_subtitle->moveToLeft(0, st::localStorageUsageTopSkip, newWidth);
	return st::localStorageUsageTopSkip
		+ _subtitle->height()
		+ st::localStorageUsageBarSkip
		+ st::localStorageUsageBarHeight
		+ st::localStorageUsageBottomSkip;
}

void LocalStorage::DeviceBar::paintEvent(QPaintEvent *e) {
	if (_total <= 0) {
		return;
	}
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto bar = QRectF(barRect());
	const auto radius = bar.height() / 2.;
	const auto used = _total - _free;
	const auto cacheColor = st::activeButtonBg->c;
	const auto usedColor = st::windowSubTextFg->c;
	const auto freeColor = st::windowBg->c;

	auto path = QPainterPath();
	path.addRoundedRect(bar, radius, radius);
	p.setClipPath(path);
	p.fillRect(bar, freeColor);

	const auto fill = [&](int64 from, int64 size, QColor color) {
		if (size <= 0) {
			return;
		}
		const auto x1 = bar.x() + bar.width() * (from / float64(_total));
		const auto x2 = bar.x()
			+ bar.width() * ((from + size) / float64(_total));
		p.fillRect(QRectF(x1, bar.y(), x2 - x1, bar.height()), color);
	};
	fill(0, used, usedColor);
	fill(0, _cache, cacheColor);
}

void LocalStorage::DeviceBar::mouseMoveEvent(QMouseEvent *e) {
	if (_total > 0 && barRect().marginsAdded(st::localStorageUsageBarMargin)
			.contains(e->pos())) {
		showTooltip();
	} else {
		hideTooltip();
	}
}

void LocalStorage::DeviceBar::leaveEventHook(QEvent *e) {
	hideTooltip();
}

TextWithEntities LocalStorage::DeviceBar::tooltipText() const {
	const auto used = _total - _free;
	const auto other = std::max(used - _cache, int64());
	auto result = TextWithEntities();
	const auto line = [&](const QString &label, int64 size) {
		if (!result.text.isEmpty()) {
			result.append('\n');
		}
		result.append(label).append(u": "_q).append(
			Ui::Text::Bold(FormatStorageSize(size)));
	};
	line(tr::lng_local_storage_device_telegram(tr::now), _cache);
	line(tr::lng_local_storage_device_other(tr::now), other);
	line(tr::lng_local_storage_device_free(tr::now), _free);
	line(tr::lng_local_storage_device_total(tr::now), _total);
	return result;
}

void LocalStorage::DeviceBar::showTooltip() {
	if (_tooltip || _total <= 0) {
		return;
	}
	const auto parent = window();
	if (!parent) {
		return;
	}
	_tooltip = std::make_unique<Ui::ImportantTooltip>(
		parent,
		Ui::MakeNiceTooltipLabel(
			parent,
			rpl::single(tooltipText()),
			st::localStorageUsageTooltipMaxWidth,
			st::defaultImportantTooltipLabel),
		st::defaultImportantTooltip);
	const auto tooltip = _tooltip.get();
	const auto weak = base::make_weak(tooltip);
	tooltip->setAttribute(Qt::WA_TransparentForMouseEvents);
	tooltip->setHiddenCallback([=] { delete weak.get(); });
	auto area = Ui::MapFrom(parent, this, barRect());
	area.translate(0, -st::localStorageUsageTooltipSkip);
	tooltip->pointAt(area, RectPart::Top);
	tooltip->toggleAnimated(true);
}

void LocalStorage::DeviceBar::hideTooltip() {
	if (const auto tooltip = _tooltip.release()) {
		tooltip->toggleAnimated(false);
	}
}

class LocalStorage::Row : public Ui::RpWidget {
public:
	Row(
		QWidget *parent,
		Fn<QString(size_type)> title,
		rpl::producer<QString> clear,
		const Database::TaggedSummary &data);

	void update(const Database::TaggedSummary &data);
	void toggleProgress(bool shown);

	rpl::producer<> clearRequests() const;
	[[nodiscard]] not_null<Ui::RoundButton*> clearButton() const;

protected:
	int resizeGetHeight(int newWidth) override;

private:
	QString titleText(const Database::TaggedSummary &data) const;
	QString sizeText(const Database::TaggedSummary &data) const;
	void radialAnimationCallback();

	Fn<QString(size_type)> _titleFactory;
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _description;
	object_ptr<Ui::FlatLabel> _clearing = { nullptr };
	object_ptr<Ui::RoundButton> _clear;
	std::unique_ptr<Ui::InfiniteRadialAnimation> _progress;

};

LocalStorage::Row::Row(
	QWidget *parent,
	Fn<QString(size_type)> title,
	rpl::producer<QString> clear,
	const Database::TaggedSummary &data)
: RpWidget(parent)
, _titleFactory(std::move(title))
, _title(
	this,
	titleText(data),
	st::localStorageRowTitle)
, _description(
	this,
	sizeText(data),
	st::localStorageRowSize)
, _clear(this, std::move(clear), st::localStorageClear) {
	_clear->setVisible(data.count != 0);
}

void LocalStorage::Row::update(const Database::TaggedSummary &data) {
	if (data.count != 0) {
		_title->setText(titleText(data));
	}
	_description->setText(sizeText(data));
	_clear->setVisible(data.count != 0);
}

void LocalStorage::Row::toggleProgress(bool shown) {
	if (!shown) {
		_progress = nullptr;
		_description->show();
		_clearing.destroy();
	} else if (!_progress) {
		_progress = std::make_unique<Ui::InfiniteRadialAnimation>(
			[=] { radialAnimationCallback(); },
			st::proxyCheckingAnimation);
		_progress->start();
		_clearing = object_ptr<Ui::FlatLabel>(
			this,
			tr::lng_local_storage_clearing(tr::now),
			st::localStorageRowSize);
		_clearing->show();
		_description->hide();
		resizeToWidth(width());
		RpWidget::update();
	}
}

void LocalStorage::Row::radialAnimationCallback() {
	if (!anim::Disabled()) {
		RpWidget::update();
	}
}

rpl::producer<> LocalStorage::Row::clearRequests() const {
	return _clear->clicks() | rpl::to_empty;
}

not_null<Ui::RoundButton*> LocalStorage::Row::clearButton() const {
	return _clear.data();
}

int LocalStorage::Row::resizeGetHeight(int newWidth) {
	const auto height = st::localStorageRowHeight;
	const auto padding = st::localStorageRowPadding;
	const auto available = newWidth - padding.left() - padding.right();
	_title->resizeToWidth(available);
	_description->resizeToWidth(available);
	_title->moveToLeft(padding.left(), padding.top(), newWidth);
	_description->moveToLeft(
		padding.left(),
		height - padding.bottom() - _description->height(),
		newWidth);
	if (_clearing) {
		_clearing->resizeToWidth(available);
		_clearing->moveToLeft(
			padding.left(),
			_description->y(),
			newWidth);
	}
	_clear->moveToRight(
		padding.right(),
		(height - _clear->height()) / 2,
		newWidth);
	return height;
}

QString LocalStorage::Row::titleText(
		const Database::TaggedSummary &data) const {
	return _titleFactory(data.count);
}

QString LocalStorage::Row::sizeText(
		const Database::TaggedSummary &data) const {
	return data.totalSize
		? Ui::FormatSizeText(data.totalSize)
		: tr::lng_local_storage_empty(tr::now);
}

LocalStorage::LocalStorage(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller)
, _session(&controller->session())
, _db(&_session->data().cache())
, _dbBig(&_session->data().cacheBigFile()) {
	const auto &settings = _session->local().cacheSettings();
	const auto &settingsBig = _session->local().cacheBigFileSettings();
	_totalSizeLimit = settings.totalSizeLimit + settingsBig.totalSizeLimit;
	_mediaSizeLimit = settingsBig.totalSizeLimit;
	_timeLimit = settings.totalTimeLimit;

	setupContent();
}

rpl::producer<QString> LocalStorage::title() {
	return tr::lng_local_storage_title();
}

void LocalStorage::showFinished() {
	Section::showFinished();

	if (const auto i = _rows.find(0); i != end(_rows)) {
		controller()->checkHighlightControl(
			u"storage/clear-cache"_q,
			i->second->entity()->clearButton(),
			{ .rippleShape = true });
	}
	if (_totalSlider) {
		const auto add = st::roundRadiusSmall;
		controller()->checkHighlightControl(
			u"storage/max-cache"_q,
			_totalSlider,
			{ .margin = Margins(-add), .radius = add });
	}
}

void LocalStorage::updateRow(
		not_null<Ui::SlideWrap<Row>*> row,
		const Database::TaggedSummary *data) {
	const auto summary = (_rows.find(0)->second == row);
	const auto shown = (data && data->count && data->totalSize) || summary;
	if (shown) {
		row->entity()->update(*data);
	}
	row->toggle(shown, anim::type::normal);
}

void LocalStorage::update(
		Database::Stats &&stats,
		Database::Stats &&statsBig) {
	_stats = std::move(stats);
	_statsBig = std::move(statsBig);
	if (const auto i = _rows.find(0); i != end(_rows)) {
		i->second->entity()->toggleProgress(
			_stats.clearing || _statsBig.clearing);
	}
	for (const auto &entry : _rows) {
		if (entry.first == kFakeMediaCacheTag) {
			updateRow(entry.second, &_statsBig.full);
		} else if (entry.first) {
			const auto i = _stats.tagged.find(entry.first);
			updateRow(
				entry.second,
				(i != end(_stats.tagged)) ? &i->second : nullptr);
		} else {
			const auto full = summary();
			updateRow(entry.second, &full);
		}
	}
	updateChart();
	updateDeviceBar();
}

void LocalStorage::updateChart() {
	if (!_chart) {
		return;
	}
	const auto tagged = [&](uint8 tag) {
		const auto i = _stats.tagged.find(tag);
		return (i != end(_stats.tagged)) ? i->second.totalSize : int64();
	};
	auto sizes = std::array<int64, kChartPartsCount>{ {
		tagged(Data::kImageCacheTag),
		tagged(Data::kStickerCacheTag),
		tagged(Data::kVoiceMessageCacheTag),
		tagged(Data::kVideoMessageCacheTag),
		tagged(Data::kAnimationCacheTag),
		_statsBig.full.totalSize,
	} };
	_chart->setParts(sizes, summary().totalSize);
}

void LocalStorage::updateDeviceBar() {
	if (_deviceBar) {
		_deviceBar->setCache(summary().totalSize);
	}
}

auto LocalStorage::summary() const -> Database::TaggedSummary {
	auto result = _stats.full;
	result.count += _statsBig.full.count;
	result.totalSize += _statsBig.full.totalSize;
	return result;
}

void LocalStorage::clearByTag(uint16 tag) {
	if (tag == kFakeMediaCacheTag) {
		_dbBig->clear();
	} else if (tag) {
		_db->clearByTag(tag);
	} else {
		_db->clear();
		_dbBig->clear();
		Ui::Emoji::ClearIrrelevantCache();
	}
}

void LocalStorage::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	setupControls(content);

	rpl::combine(
		_db->statsOnMain(),
		_dbBig->statsOnMain()
	) | rpl::on_next([=](
			Database::Stats &&stats,
			Database::Stats &&statsBig) {
		update(std::move(stats), std::move(statsBig));
	}, content->lifetime());

	Ui::ResizeFitChild(this, content);
}

void LocalStorage::setupControls(not_null<Ui::VerticalLayout*> container) {
	const auto createRow = [&](
			uint16 tag,
			Fn<QString(size_type)> title,
			rpl::producer<QString> clear,
			const Database::TaggedSummary &data) {
		auto result = container->add(object_ptr<Ui::SlideWrap<Row>>(
			container,
			object_ptr<Row>(
				container,
				std::move(title),
				std::move(clear),
				data)));
		const auto shown = (data.count && data.totalSize) || !tag;
		result->toggle(shown, anim::type::instant);
		result->entity()->clearRequests(
		) | rpl::on_next([=] {
			clearByTag(tag);
		}, result->lifetime());
		_rows.emplace(tag, result);
		return result;
	};
	auto tracker = Ui::MultiSlideTracker();
	const auto createTagRow = [&](uint8 tag, auto &&titleFactory) {
		static const auto empty = Database::TaggedSummary();
		const auto i = _stats.tagged.find(tag);
		const auto &data = (i != end(_stats.tagged)) ? i->second : empty;
		auto factory = std::forward<decltype(titleFactory)>(titleFactory);
		auto title = [factory = std::move(factory)](size_type count) {
			return factory(tr::now, lt_count, count);
		};
		tracker.track(createRow(
			tag,
			std::move(title),
			tr::lng_local_storage_clear_some(),
			data));
	};
	auto summaryTitle = [](size_type) {
		return tr::lng_local_storage_summary(tr::now);
	};
	auto mediaCacheTitle = [](size_type) {
		return tr::lng_local_storage_media(tr::now);
	};
	_chart = container->add(object_ptr<Chart>(container));
	_deviceBar = container->add(object_ptr<DeviceBar>(container));
	updateChart();
	updateDeviceBar();
	createRow(
		0,
		std::move(summaryTitle),
		tr::lng_local_storage_clear(),
		summary());
	setupLimits(container);
	const auto shadow = container->add(object_ptr<Ui::SlideWrap<>>(
		container,
		object_ptr<Ui::PlainShadow>(container),
		st::localStorageRowPadding));
	createTagRow(Data::kImageCacheTag, tr::lng_local_storage_image);
	createTagRow(Data::kStickerCacheTag, tr::lng_local_storage_sticker);
	createTagRow(Data::kVoiceMessageCacheTag, tr::lng_local_storage_voice);
	createTagRow(Data::kVideoMessageCacheTag, tr::lng_local_storage_round);
	createTagRow(Data::kAnimationCacheTag, tr::lng_local_storage_animation);
	tracker.track(createRow(
		kFakeMediaCacheTag,
		std::move(mediaCacheTitle),
		tr::lng_local_storage_clear_some(),
		_statsBig.full));
	shadow->toggleOn(
		std::move(tracker).atLeastOneShownValue());
}

template <
	typename Value,
	typename Convert,
	typename Callback,
	typename>
not_null<Ui::MediaSlider*> LocalStorage::createLimitsSlider(
		not_null<Ui::VerticalLayout*> container,
		int valuesCount,
		Convert &&convert,
		Value currentValue,
		Callback &&callback) {
	const auto label = container->add(
		object_ptr<Ui::LabelSimple>(container, st::localStorageLimitLabel),
		st::localStorageLimitLabelMargin);
	callback(label, currentValue);
	const auto slider = container->add(
		object_ptr<Ui::MediaSlider>(container, st::localStorageLimitSlider),
		st::localStorageLimitMargin);
	slider->resize(st::localStorageLimitSlider.seekSize);
	slider->setPseudoDiscrete(
		valuesCount,
		std::forward<Convert>(convert),
		currentValue,
		[=, callback = std::forward<Callback>(callback)](Value value) {
			callback(label, value);
		});
	return slider;
}

void LocalStorage::updateMediaLimit() {
	const auto good = [&](int64 mediaLimit) {
		return (_totalSizeLimit - mediaLimit >= kMinimalSizeLimit);
	};
	if (good(_mediaSizeLimit) || !_mediaSlider || !_mediaLabel) {
		return;
	}
	auto index = 1;
	while ((index < kMediaSizeLimitsCount)
		&& (MediaSizeLimit(index) * 2 <= _totalSizeLimit)) {
		++index;
	}
	--index;
	_mediaSizeLimit = MediaSizeLimit(index);
	_mediaSlider->setValue(index / float64(kMediaSizeLimitsCount - 1));
	updateMediaLabel();

	Ensures(good(_mediaSizeLimit));
}

void LocalStorage::updateTotalLimit() {
	const auto good = [&](int64 totalLimit) {
		return (totalLimit - _mediaSizeLimit >= kMinimalSizeLimit);
	};
	if (good(_totalSizeLimit) || !_totalSlider || !_totalLabel) {
		return;
	}
	auto index = kTotalSizeLimitsCount - 1;
	while ((index > 0)
		&& (TotalSizeLimit(index - 1) >= 2 * _mediaSizeLimit)) {
		--index;
	}
	_totalSizeLimit = TotalSizeLimit(index);
	_totalSlider->setValue(index / float64(kTotalSizeLimitsCount - 1));
	updateTotalLabel();

	Ensures(good(_totalSizeLimit));
}

void LocalStorage::updateTotalLabel() {
	Expects(_totalLabel != nullptr);

	const auto text = SizeLimitText(_totalSizeLimit);
	_totalLabel->setText(
		tr::lng_local_storage_size_limit(tr::now, lt_size, text));
}

void LocalStorage::updateMediaLabel() {
	Expects(_mediaLabel != nullptr);

	const auto text = SizeLimitText(_mediaSizeLimit);
	_mediaLabel->setText(
		tr::lng_local_storage_media_limit(tr::now, lt_size, text));
}

void LocalStorage::setupLimits(not_null<Ui::VerticalLayout*> container) {
	container->add(
		object_ptr<Ui::PlainShadow>(container),
		st::localStorageRowPadding);

	_totalSlider = createLimitsSlider(
		container,
		kTotalSizeLimitsCount,
		TotalSizeLimit,
		_totalSizeLimit,
		[=](not_null<Ui::LabelSimple*> label, int64 limit) {
			_totalSizeLimit = limit;
			_totalLabel = label;
			updateTotalLabel();
			updateMediaLimit();
			applyLimits();
		});

	_mediaSlider = createLimitsSlider(
		container,
		kMediaSizeLimitsCount,
		MediaSizeLimit,
		_mediaSizeLimit,
		[=](not_null<Ui::LabelSimple*> label, int64 limit) {
			_mediaSizeLimit = limit;
			_mediaLabel = label;
			updateMediaLabel();
			updateTotalLimit();
			applyLimits();
		});

	createLimitsSlider(
		container,
		kTimeLimitsCount,
		TimeLimit,
		LimitToValue(_timeLimit),
		[=](not_null<Ui::LabelSimple*> label, size_type limit) {
			_timeLimit = ValueToLimit(limit);
			const auto text = TimeLimitText(_timeLimit);
			label->setText(
				tr::lng_local_storage_time_limit(tr::now, lt_limit, text));
			applyLimits();
		});
}

void LocalStorage::applyLimits() {
	const auto &settings = _session->local().cacheSettings();
	const auto &settingsBig = _session->local().cacheBigFileSettings();
	const auto sizeLimit = _totalSizeLimit - _mediaSizeLimit;
	const auto changed = (settings.totalSizeLimit != sizeLimit)
		|| (settingsBig.totalSizeLimit != _mediaSizeLimit)
		|| (settings.totalTimeLimit != _timeLimit)
		|| (settingsBig.totalTimeLimit != _timeLimit);
	if (!changed) {
		return;
	}
	auto update = Storage::Cache::Database::SettingsUpdate();
	update.totalSizeLimit = sizeLimit;
	update.totalTimeLimit = _timeLimit;
	auto updateBig = Storage::Cache::Database::SettingsUpdate();
	updateBig.totalSizeLimit = _mediaSizeLimit;
	updateBig.totalTimeLimit = _timeLimit;
	_session->local().updateCacheSettings(update, updateBig);
	_session->data().cache().updateSettings(update);
}

Type LocalStorageId() {
	return LocalStorage::Id();
}

} // namespace Settings
