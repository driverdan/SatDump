#include "cli_test_helpers.h"
#include "goes_support/goes/hrit/module_goes_lrit_data_decoder.h"
#include "helpers/compare.h"
#include "helpers/random.h"
#include "image/io.h"
#include "libs/miniz/miniz.h"
#include "libs/miniz/miniz_zip.h"
#include "pipeline/pipeline_test_helpers.h"
#include "plugins_test_helpers.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <catch2/generators/catch_generators.hpp>
#include <thread>

// The decoder is given the CADUs of LRIT files sent by xrit_support/xrit_test_helpers.h, and
// must write the right files in the output directory. Unless stated otherwise it is set up
// like the "products" step of the goes_hrit pipeline.

using namespace pipeline_test;
using namespace xrit_test;
using test_helpers::bytesDiff;
using test_helpers::randomBytes;
namespace pl = satdump::pipeline;

namespace
{
    // GOES-R sends each product on a virtual channel, and each file of it on an APID
    const int VCID = 13;
    const int APID = 40;

    const int COLUMNS = 64;
    const int LINES = 24;

    // The decoder saves images in the configured format
    struct DecoderEnv
    {
        ConfigGuard cfg;
        cli_test::LogCapture log;
        TempDir tmp{"goes_lrit"};
        std::string out = tmp.file("out");

        DecoderEnv()
        {
            auto &general = satdump::satdump_cfg.main_cfg["satdump_general"];
            general["image_format"]["value"] = "png";
            general["product_format"]["value"] = "png";
            general["auto_process_products"]["value"] = false;
        }

        std::string path(const std::string &name) { return out + "/" + name; }
        bool exists(const std::string &name) { return std::filesystem::exists(path(name)); }
        std::string read(const std::string &name) { return readFile(path(name)); }

        // Every file below the output directory, relative to it
        std::vector<std::string> files()
        {
            std::vector<std::string> list;
            if (std::filesystem::exists(out))
                for (auto &e : std::filesystem::recursive_directory_iterator(out))
                    if (e.is_regular_file())
                        list.push_back(std::filesystem::relative(e.path(), out).generic_string());
            std::sort(list.begin(), list.end());
            return list;
        }
    };

    // Parameters of the "products" step of goes_hrit (resources/pipelines/GOES.json), with
    // the defaults of the parameters of the pipeline
    nlohmann::json hritParams()
    {
        return {{"max_fill_lines", 50},  {"write_images", true}, {"write_emwin", true}, {"write_messages", true},  {"write_dcs", false},      {"write_unknown", false},
                {"fill_missing", false}, {"parse_dcs", false},   {"write_lrit", false}, {"tracked_addresses", ""}, {"write_emwin_nws", true}, {"write_emwin_text", false}};
    }

    // Runs the decoder on the .cadu file of what the mux holds. A fill frame is added at the
    // end, as there always is one on the air.
    std::shared_ptr<pl::ProcessingModule> decode(DecoderEnv &env, Mux &mux, nlohmann::json params = hritParams())
    {
        mux.flush(VCID);
        mux.fill();
        auto vcdus = mux.vcdus();
        std::string input = env.tmp.file("in.cadu");
        writeBytes(input, caduFile(vcdus));

        auto m = goes::hrit::GOESLRITDataDecoderModule::getInstance(input, env.out + "/goes_hrit", params);
        m->setInputType(pl::DATA_FILE);
        m->setOutputType(pl::DATA_FILE);
        m->init();
        m->process();
        return m;
    }

    // Text file: file type 2. NOAA product 9 or 6 is EMWIN, anything else a message.
    Bytes textFile(const std::string &name, const std::string &text, uint16_t product = 1, uint8_t compression = 0)
    {
        return lritFile(2, {annotationRecord(name), noaaRecord(product, 0, 0, compression)}, Bytes(text.begin(), text.end()));
    }

