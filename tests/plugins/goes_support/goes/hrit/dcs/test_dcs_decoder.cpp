#include "cli_test_helpers.h"
#include "goes_support/goes/hrit/dcs/dcs_settings.h"
#include "goes_support/goes/hrit/module_goes_lrit_data_decoder.h"
#include "helpers/guards.h"
#include "helpers/satdump_guards.h"
#include "helpers/temp_files.h"
#include "helpers/wait.h"
#include "init.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
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
        // instead of stalling the run. Only then is the thread left running, with its own
        // references to the decoder and the file; otherwise it is joined, so that the decoder is
        // destroyed on the test's thread. Call it as a statement of its own, not inside a CHECK,
        // so that a timeout is reported once.
        static bool processBounded(std::shared_ptr<DCSDecoder> decoder, std::vector<uint8_t> file)
        {
            std::packaged_task<bool()> task([decoder, file]() mutable { return decoder->process(file); });
            std::future<bool> result = task.get_future();
            std::thread parser(std::move(task));
            if (result.wait_for(test_helpers::kTimeout) != std::future_status::ready)
            {
                parser.detach();
                FAIL("processDCS() did not return");
            }
            parser.join();
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
        bool parsed = DCSDecoder::processBounded(decoder, dcsFile(body));
        REQUIRE(parsed);

        nlohmann::json out = nlohmann::json::parse(readFile(tmp.file("DCS/" + FILE_NAME + ".json")));
        REQUIRE(out["blocks"].size() == 1);
        return out["blocks"][0];
    }
} // namespace

// loadDCPs() reads the PDT list (PDTS_COMPRESSED.txt, fixed width lines) and the HADS list
// (all_dcp_defs.txt, '|' separated lines) from satdump::user_path into the DCP list.
namespace
{
    using DCPList = std::map<uint32_t, std::shared_ptr<goes::hrit::DCP>>;

    struct DCPLoader : goes::hrit::GOESLRITDataDecoderModule
    {
        static void load() { loadDCPs(); }
        static void update() { updateDCPs({}); }
        static DCPList &list() { return dcp_list; }
        static std::mutex &mutex() { return dcp_mtx; }
    };

    // The DCP list is loaded from these files, in a list of its own
    struct LoaderFixture
    {
        TempDir tmp{"goes_dcp_load"};
        test_helpers::ValueGuard<std::string> user_path{satdump::user_path, tmp.str()};
        test_helpers::ValueGuard<DCPList> dcps{DCPLoader::list(), DCPList()};
        cli_test::LogCapture log;

        void pdt(const std::string &content) { test_helpers::writeFile(tmp.file("PDTS_COMPRESSED.txt"), content); }
        void hads(const std::string &content) { test_helpers::writeFile(tmp.file("all_dcp_defs.txt"), content); }

        // Runs loadDCPs() on a thread of its own, so that a loader blocked on dcp_mtx fails the
        // test instead of stalling it. Then checks that dcp_mtx was left unlocked.
        void load()
        {
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::thread loader(
                [done]
                {
                    DCPLoader::load();
                    *done = true;
                });
            if (!test_helpers::waitUntil([&] { return done->load(); }))
            {
                // Still waiting for dcp_mtx, which it will never get. dcp_mtx stays locked for the
                // rest of the run (it cannot be unlocked from another thread), so the tests that
                // decode DCS files after this one would wait on it too: their results are not to
                // be trusted once this has failed.
                loader.detach();
                FAIL("loadDCPs() did not return");
            }
            loader.join();

            bool unlocked = DCPLoader::mutex().try_lock();
            if (unlocked)
                DCPLoader::mutex().unlock();
            REQUIRE(unlocked);
        }
    };

    // The page a PDT server answered with instead of the file (#61)
    const std::string REDIRECT_PAGE = "<head><title>Document Moved</title></head>\n"
                                      "<body><h1>Object Moved</h1>This document may be found <a HREF=\"https://example.com\">here</a></body>\n";

    // A PDT line: a scheduled DCP sending every hour in a 10 s window, at 58 22 53 N 134 38 46 W
    std::string pdtLine(const std::string &address)
    {
        std::string line =
            "AGENCY" + address + "S" + std::string(13, ' ') + "010000" + "0010" + std::string(4, ' ') + "A" + "AK" + "  " + "TEST STATION" + std::string(19, ' ') + "+582253" + "-1343846";
        return line + std::string(239 - line.size(), ' ');
    }

    // A HADS line with one physical element (HG, every 15 minutes), sending every 60 minutes
    std::string hadsLine(const std::string &address, const std::string &interval = "60")
    {
        return address + "|TEST1|NWS| AK|AJK|58 22 53|-134 38 46|005200|" + interval + "|HADS STATION|S|HG| 15|  6|   0.00|  -0.15|  |   |   |       |       |";
    }
} // namespace

TEST_CASE("The DCP list is loaded from the PDT and HADS files", "[plugins][goes_support][goes_dcs]")
{
    LoaderFixture f;
    f.pdt(pdtLine("ABCDEF12") + "\n" + pdtLine("ABCDEF99") + "\n");
    f.hads(hadsLine("ABCDEF12") + "\n" + hadsLine("ABCDEF34") + "\n");
    f.load();
    REQUIRE(DCPLoader::list().size() == 3);

    SECTION("a DCP only in the PDT file")
    {
        auto &dcp = DCPLoader::list().at(0xABCDEF99);
        CHECK(dcp->agency == "AGENCY");
        CHECK(dcp->transmit_interval == 3600);
        CHECK(dcp->transmit_window == 10);
        CHECK(dcp->data_format == 'A');
        CHECK(dcp->state == "AK");
        CHECK(dcp->description == "TEST STATION");
        CHECK_THAT(dcp->lat, Catch::Matchers::WithinAbs(58 + 22 / 60.0 + 53 / 3600.0, 1e-4));
        CHECK_THAT(dcp->lon, Catch::Matchers::WithinAbs(-(134 + 38 / 60.0 + 46 / 3600.0), 1e-4));
        CHECK(dcp->pe_info.empty());
    }

    SECTION("a DCP in both: HADS adds its physical elements and replaces the description")
    {
        auto &dcp = DCPLoader::list().at(0xABCDEF12);
        CHECK(dcp->agency == "AGENCY");
        CHECK(dcp->transmit_interval == 3600);
        CHECK(dcp->description == "HADS STATION");
        REQUIRE(dcp->pe_info.size() == 1);
        CHECK(dcp->pe_info[0].record_interval == 15);
        CHECK(dcp->pe_info[0].read_to_transmit_offset == 6);
    }

    SECTION("a DCP only in the HADS file")
    {
        auto &dcp = DCPLoader::list().at(0xABCDEF34);
        CHECK(dcp->agency == "NWS");
        CHECK(dcp->state == "AK");
        CHECK(dcp->transmit_interval == 60);
        CHECK_THAT(dcp->lat, Catch::Matchers::WithinAbs(58 + 22 / 60.0 + 53 / 3600.0, 1e-4));
        CHECK_THAT(dcp->lon, Catch::Matchers::WithinAbs(-(134 + 38 / 60.0 + 46 / 3600.0), 1e-4));
        CHECK(dcp->pe_info.size() == 1);
    }
}

