// SPDX-License-Identifier: GPL-3.0-or-later
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <boost/test/unit_test.hpp>
#include <pv/application.hpp>
#include <pv/data/analogsegment.hpp>
#include <pv/data/mathsignal.hpp>
#include <pv/devicemanager.hpp>
#include <pv/devices/device.hpp>
#include <pv/dialogs/analogtools.hpp>
#include <pv/session.hpp>

static bool until(const std::function<bool()> &done, int timeout = 5000)
{
	QElapsedTimer clock;
	clock.start();
	while (!done() && clock.elapsed() < timeout) {
		QApplication::processEvents();
		QThread::msleep(2);
	}
	return done();
}

BOOST_AUTO_TEST_CASE(AnalogScopeCaptureAndProcessing)
{
	int argc = 1;
	char name[] = "analog-scope-test";
	char *argv[] = {name, nullptr};
	Application app(argc, argv);
	QTemporaryDir temp;
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
	app.setOrganizationName("PulseViewTests");
	app.setApplicationName("AnalogScope");
	auto context = sigrok::Context::create();
	pv::DeviceManager manager(context, "demo", false);
	pv::Session session(manager, "Analog test");
	session.set_default_device();
	BOOST_REQUIRE(session.device());
	auto dev = session.device()->device();
	dev->config_set(sigrok::ConfigKey::SAMPLERATE, Glib::Variant<uint64_t>::create(10000));
	dev->config_set(sigrok::ConfigKey::LIMIT_SAMPLES, Glib::Variant<uint64_t>::create(1000));
	std::shared_ptr<pv::data::SignalBase> source;
	for (auto s : session.signalbases())
		if (s->analog_data()) {
			source = s;
			break;
		}
	BOOST_REQUIRE(source);
	int captures = 0;
	QString error;
	QObject::connect(
		&session, &pv::Session::capture_finished, &app, [&]() { ++captures; },
		Qt::QueuedConnection);
	auto failure = [&](QString text) {
		QMetaObject::invokeMethod(&app, [&, text]() { error = text; }, Qt::QueuedConnection);
	};
	session.start_capture(failure);
	BOOST_REQUIRE(until([&]() { return captures == 1 || !error.isEmpty(); }));
	session.stop_capture();
	BOOST_REQUIRE(error.isEmpty());
	auto math = std::make_shared<pv::data::MathSignal>(session);
	session.add_generated_signal(math);
	pv::data::dsp::Options o;
	o.filter = pv::data::dsp::Filter::Average;
	o.window = 4;
	math->set_processing_options(o);
	math->set_expression(source->name() + " * 2");
	auto complete = [&]() {
		auto a = math->analog_data();
		return !a->analog_segments().empty() && a->analog_segments().back()->is_complete();
	};
	BOOST_REQUIRE(until(complete));
	math->pause_generation();
	const auto raw = source->analog_data()->analog_segments().back();
	const auto generated = math->analog_data()->analog_segments().back();
	BOOST_CHECK_EQUAL(raw->get_sample_count(), generated->get_sample_count());
	double sum = 0;
	for (int i = 0; i < 20; ++i) {
		sum += 2 * raw->get_sample(i);
		if (i >= 4)
			sum -= 2 * raw->get_sample(i - 4);
		BOOST_CHECK_SMALL(generated->get_sample(i) - sum / std::min(i + 1, 4), 1e-4);
	}
	QSettings saved(temp.filePath("math.ini"), QSettings::IniFormat);
	math->save_settings(saved);
	auto restored = std::make_shared<pv::data::MathSignal>(session);
	restored->restore_settings(saved);
	BOOST_CHECK_EQUAL(restored->get_expression().toStdString(),
					  math->get_expression().toStdString());
	BOOST_CHECK_EQUAL(restored->processing_options().window, 4u);
	BOOST_CHECK(restored->processing_options().filter == o.filter);
	restored.reset();
	session.start_repeated_capture(failure);
	BOOST_REQUIRE(until([&]() { return captures >= 4 || !error.isEmpty(); }));
	session.stop_capture();
	BOOST_REQUIRE(error.isEmpty());
	const int stopped = captures;
	QElapsedTimer wait;
	wait.start();
	until([&]() { return wait.elapsed() > 400; });
	BOOST_CHECK_EQUAL(captures, stopped);
	BOOST_REQUIRE(until(complete));
	math->pause_generation();
	std::unique_ptr<QDialog> scope(pv::dialogs::create_analog_view(session, nullptr, false));
	scope->setAttribute(Qt::WA_DeleteOnClose, false);
	scope->show();
	auto *single = scope->findChild<QPushButton *>("scopeSingle");
	BOOST_REQUIRE(single);
	single->click();
	BOOST_REQUIRE(until([&]() { return captures > stopped || !error.isEmpty(); }));
	session.stop_capture();
	BOOST_REQUIRE(error.isEmpty());
	BOOST_REQUIRE(until(complete));
	math->pause_generation();
	const QString artifacts = qEnvironmentVariable("PV_TEST_ARTIFACT_DIR", temp.path());
	QDir().mkpath(artifacts);
	auto *autoset = scope->findChild<QPushButton *>("scopeAutoset");
	BOOST_REQUIRE(autoset);
	autoset->click();
	scope->grab().save(artifacts + "/scope.png");
	std::unique_ptr<QDialog> spectrum(pv::dialogs::create_analog_view(session, nullptr, true));
	spectrum->setAttribute(Qt::WA_DeleteOnClose, false);
	spectrum->show();
	QApplication::processEvents();
	auto *fft_size = spectrum->findChild<QComboBox *>("spectrumSize");
	BOOST_REQUIRE(fft_size);
	fft_size->setCurrentText("64");
	wait.restart();
	until([&]() { return wait.elapsed() > 600; });
	spectrum->grab().save(artifacts + "/spectrum.png");
	spectrum.reset();
	scope.reset();
	session.remove_generated_signal(math);
	math.reset();
	restored.reset();
}
