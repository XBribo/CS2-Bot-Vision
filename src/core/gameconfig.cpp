#include "core/gameconfig.h"
#include "utils/platform.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
namespace cs2bv::gameconfig {
namespace {
// Formats a configuration resolution error for the caller.
void SetError(char* out, size_t outLen, const char* fmt, const char* a, const char* b = nullptr)
{
    if (!out || outLen == 0) return;
    if (b) std::snprintf(out, outLen, fmt, a, b);
    else
        std::snprintf(out, outLen, fmt, a);
}
}
// Loads the JSON object without letting malformed input escape.
bool LoadGamedata(const char* path, nlohmann::json& out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    try
    {
        out = nlohmann::json::parse(ifs);
    }
    catch (...)
    {
        return false;
    }
    return out.is_object();
}
// Returns the current platform's gamedata key.
const char* PlatformName()
{
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}
// Reads a platform signature.
std::string FindPlatformSig(const nlohmann::json& gamedata, const std::string& name)
{
    auto it = gamedata.find(name);
    if (it == gamedata.end() || !it->is_object()) return "";
    auto sigIt = it->find("signatures");
    if (sigIt == it->end() || !sigIt->is_object()) return "";
    auto platformIt = sigIt->find(PlatformName());
    if (platformIt == sigIt->end() || !platformIt->is_string()) return "";
    return platformIt->get<std::string>();
}
// Resolves a configured signature using the module scanner.
void* ResolveSig(const nlohmann::json& gamedata, const modules::ModuleInfo& module, const char* name, char* errorOut, size_t errorOutLen)
{
    std::string sig = FindPlatformSig(gamedata, name);
    if (sig.empty())
    {
        SetError(errorOut, errorOutLen, "gamedata missing '%s.signatures.%s'", name, PlatformName());
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    std::vector<bool> wild;
    if (!modules::ParseSigString(sig, bytes, wild))
    {
        SetError(errorOut, errorOutLen, "failed to parse '%s' sig: '%s'", name, sig.c_str());
        return nullptr;
    }
    void* addr = modules::FindPatternIn(module, bytes, wild);
    if (!addr)
    {
        SetError(errorOut, errorOutLen, "sig '%s' not found in target module", name);
        return nullptr;
    }
    return addr;
}
// Reads a platform-specific byte offset or virtual index.
int ResolveOffset(const nlohmann::json& gamedata, const char* name, int defVal)
{
    auto it = gamedata.find(name);
    if (it == gamedata.end() || !it->is_object()) return defVal;
    auto offIt = it->find("offsets");
    if (offIt == it->end() || !offIt->is_object()) return defVal;
    auto platIt = offIt->find(PlatformName());
    if (platIt == offIt->end() || !platIt->is_number_integer()) return defVal;
    return platIt->get<int>();
}
// Resolves gamedata.json beside the plugin directory
std::string ComputePath()
{
    std::filesystem::path path(cs2bv::platform::SelfModulePath());
    if (path.empty()) return "";

    for (int i = 0; i < 3; ++i)
    {
        if (!path.has_parent_path()) return "";
        path = path.parent_path();
    }
    return (path / "gamedata.json").string();
}


}
