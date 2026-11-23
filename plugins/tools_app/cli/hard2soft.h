#pragma once

#include "core/cli/subcommand.h"

namespace satdump
{
    class HardToSoftCmdHandler : public CmdHandler
    {
    public:
        HardToSoftCmdHandler() : CmdHandler("hard2soft") {}

        void reg(CLI::App *app);
        void run(CLI::App *app, CLI::App *subcom, bool is_gui);
    };
} // namespace satdump