// #61: these lines threw while dcp_mtx was locked, and left it locked: the loader, or the
// decoder at its next DCS file, then waited on it forever. Others read past the line.
TEST_CASE("Bad PDT and HADS lines are skipped and leave the DCP list usable", "[plugins][goes_support][goes_dcs]")
{
    LoaderFixture f;

    SECTION("a PDT file that is a web page, and no HADS file")
    {
        f.pdt(REDIRECT_PAGE);
        f.load();
        CHECK(DCPLoader::list().empty());
        CHECK(f.log.contains("Error loading PDT line"));
    }

    SECTION("a PDT file that is a web page, then the HADS file")
    {
        f.pdt(REDIRECT_PAGE);
        f.hads(hadsLine("ABCDEF12") + "\n" + hadsLine("ABCDEF34") + "\n");
        f.load();
        CHECK(DCPLoader::list().size() == 2);
    }

    SECTION("PDT lines that are blank, or end before the data format")
    {
        f.pdt("\n" + pdtLine("ABCDEF12").substr(0, 42) + "\n" + pdtLine("ABCDEF34") + "\n");
        f.load();
        CHECK(DCPLoader::list().size() == 1);
        CHECK(DCPLoader::list().count(0xABCDEF34) == 1);
    }

    SECTION("PDT lines that end inside a field")
    {
        // In the state, the description, the description padding and the longitude
        f.pdt(pdtLine("ABCDEF01").substr(0, 43) + "\n" + pdtLine("ABCDEF02").substr(0, 44) + "\n" + pdtLine("ABCDEF03").substr(0, 53) + "\n" + pdtLine("ABCDEF04").substr(0, 70) + "\n" +
              pdtLine("ABCDEF05").substr(0, 90) + "\n");
        f.load();
        REQUIRE(DCPLoader::list().size() == 5);
        CHECK(DCPLoader::list().at(0xABCDEF01)->state == "");
        CHECK(DCPLoader::list().at(0xABCDEF02)->state == "A");
        CHECK(DCPLoader::list().at(0xABCDEF03)->description == "TEST S");
        CHECK(DCPLoader::list().at(0xABCDEF04)->description == "TEST STATION");
        // Not read from a cut longitude
        CHECK(DCPLoader::list().at(0xABCDEF05)->lat == 0);
        CHECK(DCPLoader::list().at(0xABCDEF05)->lon == 0);
    }

    SECTION("a HADS line with a transmit interval that is not a number, then good lines")
    {
        f.hads(hadsLine("ABCDEF12", "XX") + "\n" + hadsLine("ABCDEF34") + "\n" + hadsLine("ABCDEF56") + "\n");
        f.load();
        CHECK(f.log.contains("Error parsing HADS line"));
        // Not left in the list half filled
        CHECK(DCPLoader::list().count(0xABCDEF12) == 0);
        CHECK(DCPLoader::list().size() == 2);
    }

    SECTION("HADS lines whose latitude is not a number")
    {
        std::string bad_lat = "|58 22 53|";
        auto badLatitude = [&](const std::string &address)
        {
            std::string line = hadsLine(address);
            return line.replace(line.find(bad_lat), bad_lat.size(), "|XX 22 53|");
        };
        f.pdt(pdtLine("ABCDEF12") + "\n");
        f.hads(badLatitude("ABCDEF12") + "\n" + badLatitude("ABCDEF34") + "\n" + hadsLine("ABCDEF56") + "\n");
        f.load();
        CHECK(f.log.contains("Error parsing HADS line"));
        REQUIRE(DCPLoader::list().size() == 2);
        // The DCP of the PDT file is left as it was
        auto &dcp = DCPLoader::list().at(0xABCDEF12);
        CHECK(dcp->description == "TEST STATION");
        CHECK(dcp->pe_info.empty());
        // And the one only in HADS is not added half filled
        CHECK(DCPLoader::list().count(0xABCDEF34) == 0);
        CHECK(DCPLoader::list().count(0xABCDEF56) == 1);
    }

    SECTION("a HADS line with another latitude, then a longitude that is not a number")
    {
        // The latitude is read before the longitude fails: it must not be kept either
        std::string line = hadsLine("ABCDEF12");
        line.replace(line.find("|58 22 53|-134 38 46|"), 21, "|40 00 00|XX 38 46|");
        f.pdt(pdtLine("ABCDEF12") + "\n");
        f.hads(line + "\n");
        f.load();
        CHECK(f.log.contains("Error parsing HADS line"));
        REQUIRE(DCPLoader::list().size() == 1);
        auto &dcp = DCPLoader::list().at(0xABCDEF12);
        CHECK_THAT(dcp->lat, Catch::Matchers::WithinAbs(58 + 22 / 60.0 + 53 / 3600.0, 1e-4));
        CHECK_THAT(dcp->lon, Catch::Matchers::WithinAbs(-(134 + 38 / 60.0 + 46 / 3600.0), 1e-4));
        CHECK(dcp->description == "TEST STATION");
    }

    SECTION("HADS lines that are blank, or have too few fields")
    {
        f.hads("\n" + std::string("ABCDEF12") + "\n" + "ABCDEF34|TEST1|NWS| AK|AJK|58 22 53|-134 38 46|005200|60\n" + hadsLine("ABCDEF56") + "\n");
        f.load();
        CHECK(DCPLoader::list().size() == 1);
        CHECK(DCPLoader::list().count(0xABCDEF56) == 1);
    }
}

