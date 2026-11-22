#include "elektro_arktika_support/elektro_arktika/lrit/module_elektro_lrit_data_decoder.h"
#include "xrit_support/xrit_decoder_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

using namespace xrit_decoder_test;

namespace
{
    const int VCID = 2;
    const int APID = 40;
} // namespace

TEST_CASE("ELEKTROLRITDataDecoderModule saves the files it does not decode", "[plugins][elektro_arktika_support][elektro_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    Bytes file = unknownFile("TEST_0001.lrit", "unknown");
    mux.add(VCID, packetize(file, APID, 0));
    env.decode<elektro::lrit::ELEKTROLRITDataDecoderModule>(mux);

    INFO(env.log.dump());
    CHECK(env.files() == std::vector<std::string>{"LRIT/TEST_0001.lrit"});
    CHECK(env.read("LRIT/TEST_0001.lrit") == std::string(file.begin(), file.end()));
    CHECK_FALSE(env.log.contains("", slog::LOG_ERROR));
}

// A file that cannot be written is lost, with one error naming it, and the file after it is
// still saved
TEST_CASE("ELEKTROLRITDataDecoderModule carries on when it cannot write a file", "[plugins][elektro_arktika_support][elektro_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    std::filesystem::create_directories(env.out);
    Bytes next = unknownFile("TEST_0009.lrit", "still here");
    std::string lost = env.path("LRIT/TEST_0001.lrit");
    std::string error;

    SECTION("the file cannot be opened")
    {
        std::filesystem::create_directories(lost); // A directory where the file would go
        error = "Could not process file TEST_0001.lrit! cannot open " + lost + ": ";
    }

#ifdef __linux__
    SECTION("the file cannot be written")
    {
        if (!std::filesystem::exists("/dev/full"))
            SKIP("No /dev/full");
        std::filesystem::create_directories(env.path("LRIT"));
        std::filesystem::create_symlink("/dev/full", lost); // Opens, but every write fails
        error = "Could not process file TEST_0001.lrit! cannot write " + lost + ": ";
    }
#endif

    mux.add(VCID, packetize(unknownFile("TEST_0001.lrit", "lost"), APID, 0));
    mux.add(VCID, packetize(next, APID, 10));
    env.decode<elektro::lrit::ELEKTROLRITDataDecoderModule>(mux);

    env.checkErrorOnce(error);
    CHECK(env.read("LRIT/TEST_0009.lrit") == std::string(next.begin(), next.end()));
}
