#include "helpers/prefilled.h"
#include "noaa_metop_support/metop/module_metop_ahrpt_decoder.h"
#include "pipeline/pipeline_test_helpers.h"
#include <catch2/catch_test_macros.hpp>

using namespace pipeline_test;
namespace pl = satdump::pipeline;

// The Reed-Solomon results are only written for the frames the module decodes. The module is
// built over memory filled with 0x01, so that a read of them before the first frame would give
// 0x01010101 every time rather than whatever the heap held (#103)
TEST_CASE("MetOpAHRPTDecoderModule reports an rs_avg of 0 until it has a frame", "[pipeline][modules][metop_ahrpt_decoder]")
{
    TempDir tmp("pipe_metop");
    std::string input = tmp.file("in.soft");
    writeBytes(input, std::vector<uint8_t>(1000, 0));

    auto module = test_helpers::makePrefilled<metop::MetOpAHRPTDecoderModule>(0x01, input, tmp.file("out"), metop::MetOpAHRPTDecoderModule::getParams());
    pl::ProcessingModule *m = module.get(); // init() is protected in the module class
    m->setInputType(pl::DATA_FILE);
    m->setOutputType(pl::DATA_FILE);
    m->init();
    CHECK(m->getModuleStats()["rs_avg"] == 0);
}
