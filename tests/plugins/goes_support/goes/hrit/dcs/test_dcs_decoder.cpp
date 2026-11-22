#include "cli_test_helpers.h"
#include "goes_support/goes/hrit/module_goes_lrit_data_decoder.h"
#include "helpers/temp_files.h"
#include "helpers/wait.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <future>
#include <thread>

// processDCS() is given a DCS file holding one DCP message, and writes what it decoded of it
// to DCS/<file name>.json. The DCP of the message is put in the PDT list the decoder uses.

using test_helpers::readFile;
using test_helpers::TempDir;

namespace
{
    const uint32_t ADDRESS = 0x5a5a0037;
    const std::string FILE_NAME = "pH-24123120000-A";

    // The decoder, with access to its DCS parser and its list of DCPs
    struct DCSDecoder : goes::hrit::GOESLRITDataDecoderModule
    {
        DCSDecoder(const std::string &dir)
            : goes::hrit::GOESLRITDataDecoderModule(
                  "", dir + "/goes_hrit",
                  {{"max_fill_lines", 50}, {"write_images", false}, {"write_emwin", false}, {"write_messages", false}, {"write_dcs", false}, {"write_unknown", false}, {"parse_dcs", true}})
        {
            directory = dir;
        }

        ~DCSDecoder() { dcp_list.erase(ADDRESS); }

        bool process(std::vector<uint8_t> &file) { return processDCS(file.data(), file.size()); }

        // process() on a thread of its own, so that a parser that never returns fails the test
        // instead of stalling the run. The thread is then left running, with its own references
        // to the decoder and the file.
        static bool processBounded(std::shared_ptr<DCSDecoder> decoder, std::vector<uint8_t> file)
        {
            std::packaged_task<bool()> task([decoder, file]() mutable { return decoder->process(file); });
            std::future<bool> result = task.get_future();
            std::thread(std::move(task)).detach();
            if (result.wait_for(test_helpers::kTimeout) != std::future_status::ready)
                FAIL("processDCS() did not return");
            return result.get();
        }

        void addDCP(const std::vector<std::string> &physical_elements)
        {
            auto dcp = std::make_shared<goes::hrit::DCP>();
            for (auto &name : physical_elements)
            {
                goes::hrit::PEInfo pe;
                pe.name = name;
                pe.record_interval = 15;
                dcp->pe_info.push_back(pe);
            }
            dcp_list[ADDRESS] = dcp;
        }
    };

    // A DCS file of one DCP message from ADDRESS, with this body. The CRCs are left at 0, the
    // decoder only reports them.
    std::vector<uint8_t> dcsFile(const std::string &body)
    {
        std::vector<uint8_t> block(40, 0);
        block[0] = 1;
        size_t block_length = block.size() + body.size() + 2;
        block[1] = block_length & 0xff;
        block[2] = block_length >> 8;
        for (int i = 0; i < 4; i++)
            block[8 + i] = (ADDRESS >> (8 * i)) & 0xff;
        block.insert(block.end(), body.begin(), body.end());
        block.insert(block.end(), 2, 0);

        size_t size = 64 + block.size() + 4;
        char size_str[9];
        snprintf(size_str, sizeof(size_str), "%08zu", size);
        std::string name = FILE_NAME + std::string(32 - FILE_NAME.size(), ' ');
        std::string header = name + size_str + "SRC " + "DCSH" + std::string(12, ' ') + std::string(4, '\0');

        std::vector<uint8_t> file(header.begin(), header.end());
        file.insert(file.end(), block.begin(), block.end());
        file.insert(file.end(), 4, 0);
        return file;
    }

    // The message the decoder wrote for this body, from a DCP with these physical elements
    nlohmann::json decode(const std::vector<std::string> &physical_elements, const std::string &body)
    {
        cli_test::LogCapture log;
        TempDir tmp("goes_dcs");
        std::filesystem::create_directories(tmp.file("DCS"));

        auto decoder = std::make_shared<DCSDecoder>(tmp.str());
        decoder->addDCP(physical_elements);
        REQUIRE(DCSDecoder::processBounded(decoder, dcsFile(body)));

        nlohmann::json out = nlohmann::json::parse(readFile(tmp.file("DCS/" + FILE_NAME + ".json")));
        REQUIRE(out["blocks"].size() == 1);
        return out["blocks"][0];
    }
} // namespace

