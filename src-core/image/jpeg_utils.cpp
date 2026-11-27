#include "jpeg_utils.h"
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
extern "C"
{
#include "libs/jpeg/jpeglib.h"
}
// #include "libs/openjp2/openjpeg.h"

namespace satdump
{
    namespace image
    {
        struct jpeg_error_struct
        {
            struct jpeg_error_mgr pub;
            jmp_buf setjmp_buffer;
        };

        static void libjpeg_error_func(j_common_ptr cinfo) { longjmp(((jpeg_error_struct *)cinfo->err)->setjmp_buffer, 1); }

        static void libjpeg_error_func_ignore(j_common_ptr /*cinfo*/)
        {
            // longjmp(((jpeg_error_struct *)cinfo->err)->setjmp_buffer, 1);
        }

        // Reads all scanlines with the libjpeg API of the right sample size
        // (jpeg_read_scanlines, jpeg12_read_scanlines or jpeg16_read_scanlines)
        template <typename T, typename F>
        static void read_jpeg_samples(jpeg_decompress_struct &cinfo, std::vector<uint16_t> &samples, F read_scanlines)
        {
            std::vector<T> line(cinfo.output_width);
            while (cinfo.output_scanline < cinfo.output_height)
            {
                T *buffer_array[1] = {line.data()};
                JDIMENSION y = cinfo.output_scanline;
                if (read_scanlines(&cinfo, buffer_array, 1) != 1)
                    break;
                for (JDIMENSION x = 0; x < cinfo.output_width; x++)
                    samples[y * cinfo.output_width + x] = (uint16_t)line[x];
            }
        }

        Image decompress_jpeg_samples(uint8_t *data, int length, bool ignore_errors, int &precision)
        {
            precision = 0;
            std::vector<uint16_t> samples;

            // Huge thanks to https://gist.github.com/PhirePhly/3080633
            jpeg_error_struct jerr;
            jpeg_decompress_struct cinfo;
            memset(&cinfo, 0, sizeof(cinfo));

            // Init
            cinfo.err = jpeg_std_error(&jerr.pub);
            jerr.pub.error_exit = ignore_errors ? libjpeg_error_func_ignore : libjpeg_error_func;

            if (setjmp(jerr.setjmp_buffer))
            {
                jpeg_destroy_decompress(&cinfo);
                return Image();
            }

            jpeg_create_decompress(&cinfo);

            // Parse and start decompressing
            jpeg_mem_src(&cinfo, data, length);
            jpeg_read_header(&cinfo, FALSE);
            cinfo.out_color_space = JCS_GRAYSCALE; // A single channel is returned, convert colour images
            jpeg_start_decompress(&cinfo);

            // Init output buffer
            int data_precision = cinfo.data_precision;
            samples.resize((size_t)cinfo.output_width * cinfo.output_height);

            // Decompress. libjpeg-turbo has one API per sample size, which
            // must match the data precision (lossless JPEGs can have 2 to 16
            // bits, lossy ones 8 or 12)
            if (data_precision <= 8)
                read_jpeg_samples<JSAMPLE>(cinfo, samples, jpeg_read_scanlines);
            else if (data_precision <= 12)
                read_jpeg_samples<J12SAMPLE>(cinfo, samples, jpeg12_read_scanlines);
            else
                read_jpeg_samples<J16SAMPLE>(cinfo, samples, jpeg16_read_scanlines);

            // Cleanup
            // jpeg_finish_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);

            // Init SatDump image
            Image img(16, cinfo.output_width, cinfo.output_height, 1);

            // Copy over
            for (size_t i = 0; i < samples.size(); i++)
                img.set(i, samples[i]);

            precision = data_precision;
            return img;
        }

        Image decompress_jpeg(uint8_t *data, int length, bool ignore_errors)
        {
            int precision;
            Image samples = decompress_jpeg_samples(data, length, ignore_errors, precision);
            if (samples.size() == 0)
                return Image();

            // Init SatDump image
            Image img(8, samples.width(), samples.height(), 1);

            // Copy over, scaling down data of more than 8 bits
            int shift = precision > 8 ? precision - 8 : 0;
            for (size_t i = 0; i < samples.size(); i++)
                img.set(i, samples.get(i) >> shift);

            return img;
        }

        /*Image<uint16_t> decompress_j2k_openjp2(uint8_t *data, int length)
        {
            Image<uint16_t> img;

            // Init decoder parameters
            opj_dparameters_t core;
            memset(&core, 0, sizeof(opj_dparameters_t));
            opj_set_default_decoder_parameters(&core);

            // Set input buffer info struct
            opj_buffer_info bufinfo;
            bufinfo.buf = data;
            bufinfo.cur = data;
            bufinfo.len = length;

            // Setup image, stream and codec
            opj_image_t *image = NULL;
            opj_stream_t *l_stream = opj_stream_create_buffer_stream(&bufinfo, true);
            opj_codec_t *l_codec = opj_create_decompress(OPJ_CODEC_J2K);

            // Check we could open the stream
            if (!l_stream)
            {
                opj_destroy_codec(l_codec);
                return img;
            }

            // Setup decoder
            if (!opj_setup_decoder(l_codec, &core))
            {
                opj_stream_destroy(l_stream);
                opj_destroy_codec(l_codec);
                return img;
            }

            // Read header
            if (!opj_read_header(l_stream, l_codec, &image))
            {
                opj_stream_destroy(l_stream);
                opj_destroy_codec(l_codec);
                opj_image_destroy(image);
                return img;
            }

            // Decode image
            if (!(opj_decode(l_codec, l_stream, image) &&
                  opj_end_decompress(l_codec, l_stream)))
            {
                opj_destroy_codec(l_codec);
                opj_stream_destroy(l_stream);
                opj_image_destroy(image);
                return img;
            }

            // Parse into CImg
            img = Image<uint16_t>(image->x1, image->y1, 1);
            for (int i = 0; i < int(image->x1 * image->y1); i++)
                img[i] = image->comps[0].data[i];

            // Free everything up
            opj_destroy_codec(l_codec);
            opj_stream_destroy(l_stream);
            opj_image_destroy(image);

            return img;
        }*/
    } // namespace image
} // namespace satdump
