#pragma once

// Shared helpers for the xRIT tests (xrit_support and the plugins built on it): the sending
// side of the LRIT / HRIT file transfer, which SatDump only receives. Everything lives in the
// xrit_test namespace and is inline, so including this header from several translation units
// is fine.
//
// - The record functions build the header records of a file, and lritFile() puts them
//   together with the primary header and the data field.
// - packetize() cuts a file into CCSDS source packets (transport file header first, CRC at
//   the end of each), or riceImagePackets() sends one Rice compressed image line per packet
//   as GOES-R does.
// - Mux lays the packets of each virtual channel out in M_PDUs, and gives the 892 byte VCDUs
//   in the order they were completed. cadu() frames one for a .cadu file.

#include "common/ccsds/ccsds.h"
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

extern "C"
{
#include "libs/aec/szlib.h"
}

namespace xrit_test
{
    using Bytes = std::vector<uint8_t>;

    constexpr int VCDU_BYTES = 892;      // VCDU primary header, M_PDU header and packet zone
    constexpr int MPDU_ZONE_BYTES = 884; // Packet zone of a M_PDU
    constexpr int VCID_FILL = 63;
    constexpr int APID_IDLE = 2047;

    inline void put16(Bytes &b, uint16_t v)
    {
        b.push_back(v >> 8);
        b.push_back(v & 0xFF);
    }

    inline void put32(Bytes &b, uint32_t v)
    {
        put16(b, v >> 16);
        put16(b, v & 0xFFFF);
    }

    ///////////////////////////////////////////////////////////
    // Header records
    ///////////////////////////////////////////////////////////

    // Header type, record length, then the content
    inline Bytes record(uint8_t type, const Bytes &content)
    {
        Bytes r = {type};
        put16(r, content.size() + 3);
        r.insert(r.end(), content.begin(), content.end());
        return r;
    }

    inline Bytes textRecord(uint8_t type, const std::string &text) { return record(type, Bytes(text.begin(), text.end())); }

    // Type 1. compression is 0 (none), 1 (lossless) or 2 (lossy)
    inline Bytes imageStructureRecord(uint8_t bits_per_pixel, uint16_t columns, uint16_t lines, uint8_t compression)
    {
        Bytes c = {bits_per_pixel};
        put16(c, columns);
        put16(c, lines);
        c.push_back(compression);
        return record(1, c);
    }

    // Type 2. The projection name is padded to 32 characters
    inline Bytes imageNavigationRecord(std::string projection, int32_t column_scaling, int32_t line_scaling, int32_t column_offset, int32_t line_offset)
    {
        projection.resize(32, ' ');
        Bytes c(projection.begin(), projection.end());
        put32(c, column_scaling);
        put32(c, line_scaling);
        put32(c, column_offset);
        put32(c, line_offset);
        return record(2, c);
    }

    // Type 4: the file name
    inline Bytes annotationRecord(const std::string &text) { return textRecord(4, text); }

    // Type 5: CCSDS day segmented time
    inline Bytes timeStampRecord(uint16_t days, uint32_t milliseconds_of_day)
    {
        Bytes c = {0x40}; // P-field
        put16(c, days);
        put32(c, milliseconds_of_day);
        return record(5, c);
    }

    // Type 6: "key = value;" pairs
    inline Bytes ancillaryTextRecord(const std::string &text) { return textRecord(6, text); }

    // Type 128, NOAA specific: place of a segment in its image
    inline Bytes segmentIdentificationRecord(uint16_t image_id, uint16_t segment, uint16_t start_column, uint16_t start_line, uint16_t max_segment, uint16_t max_column, uint16_t max_row)
    {
        Bytes c;
        for (uint16_t v : {image_id, segment, start_column, start_line, max_segment, max_column, max_row})
            put16(c, v);
        return record(128, c);
    }

    // Type 129, NOAA specific: product, and the compression of the data field (0 none,
    // 1 Rice, 5 GIF, 10 ZIP)
    inline Bytes noaaRecord(uint16_t product_id, uint16_t product_subid, uint16_t parameter, uint8_t compression)
    {
        Bytes c = {'N', 'O', 'A', 'A'};
        put16(c, product_id);
        put16(c, product_subid);
        put16(c, parameter);
        c.push_back(compression);
        return record(129, c);
    }

    // Type 131, NOAA specific: Rice compression settings
    inline Bytes riceCompressionRecord(uint16_t flags, uint8_t pixels_per_block, uint8_t scanlines_per_packet)
    {
        Bytes c;
        put16(c, flags);
        c.push_back(pixels_per_block);
        c.push_back(scanlines_per_packet);
        return record(131, c);
    }

