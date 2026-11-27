#include "bind_satdump.h"

#include "angelscript/scriptarray/scriptarray.h"
#include "angelscript/scriptdictionary/scriptdictionary.h"
#include "angelscript/scriptstdstring/scriptstdstring.h"
#include "bind_product.h"
#include "logger.h"

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

        template <typename T>
        void registerProduct(std::string name, asIScriptEngine *engine);
        void registerImageProduct(asIScriptEngine *engine);

        namespace
        {
            // The engine reports each failed registration to the message callback
            void registrationMessageCallback(const asSMessageInfo *msg, void *param)
            {
                if (msg->type != asMSGTYPE_ERROR)
                    return;
                (*(int *)param)++;
                logger->error("[AngelScript] %s", msg->message);
            }
        } // namespace

        bool registerAll(asIScriptEngine *engine)
        {
            // Registration errors go to our own callback, so that they are logged even if the
            // caller has none. The caller's callback is put back afterwards
            asSFuncPtr prev_callback;
            void *prev_obj = nullptr;
            asDWORD prev_conv = 0;
            bool had_callback = engine->GetMessageCallback(&prev_callback, &prev_obj, &prev_conv) >= 0;

            int errors = 0;
            engine->SetMessageCallback(asFUNCTION(registrationMessageCallback), &errors, asCALL_CDECL);

            RegisterStdString(engine);
            RegisterScriptArray(engine, true);
            RegisterStdStringUtils(engine);
            RegisterScriptDictionary(engine);

            registerLogger(engine);
            registerSystem(engine);
            registerJson(engine);
            registerGeodetic(engine);
            registerImage(engine);
            registerProj(engine);

            registerProduct<CScriptProduct>("Product", engine);
            registerImageProduct(engine);

            if (had_callback)
                engine->SetMessageCallback(prev_callback, prev_obj, prev_conv);
            else
                engine->ClearMessageCallback();

            // The engine refuses to build scripts after a failed registration. The bindings use native
            // calling conventions, so all of them fail if the library is built with AS_MAX_PORTABILITY
            if (errors > 0)
            {
                logger->error("[AngelScript] %d registrations failed, scripts cannot be built (AngelScript options: %s)", errors,
                              asGetLibraryOptions());
                return false;
            }

            return true;
        }
    } // namespace script
} // namespace satdump
