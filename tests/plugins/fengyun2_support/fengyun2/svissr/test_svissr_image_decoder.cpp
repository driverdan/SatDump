#include "cli_test_helpers.h"
#include "fengyun2_support/fengyun2/svissr/module_svissr_image_decoder.h"
#include "helpers/compare.h"
#include "helpers/wait.h"
#include "pipeline/pipeline_test_helpers.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>
#include <thread>

// The S-VISSR image decoder is given frames made by the test: one full disk carrying a
// complete subcommunication frame, optionally followed by the imager rollback. The MANAM
// and telemetry it writes come from that subcommunication state, and in live mode they are
// written by the saving thread while the decoder goes on.

using namespace pipeline_test;
namespace pl = satdump::pipeline;

namespace
{
    const int FRAME_SIZE = 44356;
    const int SUBCOM_START_OFFSET = 196;
    const int SUBCOM_GROUP_SIZE = 2097;
    const int MANAM_START = 228;
    const int MANAM_SIZE = 410;
    const int SCID = 40;            // FY-2H
    const uint8_t DETECTORS = 0xFF; // Every detector on its primary unit

    // Gives access to the saving thread, which is protected
    class TestDecoder : public fengyun_svissr::SVISSRImageDecoderModule
    {
    public:
        using SVISSRImageDecoderModule::SVISSRImageDecoderModule;
        bool savingThreadJoinable() { return images_queue_thread.joinable(); }
    };

    uint8_t manamByte(int group, int i) { return (group * 31 + i * 7 + 1) & 0xFF; }

    std::vector<uint8_t> makeFrame(int line, int group_id, bool backward)
    {
        std::vector<uint8_t> f(FRAME_SIZE, 0);
        f[3] = backward ? 0 : 1;
        f[67] = (line >> 8) & 0x0F;
        f[68] = line & 0xFF;
        f[69] = DETECTORS;
        f[91] = SCID;
        f[193] = group_id;
        // The spare of the subcommunication group stays zero, so that it passes the integrity check
        for (int i = 0; i < MANAM_SIZE; i++)
            f[SUBCOM_START_OFFSET + MANAM_START + i] = manamByte(group_id, i);
        return f;
    }

    // A forward scan with the 25 subcommunication groups sent 8 times each, then the first
    // group of the next subcommunication frame, which completes the previous one
    std::vector<uint8_t> makeDisk(bool with_rollback)
    {
        std::vector<uint8_t> out;
        int line = 1;
        for (int group = 0; group <= 25; group++)
            for (int rep = 0; rep < 8; rep++)
            {
                auto f = makeFrame(line++, group % 25, false);
                out.insert(out.end(), f.begin(), f.end());
            }
        // The decoder detects the end of a disk on the rollback
        if (with_rollback)
            for (int i = 0; i < 25; i++)
            {
                auto f = makeFrame(line, 0, true);
                out.insert(out.end(), f.begin(), f.end());
            }
        return out;
    }

    std::vector<uint8_t> expectedManam()
    {
        std::vector<uint8_t> m;
        for (int group = 0; group < 25; group++)
            for (int i = 0; i < MANAM_SIZE; i++)
                m.push_back(manamByte(group, i));
        return m;
    }

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
} // namespace

TEST_CASE("SVISSRImageDecoderModule saves a disk with its subcommunication data", "[plugins][fengyun2_support][svissr_image_decoder]")
{
    bool live = GENERATE(false, true);
    bool with_rollback = GENERATE(false, true);
    CAPTURE(live, with_rollback);

    ConfigGuard cfg;
    cli_test::LogCapture log;
    TempDir tmp("svissr");
    auto &general = satdump::satdump_cfg.main_cfg["satdump_general"];
    general["product_format"]["value"] = "pbm";
    general["auto_process_products"]["value"] = false;

    std::vector<uint8_t> frames = makeDisk(with_rollback);
    std::string out = tmp.file("out");
    std::filesystem::create_directories(out);

    auto decoder = std::make_shared<TestDecoder>(tmp.file("frames.bin"), out + "/svissr", nlohmann::json{{"save_telemetry", true}});
    std::shared_ptr<pl::ProcessingModule> m = decoder;
    if (live)
    {
        m->input_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
        m->setInputType(pl::DATA_STREAM);
    }
    else
    {
        writeBytes(tmp.file("frames.bin"), frames);
        m->setInputType(pl::DATA_FILE);
    }
    m->setOutputType(pl::DATA_FILE);
    m->init();

    if (live)
    {
        m->input_active = true;
        std::thread t([m]() { m->process(); });
        m->input_fifo->write(frames.data(), frames.size());
        bool drained = test_helpers::waitUntil([&]() { return m->input_fifo->getReadable() == 0; });
        // The decoder is then on the last frame, or waiting for the next one
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        m->input_active = false;
        m->input_fifo->stopReader();
        m->input_fifo->stopWriter();
        m->stop();
        t.join();
        REQUIRE(drained);
        // process() has waited for the saving thread, even when no disk was in progress
        CHECK_FALSE(decoder->savingThreadJoinable());
    }
    else
    {
        m->process();
    }

    INFO(log.dump());
    CHECK(countOf(log.dump(), "Full disk end detected!") == 1);

    // One product, with the images of the five channels
    auto disks = filesIn(out + "/IMAGE");
    REQUIRE(disks.size() == 1);
    CHECK(std::filesystem::exists(disks[0] + "/product.cbor"));
    CHECK(filesIn(disks[0]).size() == 6);

    // MANAM from the subcommunication frame of the disk
    auto manam = filesIn(out + "/MANAM");
    REQUIRE(manam.size() == 1);
    CHECK(test_helpers::bytesDiff(readBytes(manam[0]), expectedManam()) == "");

    // Telemetry with the detector status of the disk's frames
    auto telemetry = filesIn(out + "/Telemetry");
    REQUIRE(telemetry.size() == 1);
    std::string tlm = readFile(telemetry[0]);
    CHECK(countOf(tlm, "\"Primary\"") == 8);
}

TEST_CASE("SVISSRImageDecoderModule keeps the subcommunication data of the next disk while saving one live", "[plugins][fengyun2_support][svissr_image_decoder]")
{
    ConfigGuard cfg;
    cli_test::LogCapture log;
    TempDir tmp("svissr");
    auto &general = satdump::satdump_cfg.main_cfg["satdump_general"];
    general["product_format"]["value"] = "pbm";
    general["auto_process_products"]["value"] = false;

    std::vector<uint8_t> disk = makeDisk(true);
    std::string out = tmp.file("out");
    std::filesystem::create_directories(out);

    auto decoder = std::make_shared<TestDecoder>("", out + "/svissr", nlohmann::json{{"save_telemetry", true}});
    std::shared_ptr<pl::ProcessingModule> m = decoder;
    m->input_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
    m->setInputType(pl::DATA_STREAM);
    m->setOutputType(pl::DATA_FILE);
    m->init();
    m->input_active = true;
    std::thread t([m]() { m->process(); });

    // The second disk is decoded while the first one is being saved, which takes much longer
    m->input_fifo->write(disk.data(), disk.size());
    bool saving = test_helpers::waitUntil([&]() { return log.contains("Full disk finished, saving at"); });
    m->input_fifo->write(disk.data(), disk.size());
    // Both are saved by the thread before the stream stops
    bool saved = test_helpers::waitUntil([&]() { return countOf(log.dump(), "Full disk finished, saving at") == 2; });
    m->input_active = false;
    m->input_fifo->stopReader();
    m->input_fifo->stopWriter();
    m->stop();
    t.join();
    REQUIRE(saving);
    REQUIRE(saved);

    INFO(log.dump());
    CHECK(countOf(log.dump(), "Full disk end detected!") == 2);
    // Both disks had a complete subcommunication frame. Their MANAM and telemetry go to the same
    // files, as the subcommunication data has no valid timestamp.
    CHECK(countOf(log.dump(), "Saving MANAM to") == 2);
    CHECK(countOf(log.dump(), "Saving telemetry to") == 2);
    CHECK_FALSE(log.contains("Reception was too short"));
}