// #75: the PDT servers answered with a redirect page, which was saved as the PDT file
TEST_CASE("Downloaded PDT and HADS files are told apart from web pages", "[plugins][goes_support][goes_dcs]")
{
    const std::string spa_page = "<!doctype html>\n<html lang=\"en\">\n  <head>\n    <title>NOAA DADDS</title>\n";

    CHECK(goes::hrit::isPdtFile(pdtLine("ABCDEF12") + "\n" + pdtLine("ABCDEF34") + "\n"));
    CHECK(goes::hrit::isPdtFile(pdtLine("abcdef12") + "\r\n"));
    CHECK(goes::hrit::isPdtFile(pdtLine("ABCDEF12")));
    CHECK_FALSE(goes::hrit::isPdtFile(REDIRECT_PAGE));
    CHECK_FALSE(goes::hrit::isPdtFile(spa_page));
    CHECK_FALSE(goes::hrit::isPdtFile(""));
    CHECK_FALSE(goes::hrit::isPdtFile(pdtLine("ABCDEF12").substr(0, 42) + "\n" + pdtLine("ABCDEF34") + "\n"));
    CHECK_FALSE(goes::hrit::isPdtFile(pdtLine("ABCDEFXY")));
    CHECK_FALSE(goes::hrit::isPdtFile(hadsLine("ABCDEF12")));

    CHECK(goes::hrit::isHadsFile(hadsLine("ABCDEF12") + "\n" + hadsLine("ABCDEF34") + "\n"));
    CHECK(goes::hrit::isHadsFile(hadsLine("ABCDEF12") + "\r\n"));
    CHECK_FALSE(goes::hrit::isHadsFile(REDIRECT_PAGE));
    CHECK_FALSE(goes::hrit::isHadsFile(spa_page));
    CHECK_FALSE(goes::hrit::isHadsFile(""));
    CHECK_FALSE(goes::hrit::isHadsFile("ABCDEF34|TEST1|NWS| AK|AJK|58 22 53|-134 38 46|005200|60\n"));
    CHECK_FALSE(goes::hrit::isHadsFile("ABCDEF3|" + hadsLine("ABCDEF34").substr(9)));
    CHECK_FALSE(goes::hrit::isHadsFile(pdtLine("ABCDEF12")));
}

