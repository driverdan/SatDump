#pragma once

#include "common/dsp/block.h"
#include "common/dsp/complex.h"
#include "common/dsp/filter/firdes.h"
#include "dsp/block.h"
#include <volk/volk.h>

namespace satdump
{
    namespace ndsp
    {
        class HilbertBlock : public Block
        {
        private:
            // Parameters, see Block::set_param()
            int p_ntaps = 65;
            int p_buffer_size = 8192 * 8;

        private:
            int d_ntaps = 0;
            int buffer_size = 0;
            float *buffer = nullptr;
            float *taps = nullptr;

            bool work();

            size_t lbuf_size;
            size_t lbuf_offset;

        public:
            HilbertBlock();
            ~HilbertBlock();

            void init()
            {
                // Free if needed
                if (taps != nullptr)
                    volk_free(taps);
                if (buffer != nullptr)
                    volk_free(buffer);
                // Not freed again if firdes::hilbert() throws below
                taps = nullptr;
                buffer = nullptr;

                int ntaps;
                apply_params(
                    [&]()
                    {
                        ntaps = p_ntaps;
                        buffer_size = p_buffer_size;
                    });

                // Init taps
                auto new_taps = dsp::firdes::hilbert(ntaps, dsp::fft::window::WIN_HAMMING, 6.76);
                d_ntaps = new_taps.size();

                // Init buffer : the history of d_ntaps samples, then up to buffer_size new ones
                buffer = dsp::create_volk_buffer<float>(buffer_size + d_ntaps); // TODOREWORK How to handle this from the initial buffer size?

                // Init taps
                taps = (float *)volk_malloc(d_ntaps * sizeof(float), volk_get_alignment());
                for (int j = 0; j < d_ntaps; j++)
                    taps[j] = new_taps[(d_ntaps - 1) - j]; // Reverse taps
            }

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "ntaps", "int");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "ntaps")
                    return get_param(p_ntaps);
                else if (key == "buffer_size")
                    return get_param(p_buffer_size);
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "ntaps")
                    set_param(p_ntaps, v.get<int>());
                else if (key == "buffer_size")
                {
                    // work() processes the input buffer_size samples at a time
                    int bs = v.get<int>();
                    if (bs < 1)
                        return RES_ERR;
                    set_param(p_buffer_size, bs);
                }
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump
