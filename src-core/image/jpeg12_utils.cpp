#include "jpeg12_utils.h"
#include "jpeg_utils.h"

namespace satdump
{
    namespace image
    {
        Image decompress_jpeg12(uint8_t *data, int length, bool ignore_errors)
        {
            int precision;
            Image samples = decompress_jpeg_samples(data, length, ignore_errors, precision);
            if (samples.size() == 0)
                return Image();

            // Init SatDump image
            Image img(16, samples.width(), samples.height(), 1);

            // Copy over, as 12-bit data scaled to 16 bits. Data of more than
            // 12 bits is scaled down to 12 first
            int shift = precision > 12 ? precision - 12 : 0;
            for (size_t i = 0; i < samples.size(); i++)
                img.set(i, (samples.get(i) >> shift) << 4);

            return img;
        }
    } // namespace image
} // namespace satdump
