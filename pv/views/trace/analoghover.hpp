// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PV_VIEWS_TRACE_ANALOGHOVER_HPP
#define PV_VIEWS_TRACE_ANALOGHOVER_HPP

#include <pv/data/analogsegment.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace pv { namespace views { namespace trace {

// Use the same time coordinates in both trace and envelope rendering. Read an
// actual sample, not an interpolated value or the midpoint of a pixel envelope.
inline float analog_hover_value(const data::AnalogSegment *segment,
	const util::Timestamp &offset, double seconds_per_pixel, int x, int width)
{
	const float unavailable = std::numeric_limits<float>::quiet_NaN();
	if (!segment || x < 0 || x >= width || !std::isfinite(seconds_per_pixel) ||
		seconds_per_pixel <= 0 || !std::isfinite(segment->samplerate()))
		return unavailable;
	const uint64_t count = segment->get_sample_count();
	if (!count)
		return unavailable;
	// An unknown rate uses sample coordinates, as in paint_mid().
	const double rate = std::max(1.0, segment->samplerate());
	const util::Timestamp position =
		(offset - segment->start_time() + util::Timestamp(seconds_per_pixel) * x) * rate;
	const uint64_t last = std::min<uint64_t>(count - 1, std::numeric_limits<int64_t>::max());
	// This form also rejects NaN/infinite timestamps before integer conversion.
	const util::Timestamp tolerance("1e-9"); // Double-valued zoom roundoff, in samples.
	if (!(position >= -tolerance && position <= util::Timestamp(last) + tolerance))
		return unavailable;
	const util::Timestamp bounded = std::max(util::Timestamp(0), std::min(position, util::Timestamp(last)));
	const int64_t nearest = floor(bounded + util::Timestamp("0.5")).convert_to<int64_t>();
	const float value = segment->get_sample(nearest);
	return std::isfinite(value) ? value : unavailable;
}

}}}
#endif
