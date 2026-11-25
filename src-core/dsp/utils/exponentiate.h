#pragma once

#include "dsp/block.h"
#include "dsp/block_helpers.h"
#include "dsp/block_simple.h"
#include <cstdint>
#include <volk/volk.h>
#include <volk/volk_complex.h>

namespace satdump
{
    namespace ndsp
    {
        class ExponentiateBlock : public BlockSimple<complex_t, complex_t>
        {
        private:
            // Parameters, see Block::set_param()
            uint32_t p_expo = 2;

        private:
            uint32_t d_expo;

        public:
            ExponentiateBlock();
            ~ExponentiateBlock();

            void init()
            {
                apply_params([&]() { d_expo = p_expo; });
            }

            uint32_t process(complex_t *input, uint32_t nsamples, complex_t *output);

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "expo", "uint");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "expo")
                    return get_param(p_expo);
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "expo")
                    set_param(p_expo, v.get<uint32_t>());
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump
