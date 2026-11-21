#include "helpers/compare.h"
#include "helpers/random.h"
#include "xrit_support/xrit/transport/xrit_demux.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// The demuxer is given the VCDUs of files sent by xrit_support/xrit_test_helpers.h. The
// source packets are kept small (200 bytes), so that files span several packets and M_PDUs
// without megabytes of data.

using namespace xrit_test;
using test_helpers::bytesDiff;
using test_helpers::randomBytes;
namespace xr = satdump::xrit;

namespace
{
    const size_t PACKET = 200;

    Bytes textFile(const std::string &name, size_t size, uint32_t seed) { return lritFile(2, {annotationRecord(name), noaaRecord(1, 0, 0, 0)}, randomBytes(size, seed)); }

    Bytes imageFile(const std::string &name, int columns, int lines, uint32_t seed)
    {
        return lritFile(0, {imageStructureRecord(8, columns, lines, 0), annotationRecord(name), noaaRecord(1, 0, 0, 0)}, randomBytes(columns * lines, seed));
    }

    // Feeds VCDUs to the demuxer, and collects the files it returns
    std::vector<xr::XRITFile> demux(xr::XRITDemux &demuxer, const std::vector<Bytes> &vcdus)
    {
        std::vector<xr::XRITFile> files;
        for (auto &v : vcdus)
        {
            Bytes c = cadu(v);
            for (auto &f : demuxer.work(c.data()))
                files.push_back(f);
        }
        return files;
    }

    std::vector<xr::XRITFile> demux(const std::vector<Bytes> &vcdus)
    {
        xr::XRITDemux demuxer;
        return demux(demuxer, vcdus);
    }
} // namespace

TEST_CASE("XRITDemux returns a file sent in a single packet", "[plugins][xrit_support][xrit_demux]")
{
    Bytes file = textFile("SINGLE.lrit", 100, 1);
    Mux mux;
    mux.add(5, packetize(file, 40));

    auto files = demux(mux.vcdus());

    REQUIRE(files.size() == 1);
    CHECK(bytesDiff(files[0].lrit_data, file) == "");
    CHECK(files[0].filename == "SINGLE.lrit");
    CHECK(files[0].vcid == 5);
    CHECK(files[0].header_parsed);
    CHECK(files[0].total_header_length == 16 + 14 + 14);
    CHECK(files[0].hasHeader<xr::AnnotationRecord>());
}

TEST_CASE("XRITDemux reassembles a file sent in several packets", "[plugins][xrit_support][xrit_demux]")
{
    // 30 packets over 8 M_PDUs
    Bytes file = textFile("LONG.lrit", 6000, 2);
    Mux mux;
    mux.add(5, packetize(file, 40, 100, PACKET));

    auto vcdus = mux.vcdus();
    CHECK(vcdus.size() == 8);
    auto files = demux(vcdus);

    REQUIRE(files.size() == 1);
    CHECK(bytesDiff(files[0].lrit_data, file) == "");
    CHECK(files[0].filename == "LONG.lrit");
}

TEST_CASE("XRITDemux returns files in the order they complete", "[plugins][xrit_support][xrit_demux]")
{
    Bytes a = textFile("A.lrit", 1500, 3), b = textFile("B.lrit", 300, 4), c = textFile("C.lrit", 50, 5);
    Mux mux;
    mux.add(5, packetize(a, 40, 0, PACKET));
    mux.add(5, packetize(b, 40, 8, PACKET));
    mux.add(5, packetize(c, 40, 10, PACKET));

    auto files = demux(mux.vcdus());

    REQUIRE(files.size() == 3);
    CHECK(files[0].filename == "A.lrit");
    CHECK(files[1].filename == "B.lrit");
    CHECK(files[2].filename == "C.lrit");
    CHECK(bytesDiff(files[0].lrit_data, a) == "");
    CHECK(bytesDiff(files[1].lrit_data, b) == "");
    CHECK(bytesDiff(files[2].lrit_data, c) == "");
}

TEST_CASE("XRITDemux keeps the files of each APID and virtual channel apart", "[plugins][xrit_support][xrit_demux]")
{
    Bytes a = textFile("A.lrit", 900, 6), b = textFile("B.lrit", 900, 7), c = textFile("C.lrit", 900, 8);
    auto pa = packetize(a, 40, 0, PACKET), pb = packetize(b, 41, 0, PACKET), pc = packetize(c, 40, 0, PACKET);
    REQUIRE(pa.size() == pb.size());
    REQUIRE(pa.size() == pc.size());

    // Two APIDs interleaved on VC 5, and the APID of the first one again on VC 6
    Mux mux;
    for (size_t i = 0; i < pa.size(); i++)
    {
        mux.add(5, pa[i]);
        mux.add(5, pb[i]);
        mux.add(6, pc[i]);
    }

    auto files = demux(mux.vcdus());

    REQUIRE(files.size() == 3);
    std::map<std::string, xr::XRITFile> by_name;
    for (auto &f : files)
        by_name[f.filename] = f;
    CHECK(bytesDiff(by_name["A.lrit"].lrit_data, a) == "");
    CHECK(bytesDiff(by_name["B.lrit"].lrit_data, b) == "");
    CHECK(bytesDiff(by_name["C.lrit"].lrit_data, c) == "");
    CHECK(by_name["A.lrit"].vcid == 5);
    CHECK(by_name["B.lrit"].vcid == 5);
    CHECK(by_name["C.lrit"].vcid == 6);
}

