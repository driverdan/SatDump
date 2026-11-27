#pragma once

// Encodes test JPEGs with the bundled libjpeg-turbo, for the jpeg_utils and
// jpeg12_utils tests. Everything is inline, as all test files link into one
// binary.

#include "image/image_test_helpers.h"
#include <csetjmp>
#include <cstdio>
#include <string>
#include <vector>
extern "C"
{
#include "libs/jpeg/jpeglib.h"
}

namespace jpeg_test
{
    struct JpegError
    {
        jpeg_error_mgr pub;
        jmp_buf jmp;
    };

    inline void jpegErrorExit(j_common_ptr cinfo) { longjmp(((JpegError *)cinfo->err)->jmp, 1); }

    // Encodes samples of the given precision (8 or 12 for lossy, 2 to 16 for
    // lossless), grayscale or RGB (interleaved) if components is 3.
    // Returns an empty vector if encoding failed.
    inline std::vector<uint8_t> encodeJpeg(const std::vector<int> &samples, int w, int h, int precision, bool lossless, int components = 1)
    {
        image_test::TempDir dir;
        std::string path = dir.file("enc.jpg");
        FILE *fp = fopen(path.c_str(), "wb");
        if (fp == nullptr)
            return {};

        jpeg_compress_struct cinfo;
        JpegError jerr;
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpegErrorExit;
        if (setjmp(jerr.jmp))
        {
            jpeg_destroy_compress(&cinfo);
            fclose(fp);
            return {};
        }

        jpeg_create_compress(&cinfo);
        jpeg_stdio_dest(&cinfo, fp);
        cinfo.image_width = w;
        cinfo.image_height = h;
        cinfo.input_components = components;
        cinfo.in_color_space = components == 3 ? JCS_RGB : JCS_GRAYSCALE;
        cinfo.data_precision = precision; // Before jpeg_set_defaults(), which depends on it
        jpeg_set_defaults(&cinfo);
        if (lossless)
            jpeg_enable_lossless(&cinfo, 1, 0);
        else
            jpeg_set_quality(&cinfo, 95, TRUE);
        jpeg_start_compress(&cinfo, TRUE);

        // Each precision range has its own sample type and write function
        std::vector<JSAMPLE> row8(w * components);
        std::vector<J12SAMPLE> row12(w * components);
        std::vector<J16SAMPLE> row16(w * components);
        while (cinfo.next_scanline < cinfo.image_height)
        {
            const int *line = &samples[cinfo.next_scanline * w * components];
            if (precision <= 8)
            {
                for (int x = 0; x < w * components; x++)
                    row8[x] = line[x];
                JSAMPROW rows[1] = {row8.data()};
                jpeg_write_scanlines(&cinfo, rows, 1);
            }
            else if (precision <= 12)
            {
                for (int x = 0; x < w * components; x++)
                    row12[x] = line[x];
                J12SAMPROW rows[1] = {row12.data()};
                jpeg12_write_scanlines(&cinfo, rows, 1);
            }
            else
            {
                for (int x = 0; x < w * components; x++)
                    row16[x] = line[x];
                J16SAMPROW rows[1] = {row16.data()};
                jpeg16_write_scanlines(&cinfo, rows, 1);
            }
        }
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
        fclose(fp);
        return image_test::readBytes(path);
    }

    // A grayscale pattern covering the whole range of the given precision
    inline std::vector<int> makeSamples(int w, int h, int precision)
    {
        int maxval = (1 << precision) - 1;
        std::vector<int> s(w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                s[y * w + x] = (x * maxval / (w - 1) + y * 37) % (maxval + 1);
        return s;
    }
} // namespace jpeg_test
