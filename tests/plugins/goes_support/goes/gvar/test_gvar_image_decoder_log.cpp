#include "cli_test_helpers.h"
#include "goes_support/goes/gvar/module_gvar_image_decoder.h"
#include "pipeline/pipeline_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <vector>

using namespace pipeline_test;
namespace pl = satdump::pipeline;

// The input file and output directory used to be part of the logger's format, so a '%' in
// them was interpreted: "%%" was logged as "%" (and "%s" or "%n" would read arguments that
// were never passed)
TEST_CASE("GVARImageDecoderModule logs paths with a '%' as written", "[plugins][goes_support][gvar_image_decoder]")
{
    ConfigGuard cfg;
    // The decoder saves what it has at the end, which needs an image format
    satdump::satdump_cfg.main_cfg["satdump_general"]["image_format"]["value"] = "png";
    cli_test::LogCapture log;
    TempDir tmp("gvar_percent");
    std::string out = tmp.file("out_100%%");
    std::filesystem::create_directories(out);
    std::string input = tmp.file("frames_100%%.bin");
    // An empty input: the decoder logs its paths and stops at the end of the file
    writeBytes(input, std::vector<uint8_t>());

    std::shared_ptr<pl::ProcessingModule> m = std::make_shared<goes::gvar::GVARImageDecoderModule>(input, out + "/gvar", nlohmann::json::object());
    m->setInputType(pl::DATA_FILE);
    m->setOutputType(pl::DATA_FILE);
    m->init();
    m->process();

    INFO(log.dump());
    CHECK(log.contains("Using input frames " + input, slog::LOG_INFO));
    CHECK(log.contains("Decoding to " + out + "/IMAGE", slog::LOG_INFO));
}
