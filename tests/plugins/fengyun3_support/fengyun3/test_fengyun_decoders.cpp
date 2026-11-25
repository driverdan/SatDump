#include "fengyun3_support/fengyun3/module_fengyun_ahrpt_decoder.h"
#include "fengyun3_support/fengyun3/module_fengyun_mpt_decoder.h"
#include "helpers/prefilled.h"
#include "pipeline/pipeline_test_helpers.h"
#include <catch2/catch_test_macros.hpp>

using namespace pipeline_test;
namespace pl = satdump::pipeline;

namespace
{
    // The Reed-Solomon results are only written for the frames the module decodes. The module
    // is built over memory filled with 0x01, so that a read of them before the first frame
    // would give 0x01010101 every time rather than whatever the heap held (#103)
    template <typename Module>
    void checkRsAvgBeforeFirstFrame(const nlohmann::json &params)
    {
        TempDir tmp("pipe_fengyun3");
        std::string input = tmp.file("in.soft");
        writeBytes(input, std::vector<uint8_t>(1000, 0));

        auto module = test_helpers::makePrefilled<Module>(0x01, input, tmp.file("out"), params);
        pl::ProcessingModule *m = module.get(); // init() is protected in the module class
        m->setInputType(pl::DATA_FILE);
        m->setOutputType(pl::DATA_FILE);
        m->init();
        CHECK(m->getModuleStats()["rs_avg"] == 0);
    }
} // namespace

TEST_CASE("FengyunAHRPTDecoderModule reports an rs_avg of 0 until it has a frame", "[pipeline][modules][fengyun_ahrpt_decoder]")
{
    checkRsAvgBeforeFirstFrame<fengyun3::FengyunAHRPTDecoderModule>({{"viterbi_outsync_after", 10}, {"viterbi_ber_thresold", 0.3}, {"invert_second_viterbi", false}});
}

TEST_CASE("FengyunMPTDecoderModule reports an rs_avg of 0 until it has a frame", "[pipeline][modules][fengyun_mpt_decoder]")
{
    checkRsAvgBeforeFirstFrame<fengyun3::FengyunMPTDecoderModule>({{"viterbi_outsync_after", 10}, {"viterbi_ber_thresold", 0.3}});
}