    // The primary header (type 0) followed by these records. file_type is 0 for an image,
    // 1 for a message, 2 for text and 130 for DCS data. data_bytes is the size of the data
    // field that will follow.
    inline Bytes lritHeaders(uint8_t file_type, const std::vector<Bytes> &records, uint64_t data_bytes)
    {
        uint32_t total = 16;
        for (auto &r : records)
            total += r.size();

        Bytes c = {file_type};
        put32(c, total);
        put32(c, (data_bytes * 8) >> 32);
        put32(c, (data_bytes * 8) & 0xFFFFFFFF);
        Bytes file = record(0, c);

        for (auto &r : records)
            file.insert(file.end(), r.begin(), r.end());
        return file;
    }

    // A whole file: headers, then the data field
    inline Bytes lritFile(uint8_t file_type, const std::vector<Bytes> &records, const Bytes &data)
    {
        Bytes file = lritHeaders(file_type, records, data.size());
        file.insert(file.end(), data.begin(), data.end());
        return file;
    }

    ///////////////////////////////////////////////////////////
    // Source packets
    ///////////////////////////////////////////////////////////

    // CRC of the LRIT / HRIT source packets: CRC-16/CCITT-FALSE
    inline uint16_t crc16(const uint8_t *data, size_t size)
    {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < size; i++)
        {
            crc ^= (uint16_t)data[i] << 8;
            for (int b = 0; b < 8; b++)
                crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
        return crc;
    }

    enum SequenceFlag
    {
        SEQ_CONTINUATION = 0,
        SEQ_FIRST = 1,
        SEQ_LAST = 2,
        SEQ_SINGLE = 3,
    };

    // One source packet: primary header, user data, CRC. With bad_crc the CRC is wrong.
    inline Bytes packet(uint16_t apid, int sequence_flag, uint16_t sequence_count, Bytes user_data, bool bad_crc = false)
    {
        uint16_t crc = crc16(user_data.data(), user_data.size()) ^ (bad_crc ? 0x5555 : 0);
        put16(user_data, crc);

        ccsds::CCSDSHeader header;
        header.version = 0;
        header.type = 0;
        header.secondary_header_flag = false;
        header.apid = apid;
        header.sequence_flag = sequence_flag;
        header.packet_sequence_count = sequence_count & 0x3FFF;
        header.packet_length = user_data.size() - 1;
        header.encodeHDR();

        Bytes out(header.raw, header.raw + 6);
        out.insert(out.end(), user_data.begin(), user_data.end());
        return out;
    }

    // The transport file header that precedes a file: file counter and length in bits
    inline Bytes transportHeader(uint64_t file_bytes, uint16_t file_counter = 0)
    {
        Bytes h;
        put16(h, file_counter);
        put32(h, (file_bytes * 8) >> 32);
        put32(h, (file_bytes * 8) & 0xFFFFFFFF);
        return h;
    }

    // A file cut into source packets of at most max_user_data bytes, numbered from
    // first_count. Smaller packets than the 8190 bytes of the standard keep the tests small.
    inline std::vector<Bytes> packetize(const Bytes &file, uint16_t apid, uint16_t first_count = 0, size_t max_user_data = 8190)
    {
        Bytes stream = transportHeader(file.size());
        stream.insert(stream.end(), file.begin(), file.end());

        std::vector<Bytes> packets;
        size_t count = (stream.size() + max_user_data - 1) / max_user_data;
        for (size_t i = 0; i < count; i++)
        {
            Bytes chunk(stream.begin() + i * max_user_data, stream.begin() + std::min(stream.size(), (i + 1) * max_user_data));
            int flag = count == 1 ? SEQ_SINGLE : (i == 0 ? SEQ_FIRST : (i == count - 1 ? SEQ_LAST : SEQ_CONTINUATION));
            packets.push_back(packet(apid, flag, first_count + i, chunk));
        }
        return packets;
    }

    // The Rice settings SatDump uses for a GOES-R image without a Rice compression record
    inline SZ_com_t riceSettings(int columns)
    {
        SZ_com_t p;
        p.options_mask = SZ_ALLOW_K13_OPTION_MASK | SZ_MSB_OPTION_MASK | SZ_NN_OPTION_MASK | SZ_RAW_OPTION_MASK;
        p.bits_per_pixel = 8;
        p.pixels_per_block = 16;
        p.pixels_per_scanline = columns;
        return p;
    }

    // One Rice compressed image line
    inline Bytes riceLine(const Bytes &line)
    {
        SZ_com_t p = riceSettings(line.size());
        Bytes out(line.size() * 2 + 64);
        size_t size = out.size();
        if (SZ_BufftoBuffCompress(out.data(), &size, line.data(), line.size(), &p) != SZ_OK)
            throw std::runtime_error("Rice compression failed");
        out.resize(size);
        return out;
    }

    // A Rice compressed image as GOES-R sends it: the headers in a first packet, then one
    // packet per image line. Packet i + 1 carries line i; set what happens to it in `lines`.
    struct RiceLine
    {
        Bytes pixels;
        bool lost = false;    // The packet is not sent at all
        bool bad_crc = false; // The packet is sent with a wrong CRC
    };