TEST_CASE("XRITDemux ignores fill frames and idle packets", "[plugins][xrit_support][xrit_demux]")
{
    Bytes file = textFile("DATA.lrit", 2000, 9);
    auto packets = packetize(file, 40, 0, PACKET);

    Mux mux;
    mux.fill();
    for (size_t i = 0; i < packets.size(); i++)
    {
        mux.add(5, packets[i]);
        if (i % 3 == 0) // The idle packet completes the M_PDU, and a fill frame follows
        {
            mux.flush(5);
            mux.fill();
        }
    }
    mux.fill();

    auto files = demux(mux.vcdus());

    REQUIRE(files.size() == 1);
    CHECK(bytesDiff(files[0].lrit_data, file) == "");
}

TEST_CASE("XRITDemux drops a file with a corrupted packet", "[plugins][xrit_support][xrit_demux]")
{
    Bytes bad = textFile("BAD.lrit", 1500, 10), good = textFile("GOOD.lrit", 1500, 11);
    auto packets = packetize(bad, 40, 0, PACKET);

    auto which = GENERATE(0, 3, 7);
    INFO("corrupted packet " << which << " of " << packets.size());
    REQUIRE((size_t)which < packets.size());
    packets[which][20] ^= 0xFF;

    Mux mux;
    mux.add(5, packets);
    mux.add(5, packetize(good, 40, 20, PACKET));

    auto files = demux(mux.vcdus());

    // Only the next file comes out
    REQUIRE(files.size() == 1);
    CHECK(files[0].filename == "GOOD.lrit");
    CHECK(bytesDiff(files[0].lrit_data, good) == "");
}

TEST_CASE("XRITDemux keeps an image going over a corrupted packet", "[plugins][xrit_support][xrit_demux]")
{
    // An image loses a few pixels, which is better than losing it all
    Bytes file = imageFile("IMAGE.lrit", 64, 32, 12);
    auto packets = packetize(file, 40, 0, PACKET);
    REQUIRE(packets.size() > 5);

    SECTION("in the data: the bytes of the packet are kept as received")
    {
        packets[4][20] ^= 0xFF;
        Mux mux;
        mux.add(5, packets);

        xr::XRITDemux demuxer;
        std::vector<bool> bad_crcs;
        demuxer.onProcessData = [&](xr::XRITFile &, ccsds::CCSDSPacket &, bool bad_crc)
        {
            bad_crcs.push_back(bad_crc);
            return true;
        };
        auto files = demux(demuxer, mux.vcdus());

        REQUIRE(files.size() == 1);
        REQUIRE(files[0].lrit_data.size() == file.size());
        size_t differing = 0;
        for (size_t i = 0; i < file.size(); i++)
            differing += files[0].lrit_data[i] != file[i];
        CHECK(differing == 1);

        // The callback is told which packet it was: the fourth one after the header
        REQUIRE(bad_crcs.size() == packets.size() - 1);
        CHECK(std::count(bad_crcs.begin(), bad_crcs.end(), true) == 1);
        CHECK(bad_crcs[3]);
    }

    SECTION("not in the first packet: without its headers the file is not known to be an image")
    {
        packets[0][20] ^= 0xFF;
        Mux mux;
        mux.add(5, packets);
        CHECK(demux(mux.vcdus()).empty());
    }

    SECTION("not in the last packet")
    {
        packets.back()[20] ^= 0xFF;
        Mux mux;
        mux.add(5, packets);
        CHECK(demux(mux.vcdus()).empty());
    }
}

TEST_CASE("XRITDemux without CRC checking accepts corrupted packets", "[plugins][xrit_support][xrit_demux]")
{
    Bytes file = textFile("ANY.lrit", 1500, 13);
    auto packets = packetize(file, 40, 0, PACKET);
    packets[3][20] ^= 0xFF;
    Mux mux;
    mux.add(5, packets);

    xr::XRITDemux demuxer(MPDU_ZONE_BYTES, false);
    auto files = demux(demuxer, mux.vcdus());

    REQUIRE(files.size() == 1);
    CHECK(files[0].lrit_data.size() == file.size());
    CHECK(bytesDiff(files[0].lrit_data, file) != "");
}

