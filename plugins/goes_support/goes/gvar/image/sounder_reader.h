#pragma once

#include <cstdint>
#include "image/image.h"

namespace goes
{
    namespace gvar
    {
        class SounderReader
        {
        public:
            uint16_t *channels[19];
            uint16_t temp_detect[11 * 4];

        public:
            // Frames pushed since the last clear()
            int frames = 0;
            SounderReader();
            ~SounderReader();

            void clear();

            void pushFrame(uint8_t *data, int counter);
            image::Image getImage(int channel);
        };
    }
}