    inline std::vector<Bytes> riceImagePackets(const Bytes &headers, const std::vector<RiceLine> &lines, uint16_t apid, uint16_t first_count = 0)
    {
        Bytes first = transportHeader(headers.size());
        first.insert(first.end(), headers.begin(), headers.end());

        std::vector<Bytes> packets = {packet(apid, SEQ_FIRST, first_count, first)};
        for (size_t i = 0; i < lines.size(); i++)
            if (!lines[i].lost)
                packets.push_back(packet(apid, i == lines.size() - 1 ? SEQ_LAST : SEQ_CONTINUATION, first_count + 1 + i, riceLine(lines[i].pixels), lines[i].bad_crc));
        return packets;
    }

    ///////////////////////////////////////////////////////////
    // Virtual channels
    ///////////////////////////////////////////////////////////

    // Multiplexes source packets into the M_PDUs of their virtual channels
    class Mux
    {
    private:
        struct Channel
        {
            Bytes zone;              // Packet zone being filled
            int first_header = 2047; // Offset of the first packet header in it, if any
            uint32_t counter = 0;
        };

        int zone_bytes;
        std::map<int, Channel> channels;
        std::vector<Bytes> done;
        uint32_t fill_counter = 0;

        void finishZone(int vcid, Channel &ch)
        {
            Bytes vcdu;
            vcdu.push_back(0x40 | (SPACECRAFT_ID >> 2));          // Version 1, spacecraft ID
            vcdu.push_back(((SPACECRAFT_ID & 0b11) << 6) | vcid); // Spacecraft ID, VCID
            vcdu.push_back(ch.counter >> 16);
            vcdu.push_back(ch.counter >> 8);
            vcdu.push_back(ch.counter);
            vcdu.push_back(0); // Signalling field
            put16(vcdu, ch.first_header);
            vcdu.insert(vcdu.end(), ch.zone.begin(), ch.zone.end());
            done.push_back(vcdu);

            ch.counter++;
            ch.zone.clear();
            ch.first_header = 2047;
        }

        void append(int vcid, const Bytes &pkt)
        {
            Channel &ch = channels[vcid];
            for (size_t i = 0; i < pkt.size(); i++)
            {
                if (i == 0 && ch.first_header == 2047)
                    ch.first_header = ch.zone.size();
                ch.zone.push_back(pkt[i]);
                if (ch.zone.size() == (size_t)zone_bytes)
                    finishZone(vcid, ch);
            }
        }

    public:
        static constexpr int SPACECRAFT_ID = 16;

        // The packet zone is 884 bytes in LRIT and HRIT, FengYun-4 LRIT uses 1012
        explicit Mux(int zone_bytes = MPDU_ZONE_BYTES) : zone_bytes(zone_bytes) {}

        // Adds a source packet to a virtual channel
        void add(int vcid, const Bytes &pkt) { append(vcid, pkt); }

        void add(int vcid, const std::vector<Bytes> &packets)
        {
            for (auto &p : packets)
                append(vcid, p);
        }

        // Completes the M_PDU a virtual channel is filling with an idle packet
        void flush(int vcid)
        {
            Channel &ch = channels[vcid];
            if (ch.zone.empty())
                return;
            size_t left = zone_bytes - ch.zone.size();
            if (left < 7) // An idle packet needs its header and one byte: it then also takes the next M_PDU
                left += zone_bytes;

            ccsds::CCSDSHeader header;
            header.version = 0;
            header.type = 0;
            header.secondary_header_flag = false;
            header.apid = APID_IDLE;
            header.sequence_flag = SEQ_SINGLE;
            header.packet_sequence_count = 0;
            header.packet_length = left - 6 - 1;
            header.encodeHDR();

            Bytes idle(header.raw, header.raw + 6);
            idle.resize(left, 0);
            append(vcid, idle);
        }

        // Adds a VCDU of the fill virtual channel, which carries nothing
        void fill()
        {
            Channel ch;
            ch.counter = fill_counter++;
            ch.first_header = 2046;
            ch.zone.assign(zone_bytes, 0);
            finishZone(VCID_FILL, ch);
        }

        // Completes every virtual channel, and returns the VCDUs in the order they were completed
        std::vector<Bytes> vcdus()
        {
            for (auto &c : channels)
                flush(c.first);
            return done;
        }
    };

    // A VCDU as a .cadu file holds it: after the ASM, and followed by its 128 Reed-Solomon
    // check symbols (zeros here, nothing reads them once the frame is decoded)
    inline Bytes cadu(const Bytes &vcdu)
    {
        Bytes c = {0x1A, 0xCF, 0xFC, 0x1D};
        c.insert(c.end(), vcdu.begin(), vcdu.end());
        c.resize(1024, 0);
        return c;
    }

    // The .cadu file of these VCDUs
    inline Bytes caduFile(const std::vector<Bytes> &vcdus)
    {
        Bytes out;
        for (auto &v : vcdus)
        {
            Bytes c = cadu(v);
            out.insert(out.end(), c.begin(), c.end());
        }
        return out;
    }
} // namespace xrit_test