    // A ZIP archive holding one file
    Bytes zipOf(const std::string &name, const std::string &content)
    {
        mz_zip_archive zip;
        MZ_CLEAR_OBJ(zip);
        mz_zip_writer_init_heap(&zip, 0, 0);
        mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_DEFAULT_COMPRESSION);
        void *buf = nullptr;
        size_t size = 0;
        mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
        Bytes out((uint8_t *)buf, (uint8_t *)buf + size);
        mz_zip_writer_end(&zip);
        mz_free(buf);
        return out;
    }

    // An EMWIN file: a ZIP archive of one file
    Bytes zipFile(const std::string &name, const std::string &zipped_name, const std::string &content, uint16_t product = 9)
    {
        Bytes zip = zipOf(zipped_name, content);
        return textFile(name, std::string(zip.begin(), zip.end()), product, 10);
    }

    // A ZIP archive holding no file
    Bytes emptyZip()
    {
        mz_zip_archive zip;
        MZ_CLEAR_OBJ(zip);
        mz_zip_writer_init_heap(&zip, 0, 0);
        void *buf = nullptr;
        size_t size = 0;
        mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
        Bytes out((uint8_t *)buf, (uint8_t *)buf + size);
        mz_zip_writer_end(&zip);
        mz_free(buf);
        return out;
    }

    // A ZIP archive of one file, whose CRC in the central directory does not match its data
    Bytes badCrcZip(const std::string &name, const std::string &content)
    {
        Bytes zip = zipOf(name, content);
        const uint8_t central_header[] = {'P', 'K', 1, 2};
        auto it = std::search(zip.begin(), zip.end(), std::begin(central_header), std::end(central_header));
        if (it != zip.end())
            *(it + 16) ^= 0xFF; // CRC-32
        return zip;
    }

    // Pixels of a test image: every line is different, and none is black
    Bytes linePixels(int line, int columns = COLUMNS)
    {
        Bytes p(columns);
        for (int x = 0; x < columns; x++)
            p[x] = 20 + (line * 7 + x) % 200;
        return p;
    }

    std::vector<RiceLine> imageLines(int lines = LINES, int first = 0)
    {
        std::vector<RiceLine> l;
        for (int y = 0; y < lines; y++)
            l.push_back({linePixels(first + y)});
        return l;
    }

    // Headers of a Rice compressed image that is not from a known imager: it is saved as is
    Bytes riceImageHeaders(const std::string &name, int lines = LINES) { return lritHeaders(0, {imageStructureRecord(8, COLUMNS, lines, 1), annotationRecord(name), noaaRecord(1, 0, 0, 1)}, 0); }

    // The lines of a saved image, as the index of the test line each one holds: -1 for a
    // black line, -2 for anything else
    std::vector<int> savedLines(const std::string &path, int first = 0, int count = LINES)
    {
        image::Image img;
        image::load_img(img, path);
        std::vector<int> lines;
        if ((int)img.width() != COLUMNS)
            return lines;

        for (size_t y = 0; y < img.height(); y++)
        {
            Bytes row(COLUMNS);
            for (int x = 0; x < COLUMNS; x++)
                row[x] = img.get(0, x, y);

            int found = row == Bytes(COLUMNS, 0) ? -1 : -2;
            for (int l = first; l < first + count && found == -2; l++)
                if (row == linePixels(l))
                    found = l;
            lines.push_back(found);
        }
        return lines;
    }

    // 0, 1, 2... with some lines replaced
    std::vector<int> linesWith(std::map<int, int> replaced, int count = LINES)
    {
        std::vector<int> l(count);
        for (int i = 0; i < count; i++)
            l[i] = replaced.count(i) ? replaced[i] : i;
        return l;
    }
} // namespace

TEST_CASE("GOESLRITDataDecoderModule identifies itself", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    CHECK(goes::hrit::GOESLRITDataDecoderModule::getID() == "goes_lrit_data_decoder");
    auto m = goes::hrit::GOESLRITDataDecoderModule::getInstance("in", "out", hritParams());
    CHECK(m->getIDM() == "goes_lrit_data_decoder");
    CHECK(m->getInputTypes() == std::vector<pl::ModuleDataType>{pl::DATA_FILE, pl::DATA_STREAM});
    // The last step of a pipeline: it outputs files, not a stream for another module
    CHECK(m->getOutputTypes() == std::vector<pl::ModuleDataType>{pl::DATA_FILE});
}

TEST_CASE("GOESLRITDataDecoderModule requires the parameters of its pipeline step", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    auto params = hritParams();

    SECTION("mandatory")
    {
        auto key = GENERATE("max_fill_lines", "write_images", "write_emwin", "write_messages", "write_dcs", "write_unknown");
        INFO(key);
        params.erase(key);
        CHECK_THROWS(goes::hrit::GOESLRITDataDecoderModule::getInstance("in", "out", params));
    }

    SECTION("the parameters of the pipeline are optional")
    {
        for (auto key : {"fill_missing", "parse_dcs", "write_lrit", "tracked_addresses", "write_emwin_nws", "write_emwin_text"})
            params.erase(key);
        CHECK_NOTHROW(goes::hrit::GOESLRITDataDecoderModule::getInstance("in", "out", params));
    }
}

TEST_CASE("goes_support registers the decoder when the plugin is loaded", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    EventBusGuard bus;
    ConfigGuard cfg;
    ModuleRegistryGuard reg;

    auto plugin = plugins_test::loadTestPlugin(SATDUMP_TEST_PLUGIN_GOES_SUPPORT);
    REQUIRE(plugin);
    CHECK(plugin->getID() == "goes_support");

    // The plugin adds its settings to the configuration
    CHECK(satdump::satdump_cfg.main_cfg["plugin_settings"]["goes_support"].contains("update_interval"));

    pl::registerModules();
    REQUIRE(pl::moduleExists("goes_lrit_data_decoder"));
    auto m = pl::getModuleInstance("goes_lrit_data_decoder", "in", "out", hritParams());
    CHECK(m->getIDM() == "goes_lrit_data_decoder");
}

TEST_CASE("GOESLRITDataDecoderModule creates its output directories", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    auto params = hritParams();

    SECTION("with the settings of goes_hrit")
    {
        decode(env, mux, params);
        CHECK(std::filesystem::is_directory(env.path("IMAGES/Unknown")));
        CHECK_FALSE(env.exists("DCS"));
        CHECK_FALSE(env.exists("DCS_LRIT"));
    }

    SECTION("without images, with the DCS files")
    {
        params["write_images"] = false;
        params["write_dcs"] = true;
        decode(env, mux, params);
        CHECK_FALSE(env.exists("IMAGES"));
        CHECK(std::filesystem::is_directory(env.path("DCS_LRIT")));
    }

    // The decoder itself writes no file, and so has no output for a next module
    CHECK(env.files().empty());
}

TEST_CASE("GOESLRITDataDecoderModule saves messages as text files", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    std::string text = "ADMIN MESSAGE\r\nHRIT schedule change on day 120.\r\n";
    auto params = hritParams();
    Mux mux;

    SECTION("text file")
    {
        mux.add(VCID, packetize(textFile("A-MSG-0001.lrit", text), APID));
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"Admin Messages/A-MSG-0001.txt"});
        CHECK(env.read("Admin Messages/A-MSG-0001.txt") == text);
    }

    SECTION("message file, in several packets")
    {
        std::string long_text;
        for (int i = 0; i < 200; i++)
            long_text += "Line " + std::to_string(i) + " of a long message\r\n";
        mux.add(VCID, packetize(lritFile(1, {annotationRecord("LONG.lrit"), noaaRecord(1, 0, 0, 0)}, Bytes(long_text.begin(), long_text.end())), APID, 0, 300));
        decode(env, mux, params);
        CHECK(env.read("Admin Messages/LONG.txt") == long_text);
    }

    SECTION("not without write_messages")
    {
        params["write_messages"] = false;
        mux.add(VCID, packetize(textFile("A-MSG-0001.lrit", text), APID));
        decode(env, mux, params);
        CHECK(env.files().empty());
    }
}

TEST_CASE("GOESLRITDataDecoderModule saves EMWIN files by kind", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    auto params = hritParams();
    Mux mux;
    std::string png = "\x89PNG, or so the decoder is told";

    // EMWIN is NOAA product 9 (or 6), in a ZIP archive (compression 10) or as plain text.
    // Names end with an 8 character identifier, which says what the file is.
    SECTION("weather service graphics")
    {
        mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "A_EMWIN-RADALLUS.PNG", png), APID));
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"EMWIN/NWS/A_EMWIN-RADALLUS.PNG"});
        CHECK(env.read("EMWIN/NWS/A_EMWIN-RADALLUS.PNG") == png);
    }

    SECTION("not without write_emwin_nws")
    {
        params["write_emwin_nws"] = false;
        mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "A_EMWIN-RADALLUS.PNG", png), APID));
        decode(env, mux, params);
        CHECK(env.files().empty());
    }

    SECTION("satellite imagery, sorted by satellite")
    {
        int count = 0;
        for (std::string name : {"A_EMWIN-G16CIRUS.JPG", "A_EMWIN-GMS008JP.JPG", "A_EMWIN-INDCIRUS.JPG", "A_EMWIN-ZZZZZZZZ.JPG", "SHORT.JPG"})
        {
            mux.add(VCID, packetize(zipFile("Z_ZIP" + std::to_string(count) + ".lrit", name, png, 6), APID, count * 10));
            count++;
        }
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"EMWIN/GOES/A_EMWIN-G16CIRUS.JPG", "EMWIN/Himawari/A_EMWIN-GMS008JP.JPG", "EMWIN/Meteosat/A_EMWIN-INDCIRUS.JPG",
                                                      "EMWIN/Other/A_EMWIN-ZZZZZZZZ.JPG", "EMWIN/Other/SHORT.JPG"});
    }

    SECTION("text is not saved by default")
    {
        mux.add(VCID, packetize(textFile("A_EMWIN-FXUS61KB.lrit", "FORECAST DISCUSSION", 9, 0), APID));
        mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "A_EMWIN-METAR001.TXT", "KBOS 021200Z"), APID, 10));
        decode(env, mux, params);
        CHECK(env.files().empty());
    }

    SECTION("text is saved with write_emwin_text")
    {
        params["write_emwin_text"] = true;
        mux.add(VCID, packetize(textFile("A_EMWIN-FXUS61KB.lrit", "FORECAST DISCUSSION", 9, 0), APID));
        mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "A_EMWIN-METAR001.TXT", "KBOS 021200Z"), APID, 10));
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"EMWIN/Text/A_EMWIN-FXUS61KB.txt", "EMWIN/Text/A_EMWIN-METAR001.TXT"});
        CHECK(env.read("EMWIN/Text/A_EMWIN-FXUS61KB.txt") == "FORECAST DISCUSSION");
        CHECK(env.read("EMWIN/Text/A_EMWIN-METAR001.TXT") == "KBOS 021200Z");
    }

    SECTION("nothing without write_emwin")
    {
        params["write_emwin"] = false;
        params["write_emwin_text"] = true;
        mux.add(VCID, packetize(textFile("A_EMWIN-FXUS61KB.lrit", "FORECAST DISCUSSION", 9, 0), APID));
        decode(env, mux, params);
        CHECK(env.files().empty());
    }

    SECTION("a file that is not a ZIP archive is dropped")
    {
        mux.add(VCID, packetize(textFile("Z_ZIP1.lrit", "this is not a ZIP archive, whatever the header says", 9, 10), APID));
        decode(env, mux, params);
        CHECK(env.files().empty());
        CHECK(env.log.contains("Could not open ZIP data! Discarding...", slog::LOG_ERROR));
    }
}