// updateDCPs() downloads the PDT and HADS files from the configured URLs, here file:// ones,
// into satdump::user_path, then reloads the DCP list from them
// (built from a POSIX path)
#ifndef _WIN32
TEST_CASE("updateDCPs() skips downloads that are not PDT or HADS files", "[plugins][goes_support][goes_dcs]")
{
    LoaderFixture f;
    test_helpers::ConfigGuard cfg;
    test_helpers::ValueGuard<std::string> user_cfg(satdump::satdump_cfg.user_cfg_path, f.tmp.file("settings.json"));
    TempDir server{"goes_dcp_server"};
    auto url = [&](const std::string &name) { return "file://" + server.file(name); };

    test_helpers::writeFile(server.file("redirect.html"), REDIRECT_PAGE);
    test_helpers::writeFile(server.file("pdts.txt"), pdtLine("ABCDEF12") + "\n");
    test_helpers::writeFile(server.file("hads.txt"), hadsLine("ABCDEF34") + "\n");

    auto &goes_cfg = satdump::satdump_cfg.main_cfg["plugin_settings"]["goes_support"];
    goes_cfg["last_pdt_update"] = 0;

    SECTION("a web page, then a PDT file: the PDT file is saved")
    {
        goes_cfg["pdt_urls"] = {url("redirect.html"), url("missing.txt"), url("pdts.txt")};
        goes_cfg["hads_urls"] = {url("redirect.html"), url("hads.txt")};
        DCPLoader::update();

        CHECK(readFile(f.tmp.file("PDTS_COMPRESSED.txt")) == pdtLine("ABCDEF12") + "\n");
        CHECK(readFile(f.tmp.file("all_dcp_defs.txt")) == hadsLine("ABCDEF34") + "\n");
        CHECK(f.log.contains("is not a PDT file"));
        CHECK(f.log.contains("is not a HADS file"));
        CHECK(goes_cfg["last_pdt_update"].get<time_t>() > 0);
        CHECK(DCPLoader::list().size() == 2);
    }

    SECTION("only web pages: the saved files are kept, and the update is not recorded")
    {
        f.pdt(pdtLine("ABCDEF99") + "\n");
        goes_cfg["pdt_urls"] = {url("redirect.html")};
        goes_cfg["hads_urls"] = {url("redirect.html")};
        DCPLoader::update();

        CHECK(readFile(f.tmp.file("PDTS_COMPRESSED.txt")) == pdtLine("ABCDEF99") + "\n");
        CHECK_FALSE(std::filesystem::exists(f.tmp.file("all_dcp_defs.txt")));
        CHECK(f.log.contains("Unable to download PDTs"));
        CHECK(f.log.contains("Unable to download HADS data"));
        CHECK(goes_cfg["last_pdt_update"] == 0);
    }
}
#endif

TEST_CASE("The former default PDT URLs are replaced by the DADDS one", "[plugins][goes_support][goes_dcs]")
{
    const std::string dadds = "https://dadds-dcs-api.data.nesdis.noaa.gov/pdts/PDTS_COMPRESSED.txt";
    const std::vector<std::string> old_defaults = {"https://dcs1.noaa.gov/PDTS_COMPRESSED.txt", "https://dcs2.noaa.gov/PDTS_COMPRESSED.txt", "https://dcs3.noaa.gov/PDTS_COMPRESSED.txt",
                                                   "https://dcs4.noaa.gov/PDTS_COMPRESSED.txt"};

    CHECK(goes::hrit::migratePdtUrls(old_defaults) == std::vector<std::string>{dadds});
    CHECK(goes::hrit::migratePdtUrls({"https://example.com/pdts.txt", old_defaults[1], dadds}) == std::vector<std::string>{"https://example.com/pdts.txt", dadds});
    CHECK(goes::hrit::migratePdtUrls({"https://example.com/pdts.txt"}) == std::vector<std::string>{"https://example.com/pdts.txt"});
    CHECK(goes::hrit::migratePdtUrls({}).empty());

    SECTION("in the user config, when the plugin starts")
    {
        test_helpers::ConfigGuard cfg;
        satdump::satdump_cfg.main_cfg["plugin_settings"]["goes_support"]["pdt_urls"] = old_defaults;
        goes::hrit::initDcsConfig();
        CHECK(satdump::satdump_cfg.main_cfg["plugin_settings"]["goes_support"]["pdt_urls"] == nlohmann::ordered_json::array({dadds}));
    }
}

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
        bool parsed = DCSDecoder::processBounded(std::make_shared<DCSDecoder>(tmp.str()), file);
        CHECK_FALSE(parsed);
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
    bool parsed = DCSDecoder::processBounded(std::make_shared<DCSDecoder>(tmp.str()), dcsFileOf(blocks));
    REQUIRE(parsed);

    nlohmann::json out = nlohmann::json::parse(readFile(tmp.file("DCS/" + FILE_NAME + ".json")));
    REQUIRE(out["blocks"].size() == 4);
    CHECK(out["blocks"][0]["type"] == "DCS Message");
    CHECK(out["blocks"][1]["type"] == "DCS Message");
    CHECK(out["blocks"][1]["header"]["type_id"] == 0);
    CHECK(out["blocks"][1]["data_ascii"] == "");
    // Only the address digits that #100 leaves at its start, not the CRC byte 7f
    CHECK(out["blocks"][1]["data_raw"] == "00000000");
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
