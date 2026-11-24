#pragma once

#include "common/dsp/complex.h"
#include "common/dsp/utils/random.h"
#include "dsp/block.h"
#include "dsp/block_helpers.h"

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        class WaveformBlock : public Block
        {
        private:
            // Parameters, see Block::set_param()
            std::string p_waveform = "cosine";
            float p_samprate = 48000;
            float p_freq = 10000;
            float p_amp = 1.0;
            float p_phase = 0.0;
            int p_buffer_size = 8192;

        private:
            std::string d_waveform;
            float d_samprate;
            float d_freq;
            float d_amp;
            float d_phase;
            int d_buffer_size;

            float tmp_val_f;

            dsp::Random d_rng;

            bool work();

        public:
            WaveformBlock();
            ~WaveformBlock();

            void init();

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_list(p, "waveform", "list", {"cosine", "sine"}, "Waveform");
                add_param_simple(p, "waveform", "string", "Waveform");
                add_param_simple(p, "samprate", "float", "Samplerate");
                add_param_simple(p, "freq", "float", "Frequency");
                add_param_simple(p, "amp", "float", "Amplitude");
                add_param_simple(p, "phase", "float", "Phase");
                add_param_simple(p, "bufs", "int", "Buffer Size");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "waveform")
                    return get_param(p_waveform);
                else if (key == "samprate")
                    return get_param(p_samprate);
                else if (key == "freq")
                    return get_param(p_freq);
                else if (key == "amp")
                    return get_param(p_amp);
                else if (key == "phase")
                    return get_param(p_phase);
                else if (key == "bufs")
                    return get_param(p_buffer_size);
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "waveform")
                    set_param(p_waveform, v.get<std::string>());
                else if (key == "samprate")
                    set_param(p_samprate, v.get<float>());
                else if (key == "freq")
                    set_param(p_freq, v.get<float>());
                else if (key == "amp")
                    set_param(p_amp, v.get<float>());
                else if (key == "phase")
                    set_param(p_phase, v.get<float>());
                else if (key == "bufs")
                {
                    // Only used by the constructor TODOREWORK
                    std::lock_guard<std::mutex> lock(params_mtx);
                    p_buffer_size = v;
                }
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump
