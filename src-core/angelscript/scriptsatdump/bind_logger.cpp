#include "bind_satdump.h"
#include "logger.h"

namespace satdump
{
    namespace script
    {
        namespace namelog
        {
            void trace(std::string &f) { logger->trace("[AngelScript] %s", f.c_str()); }
            void debug(std::string &f) { logger->debug("[AngelScript] %s", f.c_str()); }
            void info(std::string &f) { logger->info("[AngelScript] %s", f.c_str()); }
            void warn(std::string &f) { logger->warn("[AngelScript] %s", f.c_str()); }
            void error(std::string &f) { logger->error("[AngelScript] %s", f.c_str()); }
            void critical(std::string &f) { logger->critical("[AngelScript] %s", f.c_str()); }
        } // namespace namelog

        void registerLogger(asIScriptEngine *engine)
        {
            // Register logger
            engine->SetDefaultNamespace("logger");
            engine->RegisterGlobalFunction("void trace(const string &in)", asFUNCTION(namelog::trace), asCALL_CDECL);
            engine->RegisterGlobalFunction("void debug(const string &in)", asFUNCTION(namelog::debug), asCALL_CDECL);
            engine->RegisterGlobalFunction("void info(const string &in)", asFUNCTION(namelog::info), asCALL_CDECL);
            engine->RegisterGlobalFunction("void warn(const string &in)", asFUNCTION(namelog::warn), asCALL_CDECL);
            engine->RegisterGlobalFunction("void error(const string &in)", asFUNCTION(namelog::error), asCALL_CDECL);
            engine->RegisterGlobalFunction("void critical(const string &in)", asFUNCTION(namelog::critical), asCALL_CDECL);
        }
    } // namespace script
} // namespace satdump