// A name of 10 to 12 characters is too short to hold the "-" and the 8 character identifier,
// so the decoder must not look for them before the start of the name
TEST_CASE("GOESLRITDataDecoderModule saves EMWIN files with a short name", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "RADAR01.PNG", "data"), APID));

    CHECK_NOTHROW(decode(env, mux));
    CHECK(env.files() == std::vector<std::string>{"EMWIN/Other/RADAR01.PNG"});
}

// Nor look for the extension before the start of a name of less than 3 characters
TEST_CASE("GOESLRITDataDecoderModule saves EMWIN files with a name shorter than an extension", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "AB", "data"), APID));

    CHECK_NOTHROW(decode(env, mux));
    CHECK(env.files() == std::vector<std::string>{"EMWIN/Other/AB"});
}

// The ZIP archive is as received: when there is nothing to save from it, the file is dropped
// and the decoder carries on with the next one
TEST_CASE("GOESLRITDataDecoderModule drops EMWIN archives it cannot unpack", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;

    SECTION("an archive holding no file")
    {
        Bytes zip = emptyZip();
        mux.add(VCID, packetize(textFile("Z_ZIP1.lrit", std::string(zip.begin(), zip.end()), 9, 10), APID));
        mux.add(VCID, packetize(zipFile("Z_ZIP2.lrit", "A_EMWIN-RADALLUS.PNG", "still here"), APID, 10));
        decode(env, mux);
        CHECK(env.log.contains("ZIP data holds no file! Discarding...", slog::LOG_ERROR));
    }

    SECTION("an archive whose file does not decompress")
    {
        Bytes zip = badCrcZip("A_EMWIN-RADBADUS.PNG", "the CRC of this is wrong");
        mux.add(VCID, packetize(textFile("Z_ZIP1.lrit", std::string(zip.begin(), zip.end()), 9, 10), APID));
        mux.add(VCID, packetize(zipFile("Z_ZIP2.lrit", "A_EMWIN-RADALLUS.PNG", "still here"), APID, 10));
        decode(env, mux);
        CHECK(env.log.contains("Could not decompress ZIP data! Discarding...", slog::LOG_ERROR));
    }

    CHECK(env.files() == std::vector<std::string>{"EMWIN/NWS/A_EMWIN-RADALLUS.PNG"});
    CHECK(env.read("EMWIN/NWS/A_EMWIN-RADALLUS.PNG") == "still here");
}

// The name of the file in the archive is used as the output file name
TEST_CASE("GOESLRITDataDecoderModule keeps EMWIN files in the EMWIN directory", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "../../A_EMWIN-RADALLUS.PNG", "data"), APID));
    decode(env, mux);
    CHECK(env.files() == std::vector<std::string>{"EMWIN/NWS/.._.._A_EMWIN-RADALLUS.PNG"});
}

TEST_CASE("GOESLRITDataDecoderModule saves the images of unknown imagers as they are", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    auto params = hritParams();
    Mux mux;

    SECTION("uncompressed")
    {
        Bytes pixels;
        for (int y = 0; y < LINES; y++)
            for (uint8_t p : linePixels(y))
                pixels.push_back(p);
        mux.add(VCID, packetize(lritFile(0, {imageStructureRecord(8, COLUMNS, LINES, 0), annotationRecord("CHART.lrit"), noaaRecord(1, 0, 0, 0)}, pixels), APID, 0, 500));
        decode(env, mux, params);

        CHECK(env.files() == std::vector<std::string>{"IMAGES/CHART.lrit.png"});
        CHECK(savedLines(env.path("IMAGES/CHART.lrit.png")) == linesWith({}));
    }

    SECTION("GIF: the data field is the file")
    {
        std::string gif = "GIF89a, or so the decoder is told";
        mux.add(VCID, packetize(lritFile(0, {imageStructureRecord(8, COLUMNS, LINES, 1), annotationRecord("CHART.lrit"), noaaRecord(1, 0, 0, 5)}, Bytes(gif.begin(), gif.end())), APID));
        decode(env, mux, params);

        CHECK(env.files() == std::vector<std::string>{"IMAGES/CHART.lrit.gif"});
        CHECK(env.read("IMAGES/CHART.lrit.gif") == gif);
    }

    SECTION("not without write_images")
    {
        params["write_images"] = false;
        mux.add(VCID, riceImagePackets(riceImageHeaders("CHART.lrit"), imageLines(), APID));
        decode(env, mux, params);
        CHECK(env.files().empty());
    }
}

