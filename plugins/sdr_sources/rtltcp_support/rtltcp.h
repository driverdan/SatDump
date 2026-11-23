#pragma once

#include "common/dsp_source_sink/dsp_sample_source.h"
#include "rtltcp_client.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "core/style.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "common/widgets/double_list.h"

class RTLTCPSource : public dsp::DSPSampleSource
{
protected:
    bool is_open = false, is_started = false;
    RTLTCPClient client;

    satdump::widgets::DoubleList samplerate_widget;
    satdump::widgets::NotatedNum<int> ppm_widget;

    std::string ip_address = "0.0.0.0";
    int port = 1234;
    int gain = 10;
    bool lna_agc_enabled = false;
    bool tuner_agc_enabled = false;
    bool bias = false;

    void set_gains();
    void set_bias();
    void set_ppm();
    void send_frequency(uint64_t frequency);
    void configure();

    // The settings, against configure() sending them again from the worker thread
    std::mutex settings_mtx;

    // When the connection is lost, connecting again is tried RECONNECT_ATTEMPTS times, waiting
    // RECONNECT_FIRST_DELAY_MS before the first attempt and twice as long before each of the
    // next ones, up to RECONNECT_MAX_DELAY_MS (about 20 seconds of waits in all). An attempt
    // is given up after RECONNECT_CONNECT_TIMEOUT_MS, for a host that does not answer at all
    // (16 seconds more at worst).
    static constexpr int RECONNECT_ATTEMPTS = 8;
    static constexpr int RECONNECT_FIRST_DELAY_MS = 250;
    static constexpr int RECONNECT_MAX_DELAY_MS = 4000;
    static constexpr int RECONNECT_CONNECT_TIMEOUT_MS = 2000;
    int reconnect_attempts = RECONNECT_ATTEMPTS;
    int reconnect_first_delay_ms = RECONNECT_FIRST_DELAY_MS;
    int reconnect_max_delay_ms = RECONNECT_MAX_DELAY_MS;
    int reconnect_connect_timeout_ms = RECONNECT_CONNECT_TIMEOUT_MS;

    std::thread work_thread;

    std::atomic<bool> thread_should_run{false};
    std::mutex reconnect_mtx;
    std::condition_variable reconnect_cv; // Wakes the reconnection wait up when stopping

    bool reconnect();

    void mainThread()
    {
        uint8_t in_buf[8192];
        while (thread_should_run)
        {
            // The buffer is only output once it was filled: never the stale one
            if (!client.receiveData(in_buf, 8192))
            {
                if (!thread_should_run)
                    break;
                logger->error("RTL-TCP connection lost (%s), trying to reconnect", client.receiveError().c_str());
                if (!reconnect())
                    break; // Idle until the source is stopped (starting it again connects again)
                continue;
            }

            for (int i = 0; i < (int)8192 / 2; i++)
                output_stream->writeBuf[i] = complex_t((in_buf[i * 2 + 0] - 127.4f) / 128.0f, (in_buf[i * 2 + 1] - 127.4f) / 128.0f);
            output_stream->swap(8192 / 2);
        }
    }

public:
    RTLTCPSource(dsp::SourceDescriptor source) : DSPSampleSource(source), samplerate_widget("Samplerate"), ppm_widget("Correction##ppm", 0, "ppm")
    {
    }

    ~RTLTCPSource()
    {
        stop();
        close();
    }

    void set_settings(nlohmann::json settings);
    nlohmann::json get_settings();

    void open();
    void start();
    void stop();
    void close();

    void set_frequency(uint64_t frequency);

    void drawControlUI();

    void set_samplerate(uint64_t samplerate);
    uint64_t get_samplerate();

    static std::string getID() { return "rtltcp"; }
    static std::shared_ptr<dsp::DSPSampleSource> getInstance(dsp::SourceDescriptor source) { return std::make_shared<RTLTCPSource>(source); }
    static std::vector<dsp::SourceDescriptor> getAvailableSources();
};
