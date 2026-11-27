#include "helpers/temp_files.h"
#include "libs/miniz/miniz.h"
#include "utils/file/folder_file_iterators.h"
#include "utils/file/zip_file_iterators.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <map>

using satdump::utils::FilesIteratorItem;
using satdump::utils::FolderFileIteratorItem;
using satdump::utils::FolderFilesIterator;
using satdump::utils::ZipFilesIterator;

namespace
{
    using test_helpers::TempDir;
    using test_helpers::uniqueTempPath;
    using test_helpers::writeBytes;

    std::vector<uint8_t> bytes(const std::string &s) { return std::vector<uint8_t>(s.begin(), s.end()); }

    std::vector<uint8_t> patterned(size_t n)
    {
        std::vector<uint8_t> v(n);
        for (size_t i = 0; i < n; i++)
            v[i] = (uint8_t)((i * 7) ^ (i >> 8));
        return v;
    }

    // Drains an iterator, returning name -> payload for every file item and the number of entries seen
    std::map<std::string, std::vector<uint8_t>> drain(satdump::utils::FilesIterator &it, int &entries)
    {
        std::map<std::string, std::vector<uint8_t>> out;
        std::shared_ptr<FilesIteratorItem> item;
        entries = 0;
        while (it.getNext(item))
        {
            entries++;
            if (item)
                out[item->name] = item->getPayload();
        }
        return out;
    }

    struct TempZip
    {
        std::filesystem::path path;
        TempZip(const std::map<std::string, std::vector<uint8_t>> &entries) : path(uniqueTempPath("archive.zip"))
        {
            for (auto &e : entries)
                if (!mz_zip_add_mem_to_archive_file_in_place(path.string().c_str(), e.first.c_str(), e.second.data(), e.second.size(), nullptr, 0, MZ_DEFAULT_COMPRESSION))
                    throw std::runtime_error("Could not build test zip");
        }
        ~TempZip()
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };
} // namespace

TEST_CASE("FolderFileIteratorItem reads the whole file and names it by filename", "[utils][file_iterators]")
{
    TempDir dir("item");
    auto data = patterned(5000);
    writeBytes(dir.path / "payload.bin", data);

    FolderFileIteratorItem item((dir.path / "payload.bin").string());
    CHECK(item.name == "payload.bin");
    CHECK(item.getPayload() == data);

    FolderFileIteratorItem named((dir.path / "payload.bin").string(), "custom");
    CHECK(named.name == "custom");
    CHECK(named.getPayload() == data);
}

TEST_CASE("FolderFileIteratorItem payload of a deleted file throws", "[utils][file_iterators]")
{
    // tellg() returns -1 for a file that cannot be opened, so the resize to SIZE_MAX throws
    FolderFileIteratorItem item(uniqueTempPath("gone.bin").string());
    CHECK_THROWS(item.getPayload());
}

TEST_CASE("FolderFilesIterator walks files recursively", "[utils][file_iterators]")
{
    TempDir dir("folder");
    std::filesystem::create_directories(dir.path / "sub" / "deeper");
    writeBytes(dir.path / "a.txt", bytes("alpha"));
    writeBytes(dir.path / "sub" / "b.dat", patterned(70000));
    writeBytes(dir.path / "sub" / "deeper" / "c", bytes("charlie"));

    FolderFilesIterator it(dir.path.string());
    int entries = 0;
    auto files = drain(it, entries);

    // Directories are reported as entries with a null item
    CHECK(entries == 5);
    REQUIRE(files.size() == 3);
    CHECK(files["a.txt"] == bytes("alpha"));
    CHECK(files["b.dat"] == patterned(70000));
    CHECK(files["c"] == bytes("charlie"));

    SECTION("getNext keeps returning false once exhausted")
    {
        std::shared_ptr<FilesIteratorItem> item = std::make_shared<FolderFileIteratorItem>("x", "x");
        CHECK_FALSE(it.getNext(item));
        CHECK_FALSE(it.getNext(item));
    }

    SECTION("reset restarts the walk")
    {
        it.reset();
        int entries2 = 0;
        auto files2 = drain(it, entries2);
        CHECK(entries2 == 5);
        CHECK(files2 == files);
    }
}

TEST_CASE("FolderFilesIterator on an empty folder yields nothing", "[utils][file_iterators]")
{
    TempDir dir("empty_folder");
    FolderFilesIterator it(dir.path.string());
    std::shared_ptr<FilesIteratorItem> item;
    CHECK_FALSE(it.getNext(item));
    CHECK(item == nullptr);
}

