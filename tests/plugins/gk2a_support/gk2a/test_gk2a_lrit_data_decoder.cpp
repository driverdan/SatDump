#include "gk2a_support/gk2a/module_gk2a_lrit_data_decoder.h"
#include "xrit_support/xrit_decoder_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

using namespace xrit_decoder_test;

namespace
{
    const int VCID = 2;
    const int APID = 40;

    nlohmann::json gk2aParams() { return {{"write_images", true}, {"write_additional", true}, {"write_unknown", true}}; }

    // The decoder downloads the decryption keys unless it finds them in the user directory:
    // give it an empty key file there, so it does not, and has no key
    struct NoKeys
    {
        test_helpers::TempDir dir{"gk2a_user"};
        pipeline_test::UserPathGuard user{dir.str()};

        NoKeys() { test_helpers::writeBytes(dir.file("gk2a_keys.bin"), Bytes{0, 0}); }
    };
} // namespace

TEST_CASE("GK2ALRITDataDecoderModule saves the files it does not decode", "[plugins][gk2a_support][gk2a_lrit_data_decoder]")
{
    NoKeys keys;
    DecoderEnv env;
    Mux mux;
    mux.add(VCID, packetize(unknownFile("TEST_0001.lrit", "unknown"), APID, 0));
    mux.add(VCID, packetize(lritFile(255, {annotationRecord("COMS_BIN_0001.lrit")}, Bytes{'d', 'a', 't', 'a'}), APID, 10));
    mux.add(VCID, packetize(unknownFile("TEST_0002.lrit", "encrypted", {keyRecord(1)}), APID, 20));
    env.decode<gk2a::lrit::GK2ALRITDataDecoderModule>(mux, gk2aParams());

    INFO(env.log.dump());
    CHECK(env.files() == std::vector<std::string>{"ADD/BIN/COMS_BIN_0001.bin", "LRIT/TEST_0001.lrit", "LRIT_ENCRYPTED/TEST_0002.lrit"});
    CHECK(env.read("ADD/BIN/COMS_BIN_0001.bin") == "data");
    CHECK_FALSE(env.log.contains("", slog::LOG_ERROR));
}

// Bug: module_gk2a_lrit_data_decoder_proc.cpp:115,123: the type of an additional file is
// kept with a "/" after it ("ANT/") and is then compared with "ANT", so the text files of
// type ANT are saved as .bin instead of .txt (#54).
TEST_CASE("GK2ALRITDataDecoderModule saves additional files of type ANT as text", "[plugins][gk2a_support][gk2a_lrit_data_decoder][!shouldfail]")
{
    NoKeys keys;
    DecoderEnv env;
    Mux mux;
    mux.add(VCID, packetize(lritFile(255, {annotationRecord("COMS_ANT_0001.lrit")}, Bytes{'a', 'n', 't', 's'}), APID, 0));
    env.decode<gk2a::lrit::GK2ALRITDataDecoderModule>(mux, gk2aParams());

    INFO(env.log.dump());
    CHECK(env.files() == std::vector<std::string>{"ADD/ANT/COMS_ANT_0001.txt"});
}

// A file that cannot be written is lost, with one error naming it, and the file after it is
// still saved
TEST_CASE("GK2ALRITDataDecoderModule carries on when it cannot write a file", "[plugins][gk2a_support][gk2a_lrit_data_decoder]")
{
    NoKeys keys;
    DecoderEnv env;
    Mux mux;
    std::filesystem::create_directories(env.out);
    Bytes next = unknownFile("TEST_0009.lrit", "still here");
    std::string error;
    std::string lost;

    SECTION("an LRIT file cannot be opened")
    {
        lost = "LRIT/TEST_0001.lrit";
        std::filesystem::create_directories(env.path(lost)); // A directory where the file would go
        mux.add(VCID, packetize(unknownFile("TEST_0001.lrit", "lost"), APID, 0));
        error = "Could not process file TEST_0001.lrit! cannot open " + env.path(lost) + ": ";
    }

    SECTION("an additional file cannot be opened")
    {
        lost = "ADD/BIN/COMS_BIN_0001.bin";
        std::filesystem::create_directories(env.path(lost));
        mux.add(VCID, packetize(lritFile(255, {annotationRecord("COMS_BIN_0001.lrit")}, Bytes{'l', 'o', 's', 't'}), APID, 0));
        error = "Could not process file COMS_BIN_0001.lrit! cannot open " + env.path(lost) + ": ";
    }

    SECTION("an encrypted file cannot be opened")
    {
        lost = "LRIT_ENCRYPTED/TEST_0001.lrit";
        std::filesystem::create_directories(env.path(lost));
        mux.add(VCID, packetize(unknownFile("TEST_0001.lrit", "lost", {keyRecord(1)}), APID, 0));
        error = "Could not process file TEST_0001.lrit! cannot open " + env.path(lost) + ": ";
    }

#ifdef __linux__
    SECTION("an LRIT file cannot be written")
    {
        if (!std::filesystem::exists("/dev/full"))
            SKIP("No /dev/full");
        lost = "LRIT/TEST_0001.lrit";
        std::filesystem::create_directories(env.path("LRIT"));
        std::filesystem::create_symlink("/dev/full", env.path(lost)); // Opens, but every write fails
        mux.add(VCID, packetize(unknownFile("TEST_0001.lrit", "lost"), APID, 0));
        error = "Could not process file TEST_0001.lrit! cannot write " + env.path(lost) + ": ";
    }
#endif

    mux.add(VCID, packetize(next, APID, 100));
    env.decode<gk2a::lrit::GK2ALRITDataDecoderModule>(mux, gk2aParams());

    env.checkErrorOnce(error);
    CHECK(env.read("LRIT/TEST_0009.lrit") == std::string(next.begin(), next.end()));
}
