#pragma once

#include "common/dsp/complex.h"
#include "common/dsp/resamp/polyphase_bank.h"
#include "dsp/block.h"
#include "dsp/block_simple.h"
#include <mutex>
#include <optional>
#include <volk/volk_alloc.hh>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        class RationalResamplerBlock : public BlockSimple<T, T>
        {
        private:
            // Parameters, see Block::set_param(). init() applies them all together
            float p_interpolation = 1;
            float p_decimation = 2;
            std::vector<float> p_taps = {};

        private:
            // Buffer
            volk::vector<T> buffer;
            size_t buffer_size = 0;
            uint32_t in_buffer = 0;

            // Settings
            int d_interpolation;
            int d_decimation;
            int d_ctr;

            int inc = 0, outc = 0;

            // Taps
            dsp::PolyphaseBank pfb;

        public:
            uint32_t process(T *input, uint32_t nsamples, T *output);

        public:
            RationalResamplerBlock();
            ~RationalResamplerBlock();

            void init();

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "interpolation", "float");
                add_param_simple(p, "decimation", "float");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                std::lock_guard<std::mutex> lock(this->params_mtx);
                if (key == "interpolation")
                    return p_interpolation;
                else if (key == "decimation")
                    return p_decimation;
                else if (key == "taps")
                    return p_taps;
                else
                    throw satdump_exception(key);
            }

            Block::cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "interpolation")
                    return set_params(v.get<float>(), std::nullopt, std::nullopt);
                else if (key == "decimation")
                    return set_params(std::nullopt, v.get<float>(), std::nullopt);
                else if (key == "taps")
                    return set_params(std::nullopt, std::nullopt, v.get<std::vector<float>>());
                else
                    throw satdump_exception(key);
            }

            /**
             * @brief Sets the interpolation and decimation (and the taps) at once, so a running
             * block never applies a ratio made of an old and a new value
             */
            Block::cfg_res_t set_ratio(float interpolation, float decimation) { return set_params(interpolation, decimation, std::nullopt); }
            Block::cfg_res_t set_ratio(float interpolation, float decimation, std::vector<float> taps) { return set_params(interpolation, decimation, std::move(taps)); }

        private:
            // Interpolation and decimation must be integers of at least 1 (they are truncated)
            static bool valid_rate(float v) { return v >= 1 && v < 2147483648.0f; }

            Block::cfg_res_t set_params(std::optional<float> interpolation, std::optional<float> decimation, std::optional<std::vector<float>> taps)
            {
                if ((interpolation && !valid_rate(*interpolation)) || (decimation && !valid_rate(*decimation)))
                    return Block::RES_ERR;

                std::lock_guard<std::mutex> lock(this->params_mtx);
                if (interpolation)
                    p_interpolation = *interpolation;
                if (decimation)
                    p_decimation = *decimation;
                if (taps)
                    p_taps = std::move(*taps);
                this->needs_reinit = true;
                return Block::RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump