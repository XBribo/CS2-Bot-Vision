#pragma once
#include "core/memory_module.h"
#include <nlohmann/json.hpp>
namespace cs2bv::gameconfig {
// Loads an object-valued JSON file and catches parse failures.
bool LoadGamedata(const char* path, nlohmann::json& out);
// Returns the current platform key.
const char* PlatformName();
// Returns a platform signature or an empty string.
std::string FindPlatformSig(const nlohmann::json& gamedata, const std::string& name);
// Returns a platform offset or the caller's fallback.
int ResolveOffset(const nlohmann::json& gamedata, const char* name, int defVal);
// Resolves a configured signature against a selected module.
void* ResolveSig(const nlohmann::json& gamedata, const modules::ModuleInfo& module, const char* name, char* errorOut, size_t errorOutLen);
// Locates gamedata relative to this plugin's binary.
std::string ComputePath();
}
