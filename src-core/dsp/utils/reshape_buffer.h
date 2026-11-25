#pragma once

#include "common/dsp/block.h"
#include "common/dsp/complex.h"
#include "common/dsp/filter/firdes.h"
#include "dsp/block.h"
#include <volk/volk.h>
#include <volk/volk_alloc.hh>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        class ReshapeBufferBlock : public Block
        {
        private:
            // Parameter, see Block::set_param()
            int buffer_size = 1024;

            int d_buffer_size = 1024;

            // Output buffer being filled, only used by the work thread (and
            // by stop() once it has exited). Only that thread writes to the
            // output stream, which has a single producer.
            DSPBuffer pending;
            bool has_pending = false;
            void dropPending();

            bool work();

        public:
            ReshapeBufferBlock();
            ~ReshapeBufferBlock();

            void stop(bool stop_now = false, bool force = false);

            void init();

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "buffer_size", "int");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "buffer_size")
                    return get_param(buffer_size);
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "buffer_size")
                {
                    int sz = v.get<int>();
                    if (sz < 1)
                        throw satdump_exception("buffer_size must be at least 1");
                    set_param(buffer_size, sz);
                }
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump
