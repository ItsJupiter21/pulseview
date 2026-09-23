/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PULSEVIEW_PV_VIEWS_TRACE_ANALOGSIGNAL_HPP
#define PULSEVIEW_PV_VIEWS_TRACE_ANALOGSIGNAL_HPP

#include <memory>

#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>

#include <pv/views/trace/signal.hpp>
#include <pv/views/trace/logicsignal.hpp>

class QLabel;

using std::pair;
using std::shared_ptr;

namespace pv {

namespace data {
class Analog;
class AnalogSegment;
class SignalBase;
}

namespace views {
namespace trace {

class AnalogSignal : public LogicSignal
{
	Q_OBJECT

private:
	static const QPen AxisPen;
	static const QColor GridMajorColor, GridMinorColor;
	static const QColor SamplingPointColorLo;
	static const QColor SamplingPointColorNe;
	static const QColor SamplingPointColorHi;
	static const QColor ThresholdColor;
	static const QColor ThresholdColorLo;
	static const QColor ThresholdColorNe;
	static const QColor ThresholdColorHi;

	static const int64_t TracePaintBlockSize;
	static const float EnvelopeThreshold;

	static const int MaximumVDivs;
	static const int MaxScaleIndex, MinScaleIndex;
	static const int InfoTextMarginRight, InfoTextMarginBottom;

	enum DisplayType {
		DisplayAnalog = 0,
		DisplayConverted = 1,
		DisplayBoth = 2
	};

public:
	AnalogSignal(pv::Session &session, shared_ptr<data::SignalBase> base);

	virtual std::map<QString, QVariant> save_settings() const;
	virtual void restore_settings(std::map<QString, QVariant> settings);

	/**
	 * Computes the vertical extents of the contents of this row item.
	 * @return A pair containing the minimum and maximum y-values.
	 */
	virtual pair<int, int> v_extents() const;

	/**
	 * Paints the background layer of the signal with a QPainter
	 * @param p the QPainter to paint into.
	 * @param pp the painting parameters object to paint with..
	 */
	virtual void paint_back(QPainter &p, ViewItemPaintParams &pp);

	/**
	 * Paints the mid-layer of the signal with a QPainter
	 * @param p the QPainter to paint into.
	 * @param pp the painting parameters object to paint with..
	 */
	virtual void paint_mid(QPainter &p, ViewItemPaintParams &pp);

	/**
	 * Paints the foreground layer of the item with a QPainter
	 * @param p the QPainter to paint into.
	 * @param pp the painting parameters object to paint with.
	 */
	virtual void paint_fore(QPainter &p, ViewItemPaintParams &pp);

private:
	void paint_grid(QPainter &p, int y, int left, int right);

	void paint_trace(QPainter &p,
		const shared_ptr<pv::data::AnalogSegment> &segment,
		int y, int left, const int64_t start, const int64_t end,
		const double pixels_offset, const double samples_per_pixel);

	void paint_envelope(QPainter &p,
		const shared_ptr<pv::data::AnalogSegment> &segment,
		int y, int left, const int64_t start, const int64_t end,
		const double pixels_offset, const double samples_per_pixel);

	shared_ptr<pv::data::AnalogSegment> get_analog_segment_to_paint() const;

	/**
	 * Computes the scale factor from the scale index and vdiv settings.
	 */
	float get_resolution(int scale_index);

	void update_scale();
	virtual void update_logic_level_offsets();

	void update_conversion_widgets();

	/**
	 * Determines the closest level change (i.e. edge) to a given sample, which
	 * is useful for e.g. the "snap to edge" functionality.
	 *
	 * @param sample_pos Sample to use
	 * @return The changes left and right of the given position
	 */
	virtual vector<data::LogicSegment::EdgePair> get_nearest_level_changes(uint64_t sample_pos);

	void perform_autoranging(bool keep_divs, bool force_update);

protected:
	void populate_popup_form(QWidget *parent, QFormLayout *form);

private Q_SLOTS:
	virtual void on_setting_changed(const QString &key, const QVariant &value);

	void on_min_max_changed(float min, float max);

	void on_pos_vdivs_changed(int vdivs);
	void on_neg_vdivs_changed(int vdivs);
	void on_div_height_changed(int height);

	void on_resolution_changed(int index);

	void on_autoranging_changed(int state);

	void on_conversion_changed(int index);
	void on_conv_threshold_changed(int index);
	void on_conv_threshold_values_changed();
	void on_delayed_conversion_starter();

	void on_display_type_changed(int index);

private:
	QComboBox *resolution_cb_, *conversion_cb_, *conv_threshold_cb_,
		*display_type_cb_;
	QWidget *conv_threshold_values_;
	QLabel *conv_threshold_low_label_, *conv_threshold_high_label_;
	QDoubleSpinBox *conv_threshold_low_sb_, *conv_threshold_high_sb_;
	QSpinBox *pvdiv_sb_, *nvdiv_sb_, *div_height_sb_;

	double signal_min_, signal_max_;  // Min/max values of this signal's analog data

	bool show_analog_minor_grid_;
	QColor high_fill_color_;
	bool show_sampling_points_, fill_high_areas_;

	int conversion_threshold_disp_mode_;

	// ---------------------------------------------------------------------------
	// Note: Make sure to update save_settings() and restore_settings() when
	//       adding a trace-configurable variable here
	float scale_;
	int scale_index_;

	int div_height_;
	int pos_vdivs_, neg_vdivs_;  // divs per positive/negative side
	float resolution_;  // e.g. 10 for 10 V/div

	DisplayType display_type_;
	bool autoranging_;
};

} // namespace trace
} // namespace views
} // namespace pv

#endif // PULSEVIEW_PV_VIEWS_TRACE_ANALOGSIGNAL_HPP
