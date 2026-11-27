#pragma once

#include "angelscript/angelscript.h"
#include <string>

namespace satdump
{
    namespace script
    {
        // TODOREWORK document
        void registerLogger(asIScriptEngine *engine);
        void registerSystem(asIScriptEngine *engine);
        void registerJson(asIScriptEngine *engine);
        void registerGeodetic(asIScriptEngine *engine);
        void registerProj(asIScriptEngine *engine);
        void registerImage(asIScriptEngine *engine);

        template <typename T, typename Factory>
        void registerProduct(std::string name, asIScriptEngine *engine);
        void registerImageProduct(asIScriptEngine *engine);

        /**
         * @brief Registers the add-ons and all SatDump bindings
         *
         * Failed registrations are logged, even if the engine has no message callback.
         *
         * @param engine engine to register them in
         * @return true if everything was registered. If not, the engine cannot build scripts
         */
        bool registerAll(asIScriptEngine *engine);
    } // namespace script
} // namespace satdump