TEST_CASE("FolderFilesIterator on a missing folder throws", "[utils][file_iterators]")
{
    CHECK_THROWS_AS(FolderFilesIterator(uniqueTempPath("no_such_dir").string()), std::filesystem::filesystem_error);
}

TEST_CASE("ZipFilesIterator lists and extracts archive entries", "[utils][file_iterators]")
{
    std::map<std::string, std::vector<uint8_t>> content = {
        {"one.txt", bytes("first file")},
        {"nested/dir/two.bin", patterned(100000)},
        {"zeros.raw", std::vector<uint8_t>(20000, 0)},
    };
    TempZip zip(content);

    ZipFilesIterator it(zip.path.string());
    int entries = 0;
    auto files = drain(it, entries);

    CHECK(entries == 3);
    REQUIRE(files.size() == 3);
    // Directory components inside the archive are dropped from the item name
    CHECK(files["one.txt"] == content["one.txt"]);
    CHECK(files["two.bin"] == content["nested/dir/two.bin"]);
    CHECK(files["zeros.raw"] == content["zeros.raw"]);

    SECTION("reset restarts from the first entry")
    {
        std::shared_ptr<FilesIteratorItem> item;
        CHECK_FALSE(it.getNext(item));
        it.reset();
        int entries2 = 0;
        auto files2 = drain(it, entries2);
        CHECK(entries2 == 3);
        CHECK(files2 == files);
    }
}

TEST_CASE("ZipFilesIterator handles an empty entry", "[utils][file_iterators]")
{
    TempZip zip({{"empty.txt", {}}, {"full.txt", bytes("data")}});
    ZipFilesIterator it(zip.path.string());
    int entries = 0;
    auto files = drain(it, entries);
    CHECK(entries == 2);
    CHECK(files.count("empty.txt") == 1);
    CHECK(files["empty.txt"].empty());
    CHECK(files["full.txt"] == bytes("data"));
}

TEST_CASE("ZipFilesIterator rejects invalid archives", "[utils][file_iterators]")
{
    CHECK_THROWS_AS(ZipFilesIterator(uniqueTempPath("missing.zip").string()), satdump::satdump_exception_t);

    TempDir dir("notzip");
    writeBytes(dir.path / "fake.zip", bytes("this is definitely not a zip archive"));
    CHECK_THROWS_AS(ZipFilesIterator((dir.path / "fake.zip").string()), satdump::satdump_exception_t);
}

namespace
{
    using test_helpers::readBytes;

    void putLE32(std::vector<uint8_t> &v, size_t off, uint32_t val)
    {
        for (int i = 0; i < 4; i++)
            v[off + i] = (val >> (8 * i)) & 0xFF;
    }