TEST_CASE("XRITDemux returns an unfinished file when the next one starts", "[plugins][xrit_support][xrit_demux]")
{
    Bytes cut = textFile("CUT.lrit", 1500, 14), next = textFile("NEXT.lrit", 300, 15);
    auto packets = packetize(cut, 40, 0, PACKET);
    size_t kept = packets.size() - 3; // The end of the file is never sent
    packets.resize(kept);

    Mux mux;
    mux.add(5, packets);
    mux.add(5, packetize(next, 40, 20, PACKET));

    auto files = demux(mux.vcdus());

    REQUIRE(files.size() == 2);
    CHECK(files[0].filename == "CUT.lrit");
    CHECK(files[0].lrit_data.size() == kept * PACKET - 10); // Minus the transport file header
    CHECK(std::equal(files[0].lrit_data.begin(), files[0].lrit_data.end(), cut.begin()));
    CHECK(files[1].filename == "NEXT.lrit");
    CHECK(bytesDiff(files[1].lrit_data, next) == "");
}

TEST_CASE("XRITDemux ignores packets of a file whose start was missed", "[plugins][xrit_support][xrit_demux]")
{
    Bytes missed = textFile("MISSED.lrit", 1500, 16), next = textFile("NEXT.lrit", 300, 17);
    auto packets = packetize(missed, 40, 0, PACKET);
    packets.erase(packets.begin()); // Reception started after the first packet

    Mux mux;
    mux.add(5, packets);
    mux.add(5, packetize(next, 40, 20, PACKET));

    auto files = demux(mux.vcdus());

    REQUIRE(files.size() == 1);
    CHECK(files[0].filename == "NEXT.lrit");
}

TEST_CASE("XRITDemux calls back once the headers are complete, for each packet of data, and at the end", "[plugins][xrit_support][xrit_demux]")
{
    // The headers (16 + 9 + 303 + 14 bytes) need two packets
    Bytes file = lritFile(0, {imageStructureRecord(8, 64, 16, 0), annotationRecord(std::string(300, 'N')), noaaRecord(1, 0, 0, 0)}, randomBytes(64 * 16, 18));
    auto packets = packetize(file, 40, 500, PACKET);
    Mux mux;
    mux.add(5, packets);

    xr::XRITDemux demuxer;
    std::vector<std::string> events;
    std::vector<int> counts;
    size_t size_at_header = 0;
    demuxer.onParseHeader = [&](xr::XRITFile &f)
    {
        events.push_back("header");
        size_at_header = f.lrit_data.size();
        CHECK(f.hasHeader<xr::ImageStructureRecord>());
        CHECK(f.filename == std::string(300, 'N'));
    };
    demuxer.onProcessData = [&](xr::XRITFile &, ccsds::CCSDSPacket &pkt, bool bad_crc)
    {
        events.push_back("data");
        counts.push_back(pkt.header.packet_sequence_count);
        CHECK_FALSE(bad_crc);
        return true;
    };
    demuxer.onFinalizeData = [&](xr::XRITFile &f)
    {
        events.push_back("final");
        f.custom_flags[7] = 42; // Changes made here are in the file that is returned
    };

    auto files = demux(demuxer, mux.vcdus());

    // The first packet is not "data": it is added without asking
    std::vector<std::string> expected = {"data", "header"};
    expected.insert(expected.end(), packets.size() - 2, "data");
    expected.push_back("final");
    CHECK(events == expected);
    CHECK(size_at_header == 2 * PACKET - 10);
    CHECK(counts.front() == 501);
    CHECK(counts.back() == 500 + (int)packets.size() - 1);

    REQUIRE(files.size() == 1);
    CHECK(files[0].custom_flags[7] == 42);
    CHECK(bytesDiff(files[0].lrit_data, file) == "");
}

TEST_CASE("XRITDemux leaves out the data its callback takes care of", "[plugins][xrit_support][xrit_demux]")
{
    // What a decompressor does: it adds what it decoded itself
    Bytes file = textFile("COMPRESSED.lrit", 1500, 19);
    auto packets = packetize(file, 40, 0, PACKET);
    Mux mux;
    mux.add(5, packets);

    xr::XRITDemux demuxer;
    demuxer.onProcessData = [&](xr::XRITFile &f, ccsds::CCSDSPacket &, bool)
    {
        f.lrit_data.push_back(0xEE);
        return false;
    };
    auto files = demux(demuxer, mux.vcdus());

    REQUIRE(files.size() == 1);
    Bytes expected(file.begin(), file.begin() + PACKET - 10);
    expected.insert(expected.end(), packets.size() - 1, 0xEE);
    CHECK(bytesDiff(files[0].lrit_data, expected) == "");
}