TEST_CASE("GOESLRITDataDecoderModule decompresses Rice compressed images", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    auto params = hritParams();
    params["fill_missing"] = GENERATE(false, true);

    // One packet per image line, as GOES-R sends them
    mux.add(VCID, riceImagePackets(riceImageHeaders("IMAGE.lrit"), imageLines(), APID, 16375)); // The 14-bit packet counter wraps around on the way
    decode(env, mux, params);

    CHECK(env.files() == std::vector<std::string>{"IMAGES/IMAGE.lrit.png"});
    CHECK(savedLines(env.path("IMAGES/IMAGE.lrit.png")) == linesWith({}));
    CHECK(env.log.contains("This is RICE-compressed! Decompressing...", slog::LOG_DEBUG));
    CHECK_FALSE(env.log.contains("Rice decompression failed"));
}

TEST_CASE("GOESLRITDataDecoderModule fill_missing replaces the lost lines of an image", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    auto params = hritParams();
    auto lines = imageLines();
    std::string image = "IMAGES/IMAGE.lrit.png";

    SECTION("lines lost in the image")
    {
        lines[5].lost = lines[6].lost = lines[15].lost = true;
        mux.add(VCID, riceImagePackets(riceImageHeaders("IMAGE.lrit"), lines, APID));

        SECTION("are black without fill_missing")
        {
            decode(env, mux, params);
            CHECK(savedLines(env.path(image)) == linesWith({{5, -1}, {6, -1}, {15, -1}}));
        }

        SECTION("are a copy of the next line with fill_missing, as with --fill_missing")
        {
            params["fill_missing"] = true;
            decode(env, mux, params);
            CHECK(savedLines(env.path(image)) == linesWith({{5, 7}, {6, 7}, {15, 16}}));
        }

        SECTION("are black with fill_missing if there are more than max_fill_lines of them")
        {
            params["fill_missing"] = true;
            params["max_fill_lines"] = 2; // The gap of lines 5 and 6 counts as 3: the lost lines, plus one
            decode(env, mux, params);
            CHECK(savedLines(env.path(image)) == linesWith({{5, -1}, {6, -1}, {15, 16}}));
        }
    }

    SECTION("a line received with a wrong CRC")
    {
        lines[9].bad_crc = true;
        mux.add(VCID, riceImagePackets(riceImageHeaders("IMAGE.lrit"), lines, APID));

        SECTION("is decoded anyway without fill_missing")
        {
            decode(env, mux, params);
            CHECK(savedLines(env.path(image)) == linesWith({}));
        }

        SECTION("is replaced by the next line with fill_missing")
        {
            params["fill_missing"] = true;
            decode(env, mux, params);
            CHECK(savedLines(env.path(image)) == linesWith({{9, 10}}));
        }

        CHECK(env.log.contains("LRIT CRC is invalid, but file can be recovered", slog::LOG_WARN));
    }

    SECTION("the end of the image lost")
    {
        // The image is then only known to be over when the next file starts
        lines[21].lost = lines[22].lost = lines[23].lost = true;
        mux.add(VCID, riceImagePackets(riceImageHeaders("IMAGE.lrit"), lines, APID));
        mux.add(VCID, packetize(textFile("NEXT.lrit", "next file"), APID, 100));

        SECTION("is black without fill_missing")
        {
            decode(env, mux, params);
            CHECK(savedLines(env.path(image)) == linesWith({{21, -1}, {22, -1}, {23, -1}}));
        }

        SECTION("is a copy of the last line received with fill_missing")
        {
            params["fill_missing"] = true;
            decode(env, mux, params);
            CHECK(savedLines(env.path(image)) == linesWith({{21, 20}, {22, 20}, {23, 20}}));
        }

        SECTION("is black with fill_missing if there are more than max_fill_lines of them")
        {
            params["fill_missing"] = true;
            params["max_fill_lines"] = 2;
            decode(env, mux, params);
            CHECK(savedLines(env.path(image)) == linesWith({{21, -1}, {22, -1}, {23, -1}}));
        }

        CHECK(env.exists("Admin Messages/NEXT.txt"));
    }
}

TEST_CASE("GOESLRITDataDecoderModule saves GOES-R level 2 products apart from the imagery", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;

    // Rainfall rate: a product of the ABI, named after itself instead of a channel, in one file
    std::string name = "OR_ABI-L2-RRQPEF-M6_G18_s20241231200207_e20241231209527_c20241231210007.lrit";
    std::string ancillary = "Time of frame start = 2024-05-02T12:00:20.7Z;Satellite = G18;Region = Full Disk;";
    Bytes headers = lritHeaders(0, {imageStructureRecord(8, COLUMNS, LINES, 1), annotationRecord(name), timeStampRecord(24228, 43220000), ancillaryTextRecord(ancillary), noaaRecord(18, 0, 0, 1)}, 0);
    mux.add(VCID, riceImagePackets(headers, imageLines(), APID));
    decode(env, mux);

    std::string dir = "L2/GOES-18/Full Disk/2024-05-02_12-00-20/";
    CHECK(env.files() == std::vector<std::string>{dir + "G18_RRQPE_20240502T120020Z.png", dir + "product.cbor"});
    CHECK(savedLines(env.path(dir + "G18_RRQPE_20240502T120020Z.png")) == linesWith({}));
}

TEST_CASE("GOESLRITDataDecoderModule uses the Rice settings of the file", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;

    // A Rice compression record with the settings the lines were compressed with
    auto settings = riceSettings(COLUMNS);
    Bytes headers = lritHeaders(
        0, {imageStructureRecord(8, COLUMNS, LINES, 1), annotationRecord("IMAGE.lrit"), noaaRecord(1, 0, 0, 1), riceCompressionRecord(settings.options_mask, settings.pixels_per_block, 1)}, 0);
    mux.add(VCID, riceImagePackets(headers, imageLines(), APID));
    decode(env, mux);

    CHECK(savedLines(env.path("IMAGES/IMAGE.lrit.png")) == linesWith({}));
    CHECK(env.log.contains("Rice header is present!", slog::LOG_DEBUG));
    CHECK(env.log.contains("Pixels per block : 16", slog::LOG_DEBUG));
}

TEST_CASE("GOESLRITDataDecoderModule decodes the files after a Rice compressed image on the same APID as they are", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    mux.add(VCID, riceImagePackets(riceImageHeaders("IMAGE.lrit"), imageLines(), APID));

    // Files in several packets, which go through the Rice decompression if it is still on
    std::string text;
    for (int i = 0; i < 50; i++)
        text += "Line " + std::to_string(i) + " of a message\r\n";
    Bytes pixels;
    for (int y = 0; y < LINES; y++)
        for (uint8_t p : linePixels(y))
            pixels.push_back(p);
    mux.add(VCID, packetize(lritFile(1, {annotationRecord("MESSAGE.lrit"), noaaRecord(1, 0, 0, 0)}, Bytes(text.begin(), text.end())), APID, 100, 300));
    mux.add(VCID, packetize(lritFile(0, {imageStructureRecord(8, COLUMNS, LINES, 0), annotationRecord("CHART.lrit"), noaaRecord(1, 0, 0, 0)}, pixels), APID, 200, 500));
    decode(env, mux);

    CHECK(env.files() == std::vector<std::string>{"Admin Messages/MESSAGE.txt", "IMAGES/CHART.lrit.png", "IMAGES/IMAGE.lrit.png"});
    CHECK(env.read("Admin Messages/MESSAGE.txt") == text);
    CHECK(savedLines(env.path("IMAGES/CHART.lrit.png")) == linesWith({}));
    CHECK(savedLines(env.path("IMAGES/IMAGE.lrit.png")) == linesWith({}));
}

TEST_CASE("riceFillLimit is what is left of the image to receive", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // Lost lines are filled up to the end of the image, and no further
    satdump::xrit::XRITFile file;
    Bytes headers = riceImageHeaders("IMAGE.lrit", 4);
    file.lrit_data = headers;
    file.parseHeaders();
    const size_t image = COLUMNS * 4;

    CHECK(goes::hrit::riceFillLimit(file, 0) == image);
    CHECK(goes::hrit::riceFillLimit(file, COLUMNS) == image - COLUMNS);

    file.lrit_data.resize(headers.size() + 3 * COLUMNS);
    CHECK(goes::hrit::riceFillLimit(file, 0) == COLUMNS);
    CHECK(goes::hrit::riceFillLimit(file, COLUMNS) == 0);

    // A file can hold more lines than its image has, which leaves nothing to fill
    CHECK(goes::hrit::riceFillLimit(file, 2 * COLUMNS) == 0);
    file.lrit_data.resize(headers.size() + 10 * COLUMNS);
    CHECK(goes::hrit::riceFillLimit(file, COLUMNS) == 0);
}

