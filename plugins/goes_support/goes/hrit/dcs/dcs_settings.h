#pragma once

#include <string>
#include <vector>

namespace goes
{
    namespace hrit
    {
        void initDcsConfig();
        void renderDcsConfig();
        void saveDcsConfig();

        // Replaces the former default PDT URLs, which no longer serve the PDT file, by the current one
        std::vector<std::string> migratePdtUrls(const std::vector<std::string> &urls);
    }
}