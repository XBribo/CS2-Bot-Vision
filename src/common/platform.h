// Cross-platform plugin helpers

#pragma once // NOLINT(portability-avoid-pragma-once)

#include <string>

#ifdef _WIN32
#define CS2BV_FASTCALL __fastcall
#else
#define CS2BV_FASTCALL
#endif

namespace cs2bv::platform {
// Returns the absolute path of this plugin module
std::string SelfModulePath();
} // namespace cs2bv::platform
