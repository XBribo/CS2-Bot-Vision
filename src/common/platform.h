// Windows platform helpers

#pragma once

#include <string>

namespace cs2bv::platform
{
    // Writes a line to the platform debug sink
    void DebugOut(const char *message);

    // Returns the absolute path of this plugin module
    std::string SelfModulePath();
}
