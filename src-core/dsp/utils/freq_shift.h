#pragma once

/**
 * @file freq_shift.h
 */

#include "common/dsp/complex.h"
#include "dsp/block_simple.h"

namespace satdump
{
    namespace ndsp
    {
        /**
         * @brief Frequency Shift class
         *
         * This is a implementation of the old block
         * using the new NDSP system. TODOREWORK
         *
         * @param raw_shift if true, allows for manual frequency shift
         * @param freq_shift frequency shift value
         * @param samplerate stream sample rate
         */
        class FreqShiftBlock : public BlockSimple<complex_t, complex_t>
        {
        private:
            // Parameters, see Block::set_param()
            bool raw_shift = false;
            double freq_shift = 0;
            double samplerate = 6e6;

        private:
            complex_t phase_delta;
            complex_t phase;

        public:
            FreqShiftBlock();
            ~FreqShiftBlock();

            uint32_t process(complex_t *input, uint32_t nsamples, complex_t *output);

            void init()
            {
                bool d_raw_shift;
                double d_freq_shift, d_samplerate;
                apply_params(
                    [&]()
                    {
                        d_raw_shift = raw_shift;
                        d_freq_shift = freq_shift;
                        d_samplerate = samplerate;
                    });

                if (d_raw_shift == false)
                {
                    phase = complex_t(1, 0);
                    phase_delta = complex_t(cos(2.0 * M_PI * (d_freq_shift / d_samplerate)), sin(2.0 * M_PI * (d_freq_shift / d_samplerate)));
                }
                else if (d_raw_shift == true)
                {
                    phase_delta = complex_t(cosf(d_freq_shift), sinf(d_freq_shift));
                }
            }

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "freq_shift", "float", "Frequency Shift");
                add_param_simple(p, "samplerate", "float", "Samplerate");
                add_param_simple(p, "raw_shift", "bool", "Raw Shift");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "raw_shift")
                    return get_param(raw_shift);
                else if (key == "freq_shift")
                    return get_param(freq_shift);
                else if (key == "samplerate")
                    return get_param(samplerate);
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "raw_shift")
                    set_param(raw_shift, v.get<bool>());
                else if (key == "freq_shift")
                    set_param(freq_shift, v.get<double>());
                else if (key == "samplerate")
                    set_param(samplerate, v.get<double>());
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump
