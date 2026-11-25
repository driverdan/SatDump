#include "net_source.h"
#include "i18n.h"
#include "imgui/imgui_stdlib.h"

void NetSource::set_settings(nlohmann::json settings)
{
    d_settings = settings;

    if (d_settings.contains("mode"))
    {
        if (d_settings["mode"].get<std::string>() == "udp")
        {
            mode = MODE_UDP;
            port = getValueOrDefault(d_settings["port"], port);
        }
        else if (d_settings["mode"].get<std::string>() == "nng_sub")
        {
            mode = MODE_NNGSUB;
            address = getValueOrDefault(d_settings["address"], address);
            port = getValueOrDefault(d_settings["port"], port);
        }
    }
}

nlohmann::json NetSource::get_settings()
{

    if (mode == MODE_UDP)
    {
        d_settings["mode"] = "udp";
        d_settings["port"] = port;
    }
    else if (mode == MODE_NNGSUB)
    {
        d_settings["mode"] = "nng_sub";
        d_settings["address"] = address;
        d_settings["port"] = port;
    }

    return d_settings;
}

void NetSource::open()
{
    // Nothing to do
    is_open = true;
}

void NetSource::run_thread()
{
    while (should_run)
    {
        if (is_started && !d_ended)
        {
            int nsamples = 0;
            std::shared_ptr<dsp::stream<complex_t>> stream; // start() replaces output_stream, so it is only read under recv_mtx

            {
                std::lock_guard<std::mutex> lock(recv_mtx);
                if (!is_started) // stop() closed the socket meanwhile
                    continue;
                stream = output_stream;

                if (mode == MODE_UDP)
                {
                    int bytes;
                    try
                    {
                        bytes = udp_server->recv((uint8_t *)stream->writeBuf, 8192 * sizeof(complex_t));
                    }
                    catch (std::exception &)
                    {
                        continue; // Timed out
                    }

                    if (bytes > 0) // -1 when interrupted on Windows
                        nsamples = bytes / sizeof(complex_t);
                }
                else if (mode == MODE_NNGSUB)
                {
                    size_t buffer_size = dsp::STREAM_BUFFER_SIZE * sizeof(complex_t);
                    size_t lpkt_size = buffer_size; // Size of the buffer in, size of the message out
                    int rv = nng_recv(n_sock, stream->writeBuf, &lpkt_size, (int)0);

                    if (rv == NNG_ETIMEDOUT)
                        continue;

                    if (rv != 0)
                    {
                        logger->error("Error receiving from tcp://%s:%d : %s", address.c_str(), port, nng_strerror(rv));
                        d_ended = true;
                        continue;
                    }

                    // nng only copies what fits in the buffer, but returns the size of the whole message
                    nsamples = std::min(lpkt_size, buffer_size) / sizeof(complex_t);
                }
            }

            if (nsamples > 0)
                stream->swap(nsamples);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void NetSource::start()
{
    if (mode == MODE_UDP)
    {
        udp_server = std::make_shared<net::UDPServer>(port);
        udp_server->enableTimeout();
    }
    else if (mode == MODE_NNGSUB)
    {
        std::string url = "tcp://" + address + ":" + std::to_string(port);
        logger->info("Opening TCP socket on %s", url.c_str());

        int rv = nng_sub0_open_raw(&n_sock);
        if (rv != 0)
            throw satdump_exception("Couldn't open nng socket : " + std::string(nng_strerror(rv)));

        // Non-blocking, so the dialer keeps retrying in the background if the server is not up yet
        if ((rv = nng_socket_set_ms(n_sock, NNG_OPT_RECVTIMEO, RECV_TIMEOUT_MS)) != 0 || (rv = nng_dialer_create(&n_dialer, n_sock, url.c_str())) != 0 ||
            (rv = nng_dialer_start(n_dialer, NNG_FLAG_NONBLOCK)) != 0)
        {
            nng_close(n_sock);
            throw satdump_exception("Couldn't connect to " + url + " : " + std::string(nng_strerror(rv)));
        }
    }

    DSPSampleSource::start();

    set_frequency(d_frequency);

    is_started = true;
}

void NetSource::stop()
{
    if (!is_started)
        return;

    is_started = false;

    {
        std::lock_guard<std::mutex> lock(recv_mtx); // Waits for a receive in progress to return or time out

        if (mode == MODE_UDP)
        {
            udp_server.reset();
        }
        else if (mode == MODE_NNGSUB)
        {
            nng_dialer_close(n_dialer);
            nng_close(n_sock);
        }
    }

    output_stream->flush();
}

void NetSource::close() { is_open = false; }

void NetSource::set_frequency(uint64_t frequency)
{
    // if (is_open && is_connected)
    // {
    //     client->setFrequency(frequency);
    //     logger->debug("Set SDR++ Server frequency to %d", frequency);
    // }
    DSPSampleSource::set_frequency(frequency);
}

void NetSource::drawControlUI()
{
    if (is_started)
        style::beginDisabled();

    current_samplerate.draw();

    if (ImGui::RadioButton(_("UDP##netsource"), mode == MODE_UDP))
        mode = MODE_UDP;
    if (ImGui::RadioButton(_("NNG Sub##netsource"), mode == MODE_NNGSUB))
        mode = MODE_NNGSUB;

    if (mode == MODE_UDP)
    {
        ImGui::InputInt(_("Port"), &port);
    }
    else if (mode == MODE_NNGSUB)
    {
        ImGui::InputText(_("Address"), &address);
        ImGui::InputInt(_("Port"), &port);
    }

    if (is_started)
        style::endDisabled();
}

void NetSource::set_samplerate(uint64_t samplerate) { current_samplerate.set(samplerate); }

uint64_t NetSource::get_samplerate() { return current_samplerate.get(); }

std::vector<dsp::SourceDescriptor> NetSource::getAvailableSources()
{
    std::vector<dsp::SourceDescriptor> results;

    results.push_back({"net_source", _("Network Source"), "0", false});

    return results;
}
