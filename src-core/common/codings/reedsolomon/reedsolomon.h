#pragma once

#include <cstdint>
extern "C"
{
#include "libs/correct/reed-solomon.h"
}

// RS Decoding based on SatHelper's implementation
namespace reedsolomon
{
    enum RS_TYPE
    {
        RS223,
        RS239
    };

    class ReedSolomon
    {
    private:
        correct_reed_solomon *rs;
        int d_coded_bits;
        int d_parity_bits;
        uint8_t buff[255];
        uint8_t odata[255];
        int fill_bytes;

    public:
        ReedSolomon(RS_TYPE type, int fill_bytes = 0);
        ~ReedSolomon();

        void decode_interlaved(uint8_t *data, bool ccsds, int i, int *errors);
        int decode(uint8_t *data, bool ccsds);

        void encode_interlaved(uint8_t *data, bool ccsds, int i);
        void encode(uint8_t *data, bool ccsds);

        void deinterleave(uint8_t *data, uint8_t *output, uint8_t pos, uint8_t i);
        void interleave(uint8_t *data, uint8_t *output, uint8_t pos, uint8_t i);
    };

    // The rs_avg statistic of the decoders, from the results decode_interlaved() wrote for the i
    // codewords of a frame: the average number of symbols corrected per codeword, or -1 if any of
    // them could not be corrected (the frame is not clean then, whatever the others needed).
    // 0 with no codewords
    inline int average_errors(const int *errors, int i)
    {
        if (i <= 0)
            return 0;
        int sum = 0;
        for (int b = 0; b < i; b++)
        {
            if (errors[b] < 0)
                return -1;
            sum += errors[b];
        }
        return sum / i;
    }
};