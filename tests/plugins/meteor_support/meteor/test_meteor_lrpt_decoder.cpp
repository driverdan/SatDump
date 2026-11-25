#include "helpers/prefilled.h"
#include "meteor_support/meteor/module_meteor_lrpt_decoder.h"
#include "pipeline/pipeline_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace pipeline_test;
namespace pl = satdump::pipeline;

// The Reed-Solomon results are only written for the frames the module decodes. The module is
// built over memory filled with 0x01, so that a read of them before the first frame would give
// 0x01010101 every time rather than whatever the heap held (#103)
TEST_CASE("METEORLRPTDecoderModule reports an rs_avg of 0 until it has a frame", "[pipeline][modules][meteor_lrpt_decoder]")
{
    TempDir tmp("pipe_meteor");
    std::string input = tmp.file("in.soft");
    writeBytes(input, std::vector<uint8_t>(1000, 0));

    // Both getModuleStats() paths compute rs_avg
    bool m2x = GENERATE(false, true);
    INFO("m2x_mode = " << m2x);
    nlohmann::json params = {{"diff_decode", false}, {"m2x_mode", m2x}, {"viterbi_outsync_after", 5}, {"viterbi_ber_thresold", 0.3}};

    auto module = test_helpers::makePrefilled<meteor::METEORLRPTDecoderModule>(0x01, input, tmp.file("out"), params);
    pl::ProcessingModule *m = module.get(); // init() is protected in the module class
    m->setInputType(pl::DATA_FILE);
    m->setOutputType(pl::DATA_FILE);
    m->init();
    CHECK(m->getModuleStats()["rs_avg"] == 0);
}