TEST_CASE("GOESLRITDataDecoderModule assembles the segments of a GOES-R ABI image", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    auto params = hritParams();
    params["fill_missing"] = true;

    // A full disk image of channel 13 from GOES-16, in 3 segments of 8 lines
    const int segments = 3, seg_lines = 8;
    std::string name = "OR_ABI-L2-CMIPF-M6C13_G16_s20241231200207_e20241231209527_c20241231210007.lrit";
    std::string ancillary = "Time of frame start = 2024-05-02T12:00:20.7Z;Satellite = G16;Instrument = FM1;Channel = 13;Region = Full Disk;";
    auto segment = [&](int seg)
    {
        Bytes headers = lritHeaders(0,
                                    {imageStructureRecord(8, COLUMNS, seg_lines, 1), annotationRecord(name), timeStampRecord(24228, 43220000), ancillaryTextRecord(ancillary), noaaRecord(16, 13, 0, 1),
                                     segmentIdentificationRecord(7, seg, 0, seg * seg_lines, segments, COLUMNS, segments * seg_lines)},
                                    0);
        return riceImagePackets(headers, imageLines(seg_lines, seg * seg_lines), APID, seg * 100);
    };

    std::string dir = "IMAGES/GOES-16/Full Disk/2024-05-02_12-00-20/";
    std::string image = dir + "G16_13_20240502T120020Z.png";

    SECTION("all segments: the image is saved as soon as it is complete")
    {
        for (int seg : {0, 1, 2})
            mux.add(VCID, segment(seg));
        decode(env, mux, params);

        CHECK(env.files() == std::vector<std::string>{image, dir + "product.cbor"});
        CHECK(savedLines(env.path(image)) == linesWith({}));
    }

    SECTION("segments in any order")
    {
        for (int seg : {2, 0, 1})
            mux.add(VCID, segment(seg));
        decode(env, mux, params);
        CHECK(savedLines(env.path(image)) == linesWith({}));
    }

    SECTION("a missing segment: the image is saved at the end, with what was received")
    {
        mux.add(VCID, segment(0));
        mux.add(VCID, segment(2));
        decode(env, mux, params);

        std::map<int, int> black;
        for (int y = seg_lines; y < 2 * seg_lines; y++)
            black[y] = -1;
        CHECK(savedLines(env.path(image)) == linesWith(black));
    }

    // The product describes the image for the rest of SatDump
    REQUIRE(env.exists(dir + "product.cbor"));
    auto product = nlohmann::json::from_cbor(readBytes(env.path(dir + "product.cbor")));
    CHECK(product["instrument"] == "abi");
    CHECK(product["product_source"] == "GOES-16");
    CHECK(product["product_timestamp"] == 1714651220);
    REQUIRE(product["images"].size() == 1);
    CHECK(product["images"][0]["name"] == "13");
    CHECK(product["images"][0]["file"] == "G16_13_20240502T120020Z.png");
}

TEST_CASE("GOESLRITDataDecoderModule saves the raw LRIT files on request", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    auto params = hritParams();
    Bytes message = textFile("A-MSG-0001.lrit", "message");
    Bytes dcs = lritFile(130, {annotationRecord("pH-24123120000-A.lrit"), noaaRecord(8, 0, 0, 0)}, randomBytes(300, 1));
    Bytes unknown = lritFile(99, {annotationRecord("WHATEVER.lrit"), noaaRecord(1, 0, 0, 0)}, randomBytes(100, 2));
    Bytes empty = lritFile(2, {annotationRecord("EMPTY.lrit"), noaaRecord(1, 0, 0, 0)}, {});
    mux.add(VCID, packetize(message, APID, 0));
    mux.add(VCID, packetize(dcs, APID, 10));
    mux.add(VCID, packetize(unknown, APID, 20));
    mux.add(VCID, packetize(empty, APID, 30));

    SECTION("by default: only the file without data")
    {
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"Admin Messages/A-MSG-0001.txt", "LRIT/EMPTY.lrit"});
        CHECK(bytesDiff(readBytes(env.path("LRIT/EMPTY.lrit")), empty) == "");
        CHECK(env.log.contains("Received LRIT header with no body! Saving as .lrit", slog::LOG_WARN));
    }

    SECTION("write_lrit: every file")
    {
        params["write_lrit"] = true;
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"Admin Messages/A-MSG-0001.txt", "LRIT/A-MSG-0001.lrit", "LRIT/EMPTY.lrit", "LRIT/WHATEVER.lrit", "LRIT/pH-24123120000-A.lrit"});
        CHECK(bytesDiff(readBytes(env.path("LRIT/A-MSG-0001.lrit")), message) == "");
        CHECK(bytesDiff(readBytes(env.path("LRIT/pH-24123120000-A.lrit")), dcs) == "");
    }

    SECTION("write_unknown: the files of an unknown type")
    {
        params["write_unknown"] = true;
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"Admin Messages/A-MSG-0001.txt", "LRIT/EMPTY.lrit", "LRIT/WHATEVER.lrit"});
        CHECK(bytesDiff(readBytes(env.path("LRIT/WHATEVER.lrit")), unknown) == "");
    }

    SECTION("write_dcs: the DCS files")
    {
        params["write_dcs"] = true;
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"Admin Messages/A-MSG-0001.txt", "DCS_LRIT/pH-24123120000-A.lrit", "LRIT/EMPTY.lrit"});
        CHECK(bytesDiff(readBytes(env.path("DCS_LRIT/pH-24123120000-A.lrit")), dcs) == "");
    }
}

TEST_CASE("GOESLRITDataDecoderModule names the raw files of image segments after the segment", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    auto params = hritParams();
    params["write_lrit"] = true;
    params["write_images"] = false;

    Bytes headers = lritHeaders(0, {imageStructureRecord(8, COLUMNS, 4, 1), annotationRecord("SEGMENTED.lrit"), noaaRecord(1, 0, 0, 1), segmentIdentificationRecord(7, 12, 0, 48, 20, COLUMNS, 80)}, 0);
    mux.add(VCID, riceImagePackets(headers, imageLines(4), APID));
    decode(env, mux, params);

    // The decompressed image follows the headers
    CHECK(env.files() == std::vector<std::string>{"LRIT/SEGMENTED_012.lrit"});
    CHECK(std::filesystem::file_size(env.path("LRIT/SEGMENTED_012.lrit")) == headers.size() + COLUMNS * 4);
}

