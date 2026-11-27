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
#ifndef _WIN32
#include <unistd.h>
#endif

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

    // A ZIP archive of one file, whose central directory claims another uncompressed size
    Bytes wrongSizeZip(const std::string &name, const std::string &content, uint32_t size)
    {
        Bytes zip = zipOf(name, content);
        const uint8_t central_header[] = {'P', 'K', 1, 2};
        auto it = std::search(zip.begin(), zip.end(), std::begin(central_header), std::end(central_header));
        if (it != zip.end())
            for (int i = 0; i < 4; i++)
                *(it + 24 + i) = size >> (8 * i); // Uncompressed size
        return zip;
    }

    // n bytes of v, little endian, as in a ZIP archive
    void putLE(Bytes &b, uint64_t v, int n)
    {
        for (int i = 0; i < n; i++)
            b.push_back(v >> (8 * i));
    }

    // A ZIP archive of one stored file, whose uncompressed size is in a ZIP64 field: the
    // central directory says 0xFFFFFFFF, as it does for files of 4 GB or more
    Bytes zip64SizeZip(const std::string &name, const std::string &content)
    {
        uint32_t crc = mz_crc32(MZ_CRC32_INIT, (const uint8_t *)content.data(), content.size());
        Bytes zip;

        // Local header and data
        putLE(zip, 0x04034b50, 4);
        putLE(zip, 45, 2); // Version needed
        putLE(zip, 0, 2);  // Flags
        putLE(zip, 0, 2);  // Stored
        putLE(zip, 0, 4);  // Time and date
        putLE(zip, crc, 4);
        putLE(zip, content.size(), 4);
        putLE(zip, content.size(), 4);
        putLE(zip, name.size(), 2);
        putLE(zip, 0, 2); // Extra field length
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), content.begin(), content.end());

        // Central directory
        size_t cd_offset = zip.size();
        putLE(zip, 0x02014b50, 4);
        putLE(zip, 45, 2); // Version made by
        putLE(zip, 45, 2); // Version needed
        putLE(zip, 0, 2);  // Flags
        putLE(zip, 0, 2);  // Stored
        putLE(zip, 0, 4);  // Time and date
        putLE(zip, crc, 4);
        putLE(zip, content.size(), 4);
        putLE(zip, 0xFFFFFFFF, 4); // Uncompressed size: in the ZIP64 field
        putLE(zip, name.size(), 2);
        putLE(zip, 12, 2); // Extra field length
        putLE(zip, 0, 2);  // Comment length
        putLE(zip, 0, 2);  // Disk
        putLE(zip, 0, 2);  // Internal attributes
        putLE(zip, 0, 4);  // External attributes
        putLE(zip, 0, 4);  // Local header offset
        zip.insert(zip.end(), name.begin(), name.end());
        putLE(zip, 0x0001, 2); // ZIP64 extended information
        putLE(zip, 8, 2);
        putLE(zip, content.size(), 8);
        size_t cd_size = zip.size() - cd_offset;

        // End of central directory
        putLE(zip, 0x06054b50, 4);
        putLE(zip, 0, 2); // Disk
        putLE(zip, 0, 2); // Disk of the central directory
        putLE(zip, 1, 2); // Files on this disk
        putLE(zip, 1, 2); // Files
        putLE(zip, cd_size, 4);
        putLE(zip, cd_offset, 4);
        putLE(zip, 0, 2); // Comment length
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

    // Makes a directory read only while it is in scope
    struct ReadOnlyDir
    {
        std::filesystem::path dir;
        std::filesystem::perms saved;

        ReadOnlyDir(const std::filesystem::path &d) : dir(d), saved(std::filesystem::status(d).permissions())
        {
            std::filesystem::permissions(dir, std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write,
                                         std::filesystem::perm_options::remove);
        }
        ~ReadOnlyDir() { std::filesystem::permissions(dir, saved); }
    };

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

    // The size the archive claims is checked before anything is allocated for it
    SECTION("an archive claiming a file of several GB")
    {
        Bytes zip = wrongSizeZip("A_EMWIN-RADBIGUS.PNG", "a few bytes, a few bytes, a few bytes, a few bytes", 0xF0000000);
        mux.add(VCID, packetize(textFile("Z_ZIP1.lrit", std::string(zip.begin(), zip.end()), 9, 10), APID));
        mux.add(VCID, packetize(zipFile("Z_ZIP2.lrit", "A_EMWIN-RADALLUS.PNG", "still here"), APID, 10));
        decode(env, mux);
        CHECK(env.log.contains("ZIP entry A_EMWIN-RADBIGUS.PNG claims 4026531840 bytes from ", slog::LOG_ERROR));
        CHECK(env.log.contains(" compressed bytes, more than deflate can produce! Discarding...", slog::LOG_ERROR));
    }

    SECTION("an archive claiming more than its data can decompress to")
    {
        // A few bytes of deflate data, 1 MB once unpacked: under the size limit, but over 1032 times the data
        Bytes zip = wrongSizeZip("A_EMWIN-RADBIGUS.PNG", "a few bytes, a few bytes, a few bytes, a few bytes", 1000000);
        mux.add(VCID, packetize(textFile("Z_ZIP1.lrit", std::string(zip.begin(), zip.end()), 9, 10), APID));
        mux.add(VCID, packetize(zipFile("Z_ZIP2.lrit", "A_EMWIN-RADALLUS.PNG", "still here"), APID, 10));
        decode(env, mux);
        CHECK(env.log.contains("ZIP entry A_EMWIN-RADBIGUS.PNG claims 1000000 bytes from ", slog::LOG_ERROR));
        CHECK(env.log.contains(" compressed bytes, more than deflate can produce! Discarding...", slog::LOG_ERROR));
    }

    CHECK(env.files() == std::vector<std::string>{"EMWIN/NWS/A_EMWIN-RADALLUS.PNG"});
    CHECK(env.read("EMWIN/NWS/A_EMWIN-RADALLUS.PNG") == "still here");
}

