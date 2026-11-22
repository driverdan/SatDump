#pragma once

/**
 * @file segment_decoder.h
 * @brief Base segmented decoder class
 */

#include "image/image.h"
#include "logger.h"
#include "xrit/identify.h"
#include "xrit/xrit_file.h"
#include <cstring>
#include <exception>
#include <string>

namespace satdump
{
    namespace xrit
    {
        /**
         * @brief Abstract Segmented decoder implementation
         * to be overriden for mission-specific decoders
         * @param image WIP image
         * @param info xRIT File Info, to be set by the user
         * on creating an instance of this class, and used when
         * saving the final image.
         */
        class SegmentedImageDecoder
        {
        public:
            /**
             * @brief Push a segment into the work-in-progress image.
             * These should already be filtered using file_info.seg_groupid
             * @param file segment to push
             */
            virtual void pushSegment(XRITFile &file) = 0;

            /**
             * @brief Allows knowing if an image is complete, to save it
             * instantly.
             * @return true if complete
             */
            virtual bool isComplete() = 0;

            /**
             * @brief Resets the decoder, to lower RAM usage
             */
            virtual void reset() = 0;

            /**
             * @brief Allows knowing if an image has data at
             * all (and hence, worth saving)
             * @return true if at least 1 segment is present
             */
            virtual bool hasData() = 0;

        public:
            image::Image image;
            XRITFileInfo info;

            /**
             * @brief The most pixels a segmented image may have, 32768 x 32768. The headers
             * can claim far more (65535 columns by 65535 lines by 65536 segments for MSG). The
             * largest real images, full disks of 22000 x 22000 pixels (0.5 km), are less than
             * half of it.
             */
            static constexpr size_t MAX_IMAGE_PIXELS = (size_t)1 << 30;

            /**
             * @brief Whether a segmented image of this size may be allocated
             */
            static bool imageSizeAllowed(size_t width, size_t height) { return height == 0 || width <= MAX_IMAGE_PIXELS / height; }

        protected:
            /**
             * @brief Allocates the work-in-progress image, filled with zeros. An empty image, one larger
             * than MAX_IMAGE_PIXELS, or one that cannot be allocated, is logged and left
             * empty, so that the file's segments are ignored instead of stopping the decoder.
             * @param filename name of the file the size comes from, for the log
             * @return true if the image is allocated, false if it is empty
             */
            bool initImage(const std::string &filename, int bit_depth, size_t width, size_t height)
            {
                image.clear();
                if (width == 0 || height == 0)
                {
                    logger->warn("File %s gives an image of %zux%zu pixels, its segments are ignored", filename.c_str(), width, height);
                    return false;
                }

                if (!imageSizeAllowed(width, height))
                {
                    logger->warn("File %s gives an image of %zux%zu pixels, more than the %zu pixels a segmented image may have, its segments are ignored", filename.c_str(), width, height,
                                 MAX_IMAGE_PIXELS);
                    return false;
                }

                try
                {
                    image.init(bit_depth, width, height, 1);
                }
                catch (std::exception &e)
                {
                    logger->warn("Could not allocate the %zux%zu image of file %s, its segments are ignored! %s", width, height, filename.c_str(), e.what());
                    image.clear();
                    return false;
                }
                return true;
            }
        };
    } // namespace xrit
} // namespace satdump