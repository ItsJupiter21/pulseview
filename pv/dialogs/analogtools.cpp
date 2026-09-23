// SPDX-License-Identifier: GPL-3.0-or-later
#include "analogtools.hpp"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <future>
#include <pv/binding/device.hpp>
#include <pv/data/analogsegment.hpp>
#include <pv/data/mathsignal.hpp>
#include <pv/devices/device.hpp>
#include <pv/session.hpp>

namespace pv
{
namespace dialogs
{
using data::MathSignal;
using data::SignalBase;
using sigrok::Capability;
using sigrok::ConfigKey;

static QDoubleSpinBox *number(QWidget *parent, double value, double low, double high)
{
	auto *spin = new QDoubleSpinBox(parent);
	spin->setDecimals(9);
	spin->setRange(low, high);
	spin->setValue(value);
	return spin;
}

void edit_analog_processing(Session &session, QWidget *parent, std::shared_ptr<MathSignal> signal)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(QObject::tr("Analog signal tools"));
	auto *form = new QFormLayout(&dialog);
	auto *sources = new QComboBox(&dialog);
	for (const auto &s : session.signalbases())
		if (s != signal && s->analog_data())
			sources->addItem(s->name());
	auto *expression =
		new QLineEdit(signal ? signal->get_expression() : sources->currentText(), &dialog);
	auto *use = new QPushButton(QObject::tr("Use source as expression"), &dialog);
	QObject::connect(use, &QPushButton::clicked, &dialog,
					 [=]() { expression->setText(sources->currentText()); });
	auto *filter = new QComboBox(&dialog);
	filter->addItems({QObject::tr("Math only"), QObject::tr("Rolling average"),
					  QObject::tr("Low-pass"), QObject::tr("High-pass"), QObject::tr("Band-pass"),
					  QObject::tr("Notch")});
	const auto options = signal ? signal->processing_options() : data::dsp::Options{};
	filter->setCurrentIndex(int(options.filter));
	auto *window = new QSpinBox(&dialog);
	window->setRange(1, 1000000);
	window->setValue(options.window);
	auto *order = new QComboBox(&dialog);
	order->addItems({"2", "4", "6", "8"});
	order->setCurrentText(QString::number(options.order));
	auto *low = number(&dialog, options.low, 0.000000001, 1e12);
	auto *high = number(&dialog, options.high, 0.000000001, 1e12);
	form->addRow(QObject::tr("Source"), sources);
	form->addRow(use);
	form->addRow(QObject::tr("Expression"), expression);
	form->addRow(QObject::tr("Processing"), filter);
	form->addRow(QObject::tr("Average window (samples)"), window);
	form->addRow(QObject::tr("Filter order"), order);
	form->addRow(QObject::tr("Cutoff / lower frequency (Hz)"), low);
	form->addRow(QObject::tr("Upper frequency (Hz)"), high);
	auto *note = new QLabel(
		QObject::tr("Filters process the expression output. Averaging uses available samples "
					"during startup.\n"
					"Low/high-pass: Butterworth. Band/notch: cascaded constant-Q sections.\n"
					"Raw samples are retained. Filter state resets for each segment."),
		&dialog);
	note->setWordWrap(true);
	form->addRow(note);
	auto enabled = [=]() {
		window->setEnabled(filter->currentIndex() == 1);
		order->setEnabled(filter->currentIndex() > 1);
		low->setEnabled(filter->currentIndex() > 1);
		high->setEnabled(filter->currentIndex() > 3);
	};
	QObject::connect(filter, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, enabled);
	enabled();
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	form->addRow(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
		data::dsp::Options o;
		o.filter = data::dsp::Filter(filter->currentIndex());
		o.window = window->value();
		o.order = order->currentText().toUInt();
		o.low = low->value();
		o.high = high->value();
		try {
			if (expression->text().trimmed().isEmpty())
				throw std::invalid_argument("Enter an expression or select a source");
			data::dsp::Processor validator;
			double rate = session.get_samplerate();
			if (rate > 0 || o.filter == data::dsp::Filter::Average)
				validator.reset(o, rate);
			if (!signal) {
				signal = std::make_shared<MathSignal>(session);
				session.add_generated_signal(signal);
			}
			signal->set_processing_options(o);
			signal->set_expression(expression->text());
			dialog.accept();
		} catch (const std::exception &e) {
			QMessageBox::warning(&dialog, QObject::tr("Analog processing"), e.what());
		}
	});
	dialog.exec();
}

// The plot owns snapshots so the last completed waveform remains visible while
// the next acquisition clears and replaces the session's sample storage.
class AnalogPlot : public QWidget
{
  public:
	struct Curve {
		QString name;
		QColor color;
		std::vector<double> y;
		double step = 1;
		double start = 0;
		double scale = 1;
		double offset = 0;
	};
	std::vector<Curve> curves;
	bool frequency = false, db = true, auto_range = true;
	double seconds_div = 0.001, y_div = 20, y_center = -60;
	QString message;
	explicit AnalogPlot(QWidget *parent) : QWidget(parent) { setMinimumSize(600, 360); }

  protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.fillRect(rect(), QColor(18, 23, 29));
		const int label_height = p.fontMetrics().height() + 6;
		QRectF area = QRectF(rect()).adjusted(90, label_height + 12, -60, -label_height - 16);
		p.setPen(QColor(55, 65, 72));
		for (int i = 0; i <= 10; ++i)
			p.drawLine(QPointF(area.left() + area.width() * i / 10, area.top()),
					   QPointF(area.left() + area.width() * i / 10, area.bottom()));
		for (int i = 0; i <= 8; ++i)
			p.drawLine(QPointF(area.left(), area.top() + area.height() * i / 8),
					   QPointF(area.right(), area.top() + area.height() * i / 8));
		double xmax = seconds_div * 10, lo = y_center - y_div * 4, hi = y_center + y_div * 4;
		if (frequency) {
			xmax = 1;
			if (auto_range) {
				lo = db ? -120 : 0;
				hi = db ? 0 : 1;
			}
			for (const auto &c : curves) {
				xmax = std::max(xmax, c.step * double(c.y.size() - 1));
				if (auto_range)
					for (double y : c.y)
						if (std::isfinite(y)) {
							if (!db)
								lo = std::min(lo, y);
							hi = std::max(hi, y);
						}
			}
		}
		p.setPen(Qt::lightGray);
		for (int i = 0; i <= 10; i += 2)
			p.drawText(QRectF(area.left() + area.width() * i / 10 - 55, area.bottom() + 8, 110,
							  label_height),
					   Qt::AlignCenter,
					   QString::number(xmax * i / 10, 'g', 4) + (frequency ? " Hz" : " s"));
		if (frequency)
			for (int i = 0; i <= 8; i += 2)
				p.drawText(QRectF(0, area.top() + area.height() * i / 8 - label_height / 2, 82,
								  label_height),
						   Qt::AlignRight | Qt::AlignVCenter,
						   QString::number(hi - (hi - lo) * i / 8, 'g', 4));
		p.drawText(QRectF(area.left(), 0, area.width(), label_height),
				   Qt::AlignLeft | Qt::AlignVCenter,
				   frequency ? (db ? tr("Peak amplitude: dB re 1 source unit")
								   : tr("Peak amplitude: source units"))
							 : tr("10 divisions × 8 divisions"));
		p.save();
		p.setClipRect(area);
		p.setRenderHint(QPainter::Antialiasing);
		for (const auto &c : curves) {
			p.setPen(QPen(c.color, 1));
			QPainterPath path;
			bool active = false;
			// Min/max envelopes keep narrow spikes visible at high zoom-out.
			const size_t stride =
				std::max<size_t>(1, c.y.size() / std::max(1, int(area.width() * 2)));
			for (size_t i = 0; i < c.y.size(); i += stride) {
				double vmin = c.y[i], vmax = c.y[i];
				for (size_t j = i; j < std::min(i + stride, c.y.size()); ++j) {
					vmin = std::min(vmin, c.y[j]);
					vmax = std::max(vmax, c.y[j]);
				}
				if (!std::isfinite(vmin) || !std::isfinite(vmax)) {
					active = false;
					continue;
				}
				const double x = area.left() + (c.start + i * c.step) / xmax * area.width();
				auto yp = [&](double y) {
					return frequency
							   ? area.bottom() - (y - lo) / (hi - lo) * area.height()
							   : area.center().y() - (y - c.offset) / c.scale * area.height() / 8;
				};
				QPointF point(x, yp(vmin));
				if (active)
					path.lineTo(point);
				else
					path.moveTo(point);
				path.lineTo(x, yp(vmax));
				active = true;
			}
			p.drawPath(path);
		}
		p.restore();
		p.fillRect(QRectF(area.left() + 4, area.top() + 4, std::min(320.0, area.width() - 8),
						  curves.size() * label_height),
				   QColor(18, 23, 29, 210));
		int row = 0;
		for (const auto &c : curves) {
			p.setPen(c.color);
			p.drawText(
				int(area.left() + 8), int(area.top() + label_height - 3 + row++ * label_height),
				c.name + (frequency
							  ? QString()
							  : QString("  %1 V/div  offset %2 V").arg(c.scale).arg(c.offset)));
		}
		p.setPen(Qt::lightGray);
		p.drawText(area, Qt::AlignCenter, message);
	}
};