// A file over the size limit that its data could decompress to may be real: it is not unpacked,
// but its archive is saved as received, where the file would have gone
TEST_CASE("GOESLRITDataDecoderModule saves EMWIN archives over the size limit as received", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    const size_t limit = 16 * 1024 * 1024;

    // Compresses about 1000 times, so the archive is small
    Bytes zip = zipOf("A_EMWIN-RADBIGUS.PNG", std::string(limit + 1, 'R'));
    mux.add(VCID, packetize(textFile("Z_ZIP1.lrit", std::string(zip.begin(), zip.end()), 9, 10), APID));
    mux.add(VCID, packetize(zipFile("Z_ZIP2.lrit", "A_EMWIN-RADALLUS.PNG", "still here"), APID, 10));
    decode(env, mux);

    CHECK(env.log.contains("ZIP entry A_EMWIN-RADBIGUS.PNG claims 16777217 bytes, over the 16777216 byte limit for EMWIN files! Saving the archive as received...", slog::LOG_WARN));
    REQUIRE(env.files() == std::vector<std::string>{"EMWIN/NWS/A_EMWIN-RADALLUS.PNG", "EMWIN/NWS/A_EMWIN-RADBIGUS.PNG.zip"});
    CHECK(bytesDiff(readBytes(env.path("EMWIN/NWS/A_EMWIN-RADBIGUS.PNG.zip")), zip) == "");
    CHECK(env.read("EMWIN/NWS/A_EMWIN-RADALLUS.PNG") == "still here");
}

