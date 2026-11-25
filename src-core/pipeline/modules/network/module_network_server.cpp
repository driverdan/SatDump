#include "module_network_server.h"
#include "common/net/udp.h"
#include "common/utils.h"
#include "core/exception.h"
#include "imgui/imgui.h"
#include "logger.h"
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>

namespace satdump
{
    namespace pipeline
    {
        namespace network
        {
            NetworkServerModule::NetworkServerModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) : ProcessingModule(input_file, output_file_hint, parameters)
            {
                if (parameters.contains("server_mode"))
                    mode = parameters["server_mode"].get<std::string>();

                if (parameters.count("pkt_size") > 0)
                    pkt_size = parameters["pkt_size"].get<int>();
                else
                    throw satdump_exception("pkt_size parameter must be present!");

                if (parameters.count("server_address") > 0)
                    address = parameters["server_address"].get<std::string>();
                else
                    throw satdump_exception("server_address parameter must be present!");

                if (parameters.count("server_port") > 0)
                    port = parameters["server_port"].get<int>();
                else
                    throw satdump_exception("server_port parameter must be present!");

                buffer = new uint8_t[pkt_size];
            }

            std::vector<ModuleDataType> NetworkServerModule::getInputTypes() { return {DATA_FILE, DATA_STREAM}; }

            std::vector<ModuleDataType> NetworkServerModule::getOutputTypes() { return {DATA_FILE}; }

            NetworkServerModule::~NetworkServerModule() { delete[] buffer; }

            void NetworkServerModule::process()
            {
                if (input_data_type == DATA_FILE)
                    filesize = getFilesize(d_input_file);
                else
                    filesize = 0;
                progress = 0;
                if (input_data_type == DATA_FILE)
                {
                    data_in = std::ifstream(d_input_file, std::ios::binary);
                    // Otherwise eof() is never set and the loops below never end
                    if (!data_in.is_open())
                        throw satdump_exception("Couldn't open input file " + d_input_file);
                }

                logger->info("Using input frames %s", d_input_file.c_str());

                // Reads the next buffer, returns how many bytes of it are valid. At the
                // end of a file this is less than pkt_size, possibly 0.
                auto read_buffer = [this]() -> size_t
                {
                    if (input_data_type == DATA_FILE)
                    {
                        data_in.read((char *)buffer, pkt_size);
                        progress += data_in.gcount();
                        return data_in.gcount();
                    }

                    input_fifo->read((uint8_t *)buffer, pkt_size);
                    return pkt_size;
                };

                if (mode == "default")
                {
                    nng_socket sock;
                    nng_listener listener;
                    std::string url = "tcp://" + address + ":" + std::to_string(port);

                    logger->info("Opening TCP socket on %s", url.c_str());

                    int rv = nng_pub0_open_raw(&sock);
                    if (rv != 0)
                        throw satdump_exception("Couldn't open nng socket : " + std::string(nng_strerror(rv)));

                    if ((rv = nng_listener_create(&listener, sock, url.c_str())) != 0 || (rv = nng_listener_start(listener, NNG_FLAG_NONBLOCK)) != 0)
                    {
                        nng_close(sock);
                        throw satdump_exception("Couldn't listen on " + url + " : " + std::string(nng_strerror(rv)));
                    }

                    while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
                    {
                        size_t size = read_buffer();
                        if (size > 0)
                            nng_send(sock, buffer, size, NNG_FLAG_NONBLOCK);
                    }

                    nng_close(sock); // Also closes the listener
                }
                else if (mode == "udp_send")
                {
                    net::UDPClient udp_send((char *)address.c_str(), port);

                    while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
                    {
                        size_t size = read_buffer();
                        if (size > 0)
                            udp_send.send(buffer, size);
                    }
                }

                if (input_data_type == DATA_FILE)
                    data_in.close();
            }

            void NetworkServerModule::drawUI(bool window)
            {
                ImGui::Begin("Network Server", NULL, window ? 0 : NOWINDOW_FLAGS);

                ImGui::Text("Address  : ");
                ImGui::SameLine();
                ImGui::TextColored(style::theme.green, "%s", address.c_str());

                ImGui::Text("Port    : ");
                ImGui::SameLine();
                ImGui::TextColored(style::theme.green, UITO_C_STR(port));

                if (!d_is_streaming_input)
                    ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

                ImGui::End();
            }

            std::string NetworkServerModule::getID() { return "network_server"; }

            std::shared_ptr<ProcessingModule> NetworkServerModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            {
                return std::make_shared<NetworkServerModule>(input_file, output_file_hint, parameters);
            }
        } // namespace network
    } // namespace pipeline
} // namespace satdump