#pragma once

// Shared helpers for the plugins/ tests. Everything lives in the plugins_test namespace and
// is inline, so including this header from several translation units is fine.

#include "core/plugin.h"
#include <memory>
#include <string>

// The loader of core/plugin.cpp, which no header declares
std::shared_ptr<satdump::Plugin> loadPlugin(std::string plugin);

namespace plugins_test
{
    // Loads a plugin the way satdump does at startup, from one of the SATDUMP_TEST_PLUGIN_*
    // paths: its init() registers its handlers on satdump::eventBus, so swap the bus (and
    // save whatever else that plugin touches) with a guard first. The library itself is
    // linked into the tests, so the code of the handlers stays loaded afterwards.
    inline std::shared_ptr<satdump::Plugin> loadTestPlugin(const std::string &path) { return loadPlugin(path); }
} // namespace plugins_test
