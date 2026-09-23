// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include <pv/data/analog.hpp>
#include <pv/views/trace/analoghover.hpp>

using pv::data::Analog;
using pv::data::AnalogSegment;
using pv::util::Timestamp;
using pv::views::trace::analog_hover_value;

BOOST_AUTO_TEST_SUITE(AnalogHover)

BOOST_AUTO_TEST_CASE(TraceAndEnvelopeZoom)
{
	Analog data;
	auto storage = std::make_shared<AnalogSegment>(data, 0, 1024);
	auto &segment = *storage;
	std::vector<float> samples(4096);
	for (size_t i = 0; i < samples.size(); ++i) samples[i] = float(i);
	segment.append_interleaved_samples(samples.data(), samples.size(), 1);
	// Keep the same sample beneath the same stationary cursor across the
	// 64-samples/pixel rendering boundary, and at individual-sample zoom.
	for (double density : {0.125, 1.0, 63.5, 64.0, 64.5, 256.0}) {
		const double scale = density / 1024;
		const Timestamp offset = Timestamp(2048) / 1024 - Timestamp(scale) * 5;
		BOOST_CHECK_EQUAL(analog_hover_value(&segment, offset, scale, 5, 100), 2048);
	}
}

BOOST_AUTO_TEST_CASE(NearestSamplesAndBoundaries)
{
	Analog data;
	auto storage = std::make_shared<AnalogSegment>(data, 0, 1000);
	auto &segment = *storage;
	float values[] = {1, 5, -2, 9};
	segment.append_interleaved_samples(values, 4, 1);
	BOOST_CHECK_EQUAL(analog_hover_value(&segment, 0, 0.001, 0, 4), 1);
	BOOST_CHECK_EQUAL(analog_hover_value(&segment, 0, 0.001, 3, 4), 9);
	BOOST_CHECK_EQUAL(analog_hover_value(&segment, Timestamp("0.00049"), 0.001, 0, 4), 1);
	BOOST_CHECK_EQUAL(analog_hover_value(&segment, Timestamp("0.0005"), 0.001, 0, 4), 5);
	BOOST_CHECK(std::isnan(analog_hover_value(&segment, Timestamp("-0.001"), 0.001, 0, 4)));
	BOOST_CHECK(std::isnan(analog_hover_value(&segment, Timestamp("0.0031"), 0.001, 0, 4)));
	BOOST_CHECK(std::isnan(analog_hover_value(&segment, 0, 0.001, -1, 4)));
	BOOST_CHECK(std::isnan(analog_hover_value(&segment, 0, 0.001, 4, 4)));
}

BOOST_AUTO_TEST_CASE(StationaryCursorPanSegmentAndCaptureChanges)
{
	Analog data;
	auto first_storage = std::make_shared<AnalogSegment>(data, 0, 1024);
	auto second_storage = std::make_shared<AnalogSegment>(data, 1, 1024);
	auto &first = *first_storage;
	auto &second = *second_storage;
	float a[] = {1, 2, 3}, b[] = {4, 5, 6};
	first.append_interleaved_samples(a, 3, 1);
	second.append_interleaved_samples(b, 3, 1);
	first.set_start_time(Timestamp("1000000.125"));
	second.set_start_time(first.start_time());
	BOOST_CHECK_EQUAL(analog_hover_value(&first, first.start_time(), 1.0/1024, 1, 10), 2);
	BOOST_CHECK_EQUAL(analog_hover_value(&first, first.start_time()+Timestamp(1)/1024, 1.0/1024, 1, 10), 3);
	BOOST_CHECK_EQUAL(analog_hover_value(&second, second.start_time(), 1.0/1024, 1, 10), 5);
	BOOST_CHECK(std::isnan(analog_hover_value(&first, first.start_time(), 1.0/1024, 3, 10)));
	float added = 7;
	first.append_interleaved_samples(&added, 1, 1);
	BOOST_CHECK_EQUAL(analog_hover_value(&first, first.start_time(), 1.0/1024, 3, 10), 7);
	BOOST_CHECK(std::isnan(analog_hover_value(nullptr, first.start_time(), 1.0/1024, 1, 10)));
}

BOOST_AUTO_TEST_CASE(EmptySingleAndInvalidSamples)
{
	Analog data;
	auto storage = std::make_shared<AnalogSegment>(data, 0, 0);
	auto &segment = *storage;
	BOOST_CHECK(std::isnan(analog_hover_value(&segment, 0, 1, 0, 10)));
	float values[] = {7, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
	segment.append_interleaved_samples(values, 1, 1);
	BOOST_CHECK_EQUAL(analog_hover_value(&segment, 0, 1, 0, 10), 7);
	segment.append_interleaved_samples(values + 1, 2, 1);
	BOOST_CHECK(std::isnan(analog_hover_value(&segment, 0, 1, 1, 10)));
	BOOST_CHECK(std::isnan(analog_hover_value(&segment, 0, 1, 2, 10)));
	BOOST_CHECK(std::isnan(analog_hover_value(&segment, 0, 0, 0, 10)));
	BOOST_CHECK(std::isnan(analog_hover_value(&segment, Timestamp("nan"), 1, 0, 10)));
}

BOOST_AUTO_TEST_SUITE_END()
