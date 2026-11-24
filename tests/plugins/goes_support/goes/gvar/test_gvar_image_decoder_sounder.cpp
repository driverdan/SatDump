#include "cli_test_helpers.h"
#include "goes_support/goes/gvar/module_gvar_image_decoder.h"
#include "pipeline/pipeline_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <vector>

// At the end of process(), the decoder saves the sounder scan in progress only if it holds more
// than 2 sounder frames. The frame count used to be left uninitialised, so whether a scan was
// saved did not depend on the input.

using namespace pipeline_test;
namespace pl = satdump::pipeline;

namespace
{
    const size_t FRAME_SIZE = 32786;

    // A sounder block (block 11, data ID 35) of the current scan, with all positions 0
    std::vector<uint8_t> sounderFrame()
    {
        std::vector<uint8_t> f(FRAME_SIZE, 0);
        // Triple redundant primary block header
        for (int i = 0; i < 3; i++)
            f[8 + 30 * i] = 11;
        f[8 + 30 * 3 + 2] = 35; // Data ID
        return f;
    }

    // Runs the decoder on full sounder frames followed by the first partial_bytes of another one,
    // and returns the number of sounder scans saved
    size_t runOnSounderFrames(int full_frames, size_t partial_bytes)
    {
        ConfigGuard cfg;
        satdump::satdump_cfg.main_cfg["satdump_general"]["image_format"]["value"] = "pbm";
        cli_test::LogCapture log;
        TempDir tmp("gvar_sounder");
        std::string out = tmp.file("out");
        std::filesystem::create_directories(out);

        std::vector<uint8_t> data;
        auto f = sounderFrame();
        for (int i = 0; i < full_frames; i++)
            data.insert(data.end(), f.begin(), f.end());
        data.insert(data.end(), f.begin(), f.begin() + partial_bytes);
        std::string input = tmp.file("frames.bin");
        writeBytes(input, data);

        std::shared_ptr<pl::ProcessingModule> m = std::make_shared<goes::gvar::GVARImageDecoderModule>(input, out + "/gvar", nlohmann::json::object());
        m->setInputType(pl::DATA_FILE);
        m->setOutputType(pl::DATA_FILE);
        m->init();
        m->process();

        INFO(log.dump());
        size_t scans = 0;
        for (auto &e : std::filesystem::directory_iterator(out + "/SOUNDER"))
        {
            scans++;
            size_t files = 0;
            for (auto &img : std::filesystem::directory_iterator(e.path()))
                files += img.path().filename().string().rfind("Sounder_", 0) == 0;
            CHECK(files == 19);
        }
        CHECK(log.contains("New sounder scan starting") == (scans > 0));
        return scans;
    }
} // namespace

TEST_CASE("GVARImageDecoderModule saves the last sounder scan only if it received sounder frames", "[plugins][goes_support][gvar_image_decoder]")
{
    CHECK(runOnSounderFrames(0, 0) == 0);
    CHECK(runOnSounderFrames(2, 0) == 0);
    CHECK(runOnSounderFrames(3, 0) == 1);
}

TEST_CASE("GVARImageDecoderModule does not parse a frame cut short by the end of the file", "[plugins][goes_support][gvar_image_decoder]")
{
    // The last read only fills the header of the buffer, which still holds the previous frame.
    // Parsing it would count a third sounder frame.
    CHECK(runOnSounderFrames(2, 200) == 0);
}