// The segment number goes before the ".lrit" extension, or at the end of a shorter name
TEST_CASE("GOESLRITDataDecoderModule names the raw files of segments with a short name", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    auto params = hritParams();
    Bytes segment = segmentIdentificationRecord(7, 12, 0, 48, 20, COLUMNS, 80);

    SECTION("a file without data, saved by default")
    {
        mux.add(VCID, packetize(lritFile(2, {annotationRecord("AB"), noaaRecord(1, 0, 0, 0), segment}, {}), APID));
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"LRIT/AB_012"});
    }

    SECTION("write_lrit")
    {
        params["write_lrit"] = true;
        params["write_messages"] = false;
        mux.add(VCID, packetize(lritFile(2, {annotationRecord("AB"), noaaRecord(1, 0, 0, 0), segment}, Bytes{'x'}), APID));
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"LRIT/AB_012"});
    }
}

TEST_CASE("GOESLRITDataDecoderModule drops corrupted files and carries on", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;

    auto bad = packetize(textFile("BAD.lrit", std::string(2000, 'x')), APID, 0, 300);
    bad[3][50] ^= 0xFF;
    mux.add(VCID, bad);
    mux.add(VCID, packetize(textFile("GOOD.lrit", "still here"), APID, 20));
    decode(env, mux);

    CHECK(env.files() == std::vector<std::string>{"Admin Messages/GOOD.txt"});
    CHECK(env.log.contains("LRIT CRC is invalid... Skipping.", slog::LOG_ERROR));
}

TEST_CASE("GOESLRITDataDecoderModule drops files cut before the end of their headers and carries on", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // Headers of 16 + 303 + 14 bytes, sent in packets of 200 bytes: when the second packet
    // is lost, the file ends before its headers do
    DecoderEnv env;
    Mux mux;
    auto cut = packetize(lritFile(2, {annotationRecord(std::string(300, 'N')), noaaRecord(1, 0, 0, 0)}, Bytes(98, 'x')), APID, 10, 200);
    REQUIRE(cut.size() == 3);
    cut.erase(cut.begin() + 1);

    mux.add(VCID, packetize(textFile("FIRST.lrit", "first"), APID, 0));
    mux.add(VCID, cut);
    mux.add(VCID, packetize(textFile("NEXT.lrit", "next"), APID, 20));
    decode(env, mux);

    CHECK(env.files() == std::vector<std::string>{"Admin Messages/FIRST.txt", "Admin Messages/NEXT.txt"});
    CHECK(env.read("Admin Messages/FIRST.txt") == "first");
    CHECK(env.read("Admin Messages/NEXT.txt") == "next");
    // The lost packet is seen as soon as the last one comes
    CHECK(env.log.contains("LRIT packet lost before the headers were complete... Skipping.", slog::LOG_ERROR));
}

TEST_CASE("GOESLRITDataDecoderModule skips a first packet too short to start a file", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    mux.add(VCID, packet(APID, SEQ_FIRST, 0, Bytes{1, 2, 3, 4}));
    mux.add(VCID, packetize(textFile("NEXT.lrit", "next"), APID, 1));

    CHECK_NOTHROW(decode(env, mux));
    CHECK(env.files() == std::vector<std::string>{"Admin Messages/NEXT.txt"});
    CHECK(env.log.contains("LRIT header packet is too short... Skipping.", slog::LOG_ERROR));
}

TEST_CASE("GOESLRITDataDecoderModule decodes a live stream", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // As in a live pipeline: CADUs come from the previous module through a FIFO
    DecoderEnv env;
    Mux mux;
    mux.fill();
    mux.add(VCID, packetize(textFile("FIRST.lrit", "first"), APID, 0));
    mux.add(VCID, riceImagePackets(riceImageHeaders("IMAGE.lrit"), imageLines(), APID, 10));
    mux.add(VCID, packetize(textFile("LAST.lrit", "last"), APID, 100));
    mux.flush(VCID);
    mux.fill();
    Bytes cadus = caduFile(mux.vcdus());

    auto params = hritParams();
    params["fill_missing"] = true;
    auto m = goes::hrit::GOESLRITDataDecoderModule::getInstance("", env.out + "/goes_hrit", params);
    m->input_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
    m->setInputType(pl::DATA_STREAM);
    m->setOutputType(pl::DATA_FILE);
    m->init();
    m->input_active = true;
    std::thread t([m]() { m->process(); });

    m->input_fifo->write(cadus.data(), cadus.size());
    bool got = test_helpers::waitUntil([&]() { return env.exists("Admin Messages/LAST.txt"); }, test_helpers::kTimeout, std::chrono::milliseconds(5));

    m->input_active = false;
    m->input_fifo->stopReader();
    m->input_fifo->stopWriter();
    m->stop();
    t.join();

    REQUIRE(got);
    CHECK(env.files() == std::vector<std::string>{"Admin Messages/FIRST.txt", "Admin Messages/LAST.txt", "IMAGES/IMAGE.lrit.png"});
    CHECK(savedLines(env.path("IMAGES/IMAGE.lrit.png")) == linesWith({}));
    CHECK(m->getOutput() == "");
}
