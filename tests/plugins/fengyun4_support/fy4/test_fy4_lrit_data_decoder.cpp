#include "fengyun4_support/fy4/module_fy4_lrit_data_decoder.h"
#include "xrit_support/xrit_decoder_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

using namespace xrit_decoder_test;

namespace
{
    const int VCID = 2;
    const int APID = 40;
    const int FY4_ZONE_BYTES = 1012; // Packet zone of a FengYun-4 LRIT M_PDU
} // namespace

TEST_CASE("FY4LRITDataDecoderModule saves the files it does not decode", "[plugins][fengyun4_support][fy4_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux(FY4_ZONE_BYTES);
    Bytes file = unknownFile("TEST_0001.lrit", "unknown");
    Bytes encrypted = unknownFile("TEST_0002.lrit", "encrypted", {keyRecord(1)});
    mux.add(VCID, packetize(file, APID, 0));
    mux.add(VCID, packetize(encrypted, APID, 10));
    env.decode<fy4::lrit::FY4LRITDataDecoderModule>(mux);

    INFO(env.log.dump());
    CHECK(env.files() == std::vector<std::string>{"LRIT/TEST_0001.lrit", "LRIT_ENCRYPTED/TEST_0002.lrit"});
    CHECK(env.read("LRIT/TEST_0001.lrit") == std::string(file.begin(), file.end()));
    CHECK(env.read("LRIT_ENCRYPTED/TEST_0002.lrit") == std::string(encrypted.begin(), encrypted.end()));
    CHECK_FALSE(env.log.contains("", slog::LOG_ERROR));
}

// A file that cannot be written is lost, with one error naming it, and the file after it is
// still saved
TEST_CASE("FY4LRITDataDecoderModule carries on when it cannot write a file", "[plugins][fengyun4_support][fy4_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux(FY4_ZONE_BYTES);
    std::filesystem::create_directories(env.out);
    Bytes next = unknownFile("TEST_0009.lrit", "still here");
    std::string error;

    SECTION("an LRIT file cannot be opened")
    {
        std::filesystem::create_directories(env.path("LRIT/TEST_0001.lrit")); // A directory where the file would go
        mux.add(VCID, packetize(unknownFile("TEST_0001.lrit", "lost"), APID, 0));
        error = "Could not process file TEST_0001.lrit! cannot open " + env.path("LRIT/TEST_0001.lrit") + ": ";
    }

    SECTION("an encrypted file cannot be opened")
    {
        std::filesystem::create_directories(env.path("LRIT_ENCRYPTED/TEST_0001.lrit"));
        mux.add(VCID, packetize(unknownFile("TEST_0001.lrit", "lost", {keyRecord(1)}), APID, 0));
        error = "Could not process file TEST_0001.lrit! cannot open " + env.path("LRIT_ENCRYPTED/TEST_0001.lrit") + ": ";
    }

#ifdef __linux__
    SECTION("an encrypted file cannot be written")
    {
        if (!std::filesystem::exists("/dev/full"))
            SKIP("No /dev/full");
        std::filesystem::create_directories(env.path("LRIT_ENCRYPTED"));
        std::filesystem::create_symlink("/dev/full", env.path("LRIT_ENCRYPTED/TEST_0001.lrit")); // Opens, but every write fails
        mux.add(VCID, packetize(unknownFile("TEST_0001.lrit", "lost", {keyRecord(1)}), APID, 0));
        error = "Could not process file TEST_0001.lrit! cannot write " + env.path("LRIT_ENCRYPTED/TEST_0001.lrit") + ": ";
    }
#endif

    mux.add(VCID, packetize(next, APID, 100));
    env.decode<fy4::lrit::FY4LRITDataDecoderModule>(mux);

    env.checkErrorOnce(error);
    CHECK(env.read("LRIT/TEST_0009.lrit") == std::string(next.begin(), next.end()));
}
