#include "legacy.h"
#include "logger.h"

int main_old(int argc, char *argv[]);

namespace satdump
{
    void LegacyCmdHandler::reg(CLI::App *app)
    {
        CLI::App *sub = app->add_subcommand("legacy", "Run legacy command from pre-2.0.0-style satdump CLI");
        sub->prefix_command();
        sub->set_help_flag();
    }

    std::vector<std::string> LegacyCmdHandler::get_forwarded_args(CLI::App *subcom)
    {
        auto args = subcom->remaining();

        // Since CLI11 2.6.2, a prefix command keeps a leading "--" in remaining(). The legacy
        // dispatcher would take it for a pipeline id, so drop it (#279)
        if (!args.empty() && args.front() == "--")
            args.erase(args.begin());

        return args;
    }

    void LegacyCmdHandler::run(CLI::App *app, CLI::App *subcom, bool is_gui)
    {
        auto args = get_forwarded_args(subcom);

        std::vector<char *> vec;
        args.insert(args.begin(), "satdump legacy");
        for (auto &a : args)
            vec.push_back((char *)a.c_str());

        exit(main_old(vec.size(), vec.data()));
    }
} // namespace satdump