    // Overwrites the uncompressed size of the only entry, in both its local and central directory headers
    void patchUncompressedSize(std::vector<uint8_t> &zip, uint32_t size)
    {
        int patched = 0;
        for (size_t i = 0; i + 30 <= zip.size(); i++)
        {
            if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 3 && zip[i + 3] == 4)
                putLE32(zip, i + 22, size), patched++;
            else if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 1 && zip[i + 3] == 2)
                putLE32(zip, i + 24, size), patched++;
        }
        if (patched != 2)
            throw std::runtime_error("Unexpected test zip layout");
    }

    std::vector<uint8_t> zipBytes(const std::map<std::string, std::vector<uint8_t>> &entries)
    {
        TempZip zip(entries);
        return readBytes(zip.path);
    }

    void putLE(std::vector<uint8_t> &v, uint64_t val, int n)
    {
        for (int i = 0; i < n; i++)
            v.push_back((val >> (8 * i)) & 0xFF);
    }

    /*
     * Builds a one-entry archive by hand, so that the central directory can
     * claim any sizes. The local header holds the real ones. If z64 is not
     * empty, it is written as a ZIP64 extra field (the uncompressed then the
     * compressed size, as for central directory fields set to 0xFFFFFFFF).
     */
    std::vector<uint8_t> rawZip(const std::vector<uint8_t> &data, bool deflate, uint32_t cd_comp, uint32_t cd_uncomp, std::vector<uint64_t> z64 = {})
    {
        std::vector<uint8_t> comp = data;
        if (deflate)
        {
            size_t len = 0;
            void *p = tdefl_compress_mem_to_heap(data.data(), data.size(), &len, TDEFL_DEFAULT_MAX_PROBES);
            if (!p)
                throw std::runtime_error("Could not compress test data");
            comp.assign((uint8_t *)p, (uint8_t *)p + len);
            mz_free(p);
        }
        const uint32_t crc = mz_crc32(MZ_CRC32_INIT, data.data(), data.size());
        const std::string name = "x.bin";
        const uint16_t method = deflate ? 8 : 0;

        std::vector<uint8_t> z;
        putLE(z, 0x04034B50, 4); // Local header
        putLE(z, 20, 2), putLE(z, 0, 2), putLE(z, method, 2), putLE(z, 0, 2), putLE(z, 0, 2);
        putLE(z, crc, 4), putLE(z, comp.size(), 4), putLE(z, data.size(), 4);
        putLE(z, name.size(), 2), putLE(z, 0, 2);
        z.insert(z.end(), name.begin(), name.end());
        z.insert(z.end(), comp.begin(), comp.end());

        const size_t cd_ofs = z.size();
        putLE(z, 0x02014B50, 4); // Central directory header
        putLE(z, 45, 2), putLE(z, 45, 2), putLE(z, 0, 2), putLE(z, method, 2), putLE(z, 0, 2), putLE(z, 0, 2);
        putLE(z, crc, 4), putLE(z, cd_comp, 4), putLE(z, cd_uncomp, 4);
        putLE(z, name.size(), 2), putLE(z, z64.empty() ? 0 : 4 + 8 * z64.size(), 2);
        putLE(z, 0, 2), putLE(z, 0, 2), putLE(z, 0, 2), putLE(z, 0, 4), putLE(z, 0, 4);
        z.insert(z.end(), name.begin(), name.end());
        if (!z64.empty())
        {
            putLE(z, 1, 2), putLE(z, 8 * z64.size(), 2);
            for (uint64_t v : z64)
                putLE(z, v, 8);
        }
        const size_t cd_size = z.size() - cd_ofs;

        putLE(z, 0x06054B50, 4); // End of central directory
        putLE(z, 0, 2), putLE(z, 0, 2), putLE(z, 1, 2), putLE(z, 1, 2);
        putLE(z, cd_size, 4), putLE(z, cd_ofs, 4), putLE(z, 0, 2);
        return z;
    }

    struct ZipReader
    {
        mz_zip_archive zip{};
        ZipReader(const std::filesystem::path &p)
        {
            if (!mz_zip_reader_init_file(&zip, p.string().c_str(), 0))
                throw std::runtime_error("Could not open test zip");
        }
        ~ZipReader() { mz_zip_reader_end(&zip); }
    };
} // namespace

TEST_CASE("extractZipEntry extracts valid entries", "[utils][file_iterators]")
{
    std::vector<uint8_t> data = patterned(50000);
    std::vector<uint8_t> zeros(1 << 20, 0);
    TempZip zip({{"data.bin", data}, {"empty.bin", {}}, {"zeros.bin", zeros}});
    ZipReader r(zip.path);

    std::map<std::string, std::vector<uint8_t>> got;
    for (int i = 0; i < (int)mz_zip_reader_get_num_files(&r.zip); i++)
    {
        char name[256];
        REQUIRE(mz_zip_reader_get_filename(&r.zip, i, name, sizeof(name)));
        std::vector<uint8_t> out = {1, 2, 3};
        CHECK(satdump::utils::extractZipEntry(&r.zip, i, out));
        got[name] = out;
    }

    REQUIRE(got.size() == 3);
    CHECK(got["data.bin"] == data);
    CHECK(got["empty.bin"].empty());
    // A highly compressible entry is still within the plausible expansion ratio
    CHECK(got["zeros.bin"] == zeros);
}

TEST_CASE("extractZipEntry rejects invalid indices", "[utils][file_iterators]")
{
    TempZip zip({{"a.txt", bytes("a")}});
    ZipReader r(zip.path);
    std::vector<uint8_t> out = {1};
    CHECK_FALSE(satdump::utils::extractZipEntry(&r.zip, -1, out));
    CHECK(out.empty());
    out = {1};
    CHECK_FALSE(satdump::utils::extractZipEntry(&r.zip, 1, out));
    CHECK(out.empty());
}

TEST_CASE("extractZipEntry rejects an implausibly large claimed size", "[utils][file_iterators]")
{
    TempDir dir("zip_oversize");
    std::vector<uint8_t> raw = zipBytes({{"small.txt", bytes("tiny payload")}});
    // ~2 GB claimed for a few bytes of compressed data
    patchUncompressedSize(raw, 0x7FFFFFF0);
    writeBytes(dir.path / "oversize.zip", raw);

    ZipReader r(dir.path / "oversize.zip");
    REQUIRE(mz_zip_reader_get_num_files(&r.zip) == 1);
    std::vector<uint8_t> out;
    CHECK_FALSE(satdump::utils::extractZipEntry(&r.zip, 0, out));
    CHECK(out.empty());
    // Rejected up front: miniz was never asked to allocate or extract
    CHECK(mz_zip_peek_last_error(&r.zip) == MZ_ZIP_NO_ERROR);

    // The iterator returns an empty payload instead of allocating
    ZipFilesIterator it((dir.path / "oversize.zip").string());
    int entries = 0;
    auto files = drain(it, entries);
    CHECK(entries == 1);
    REQUIRE(files.count("small.txt") == 1);
    CHECK(files["small.txt"].empty());
}

TEST_CASE("extractZipEntry checks the compressed size the archive claims", "[utils][file_iterators]")
{
    // Sizes miniz does not check against the archive at init. Each must be
    // rejected before the claimed size is allocated (several GiB here)
    const uint64_t G = 1ULL << 30;
    const std::vector<uint8_t> data = bytes("hello world");
    std::vector<uint8_t> raw;

    SECTION("both central directory sizes 0xFFFFFFFF, without a ZIP64 extra") { raw = rawZip(data, true, 0xFFFFFFFF, 0xFFFFFFFF); }
    SECTION("equal ZIP64 sizes past the archive, stored") { raw = rawZip(data, false, 0xFFFFFFFF, 0xFFFFFFFF, {3 * G, 3 * G}); }
    SECTION("equal ZIP64 sizes past the archive, deflated") { raw = rawZip(data, true, 0xFFFFFFFF, 0xFFFFFFFF, {3 * G, 3 * G}); }
    SECTION("ZIP64 compressed size 0 with data claimed") { raw = rawZip(data, true, 0xFFFFFFFF, 0xFFFFFFFF, {1000, 0}); }

    TempDir dir("zip_compsize");
    writeBytes(dir.path / "bad.zip", raw);
    ZipReader r(dir.path / "bad.zip");
    REQUIRE(mz_zip_reader_get_num_files(&r.zip) == 1);
    std::vector<uint8_t> out = {1};
    CHECK_FALSE(satdump::utils::extractZipEntry(&r.zip, 0, out));
    CHECK(out.empty());
    CHECK(mz_zip_peek_last_error(&r.zip) == MZ_ZIP_NO_ERROR);
}

TEST_CASE("extractZipEntry extracts stored and ZIP64 entries", "[utils][file_iterators]")
{
    std::vector<uint8_t> data = patterned(10240);
    std::vector<uint8_t> expected = data;
    std::vector<uint8_t> raw;

    SECTION("stored") { raw = rawZip(data, false, data.size(), data.size()); }
    SECTION("stored, empty")
    {
        expected.clear();
        raw = rawZip({}, false, 0, 0);
    }
    SECTION("deflated, empty")
    {
        expected.clear();
        raw = rawZip({}, true, 2, 0);
    }
    SECTION("ZIP64 sizes")
    {
        std::vector<uint8_t> deflated = rawZip(data, true, 0, 0);
        // The compressed size, as written in the local header
        uint64_t comp = deflated[18] | deflated[19] << 8 | deflated[20] << 16 | (uint64_t)deflated[21] << 24;
        raw = rawZip(data, true, 0xFFFFFFFF, 0xFFFFFFFF, {data.size(), comp});
    }

    TempDir dir("zip_valid_raw");
    writeBytes(dir.path / "ok.zip", raw);
    ZipReader r(dir.path / "ok.zip");
    std::vector<uint8_t> out = {1};
    CHECK(satdump::utils::extractZipEntry(&r.zip, 0, out));
    CHECK(out == expected);
}

TEST_CASE("extractZipEntry fails on a size mismatch or corrupt data", "[utils][file_iterators]")
{
    std::vector<uint8_t> data = patterned(4000);
    std::vector<uint8_t> raw = zipBytes({{"data.bin", data}});

    SECTION("claimed size larger than the real data") { patchUncompressedSize(raw, data.size() + 100); }

    SECTION("corrupted compressed data")
    {
        // Past the local header (30 bytes) and the name, inside the deflate stream
        for (size_t i = 30 + 8 + 10; i < 30 + 8 + 40; i++)
            raw[i] ^= 0x5A;
    }

    TempDir dir("zip_corrupt");
    writeBytes(dir.path / "bad.zip", raw);
    ZipReader r(dir.path / "bad.zip");
    std::vector<uint8_t> out;
    CHECK_FALSE(satdump::utils::extractZipEntry(&r.zip, 0, out));
    CHECK(out.empty());
}
