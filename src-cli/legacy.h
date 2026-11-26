#pragma once

#include "core/cli/subcommand.h"
#include <string>
#include <vector>

namespace satdump
{
    class LegacyCmdHandler : public CmdHandler
    {
    public:
        LegacyCmdHandler() : CmdHandler("legacy") {}

        void reg(CLI::App *app);
        void run(CLI::App *app, CLI::App *subcom, bool is_gui);

        // The arguments run() hands to the legacy command, after "satdump legacy". A single
        // leading "--" is dropped, so "legacy -- live ..." is the same as "legacy live ...".
        // A "--" later in the line is passed through unchanged.
        static std::vector<std::string> get_forwarded_args(CLI::App *subcom);
    };
} // namespace satdump