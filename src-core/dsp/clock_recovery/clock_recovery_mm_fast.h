#pragma once

#include "common/dsp/complex.h"
#include "common/dsp/resamp/polyphase_bank.h"
#include "dsp/block.h"

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        class MMClockRecoveryFastBlock : public Block
        {
        private:
            // Parameters, see Block::set_param()
            float p_omega = 0;
            float p_omegaGain = pow(8.7e-3, 2) / 4.0;
            float p_mu = 0.5f;
            float p_muGain = 8.7e-3;
            float p_omegaLimit = 0.005;

        private:
            // Buffer
            T *buffer = nullptr;

            // Variables
            float mu;
            float omega;
            float omega_gain;
            float mu_gain;
            float omega_relative_limit;
            float omega_mid;
            float omega_limit;

            float sample;
            float last_sample;

            complex_t p_2T, p_1T, p_0T;
            complex_t c_2T, c_1T, c_0T;

            const int ntaps = 8;

            uint32_t omega_upd_cnt = 0;

            float phase_error = 0;

            int ouc = 0;
            int inc = 0;

            bool work();

        public:
            MMClockRecoveryFastBlock();
            ~MMClockRecoveryFastBlock();

            void init();

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "omega", "float");
                add_param_simple(p, "omegaGain", "float");
                add_param_simple(p, "mu", "float");
                add_param_simple(p, "muGain", "float");
                add_param_simple(p, "omegaLimit", "float");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "omega")
                    return get_param(p_omega);
                else if (key == "omegaGain")
                    return get_param(p_omegaGain);
                else if (key == "mu")
                    return get_param(p_mu);
                else if (key == "muGain")
                    return get_param(p_muGain);
                else if (key == "omegaLimit")
                    return get_param(p_omegaLimit);
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "omega")
                    set_param(p_omega, v.get<float>());
                else if (key == "omegaGain")
                    set_param(p_omegaGain, v.get<float>());
                else if (key == "mu")
                    set_param(p_mu, v.get<float>());
                else if (key == "muGain")
                    set_param(p_muGain, v.get<float>());
                else if (key == "omegaLimit")
                    set_param(p_omegaLimit, v.get<float>());
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump