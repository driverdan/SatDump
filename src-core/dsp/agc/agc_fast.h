#pragma once

#include "dsp/block_simple.h"
#include <volk/volk.h>
#include <volk/volk_alloc.hh>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        class AGCFastBlock : public BlockSimple<T, T>
        {
        private:
            // Parameters, see Block::set_param()
            float p_rate = 1e-4;
            float p_reference = 1.0;
            float p_gain = 1.0;
            float p_max_gain = 65536;

        private:
            float rate = 0;
            float reference = 0;
            float gain = 0;
            float max_gain = 0;

            // Mag buf
            volk::vector<float> mag_buf;
            uint32_t mag_buf_size = 0;

        public:
            uint32_t process(T *input, uint32_t nsamples, T *output);

        public:
            AGCFastBlock();
            ~AGCFastBlock();

            void init()
            {
                this->apply_params(
                    [&]()
                    {
                        rate = p_rate;
                        reference = p_reference;
                        gain = p_gain;
                        max_gain = p_max_gain;
                    });
            }

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "rate", "float", "Rate");
                add_param_simple(p, "reference", "float", "Reference");
                add_param_simple(p, "gain", "float", "Gain");
                add_param_simple(p, "max_gain", "float", "Max Gain");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "rate")
                    return this->get_param(p_rate);
                else if (key == "reference")
                    return this->get_param(p_reference);
                else if (key == "gain")
                    return this->get_param(p_gain);
                else if (key == "max_gain")
                    return this->get_param(p_max_gain);
                else
                    throw satdump_exception(key);
            }

            Block::cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "rate")
                    this->set_param(p_rate, v.get<float>());
                else if (key == "reference")
                    this->set_param(p_reference, v.get<float>());
                else if (key == "gain")
                    this->set_param(p_gain, v.get<float>());
                else if (key == "max_gain")
                    this->set_param(p_max_gain, v.get<float>());
                else
                    throw satdump_exception(key);
                return Block::RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump