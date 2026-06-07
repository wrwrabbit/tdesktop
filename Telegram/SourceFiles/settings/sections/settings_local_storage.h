/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"
#include "storage/cache/storage_cache_database.h"

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Storage {
namespace Cache {
class Database;
} // namespace Cache
} // namespace Storage

namespace Ui {
class VerticalLayout;
template <typename Widget>
class SlideWrap;
class LabelSimple;
class MediaSlider;
class BoxContent;
} // namespace Ui

namespace Settings {

[[nodiscard]] Type LocalStorageId();

class LocalStorage : public Section<LocalStorage> {
public:
	using Database = Storage::Cache::Database;

	LocalStorage(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;
	void showFinished() override;

private:
	class Row;
	class Chart;
	class DeviceBar;

	void setupContent();
	void updateChart();
	void updateDeviceBar();
	void showClearingBox();
	void clearByTag(uint16 tag);
	void update(Database::Stats &&stats, Database::Stats &&statsBig);
	void updateRow(
		not_null<Ui::SlideWrap<Row>*> row,
		const Database::TaggedSummary *data);
	void setupControls(not_null<Ui::VerticalLayout*> container);
	void setupLimits(not_null<Ui::VerticalLayout*> container);
	void updateMediaLimit();
	void updateTotalLimit();
	void updateTotalLabel();
	void updateMediaLabel();
	void applyLimits();

	[[nodiscard]] Database::TaggedSummary summary() const;

	template <
		typename Value,
		typename Convert,
		typename Callback,
		typename = std::enable_if_t<
			rpl::details::is_callable_plain_v<
				Callback,
				not_null<Ui::LabelSimple*>,
				Value>
			&& std::is_same_v<Value, decltype(std::declval<Convert>()(1))>>>
	not_null<Ui::MediaSlider*> createLimitsSlider(
		not_null<Ui::VerticalLayout*> container,
		int valuesCount,
		Convert &&convert,
		Value currentValue,
		Callback &&callback);

	const not_null<Main::Session*> _session;
	const not_null<Storage::Cache::Database*> _db;
	const not_null<Storage::Cache::Database*> _dbBig;

	Database::Stats _stats;
	Database::Stats _statsBig;

	base::flat_map<uint16, not_null<Ui::SlideWrap<Row>*>> _rows;
	Chart *_chart = nullptr;
	DeviceBar *_deviceBar = nullptr;
	Ui::MediaSlider *_totalSlider = nullptr;
	Ui::LabelSimple *_totalLabel = nullptr;
	Ui::MediaSlider *_mediaSlider = nullptr;
	Ui::LabelSimple *_mediaLabel = nullptr;

	int64 _totalSizeLimit = 0;
	int64 _mediaSizeLimit = 0;
	size_type _timeLimit = 0;

	bool _clearing = false;
	bool _clearRequested = false;
	int64 _clearFreedBase = 0;
	base::weak_qptr<Ui::BoxContent> _clearingBox;

};

} // namespace Settings