// A file that cannot be written is lost, with an error, and the decoder carries on
TEST_CASE("GOESLRITDataDecoderModule carries on when it cannot write an EMWIN file", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    auto params = hritParams();
    params["write_emwin_text"] = true;
    Mux mux;
    mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "A_EMWIN-RADALLUS.PNG", "lost"), APID));
    mux.add(VCID, packetize(textFile("A_EMWIN-FXUS61KB.lrit", "lost too", 9, 0), APID, 10));
    mux.add(VCID, packetize(textFile("A-MSG-0001.lrit", "still here"), APID, 20));
    std::filesystem::create_directories(env.out);

    SECTION("its directory cannot be created")
    {
        writeBytes(env.path("EMWIN"), {0}); // A file, so no directory can be made in it
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"Admin Messages/A-MSG-0001.txt", "EMWIN"});
    }

    SECTION("the file cannot be opened")
    {
        // Directories where the files would go
        std::filesystem::create_directories(env.path("EMWIN/NWS/A_EMWIN-RADALLUS.PNG"));
        std::filesystem::create_directories(env.path("EMWIN/Text/A_EMWIN-FXUS61KB.txt"));
        decode(env, mux, params);
        CHECK(env.files() == std::vector<std::string>{"Admin Messages/A-MSG-0001.txt"});
        CHECK(env.log.contains("A_EMWIN-RADALLUS.PNG! cannot open " + env.path("EMWIN/NWS/A_EMWIN-RADALLUS.PNG") + ": ", slog::LOG_ERROR));
    }

    CHECK(env.log.contains("Could not write EMWIN file A_EMWIN-RADALLUS.PNG! ", slog::LOG_ERROR));
    CHECK(env.log.contains("Could not write EMWIN file A_EMWIN-FXUS61KB.txt! ", slog::LOG_ERROR));
    CHECK(env.read("Admin Messages/A-MSG-0001.txt") == "still here");
}

// The same for the other kinds of file: the file is lost, with one error naming it, and the
// file after it is still saved
TEST_CASE("GOESLRITDataDecoderModule carries on when it cannot write a file", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    auto params = hritParams();
    Mux mux;
    std::filesystem::create_directories(env.out);

    std::string gif = "GIF89a, or so the decoder is told";
    Bytes gif_file = lritFile(0, {imageStructureRecord(8, COLUMNS, LINES, 1), annotationRecord("CHART.lrit"), noaaRecord(1, 0, 0, 5)}, Bytes(gif.begin(), gif.end()));
    Bytes message = textFile("A-MSG-0001.lrit", "a message");
    std::string error; // Expected once
    std::vector<std::string> saved;

    SECTION("Admin Messages is a file")
    {
        writeBytes(env.path("Admin Messages"), {0}); // A file, so no directory can be made in it
        mux.add(VCID, packetize(message, APID, 0));
        mux.add(VCID, packetize(gif_file, APID, 10));
        decode(env, mux, params);
        error = "Could not process file A-MSG-0001.lrit! ";
        saved = {"Admin Messages", "IMAGES/CHART.lrit.gif"};
    }

#ifndef _WIN32
    SECTION("Admin Messages cannot be created")
    {
        if (geteuid() == 0)
            SKIP("root can write anywhere");

        // The output directory is read only, the image directories are already there
        std::filesystem::create_directories(env.path("IMAGES/Unknown"));
        ReadOnlyDir read_only(env.out);
        mux.add(VCID, packetize(message, APID, 0));
        mux.add(VCID, packetize(gif_file, APID, 10));
        decode(env, mux, params);
        error = "Could not process file A-MSG-0001.lrit! filesystem error: cannot create directory: Permission denied";
        saved = {"IMAGES/CHART.lrit.gif"};
    }
#endif

    SECTION("a message cannot be opened")
    {
        std::filesystem::create_directories(env.path("Admin Messages/A-MSG-0001.txt")); // A directory where the file would go
        mux.add(VCID, packetize(message, APID, 0));
        mux.add(VCID, packetize(gif_file, APID, 10));
        decode(env, mux, params);
        error = "Could not process file A-MSG-0001.lrit! cannot open " + env.path("Admin Messages/A-MSG-0001.txt") + ": ";
        saved = {"IMAGES/CHART.lrit.gif"};
    }

#ifdef __linux__
    SECTION("a message cannot be written")
    {
        if (!std::filesystem::exists("/dev/full"))
            SKIP("No /dev/full");
        std::filesystem::create_directories(env.path("Admin Messages"));
        std::filesystem::create_symlink("/dev/full", env.path("Admin Messages/A-MSG-0001.txt")); // Opens, but every write fails
        mux.add(VCID, packetize(message, APID, 0));
        mux.add(VCID, packetize(gif_file, APID, 10));
        decode(env, mux, params);
        error = "Could not process file A-MSG-0001.lrit! cannot write " + env.path("Admin Messages/A-MSG-0001.txt") + ": ";
        saved = {"IMAGES/CHART.lrit.gif"};
    }
#endif

    SECTION("a GIF cannot be opened")
    {
        std::filesystem::create_directories(env.path("IMAGES/CHART.lrit.gif"));
        mux.add(VCID, packetize(gif_file, APID, 0));
        mux.add(VCID, packetize(message, APID, 10));
        decode(env, mux, params);
        error = "Could not process file CHART.lrit! cannot open " + env.path("IMAGES/CHART.lrit.gif") + ": ";
        saved = {"Admin Messages/A-MSG-0001.txt"};
    }

    SECTION("an image cannot be saved")
    {
        std::filesystem::create_directories(env.path("IMAGES/CHART.lrit.png"));
        mux.add(VCID, riceImagePackets(riceImageHeaders("CHART.lrit"), imageLines(), APID));
        mux.add(VCID, packetize(message, APID, 100));
        decode(env, mux, params);
        error = "Could not process file CHART.lrit! cannot open " + env.path("IMAGES/CHART.lrit.png") + ": ";
        saved = {"Admin Messages/A-MSG-0001.txt"};
    }

#ifdef __linux__
    SECTION("an image cannot be written")
    {
        if (!std::filesystem::exists("/dev/full"))
            SKIP("No /dev/full");
        std::filesystem::create_directories(env.path("IMAGES"));
        std::filesystem::create_symlink("/dev/full", env.path("IMAGES/CHART.lrit.png"));
        mux.add(VCID, riceImagePackets(riceImageHeaders("CHART.lrit"), imageLines(), APID));
        mux.add(VCID, packetize(message, APID, 100));
        decode(env, mux, params);
        error = "Could not process file CHART.lrit! cannot write " + env.path("IMAGES/CHART.lrit.png");
        saved = {"Admin Messages/A-MSG-0001.txt"};
    }
#endif

    SECTION("LRIT is a file, with write_lrit")
    {
        params["write_lrit"] = true;
        writeBytes(env.path("LRIT"), {0});
        mux.add(VCID, packetize(message, APID, 0));
        mux.add(VCID, packetize(textFile("A-MSG-0002.lrit", "another message"), APID, 10));
        decode(env, mux, params);
        error = "Could not write LRIT file A-MSG-0001.lrit! ";
        CHECK(env.log.contains("Could not write LRIT file A-MSG-0002.lrit! ", slog::LOG_ERROR));
        saved = {"Admin Messages/A-MSG-0001.txt", "Admin Messages/A-MSG-0002.txt", "LRIT"};
    }

    SECTION("a DCS file cannot be opened, with write_dcs")
    {
        params["write_dcs"] = true;
        std::filesystem::create_directories(env.path("DCS_LRIT/pH-24123120000-A.lrit"));
        mux.add(VCID, packetize(lritFile(130, {annotationRecord("pH-24123120000-A.lrit"), noaaRecord(8, 0, 0, 0)}, randomBytes(300, 1)), APID, 0));
        mux.add(VCID, packetize(message, APID, 10));
        decode(env, mux, params);
        error = "Could not process file pH-24123120000-A.lrit! cannot open " + env.path("DCS_LRIT/pH-24123120000-A.lrit") + ": ";
        saved = {"Admin Messages/A-MSG-0001.txt"};
    }

    INFO(env.log.dump());
    CHECK(env.log.contains(error, slog::LOG_ERROR));
    std::string log = env.log.dump();
    size_t pos = log.find(error);
    CHECK((pos != std::string::npos && log.find(error, pos + 1) == std::string::npos));
    CHECK(env.files() == saved);
}

// A segmented image that cannot be saved does not hold up the images after it
TEST_CASE("GOESLRITDataDecoderModule carries on when it cannot save a segmented image", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;

    // Two full disk images of channel 13, ten minutes apart, in 2 segments of 8 lines
    const int segments = 2, seg_lines = 8;
    auto segment = [&](const std::string &time, int seg, int seq)
    {
        std::string ancillary = "Time of frame start = 2024-05-02T" + time + ".7Z;Satellite = G16;Instrument = FM1;Channel = 13;Region = Full Disk;";
        Bytes headers = lritHeaders(0,
                                    {imageStructureRecord(8, COLUMNS, seg_lines, 1), annotationRecord("OR_ABI-L2-CMIPF-M6C13_G16_s" + time.substr(0, 2) + time.substr(3, 2) + ".lrit"),
                                     timeStampRecord(24228, 43220000), ancillaryTextRecord(ancillary), noaaRecord(16, 13, 0, 1),
                                     segmentIdentificationRecord(7, seg, 0, seg * seg_lines, segments, COLUMNS, segments * seg_lines)},
                                    0);
        mux.add(VCID, riceImagePackets(headers, imageLines(seg_lines, seg * seg_lines), APID, seq));
    };

    // The directory of the first image is a file
    std::string first = "IMAGES/GOES-16/Full Disk/2024-05-02_12-00-20";
    std::string second = "IMAGES/GOES-16/Full Disk/2024-05-02_12-10-20/";
    std::filesystem::create_directories(env.path("IMAGES/GOES-16/Full Disk"));
    writeBytes(env.path(first), {0});

    SECTION("complete images, saved as soon as they are")
    {
        segment("12:00:20", 0, 0);
        segment("12:00:20", 1, 100);
        segment("12:10:20", 0, 200);
        segment("12:10:20", 1, 300);
    }

    // The first image is saved when the next one starts, the second at the end
    SECTION("incomplete images")
    {
        segment("12:00:20", 0, 0);
        segment("12:10:20", 1, 100);
    }

    mux.add(VCID, packetize(textFile("A-MSG-0001.lrit", "still here"), APID, 400));
    decode(env, mux);

    INFO(env.log.dump());
    CHECK(env.files() == std::vector<std::string>{"Admin Messages/A-MSG-0001.txt", first, second + "G16_13_20240502T121020Z.png", second + "product.cbor"});
    std::string error = "Could not save image OR_ABI-L2-CMIPF-M6C13_G16_s1200.lrit! ";
    CHECK(env.log.contains(error, slog::LOG_ERROR));
    std::string log = env.log.dump();
    CHECK(log.find(error, log.find(error) + 1) == std::string::npos);
}

TEST_CASE("GOESLRITDataDecoderModule unpacks EMWIN files at their real size", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    Mux mux;
    Bytes content;

    // The largest real products are GOES images of about 700 kB, which do not compress
    SECTION("a large image")
    {
        content = randomBytes(1000000, 1);
        mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "A_EMWIN-G16CIRUS.JPG", std::string(content.begin(), content.end())), APID));
    }

    SECTION("a file of the size limit")
    {
        content = Bytes(16 * 1024 * 1024, 'G');
        mux.add(VCID, packetize(zipFile("Z_ZIP1.lrit", "A_EMWIN-G16CIRUS.JPG", std::string(content.begin(), content.end())), APID));
    }

    // The size in the central directory is only the marker for a ZIP64 field, which holds
    // the real one
    SECTION("a size in a ZIP64 field")
    {
        std::string text = "the real size of this is in the ZIP64 field";
        content = Bytes(text.begin(), text.end());
        Bytes zip = zip64SizeZip("A_EMWIN-G16CIRUS.JPG", text);
        mux.add(VCID, packetize(textFile("Z_ZIP1.lrit", std::string(zip.begin(), zip.end()), 9, 10), APID));
    }

    decode(env, mux);
    REQUIRE(env.files() == std::vector<std::string>{"EMWIN/GOES/A_EMWIN-G16CIRUS.JPG"});
    // Checked before reading it back, as a file of the size in the 32-bit field would be 4 GB
    REQUIRE(std::filesystem::file_size(env.path("EMWIN/GOES/A_EMWIN-G16CIRUS.JPG")) == content.size());
    CHECK(bytesDiff(readBytes(env.path("EMWIN/GOES/A_EMWIN-G16CIRUS.JPG")), content) == "");
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

TEST_CASE("GOESLRITDataDecoderModule pads a short uncompressed image without a NOAA header to its size", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // 2 lines of an image of LINES lines
    DecoderEnv env;
    auto params = hritParams();
    Mux mux;
    Bytes pixels;
    for (int y = 0; y < 2; y++)
        for (uint8_t p : linePixels(y))
            pixels.push_back(p);
    Bytes file = lritFile(0, {imageStructureRecord(8, COLUMNS, LINES, 0), annotationRecord("CHART.lrit")}, pixels);
    size_t headers = file.size() - pixels.size();
    mux.add(VCID, packetize(file, APID));

    SECTION("raw file")
    {
        params["write_images"] = false;
        params["write_lrit"] = true;
        decode(env, mux, params);
        REQUIRE(env.exists("LRIT/CHART.lrit"));
        CHECK(std::filesystem::file_size(env.path("LRIT/CHART.lrit")) == headers + COLUMNS * LINES);
    }

    SECTION("image")
    {
        decode(env, mux, params);
        REQUIRE(env.files() == std::vector<std::string>{"IMAGES/CHART.lrit.png"});
        std::vector<int> lines = {0, 1};
        lines.resize(LINES, -1); // Zeros
        CHECK(savedLines(env.path("IMAGES/CHART.lrit.png")) == lines);
    }
}

TEST_CASE("GOESLRITDataDecoderModule does not save a compressed image that is neither Rice nor GIF and is smaller than its size", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // Compressed, but neither Rice nor GIF
    DecoderEnv env;
    Mux mux;
    mux.add(VCID, packetize(lritFile(0, {imageStructureRecord(8, COLUMNS, LINES, 1), annotationRecord("CHART.lrit"), noaaRecord(1, 0, 0, 2)}, Bytes(COLUMNS, 1)), APID));
    decode(env, mux);
    CHECK(env.files().empty());
    CHECK(env.log.contains("File CHART.lrit is smaller than its 64x24 image, not saving it as an image", slog::LOG_ERROR));
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

TEST_CASE("GOESLRITDataDecoderModule keeps the lines after a Rice compressed line cut short in place", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // The Rice decoder decodes such a line as far as its input goes, and only reports that part
    DecoderEnv env;
    Mux mux;
    auto lines = imageLines();
    lines[9].cut = riceLine(lines[9].pixels).size() / 2;
    mux.add(VCID, riceImagePackets(riceImageHeaders("IMAGE.lrit"), lines, APID));
    decode(env, mux);

    CHECK(savedLines(env.path("IMAGES/IMAGE.lrit.png")) == linesWith({{9, -2}}));
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

TEST_CASE("GOESLRITDataDecoderModule fill_missing skips an image of 0 columns", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // The lines left to fill are counted in columns
    DecoderEnv env;
    Mux mux;
    auto params = hritParams();
    params["fill_missing"] = true;
    mux.add(VCID, packetize(lritFile(0, {imageStructureRecord(8, 0, LINES, 0), annotationRecord("EMPTY.lrit"), noaaRecord(1, 0, 0, 0)}, Bytes(4 * COLUMNS, 1)), APID, 0, 100));
    mux.add(VCID, packetize(textFile("NEXT.lrit", "next file"), APID, 50));
    decode(env, mux, params);

    CHECK(env.log.contains("File EMPTY.lrit has 0 columns in its image structure record, its missing lines cannot be filled", slog::LOG_WARN));
    CHECK(env.exists("Admin Messages/NEXT.txt"));
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

TEST_CASE("GOESLRITDataDecoderModule uses 16 pixels per block when the Rice settings of the file give 0", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // The Rice decoder divides by the pixels per block
    DecoderEnv env;
    Mux mux;
    auto settings = riceSettings(COLUMNS);
    Bytes headers = lritHeaders(0, {imageStructureRecord(8, COLUMNS, LINES, 1), annotationRecord("IMAGE.lrit"), noaaRecord(1, 0, 0, 1), riceCompressionRecord(settings.options_mask, 0, 1)}, 0);
    mux.add(VCID, riceImagePackets(headers, imageLines(), APID));
    decode(env, mux);

    REQUIRE(settings.pixels_per_block == 16); // What the lines were compressed with
    CHECK(savedLines(env.path("IMAGES/IMAGE.lrit.png")) == linesWith({}));
    CHECK(env.log.contains("File IMAGE.lrit has 0 pixels per block in its Rice compression record, using 16", slog::LOG_WARN));
}

TEST_CASE("GOESLRITDataDecoderModule uses 16 pixels per block when the Rice settings of the file give a size the Rice decoder does not support", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // The Rice decoder wrote past its buffers with 1 and odd sizes (#36)
    int pixels_per_block = GENERATE(1, 2, 3, 9, 12, 128, 255);
    DecoderEnv env;
    Mux mux;
    auto settings = riceSettings(COLUMNS);
    Bytes headers =
        lritHeaders(0, {imageStructureRecord(8, COLUMNS, LINES, 1), annotationRecord("IMAGE.lrit"), noaaRecord(1, 0, 0, 1), riceCompressionRecord(settings.options_mask, pixels_per_block, 1)}, 0);
    mux.add(VCID, riceImagePackets(headers, imageLines(), APID));
    decode(env, mux);

    REQUIRE(settings.pixels_per_block == 16); // What the lines were compressed with
    CHECK(savedLines(env.path("IMAGES/IMAGE.lrit.png")) == linesWith({}));
    CHECK(env.log.contains("File IMAGE.lrit has " + std::to_string(pixels_per_block) + " pixels per block in its Rice compression record, using 16", slog::LOG_WARN));
}

TEST_CASE("GOESLRITDataDecoderModule reports the lines of a Rice compressed image of 0 columns as failed", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // The Rice decoder has no buffer to decode a line of 0 pixels to, and rejects it
    DecoderEnv env;
    Mux mux;
    Bytes headers = lritHeaders(0, {imageStructureRecord(8, 0, LINES, 1), annotationRecord("EMPTY.lrit"), noaaRecord(1, 0, 0, 1)}, 0);
    mux.add(VCID, riceImagePackets(headers, imageLines(), APID));
    mux.add(VCID, packetize(textFile("NEXT.lrit", "next file"), APID, 50));
    decode(env, mux);

    CHECK(env.log.contains("Rice decompression failed. This may be an issue!", slog::LOG_WARN));
    CHECK(env.exists("Admin Messages/NEXT.txt"));
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

TEST_CASE("GOESLRITDataDecoderModule decodes a Rice compressed image whose headers take two packets", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // The lines are counted from the last packet of the headers: the second one is not a
    // lost line
    DecoderEnv env;
    Mux mux;
    Bytes headers = riceImageHeaders("SPLIT.lrit");
    Bytes first = transportHeader(headers.size());
    first.insert(first.end(), headers.begin(), headers.end());

    std::vector<Bytes> packets = {packet(APID, SEQ_FIRST, 0, Bytes(first.begin(), first.begin() + 30)), packet(APID, SEQ_CONTINUATION, 1, Bytes(first.begin() + 30, first.end()))};
    auto lines = imageLines();
    for (size_t i = 0; i < lines.size(); i++)
        packets.push_back(packet(APID, i == lines.size() - 1 ? SEQ_LAST : SEQ_CONTINUATION, 2 + i, riceLine(lines[i].pixels)));
    mux.add(VCID, packets);
    decode(env, mux);

    CHECK(env.files() == std::vector<std::string>{"IMAGES/SPLIT.lrit.png"});
    CHECK(savedLines(env.path("IMAGES/SPLIT.lrit.png")) == linesWith({}));
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

TEST_CASE("GOESLRITDataDecoderModule saves a segment of an image of 0 segments as an image of its own", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // The segments are placed in their image by max_row / max_segment
    DecoderEnv env;
    Mux mux;
    const int seg_lines = 8;
    std::string name = "OR_ABI-L2-CMIPF-M6C13_G16_s20241231200207_e20241231209527_c20241231210007.lrit";
    std::string ancillary = "Time of frame start = 2024-05-02T12:00:20.7Z;Satellite = G16;Instrument = FM1;Channel = 13;Region = Full Disk;";
    Bytes headers = lritHeaders(0,
                                {imageStructureRecord(8, COLUMNS, seg_lines, 1), annotationRecord(name), timeStampRecord(24228, 43220000), ancillaryTextRecord(ancillary), noaaRecord(16, 13, 0, 1),
                                 segmentIdentificationRecord(7, 1, 0, seg_lines, 0, COLUMNS, 3 * seg_lines)},
                                0);
    mux.add(VCID, riceImagePackets(headers, imageLines(seg_lines, seg_lines), APID));
    decode(env, mux);

    std::string dir = "IMAGES/GOES-16/Full Disk/2024-05-02_12-00-20/";
    std::string image = dir + "G16_13_20240502T120020Z.png";
    CHECK(env.files() == std::vector<std::string>{image, dir + "product.cbor"});
    CHECK(savedLines(env.path(image), seg_lines, seg_lines) == linesWith({{0, 8}, {1, 9}, {2, 10}, {3, 11}, {4, 12}, {5, 13}, {6, 14}, {7, 15}}, seg_lines));
    CHECK(env.log.contains("File " + name + " has 0 segments in its segment identification header", slog::LOG_WARN));
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

// File names come off the air, and the output directory from the user. Both must be logged
// as they are, not used as the printf format of the logger: a "%%" in them stays "%%".
TEST_CASE("GOESLRITDataDecoderModule logs file names and the output directory as they are", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    DecoderEnv env;
    env.out = env.tmp.file("out_100%%");
    Mux mux;
    auto params = hritParams();
    params["write_lrit"] = true;
    std::string gif = "GIF89a, or so the decoder is told";
    mux.add(VCID, packetize(textFile("MSG_100%%.lrit", "message"), APID, 0));
    mux.add(VCID, packetize(lritFile(0, {imageStructureRecord(8, COLUMNS, LINES, 1), annotationRecord("CHART_%%d.lrit"), noaaRecord(1, 0, 0, 5)}, Bytes(gif.begin(), gif.end())), APID, 10));
    decode(env, mux, params);

    REQUIRE(env.files() == std::vector<std::string>{"Admin Messages/MSG_100%%.txt", "IMAGES/CHART_%%d.lrit.gif", "LRIT/CHART_%%d.lrit", "LRIT/MSG_100%%.lrit"});
    INFO(env.log.dump());
    // The demuxer, for every file
    CHECK(env.log.contains("New LRIT file : MSG_100%%.lrit", slog::LOG_INFO));
    CHECK(env.log.contains("New LRIT file : CHART_%%d.lrit", slog::LOG_INFO));
    // Each way of saving a file
    CHECK(env.log.contains("Writing file " + env.out + "/Admin Messages/MSG_100%%.txt...", slog::LOG_INFO));
    CHECK(env.log.contains("Writing file " + env.out + "/IMAGES/CHART_%%d.lrit.gif...", slog::LOG_INFO));
    CHECK(env.log.contains("Writing file " + env.out + "/LRIT/MSG_100%%.lrit...", slog::LOG_INFO));
    CHECK(env.log.contains("Writing file " + env.out + "/LRIT/CHART_%%d.lrit...", slog::LOG_INFO));
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

TEST_CASE("GOESLRITDataDecoderModule drops files that lose a packet of their headers and carries on", "[plugins][goes_support][goes_lrit_data_decoder]")
{
    // Headers of 16 + 303 + 14 bytes, sent in packets of 200 bytes: the second packet, with
    // the rest of the headers, is lost
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
    // The gap in the packet counter is seen as soon as the last packet comes
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
