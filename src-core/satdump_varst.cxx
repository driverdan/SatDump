#define SATDUMP_DLL_EXPORT 1
#include "satdump_vars.h"

namespace satdump
{
    SATDUMP_DLL const std::string RESOURCES_PATH = "@RESOURCES_PATH@";
    SATDUMP_DLL const std::string LIBRARIES_PATH = "@LIBRARIES_PATH@";
    SATDUMP_DLL const std::string SATDUMP_VERSION = "@SATDUMP_VERSION@";
    SATDUMP_DLL const std::string SATDUMP_VERSION_TAG = "@SATDUMP_VERSION_TAG@";

    // Defined in satdump_vars.cpp
    std::string init_res_path();
    std::string init_lib_path();

    // RESPATH and LIBPATH are initialised from RESOURCES_PATH and LIBRARIES_PATH, so they must be
    // defined in this translation unit, after them. Dynamic initialisation is only ordered within
    // a translation unit; across translation units it depends on the link order.
    SATDUMP_DLL std::string RESPATH = init_res_path();
    SATDUMP_DLL std::string LIBPATH = init_lib_path();
} // namespace satdump