class AnalogView : public QDialog
{
	Session &session_;
	bool frequency_;
	AnalogPlot *plot_;
	QComboBox *source_, *size_ = nullptr, *window_ = nullptr;
	QCheckBox *db_ = nullptr, *dc_ = nullptr;
	QDoubleSpinBox *time_ = nullptr, *scale_, *offset_;
	QLabel *status_;
	QWidget *hardware_ = nullptr;
	std::vector<std::shared_ptr<binding::Device>> bindings_;
	std::vector<std::pair<QString, std::shared_ptr<sigrok::Configurable>>> configurables_;
	QTimer timer_;
	std::vector<std::shared_ptr<SignalBase>> sources_;
	std::map<QString, std::pair<double, double>> ranges_;
	bool busy_ = false;
	std::future<data::dsp::Spectrum> fft_work_;
	std::weak_ptr<data::AnalogSegment> fft_segment_;
	QString fft_parameters_;
	data::dsp::Spectrum fft_cache_;
	bool fft_ready_ = false;
	QString key() const { return frequency_ ? "spectrum/" : "scope/"; }
	QVariant get(const QString &name, QVariant fallback) const
	{
		return session_.analog_tool_settings.value(key() + name, fallback);
	}
	void put(const QString &name, QVariant value)
	{
		session_.analog_tool_settings[key() + name] = value;
	}
	void populate()
	{
		const QString selected = source_->currentText().isEmpty() ? get("source", "").toString()
																  : source_->currentText();
		busy_ = true;
		source_->clear();
		sources_.clear();
		for (const auto &s : session_.signalbases())
			if (s->analog_data()) {
				sources_.push_back(s);
				source_->addItem(s->name());
			}
		int i = source_->findText(selected);
		if (i >= 0)
			source_->setCurrentIndex(i);
		busy_ = false;
	}
	void apply_ranges()
	{
		if (frequency_) {
			plot_->y_div = scale_->value();
			plot_->y_center = offset_->value();
		} else {
			ranges_[source_->currentText()] = {scale_->value(), offset_->value()};
			put("ranges/" + source_->currentText() + "/scale", scale_->value());
			put("ranges/" + source_->currentText() + "/offset", offset_->value());
			for (auto &c : plot_->curves)
				if (c.name == source_->currentText()) {
					c.scale = scale_->value();
					c.offset = offset_->value();
				}
		}
		plot_->update();
	}
	void snapshot()
	{
		if (session_.get_capture_state() != Session::Stopped)
			return;
		std::vector<AnalogPlot::Curve> curves;
		QString error;
		for (size_t i = 0; i < sources_.size(); ++i) {
			const auto &s = sources_[i];
			if (!s->enabled() || (frequency_ && int(i) != source_->currentIndex()))
				continue;
			const auto a = s->analog_data();
			if (!a || a->analog_segments().empty())
				continue;
			const auto seg = a->analog_segments().back();
			if (!seg->is_complete() || !seg->get_sample_count())
				continue;
			if (seg->samplerate() <= 0) {
				error = tr("A known sample rate is required.");
				continue;
			}
			AnalogPlot::Curve c;
			c.name = s->name();
			c.color = s->color();
			c.step = 1 / seg->samplerate();
			const uint64_t total = seg->get_sample_count();
			try {
				if (frequency_) {
					const size_t n = size_->currentText().toUInt();
					if (total < n) {
						error = tr("Need %1 samples; capture has %2.").arg(n).arg(total);
						continue;
					}
					const QString parameters = QString("%1/%2/%3/%4")
												   .arg(n)
												   .arg(window_->currentIndex())
												   .arg(dc_->isChecked())
												   .arg(db_->isChecked());
					if (fft_work_.valid()) {
						if (fft_work_.wait_for(std::chrono::seconds(0)) !=
							std::future_status::ready) {
							error = tr("Computing spectrum…");
							continue;
						}
						fft_cache_ = fft_work_.get();
						fft_ready_ = true;
					}
					if (fft_segment_.lock() != seg || fft_parameters_ != parameters ||
						!fft_ready_) {
						std::vector<float> input(n);
						seg->get_samples(total - n, total, input.data());
						const double rate = seg->samplerate();
						const auto window = data::dsp::Window(window_->currentIndex());
						const bool dc = dc_->isChecked(), db = db_->isChecked();
						fft_segment_ = seg;
						fft_parameters_ = parameters;
						fft_ready_ = false;
						fft_work_ = std::async(
							std::launch::async, [input = std::move(input), rate, window, dc, db]() {
								return data::dsp::spectrum(input, rate, window, dc, db);
							});
						error = tr("Computing spectrum…");
						continue;
					}
					c.step = fft_cache_.bin_hz;
					c.y = fft_cache_.magnitude;
				} else {
					// Bound display snapshots independently of capture size; preserve
					// per-bucket extrema rather than dropping spikes.
					const size_t count = std::min<uint64_t>(
						total,
						std::max<uint64_t>(1, std::ceil(time_->value() * 10 * seg->samplerate())));
					const size_t stride = std::max<size_t>(1, (count + 99999) / 100000);
					std::vector<float> chunk(std::min<size_t>(65536, count));
					float min = 0, max = 0;
					size_t in_bucket = 0;
					for (size_t at = 0; at < count; at += chunk.size()) {
						size_t n = std::min(chunk.size(), count - at);
						seg->get_samples(at, at + n, chunk.data());
						for (size_t j = 0; j < n; ++j) {
							float v = chunk[j];
							if (!in_bucket)
								min = max = v;
							else {
								min = std::min(min, v);
								max = std::max(max, v);
							}
							if (++in_bucket == stride) {
								c.y.push_back(min);
								c.y.push_back(max);
								in_bucket = 0;
							}
						}
					}
					if (in_bucket) {
						c.y.push_back(min);
						c.y.push_back(max);
					}
					c.step = stride / (2 * seg->samplerate());
					c.scale = get("ranges/" + s->name() + "/scale", 1.0).toDouble();
					c.offset = get("ranges/" + s->name() + "/offset", 0.0).toDouble();
					if (c.scale <= 0)
						c.scale = 1;
				}
				curves.push_back(std::move(c));
			} catch (const std::exception &e) {
				error = e.what();
			}
		}
		if (!curves.empty() || frequency_)
			plot_->curves = std::move(curves);
		plot_->message = error;
		if (plot_->curves.empty() && error.isEmpty())
			plot_->message = tr("Acquire analog samples to display a waveform.");
		plot_->update();
	}
	void capture(bool repeat)
	{
		if (!session_.device() || session_.using_file_device())
			return;
		auto dev = session_.device()->device();
		if (!dev)
			return;
		try {
			session_.stop_capture();
			for (const auto &binding : bindings_)
				binding->commit();
			if (dev->config_check(ConfigKey::TIMEBASE, Capability::SET) &&
				dev->config_check(ConfigKey::TIMEBASE, Capability::LIST)) {
				auto choices = dev->config_list(ConfigKey::TIMEBASE);
				Glib::VariantIter iter(choices);
				Glib::VariantBase choice, nearest;
				double distance = std::numeric_limits<double>::infinity(), actual = time_->value();
				while (iter.next_value(choice)) {
					if (!g_variant_is_of_type(choice.gobj(), G_VARIANT_TYPE("(tt)")))
						continue;
					guint64 num, den;
					g_variant_get(choice.gobj(), "(tt)", &num, &den);
					if (!num || !den)
						continue;
					double value = double(num) / den;
					double delta = std::abs(std::log(value / time_->value()));
					if (delta < distance) {
						distance = delta;
						actual = value;
						nearest = choice;
					}
				}
				if (nearest.gobj()) {
					dev->config_set(ConfigKey::TIMEBASE, nearest);
					time_->setValue(actual);
				}
			}
			for (const auto &entry : configurables_) {
				QVariantMap values;
				for (const auto *k :
					 {ConfigKey::TIMEBASE, ConfigKey::VDIV, ConfigKey::COUPLING,
					  ConfigKey::PROBE_FACTOR, ConfigKey::TRIGGER_SOURCE, ConfigKey::TRIGGER_SLOPE,
					  ConfigKey::TRIGGER_LEVEL, ConfigKey::CAPTURE_RATIO}) {
					if (!entry.second->config_check(k, Capability::GET) ||
						!entry.second->config_check(k, Capability::SET))
						continue;
					auto v = entry.second->config_get(k);
					gchar *text = g_variant_print(v.gobj(), true);
					values[QString::fromStdString(k->name())] = QString::fromUtf8(text);
					g_free(text);
				}
				put(entry.first, values);
			}
			if (dev->config_check(ConfigKey::LIMIT_FRAMES, Capability::SET))
				dev->config_set(ConfigKey::LIMIT_FRAMES, Glib::Variant<uint64_t>::create(1));
			else if (!dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::SET))
				throw std::runtime_error("This device does not expose finite block acquisition");
			if (dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::SET)) {
				double rate = session_.get_samplerate();
				if (dev->config_check(ConfigKey::SAMPLERATE, Capability::GET))
					rate = Glib::VariantBase::cast_dynamic<Glib::Variant<uint64_t>>(
							   dev->config_get(ConfigKey::SAMPLERATE))
							   .get();
				if (rate <= 0)
					throw std::runtime_error(
						"Set a valid sample rate in the main acquisition toolbar");
				const double count = std::ceil(time_->value() * 10 * rate);
				if (count > 100000000)
					throw std::runtime_error("Requested block exceeds 100 million samples; reduce "
											 "time/div or sample rate");
				dev->config_set(
					ConfigKey::LIMIT_SAMPLES,
					Glib::Variant<uint64_t>::create(std::max<uint64_t>(100, uint64_t(count))));
			}
			QPointer<AnalogView> self(this);
			auto report = [self](QString text) {
				if (self)
					QMetaObject::invokeMethod(
						self.data(),
						[self, text]() {
							if (self) {
								self->session_.stop_capture();
								self->status_->setText(text);
							}
						},
						Qt::QueuedConnection);
			};
			if (repeat)
				session_.start_repeated_capture(report);
			else
				session_.start_capture(report);
		} catch (const std::exception &e) {
			status_->setText(e.what());
		}
	}

  public:
	AnalogView(Session &session, QWidget *parent, bool frequency)
		: QDialog(parent), session_(session), frequency_(frequency)
	{
		if (!frequency_)
			session_.stop_capture();
		setAttribute(Qt::WA_DeleteOnClose);
		resize(1050, 700);
		put("open", true);
		setWindowTitle(frequency ? tr("Spectrum") : tr("Oscilloscope"));
		auto *layout = new QVBoxLayout(this);
		auto *body = new QHBoxLayout();
		layout->addLayout(body);
		plot_ = new AnalogPlot(this);
		plot_->frequency = frequency;
		body->addWidget(plot_, 1);
		auto *controls = new QWidget(this);
		auto *form = new QFormLayout(controls);
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setWidget(controls);
		scroll->setMinimumWidth(300);
		body->addWidget(scroll);
		source_ = new QComboBox(this);
		form->addRow(tr("Channel"), source_);
		populate();
		scale_ = number(this, get("scale", frequency ? 20.0 : 1.0).toDouble(), 1e-9, 1e12);
		offset_ = number(this, get("offset", frequency ? -60.0 : 0.0).toDouble(), -1e12, 1e12);
		form->addRow(frequency ? tr("Magnitude/div") : tr("Display V/div"), scale_);
		form->addRow(frequency ? tr("Magnitude center") : tr("Display offset (V)"), offset_);
		if (frequency) {
			size_ = new QComboBox(this);
			size_->setObjectName("spectrumSize");
			for (int n = 32; n <= 1048576; n *= 2)
				size_->addItem(QString::number(n));
			size_->setCurrentText(get("size", "4096").toString());
			window_ = new QComboBox(this);
			window_->addItems({"Rectangular", "Hann", "Hamming", "Blackman"});
			window_->setCurrentIndex(get("window", 1).toInt());
			db_ = new QCheckBox(tr("dB magnitude (re 1 source unit)"), this);
			db_->setChecked(get("db", true).toBool());
			dc_ = new QCheckBox(tr("Remove DC"), this);
			dc_->setChecked(get("dc", true).toBool());
			form->addRow(tr("FFT size"), size_);
			form->addRow(tr("Window"), window_);
			form->addRow(db_);
			form->addRow(dc_);
			auto *automatic = new QCheckBox(tr("Automatic magnitude range"), this);
			automatic->setChecked(get("auto", true).toBool());
			form->addRow(automatic);
			plot_->auto_range = automatic->isChecked();
			connect(automatic, &QCheckBox::toggled, this, [=](bool b) {
				plot_->auto_range = b;
				put("auto", b);
				plot_->update();
			});
			connect(size_, &QComboBox::currentTextChanged, this, [=](const QString &s) {
				put("size", s);
				snapshot();
			});
			connect(window_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int i) {
				put("window", i);
				snapshot();
			});
			connect(db_, &QCheckBox::toggled, this, [=](bool b) {
				put("db", b);
				plot_->db = b;
				snapshot();
			});
			plot_->db = db_->isChecked();
			connect(dc_, &QCheckBox::toggled, this, [=](bool b) {
				put("dc", b);
				snapshot();
			});
		} else {
			time_ = number(this, get("time", 0.001).toDouble(), 1e-9, 1000);
			form->addRow(tr("Time/div (s)"), time_);
			plot_->seconds_div = time_->value();
			connect(time_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
					[=](double v) {
						put("time", v);
						plot_->seconds_div = v;
						snapshot();
					});
			auto *run = new QPushButton(tr("Run"), this), *stop = new QPushButton(tr("Stop"), this),
				 *single = new QPushButton(tr("Single"), this);
			form->addRow(run, stop);
			form->addRow(single);
			connect(run, &QPushButton::clicked, this, [=]() { capture(true); });
			connect(single, &QPushButton::clicked, this, [=]() { capture(false); });
			connect(stop, &QPushButton::clicked, this, [=]() {
				session_.stop_capture();
				snapshot();
			});
			auto *fit = new QPushButton(tr("Autoset display"), this);
			fit->setObjectName("scopeAutoset");
			form->addRow(fit);
			connect(fit, &QPushButton::clicked, this, [=]() {
				for (const auto &s : sources_) {
					auto a = s->analog_data();
					if (!a || a->analog_segments().empty())
						continue;
					auto seg = a->analog_segments().back();
					if (!seg->is_complete())
						continue;
					auto mm = seg->get_min_max();
					double range = std::max(1e-9, double(mm.second - mm.first) / 6),
						   center = (double(mm.first) + mm.second) / 2;
					put("ranges/" + s->name() + "/scale", range);
					put("ranges/" + s->name() + "/offset", center);
					if (seg->samplerate() > 0)
						time_->setValue(seg->get_sample_count() / seg->samplerate() / 10);
				}
				scale_->setValue(get("ranges/" + source_->currentText() + "/scale", 1).toDouble());
				offset_->setValue(
					get("ranges/" + source_->currentText() + "/offset", 0).toDouble());
				snapshot();
			});
			hardware_ = new QGroupBox(tr("Hardware controls"), this);
			auto *hardware_form = new QFormLayout(hardware_);
			hardware_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
			form->addRow(hardware_);
			if (session_.device() && session_.device()->device() && !session_.using_file_device()) {
				auto dev = session_.device()->device();
				auto add_binding = [&](std::shared_ptr<sigrok::Configurable> c,
									   const QString &group) {
					auto hardware = std::dynamic_pointer_cast<sigrok::HardwareDevice>(dev);
					const QString settings_key =
						"hardware/" +
						(hardware ? QString::fromStdString(hardware->driver()->name())
								  : QString("unknown")) +
						"/" + group;
					std::map<std::string, Glib::VariantBase> initial;
					const QVariantMap values = get(settings_key, QVariantMap()).toMap();
					for (auto i = values.begin(); i != values.end(); ++i) {
						GError *error = nullptr;
						GVariant *value =
							g_variant_parse(nullptr, i.value().toString().toUtf8().constData(),
											nullptr, nullptr, &error);
						if (value)
							initial.emplace(i.key().toStdString(), Glib::VariantBase(value, false));
						if (error)
							g_error_free(error);
					}
					auto b = std::make_shared<binding::Device>(c, true, initial);
					if (b->properties().empty())
						return;
					if (group != "device")
						hardware_form->addRow(new QLabel(group, this));
					b->add_properties_to_form(hardware_form, false);
					bindings_.push_back(b);
					configurables_.emplace_back(settings_key, c);
				};
				add_binding(dev, "device");
				for (auto group : dev->channel_groups()) {
					add_binding(group.second, QString::fromStdString(group.first));
				}
				for (const auto *k : {ConfigKey::TRIGGER_SOURCE, ConfigKey::TRIGGER_SLOPE,
									  ConfigKey::TRIGGER_LEVEL}) {
					if (!dev->config_check(k, Capability::SET)) {
						auto *missing = new QLabel(tr("Unavailable for this device"), this);
						missing->setWordWrap(true);
						missing->setEnabled(false);
						missing->setToolTip(
							tr("The device driver does not expose this hardware control."));
						hardware_form->addRow(QString::fromStdString(k->name()), missing);
					}
				}
			}
			const bool live = session_.device() && !session_.using_file_device();
			run->setEnabled(live);
			single->setEnabled(live);
			run->setObjectName("scopeRun");
			stop->setObjectName("scopeStop");
			single->setObjectName("scopeSingle");
			connect(&session_, &Session::capture_state_changed, this, [=](int state) {
				bool stopped = state == Session::Stopped && !session_.repeating();
				run->setEnabled(stopped && live);
				single->setEnabled(stopped && live);
				hardware_->setEnabled(stopped);
				time_->setEnabled(stopped);
				status_->setText(state == Session::Stopped			 ? tr("Stopped")
								 : state == Session::AwaitingTrigger ? tr("Waiting for trigger")
																	 : tr("Acquiring"));
			});
		}
		status_ = new QLabel(this);
		status_->setWordWrap(true);
		layout->addWidget(status_);
		connect(source_, &QComboBox::currentTextChanged, this, [=](const QString &s) {
			if (busy_)
				return;
			put("source", s);
			if (!frequency_) {
				busy_ = true;
				scale_->setValue(get("ranges/" + s + "/scale", 1).toDouble());
				offset_->setValue(get("ranges/" + s + "/offset", 0).toDouble());
				busy_ = false;
			}
			snapshot();
		});
		connect(scale_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) {
			if (!busy_) {
				put("scale", v);
				apply_ranges();
			}
		});
		connect(offset_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) {
			if (!busy_) {
				put("offset", v);
				apply_ranges();
			}
		});
		connect(&session_, &Session::signals_changed, this, [=]() {
			auto previous = sources_;
			populate();
			if (previous != sources_)
				plot_->curves.clear();
			snapshot();
		});
		connect(&session_, &Session::device_changed, this, &QDialog::close);
		connect(
			&session_, &Session::capture_finished, this, [=]() { snapshot(); },
			Qt::QueuedConnection);
		connect(&timer_, &QTimer::timeout, this, [=]() { snapshot(); });
		timer_.start(250);
		if (!frequency_) {
			busy_ = true;
			scale_->setValue(get("ranges/" + source_->currentText() + "/scale", 1).toDouble());
			offset_->setValue(get("ranges/" + source_->currentText() + "/offset", 0).toDouble());
			busy_ = false;
		}
		apply_ranges();
		snapshot();
	}
	~AnalogView() override
	{
		if (!frequency_)
			session_.stop_capture();
	}

  protected:
	void closeEvent(QCloseEvent *event) override
	{
		put("open", false);
		if (!frequency_)
			session_.stop_capture();
		QDialog::closeEvent(event);
	}
};

QDialog *create_analog_view(Session &session, QWidget *parent, bool spectrum)
{
	return new AnalogView(session, parent, spectrum);
}
} // namespace dialogs
} // namespace pv