TEST_CASE("DCS Pseudobinary B messages are decoded with the physical elements of their DCP", "[plugins][goes_support][goes_dcs]")
{
    SECTION("B1: one reading of one element, then the battery voltage")
    {
        nlohmann::json msg = decode({"Stage"}, "B1A@A@L");
        CHECK(msg["data_type"] == "Pseudobinary B");
        REQUIRE(msg["data_values"].size() == 2);
        CHECK(msg["data_values"][0]["name"] == "Stage");
        CHECK(msg["data_values"][0]["values"] == nlohmann::json::array({"0.640000"}));
        CHECK(msg["data_values"][1]["name"] == "Voltage - battery (volt)");
        CHECK(msg["data_values"][1]["values"] == nlohmann::json::array({"13.408000"}));
    }

    SECTION("B1: the data ends at the first space")
    {
        nlohmann::json msg = decode({"Stage"}, "B1A@A@L junk");
        CHECK(msg["data_type"] == "Pseudobinary B");
        REQUIRE(msg["data_values"].size() == 2);
        CHECK(msg["data_values"][0]["values"] == nlohmann::json::array({"0.640000"}));
    }

    SECTION("B1: two readings of each of two elements, one of them missing")
    {
        nlohmann::json msg = decode({"Stage", "Precipitation"}, "B1A@A@///@B@@C@L");
        CHECK(msg["data_type"] == "Pseudobinary B");
        REQUIRE(msg["data_values"].size() == 3);
        CHECK(msg["data_values"][0]["name"] == "Stage");
        CHECK(msg["data_values"][0]["values"] == nlohmann::json::array({"0.640000", "0"}));
        CHECK(msg["data_values"][1]["name"] == "Precipitation");
        CHECK(msg["data_values"][1]["values"] == nlohmann::json::array({"1.280000", "1.920000"}));
        CHECK(msg["data_values"][2]["name"] == "Voltage - battery (volt)");
        CHECK(msg["data_values"][2]["values"] == nlohmann::json::array({"13.408000"}));
    }

    SECTION("BC: the last element of the DCP is the battery voltage, which is not read as a 3 byte value")
    {
        nlohmann::json msg = decode({"Stage", "Voltage - battery (volt)"}, "BCA@A@L");
        CHECK(msg["data_type"] == "Pseudobinary B");
        REQUIRE(msg["data_values"].size() == 2);
        CHECK(msg["data_values"][0]["name"] == "Stage");
        CHECK(msg["data_values"][0]["values"] == nlohmann::json::array({"0.640000"}));
        CHECK(msg["data_values"][1]["name"] == "Voltage - battery (volt)");
        CHECK(msg["data_values"][1]["values"] == nlohmann::json::array({"4.041000"}));
    }
}

// #37: these took a modulo by 0 (SIGFPE), or read far past the end of the message
TEST_CASE("DCS Pseudobinary B messages that cannot be decoded are left undecoded", "[plugins][goes_support][goes_dcs]")
{
    SECTION("BC from a DCP whose only element is the battery voltage: no other element to read")
    {
        nlohmann::json msg = decode({"Voltage - battery (volt)"}, "BCA@A@L");
        CHECK(msg["data_type"] == "Unknown");
        CHECK(msg["data_values"].empty());
        CHECK(msg["data_ascii"] == "BCA@A@L");
    }

    SECTION("a space before the data: a negative number of data bytes")
    {
        nlohmann::json msg = decode({"Stage"}, "B1A @A@L");
        CHECK(msg["data_type"] == "Unknown");
        CHECK(msg["data_values"].empty());
    }

    SECTION("no data bytes at all")
    {
        nlohmann::json msg = decode({"Stage"}, "B1AL junk");
        CHECK(msg["data_type"] == "Unknown");
        CHECK(msg["data_values"].empty());
    }

    SECTION("a DCP with no physical element")
    {
        nlohmann::json msg = decode({}, "B1A@A@L");
        CHECK(msg["data_type"] == "Unknown");
        CHECK(msg["data_values"].empty());
    }

    SECTION("data that is not a whole number of readings of each element")
    {
        nlohmann::json msg = decode({"Stage", "Precipitation"}, "B1A@A@L");
        CHECK(msg["data_type"] == "Unknown");
        CHECK(msg["data_values"].empty());
    }
}

namespace
{
    // A block of this ID with this length field, and as many bytes (at least its 3 byte header)
    std::vector<uint8_t> dcsBlock(uint8_t id, size_t length)
    {
        std::vector<uint8_t> block(length < 3 ? 3 : length, 0);
        block[0] = id;
        block[1] = length & 0xff;
        block[2] = length >> 8;
        return block;
    }

    // A DCS file holding these blocks, whose size field is its size. The CRCs are left at 0.
    std::vector<uint8_t> dcsFileOf(const std::vector<uint8_t> &blocks)
    {
        size_t size = 64 + blocks.size() + 4;
        char size_str[9];
        snprintf(size_str, sizeof(size_str), "%08zu", size);
        std::string name = FILE_NAME + std::string(32 - FILE_NAME.size(), ' ');
        std::string header = name + size_str + "SRC " + "DCSH" + std::string(12, ' ') + std::string(4, '\0');

        std::vector<uint8_t> file(header.begin(), header.end());
        file.insert(file.end(), blocks.begin(), blocks.end());
        file.insert(file.end(), 4, 0);
        return file;
    }

