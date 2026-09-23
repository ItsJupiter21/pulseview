// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PV_DATA_DSP_HPP
#define PV_DATA_DSP_HPP

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <stdexcept>
#include <vector>

namespace pv
{
namespace data
{
namespace dsp
{
constexpr double pi = 3.14159265358979323846;
enum class Filter { None, Average, LowPass, HighPass, BandPass, Notch };
struct Options {
	Filter filter = Filter::None;
	unsigned window = 16;
	unsigned order = 2;
	double low = 1000, high = 10000;
};

// Streaming second-order sections. State survives chunks, never captures.
class Processor
{
	struct Section {
		double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
		double step(double x)
		{
			const double y = b0 * x + z1;
			z1 = b1 * x - a1 * y + z2;
			z2 = b2 * x - a2 * y;
			return y;
		}
	};
	Options options_;
	std::vector<Section> sections_;
	std::deque<double> history_;
	double sum_ = 0;

  public:
	void reset(const Options &o, double rate)
	{
		options_ = o;
		sections_.clear();
		history_.clear();
		sum_ = 0;
		if (o.filter == Filter::None)
			return;
		if (o.filter == Filter::Average) {
			if (!o.window || o.window > 1000000)
				throw std::invalid_argument("Average window must be 1–1000000 samples");
			return;
		}
		if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(o.low) || o.low <= 0 ||
			o.low >= rate / 2)
			throw std::invalid_argument(
				"Cutoff must be positive and below Nyquist (sample rate / 2)");
		if (o.order != 2 && o.order != 4 && o.order != 6 && o.order != 8)
			throw std::invalid_argument("Filter order must be 2, 4, 6 or 8");
		const bool band = o.filter == Filter::BandPass || o.filter == Filter::Notch;
		if (band && (!std::isfinite(o.high) || o.high <= o.low || o.high >= rate / 2))
			throw std::invalid_argument(
				"Upper cutoff must exceed lower cutoff and be below Nyquist");
		for (unsigned i = 0; i < o.order / 2; ++i) {
			const double f = band ? std::sqrt(o.low * o.high) : o.low;
			const double q =
				band ? f / (o.high - o.low) : 1 / (2 * std::cos(pi * (2 * i + 1) / (2 * o.order)));
			const double w = 2 * pi * f / rate, c = std::cos(w), a = std::sin(w) / (2 * q),
						 d = 1 + a;
			Section s{};
			if (o.filter == Filter::LowPass) {
				s.b0 = (1 - c) / 2;
				s.b1 = 1 - c;
				s.b2 = s.b0;
			}
			if (o.filter == Filter::HighPass) {
				s.b0 = (1 + c) / 2;
				s.b1 = -1 - c;
				s.b2 = s.b0;
			}
			if (o.filter == Filter::BandPass) {
				s.b0 = a;
				s.b1 = 0;
				s.b2 = -a;
			}
			if (o.filter == Filter::Notch) {
				s.b0 = 1;
				s.b1 = -2 * c;
				s.b2 = 1;
			}
			s.b0 /= d;
			s.b1 /= d;
			s.b2 /= d;
			s.a1 = -2 * c / d;
			s.a2 = (1 - a) / d;
			sections_.push_back(s);
		}
	}
	double step(double x)
	{
		if (!std::isfinite(x)) {
			history_.clear();
			sum_ = 0;
			for (auto &s : sections_)
				s.z1 = s.z2 = 0;
			return x;
		}
		if (options_.filter == Filter::Average) {
			history_.push_back(x);
			sum_ += x;
			if (history_.size() > options_.window) {
				sum_ -= history_.front();
				history_.pop_front();
			}
			return sum_ / history_.size();
		}
		for (auto &s : sections_)
			x = s.step(x);
		return x;
	}
};

enum class Window { Rectangular, Hann, Hamming, Blackman };
struct Spectrum {
	double bin_hz = 0;
	std::vector<double> magnitude;
};
// One-sided peak amplitude, corrected for window coherent gain.
inline Spectrum spectrum(const std::vector<float> &input, double rate, Window window = Window::Hann,
						 bool remove_dc = true, bool db = true)
{
	const size_t n = input.size();
	if (n < 2 || (n & (n - 1)) || n > 1048576 || !std::isfinite(rate) || rate <= 0)
		throw std::invalid_argument(
			"FFT requires 2–1048576 power-of-two samples and a positive sample rate");
	double mean = 0, gain = 0;
	for (float x : input) {
		if (!std::isfinite(x))
			throw std::invalid_argument("FFT input contains non-finite samples");
		mean += x;
	}
	mean = remove_dc ? mean / n : 0;
	std::vector<std::complex<double>> a(n);
	for (size_t i = 0; i < n; ++i) {
		const double phase = 2 * pi * i / n;
		double w = 1;
		if (window == Window::Hann)
			w = 0.5 - 0.5 * std::cos(phase);
		if (window == Window::Hamming)
			w = 0.54 - 0.46 * std::cos(phase);
		if (window == Window::Blackman)
			w = 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2 * phase);
		gain += w;
		a[i] = (input[i] - mean) * w;
	}
	for (size_t i = 1, j = 0; i < n; ++i) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
			std::swap(a[i], a[j]);
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		const std::complex<double> root = std::polar(1.0, -2 * pi / len);
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w = 1;
			for (size_t j = 0; j < len / 2; ++j) {
				auto u = a[i + j], v = a[i + j + len / 2] * w;
				a[i + j] = u + v;
				a[i + j + len / 2] = u - v;
				w *= root;
			}
		}
	}
	Spectrum result;
	result.bin_hz = rate / n;
	for (size_t i = 0; i <= n / 2; ++i) {
		double v = std::abs(a[i]) / gain * ((i == 0 || i == n / 2) ? 1 : 2);
		result.magnitude.push_back(db ? 20 * std::log10(std::max(v, 1e-12)) : v);
	}
	return result;
}
} // namespace dsp
} // namespace data
} // namespace pv
#endif
