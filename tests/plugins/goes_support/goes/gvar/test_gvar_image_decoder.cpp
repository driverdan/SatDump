#include "cli_test_helpers.h"
#include "goes_support/goes/gvar/module_gvar_image_decoder.h"
#include "helpers/wait.h"
#include "pipeline/pipeline_test_helpers.h"
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// In live mode, the GVAR image decoder queues each full disk for its saving thread. These
// tests run the decoder on an empty stream and queue the disks themselves, as building GVAR
// frames that make the decoder end a disk is not needed to test the thread.

using namespace pipeline_test;
namespace pl = satdump::pipeline;

namespace
{
    // Gives access to the saving thread and its queue, which are protected
    class TestDecoder : public goes::gvar::GVARImageDecoderModule
    {
    public:
        using GVARImageDecoderModule::GVARImageDecoderModule;
        bool savingThreadJoinable() { return imageSavingThread.joinable(); }

        // Queues a small disk, the way process_frame_buffer() does when a disk ends
        void queueDisk(time_t image_time)
        {
            const int vis = 16;
            image::Image ir(16, vis / 4, vis / 4, 1);
            std::lock_guard<std::mutex> l(imageVectorMutex);
            imagesVector.push_back({ir, ir, ir, ir, image::Image(16, vis, vis, 1), 15, vis, vis, image_time, -1, -1, -75});
        }
    };

    std::vector<std::string> filesIn(const std::string &dir)
    {
        std::vector<std::string> list;
        if (std::filesystem::exists(dir))
            for (auto &e : std::filesystem::directory_iterator(dir))
                list.push_back(e.path().string());
        std::sort(list.begin(), list.end());
        return list;
    }

    size_t countOf(const std::string &s, const std::string &what)
    {
        size_t n = 0;
        for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1))
            n++;
        return n;
    }

    void setUpLive(pl::ProcessingModule &m)
    {
        m.input_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
        m.setInputType(pl::DATA_STREAM);
        m.setOutputType(pl::DATA_FILE);
        m.init();
        m.input_active = true;
    }

    void stopLive(pl::ProcessingModule &m)
    {
        m.input_active = false;
        m.input_fifo->stopReader();
        m.input_fifo->stopWriter();
        m.stop();
    }
} // namespace

TEST_CASE("GVARImageDecoderModule saves every queued disk when a live stream stops", "[plugins][goes_support][gvar_image_decoder]")
{
    ConfigGuard cfg;
    cli_test::LogCapture log;
    TempDir tmp("gvar");
    auto &general = satdump::satdump_cfg.main_cfg["satdump_general"];
    general["product_format"]["value"] = "pbm";
    general["auto_process_products"]["value"] = false;

    std::string out = tmp.file("out");
    std::filesystem::create_directories(out);

    auto decoder = std::make_shared<TestDecoder>("", out + "/gvar", nlohmann::json::object());
    setUpLive(*decoder);
    std::thread t([&decoder]() { decoder->process(); });
    // Logged once the output directories are set, which the saving thread uses
    bool started = test_helpers::waitUntil([&]() { return log.contains("Decoding to "); });

    // Three disks, one minute apart so that each has its own directory, queued just before the stream stops
    const time_t first = 1700000000;
    if (started)
        for (int i = 0; i < 3; i++)
            decoder->queueDisk(first + i * 60);
    stopLive(*decoder);
    t.join();
    REQUIRE(started);

    INFO(log.dump());
    CHECK_FALSE(decoder->savingThreadJoinable());
    CHECK(countOf(log.dump(), "Full disk finished, saving at") == 3);
    auto disks = filesIn(out + "/IMAGE/GOES-15");
    REQUIRE(disks.size() == 3);
    for (auto &d : disks)
        CHECK(std::filesystem::exists(d + "/product.cbor"));
}

TEST_CASE("GVARImageDecoderModule stops its saving thread when destroyed after process() threw", "[plugins][goes_support][gvar_image_decoder]")
{
    ConfigGuard cfg;
    cli_test::LogCapture log;
    TempDir tmp("gvar");
    auto &general = satdump::satdump_cfg.main_cfg["satdump_general"];
    general["product_format"]["value"] = "pbm";
    general["auto_process_products"]["value"] = false;

    // The parent of the output directory does not exist, so process() throws when it creates
    // IMAGE, after it has started the saving thread
    auto decoder = std::make_shared<TestDecoder>("", tmp.file("missing") + "/out/gvar", nlohmann::json::object());
    setUpLive(*decoder);
    std::atomic<bool> threw = false;
    std::thread t(
        [&]()
        {
            try
            {
                decoder->process();
            }
            catch (std::exception &)
            {
                threw = true;
            }
        });
    t.join();
    REQUIRE(threw);
    CHECK(decoder->savingThreadJoinable());

    // Destroying a module whose std::thread is still joinable would end the program
    stopLive(*decoder);
    decoder.reset();
    CHECK(log.contains("Started saving thread..."));
}
