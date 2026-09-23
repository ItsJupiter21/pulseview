// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include <limits>
#include <pv/data/dsp.hpp>

using namespace pv::data::dsp;
BOOST_AUTO_TEST_SUITE(AnalogDsp)
BOOST_AUTO_TEST_CASE(AverageStartupAndReset)
{
	Options o;
	o.filter = Filter::Average;
	o.window = 4;
	Processor p;
	p.reset(o, 1000);
	BOOST_CHECK_CLOSE(p.step(1), 1.0, 1e-8);
	BOOST_CHECK_CLOSE(p.step(3), 2.0, 1e-8);
	BOOST_CHECK_CLOSE(p.step(5), 3.0, 1e-8);
	BOOST_CHECK_CLOSE(p.step(7), 4.0, 1e-8);
	BOOST_CHECK_CLOSE(p.step(9), 6.0, 1e-8);
	p.reset(o, 1000);
	BOOST_CHECK_CLOSE(p.step(9), 9.0, 1e-8);
	BOOST_CHECK(std::isnan(p.step(std::numeric_limits<double>::quiet_NaN())));
	BOOST_CHECK_CLOSE(p.step(2), 2.0, 1e-8);
}
static double response(Filter filter, double frequency, unsigned order = 2)
{
	Options o;
	o.filter = filter;
	o.low = 1000;
	o.high = 4000;
	o.order = order;
	Processor p;
	p.reset(o, 48000);
	double energy = 0;
	for (int i = 0; i < 48000; i++) {
		double y = p.step(std::sin(2 * pi * frequency * i / 48000));
		if (i >= 24000)
			energy += y * y;
	}
	return std::sqrt(energy / 12000);
}
BOOST_AUTO_TEST_CASE(FilterResponses)
{
	for (unsigned order : {2u, 4u, 6u, 8u}) {
		BOOST_CHECK(response(Filter::LowPass, 100, order) > 0.99);
		BOOST_CHECK(response(Filter::LowPass, 10000, order) < 0.02);
		BOOST_CHECK(response(Filter::HighPass, 100, order) < 0.02);
		BOOST_CHECK(response(Filter::HighPass, 10000, order) > 0.99);
		BOOST_CHECK_SMALL(response(Filter::LowPass, 1000, order) - std::sqrt(0.5), 1e-6);
	}
	BOOST_CHECK(response(Filter::BandPass, 2000) > 0.99);
	BOOST_CHECK(response(Filter::Notch, 2000) < 1e-8);
}
BOOST_AUTO_TEST_CASE(SpectrumCalibration)
{
	for (Window w : {Window::Rectangular, Window::Hann, Window::Hamming, Window::Blackman}) {
		std::vector<float> x(4096);
		for (size_t i = 0; i < x.size(); ++i)
			x[i] = 3 + 2 * std::sin(2 * pi * 128 * i / x.size());
		auto s = spectrum(x, 40960, w, true, false);
		BOOST_CHECK_CLOSE(s.bin_hz, 10.0, 1e-8);
		BOOST_CHECK_EQUAL(s.magnitude.size(), 2049);
		BOOST_CHECK_SMALL(s.magnitude[128] - 2, 1e-6);
		BOOST_CHECK_SMALL(s.magnitude[0], 1e-6);
		s = spectrum(x, 40960, w, false, false);
		BOOST_CHECK_SMALL(s.magnitude[0] - 3, 1e-6);
		s = spectrum(x, 40960, w, true, true);
		BOOST_CHECK_SMALL(s.magnitude[128] - 20 * std::log10(2), 1e-5);
	}
	std::vector<float> nyquist(64);
	for (size_t i = 0; i < 64; ++i)
		nyquist[i] = i % 2 ? -1 : 1;
	BOOST_CHECK_SMALL(spectrum(nyquist, 64, Window::Rectangular, false, false).magnitude[32] - 1,
					  1e-9);
}
BOOST_AUTO_TEST_CASE(InvalidParameters)
{
	Processor p;
	Options o;
	o.filter = Filter::Average;
	o.window = 0;
	BOOST_CHECK_THROW(p.reset(o, 1000), std::invalid_argument);
	o.filter = Filter::LowPass;
	o.low = 500;
	BOOST_CHECK_THROW(p.reset(o, 1000), std::invalid_argument);
	o.low = 100;
	o.order = 3;
	BOOST_CHECK_THROW(p.reset(o, 1000), std::invalid_argument);
	o.order = 2;
	o.filter = Filter::BandPass;
	o.high = 50;
	BOOST_CHECK_THROW(p.reset(o, 1000), std::invalid_argument);
	BOOST_CHECK_THROW(spectrum(std::vector<float>(63), 1000), std::invalid_argument);
	BOOST_CHECK_THROW(spectrum(std::vector<float>(64), 0), std::invalid_argument);
	std::vector<float> bad(64);
	bad[1] = std::numeric_limits<float>::infinity();
	BOOST_CHECK_THROW(spectrum(bad, 1000), std::invalid_argument);
}
BOOST_AUTO_TEST_SUITE_END()
