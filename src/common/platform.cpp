// Windows platform helpers

#include "platform.h"

#include <Windows.h>

namespace cs2bv::platform {
// Writes a message to the Windows debug sink
void DebugOut(const char* message) { OutputDebugStringA(message); }

// Resolves the module containing this function
std::string SelfModulePath()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&SelfModulePath), &module))
        return "";

    char path[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(module, path, MAX_PATH) == 0) return "";
    return std::string(path);
}
} // namespace cs2bv::platform