    // processDCS() of this file, which must not be parsed
    void checkRejected(std::vector<uint8_t> file, const std::string &warning)
    {
        cli_test::LogCapture log;
        TempDir tmp("goes_dcs");
        std::filesystem::create_directories(tmp.file("DCS"));
        CHECK_FALSE(DCSDecoder::processBounded(std::make_shared<DCSDecoder>(tmp.str()), file));
        CHECK(log.contains(warning));
        CHECK_FALSE(std::filesystem::exists(tmp.file("DCS/" + FILE_NAME + ".json")));
    }
} // namespace

// #67: the block CRC loop ran 64 KiB past a block of length 0 or 1 (and never ended for 0),
// and the fields of a short missed message block were read past it. The minimum lengths are
// those of the HRIT DCS file format: 41 for a DCP message with no data, 29 for a missed message.
TEST_CASE("DCS blocks shorter than their fields are not parsed", "[plugins][goes_support][goes_dcs]")
{
    SECTION("a DCP message block of length 0") { checkRejected(dcsFileOf(dcsBlock(1, 0)), "Invalid DCS block length"); }
    SECTION("a DCP message block of length 1") { checkRejected(dcsFileOf(dcsBlock(1, 1)), "Invalid DCS block length"); }
    SECTION("a DCP message block of length 2") { checkRejected(dcsFileOf(dcsBlock(1, 2)), "Invalid DCS block length"); }
    SECTION("a DCP message block that ends inside its header") { checkRejected(dcsFileOf(dcsBlock(1, 40)), "Invalid DCS block length"); }
    SECTION("a missed message block that ends inside its header") { checkRejected(dcsFileOf(dcsBlock(2, 26)), "Invalid DCS block length"); }
    SECTION("a missed message block that ends inside its CRC") { checkRejected(dcsFileOf(dcsBlock(2, 28)), "Invalid DCS block length"); }
    SECTION("a block of an unknown type shorter than a block header and CRC") { checkRejected(dcsFileOf(dcsBlock(3, 4)), "Invalid DCS block length"); }
}

TEST_CASE("DCS blocks of the smallest length are parsed", "[plugins][goes_support][goes_dcs]")
{
    cli_test::LogCapture log;
    TempDir tmp("goes_dcs");
    std::filesystem::create_directories(tmp.file("DCS"));

    // A message, one with no data, a missed message, and a message
    std::vector<uint8_t> no_data = dcsBlock(1, 41);
    no_data[39] = 0x7f; // Its CRC, not a first data byte
    no_data[40] = 0x01;
    std::vector<uint8_t> blocks;
    for (auto block : {dcsBlock(1, 50), no_data, dcsBlock(2, 29), dcsBlock(1, 50)})
        blocks.insert(blocks.end(), block.begin(), block.end());
    REQUIRE(DCSDecoder::processBounded(std::make_shared<DCSDecoder>(tmp.str()), dcsFileOf(blocks)));

    nlohmann::json out = nlohmann::json::parse(readFile(tmp.file("DCS/" + FILE_NAME + ".json")));
    REQUIRE(out["blocks"].size() == 4);
    CHECK(out["blocks"][0]["type"] == "DCS Message");
    CHECK(out["blocks"][1]["type"] == "DCS Message");
    CHECK(out["blocks"][1]["header"]["type_id"] == 0);
    CHECK(out["blocks"][1]["data_ascii"] == "");
    CHECK(out["blocks"][2]["type"] == "Missed Message");
    CHECK(out["blocks"][3]["type"] == "DCS Message");
}

// #67: the header was read before the size was known to hold it
TEST_CASE("DCS files shorter than their header and CRC are not parsed", "[plugins][goes_support][goes_dcs]")
{
    // Its size field is right: "00000040"
    std::string header = FILE_NAME + std::string(32 - FILE_NAME.size(), ' ') + "00000040";
    checkRejected(std::vector<uint8_t>(header.begin(), header.end()), "Invalid DCS File size");
}

TEST_CASE("DCS Pseudobinary C messages are decoded by sensor", "[plugins][goes_support][goes_dcs]")
{
    // Sensor A, day @A, minute @A, every 15 minutes (@O), one reading of 0.64 (@A@)
    SECTION("one sensor")
    {
        nlohmann::json msg = decode({}, "C1+A@A@A@O@A@.");
        CHECK(msg["data_type"] == "Pseudobinary C");
        REQUIRE(msg["data_values"].size() == 1);
        CHECK(msg["data_values"][0]["name"] == "Sensor 1");
        CHECK(msg["data_values"][0]["interval"] == 15);
        CHECK(msg["data_values"][0]["values"] == nlohmann::json::array({"0.640000"}));
    }

    // #67: its minute of day and interval were read past the end of the block
    SECTION("a sensor block too short for its header is skipped")
    {
        nlohmann::json msg = decode({}, "C1+A@A@A@O@A@+B@.");
        CHECK(msg["data_type"] == "Pseudobinary C");
        REQUIRE(msg["data_values"].size() == 1);
        CHECK(msg["data_values"][0]["name"] == "Sensor 1");
    }
}
