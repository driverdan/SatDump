#include "rtltcp.h"
#include "imgui/imgui_stdlib.h"
#include "common/widgets/stepped_slider.h"
#include <algorithm>

// set_gains(), set_bias(), set_ppm() and send_frequency() are called with settings_mtx held.
// Nothing is logged for a command dropped while the connection is lost.
void RTLTCPSource::set_gains()
{
    if (!is_started)
        return;

    if (client.setAGCMode(lna_agc_enabled))
        logger->debug("Set RTL-TCP LNA AGC to %d", (int)lna_agc_enabled);

    if (tuner_agc_enabled)
    {
        if (client.setGainMode(0))
            logger->debug("Set RTL-TCP Tuner AGC to %d", 1);
    }
    else
    {
        if (client.setGainMode(1))
            logger->debug("Set RTL-TCP Tuner AGC to %d", 0);
        if (client.setGain(gain * 10))
            logger->debug("Set RTL-TCP Gain to %d", gain * 10);
    }
}

void RTLTCPSource::set_bias()
{
    if (!is_started)
        return;
    if (client.setBiasTee(bias))
        logger->debug("Set RTL-TCP Bias to %d", (int)bias);
}

void RTLTCPSource::set_ppm()
{
    if (!is_started)
        return;
    int ppm = ppm_widget.get();
    if (client.setPPM(ppm))
        logger->debug("Set RTL-TCP PPM Correction to %d", ppm);
}

void RTLTCPSource::send_frequency(uint64_t frequency)
{
    if (is_open && is_started && client.setFrequency(frequency))
        logger->debug("Set RTL-TCP frequency to %llu", (unsigned long long)frequency);
}

void RTLTCPSource::set_settings(nlohmann::json settings)
{
    std::lock_guard<std::mutex> lock(settings_mtx);
    d_settings = settings;

    // Convert legacy AGC setting to new (was not used for RTL-TCP before, but it was for USB)
    if (d_settings.contains("agc"))
    {
        lna_agc_enabled = tuner_agc_enabled = getValueOrDefault(d_settings["agc"], false);
        d_settings.erase("agc");
    }

    ip_address = getValueOrDefault(d_settings["ip_address"], ip_address);
    port = getValueOrDefault(d_settings["port"], port);
    gain = getValueOrDefault(d_settings["gain"], gain);
    lna_agc_enabled = getValueOrDefault(d_settings["lna_agc"], lna_agc_enabled);
    tuner_agc_enabled = getValueOrDefault(d_settings["tuner_agc"], tuner_agc_enabled);
    bias = getValueOrDefault(d_settings["bias"], bias);
    ppm_widget.set(getValueOrDefault(d_settings["ppm_correction"], ppm_widget.get()));

    if (is_open && is_started)
    {
        set_gains();
        set_bias();
        set_ppm();
    }
}

nlohmann::json RTLTCPSource::get_settings()
{
    std::lock_guard<std::mutex> lock(settings_mtx);
    d_settings["ip_address"] = ip_address;
    d_settings["port"] = port;
    d_settings["gain"] = gain;
    d_settings["lna_agc"] = lna_agc_enabled;
    d_settings["tuner_agc"] = tuner_agc_enabled;
    d_settings["bias"] = bias;
    d_settings["ppm_correction"] = ppm_widget.get();

    return d_settings;
}

void RTLTCPSource::open()
{
    // Nothing to do
    is_open = true;

    // Set available samplerate
    std::vector<double> available_samplerates;
    available_samplerates.push_back(250000);
    available_samplerates.push_back(1024000);
    available_samplerates.push_back(1536000);
    available_samplerates.push_back(1792000);
    available_samplerates.push_back(1920000);
    available_samplerates.push_back(2048000);
    available_samplerates.push_back(2160000);
    available_samplerates.push_back(2400000);
    available_samplerates.push_back(2560000);
    available_samplerates.push_back(2880000);
    available_samplerates.push_back(3200000);

    samplerate_widget.set_list(available_samplerates, true);
}

void RTLTCPSource::start()
{
    if (client.connectToRTL((char *)ip_address.c_str(), port) != 1)
        throw satdump_exception("Could not connect to RTL-TCP");
    logger->info("RTL-TCP connected");

    DSPSampleSource::start(); // Do NOT reset the stream

    is_started = true;

    configure();

    thread_should_run = true;
    work_thread = std::thread(&RTLTCPSource::mainThread, this);
}

// Sends every setting to the dongle, on start and after connecting again (from the worker
// thread then: the settings are locked against the setters)
void RTLTCPSource::configure()
{
    std::lock_guard<std::mutex> lock(settings_mtx);

    uint64_t current_samplerate = samplerate_widget.get_value();

    client.setSampleRate(current_samplerate);

    send_frequency(d_frequency);

    set_gains();
    set_bias();
    set_ppm();
}

// Tries to connect again after the connection was lost, and configures the dongle again.
// Returns false once every attempt failed, or when the source is being stopped.
bool RTLTCPSource::reconnect()
{
    client.disconnect();

    std::string address;
    int address_port;
    {
        std::lock_guard<std::mutex> lock(settings_mtx);
        address = ip_address;
        address_port = port;
    }

    int delay_ms = reconnect_first_delay_ms;
    for (int attempt = 1; attempt <= reconnect_attempts; attempt++)
    {
        // The wait ends early when stop() is called
        {
            std::unique_lock<std::mutex> lock(reconnect_mtx);
            if (reconnect_cv.wait_for(lock, std::chrono::milliseconds(delay_ms), [this]() { return !thread_should_run; }))
                return false;
        }

        if (client.connectToRTL((char *)address.c_str(), address_port, [this]() { return thread_should_run.load(); }, reconnect_connect_timeout_ms))
        {
            logger->info("RTL-TCP reconnected (attempt %d/%d)", attempt, reconnect_attempts);
            configure();
            return true;
        }

        if (!thread_should_run)
            return false;
        logger->warn("RTL-TCP reconnection attempt %d/%d failed (%s)", attempt, reconnect_attempts, client.connectError().c_str());
        delay_ms = (std::min)(delay_ms * 2, reconnect_max_delay_ms); // (std::min): min may be a macro of windows.h
    }

    logger->error("RTL-TCP could not reconnect after %d attempts, no more samples will be received. Stop and start the source to try again.", reconnect_attempts);
    return false;
}

void RTLTCPSource::stop()
{
    if (is_started)
    {
        {
            std::lock_guard<std::mutex> lock(reconnect_mtx);
            thread_should_run = false;
        }
        reconnect_cv.notify_all();
        logger->info("Waiting for the thread...");
        if (is_started)
            output_stream->stopWriter();
        if (work_thread.joinable())
            work_thread.join();
        logger->info("Thread stopped");

        client.setBiasTee(false);
        client.disconnect();
    }
    is_started = false;
}

void RTLTCPSource::close()
{
    is_open = false;
}

void RTLTCPSource::set_frequency(uint64_t frequency)
{
    std::lock_guard<std::mutex> lock(settings_mtx);
    send_frequency(frequency);
    DSPSampleSource::set_frequency(frequency);
}

void RTLTCPSource::drawControlUI()
{
    std::lock_guard<std::mutex> lock(settings_mtx); // The widgets write the settings

    if (is_started)
        style::beginDisabled();

    samplerate_widget.render();

    if (is_started)
        style::endDisabled();

    if (is_started)
        style::beginDisabled();
    ImGui::InputText("Address", &ip_address);
    ImGui::InputInt("Port", &port);
    if (is_started)
        style::endDisabled();

    if (ppm_widget.draw())
        set_ppm();

    bool gain_changed = false;

    if (tuner_agc_enabled)
        style::beginDisabled();
    gain_changed |= satdump::widgets::SteppedSliderInt("Tuner Gain", &gain, 0, 49);
    if (tuner_agc_enabled)
        style::endDisabled();

    gain_changed |= ImGui::Checkbox("LNA AGC", &lna_agc_enabled);
    gain_changed |= ImGui::Checkbox("Tuner AGC", &tuner_agc_enabled);
    if (gain_changed)
        set_gains();

    if (ImGui::Checkbox("Bias-Tee", &bias))
        set_bias();
}

void RTLTCPSource::set_samplerate(uint64_t samplerate)
{
    std::lock_guard<std::mutex> lock(settings_mtx);
    if (!samplerate_widget.set_value(samplerate, 3.2e6))
        throw satdump_exception("Unsupported samplerate : " + std::to_string(samplerate) + "!");
}

uint64_t RTLTCPSource::get_samplerate()
{
    // DoubleList::get_value() writes the widget too, and configure() calls it from the worker
    std::lock_guard<std::mutex> lock(settings_mtx);
    return samplerate_widget.get_value();
}

std::vector<dsp::SourceDescriptor> RTLTCPSource::getAvailableSources()
{
    std::vector<dsp::SourceDescriptor> results;

    results.push_back({"rtltcp", "RTL-TCP", "0", false});

    return results;
}
