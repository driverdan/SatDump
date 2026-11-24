#pragma once

#include "dsp/block.h"
#include "options_displayer.h"
#include <memory>

namespace satdump
{
    namespace ndsp
    {
        // TODOREWORK figure out how exactly this will be used longterm
        class OptDisplayerWarper
        {
        private:
            OptionsDisplayer disp;
            std::shared_ptr<Block> blk;

        public:
            OptDisplayerWarper(std::shared_ptr<Block> b) : blk(b)
            {
                disp.add_options(blk->get_cfg_list());
                disp.set_values(blk->get_cfg());
            }

            bool draw()
            {
                nlohmann::json changed = disp.draw();

                if (changed.size() > 0)
                    return apply(changed);

                return false;
            }

            /**
             * @brief Apply changed values to the block and refresh
             * the displayed options.
             * @param changed values to set
             * @return true if the block's IO configuration changed
             */
            bool apply(nlohmann::json changed)
            {
                // RES_ERR is above RES_IOUPD, so the IO change must be tracked separately
                bool io_changed = false;
                auto r = blk->set_cfg_io(changed, io_changed);
                if (r >= ndsp::Block::RES_LISTUPD)
                {
                    disp.clear();
                    disp.add_options(blk->get_cfg_list());
                }

                disp.set_values(blk->get_cfg());

                return io_changed;
            }

            void update()
            {
                disp.clear();
                disp.add_options(blk->get_cfg_list());
                disp.set_values(blk->get_cfg());
            }

            void setShowStats(bool v) { disp.show_stats = v; }
        };
    } // namespace ndsp
} // namespace satdump