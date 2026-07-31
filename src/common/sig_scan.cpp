// Signature scanning and gamedata helpers
//
// Signature scanning + gamedata.json loader.

#include "sig_scan.h"

#if defined(_WIN32)
#include <Windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#include <link.h>
#include <strings.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace cs2bv::sig {
namespace {
// Returns the final component of a platform path
const char* BaseName(const char* path)
{
    if (!path) return "";
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* base = slash && backslash ? std::max(slash, backslash) : (slash ? slash : backslash);
    return base ? base + 1 : path;
}

// Formats a signature error into an optional output buffer
void SetError(char* out, size_t outLen, const char* fmt, const char* a, const char* b = nullptr)
{
    if (!out || outLen == 0) return;
    if (b) std::snprintf(out, outLen, fmt, a, b);
    else
        std::snprintf(out, outLen, fmt, a);
}

#if defined(_WIN32)
// Resolves module boundaries from a Windows module handle
ModuleInfo ModuleFromHandle(HMODULE handle)
{
    ModuleInfo out;
    if (!handle) return out;

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), handle, &mi, sizeof(mi))) return out;

    out.Base = static_cast<unsigned char*>(mi.lpBaseOfDll);
    out.Size = static_cast<size_t>(mi.SizeOfImage);
    out.Segments.push_back({ out.Base, out.Size });
    return out;
}
#else
// Compares a loaded module path with a requested module name
bool NameMatches(const char* loadedPath, const char* moduleName)
{
    if (!loadedPath || !loadedPath[0] || !moduleName || !moduleName[0]) return false;
    const char* loadedBase = BaseName(loadedPath);
    const char* wantBase = BaseName(moduleName);
    return std::strcmp(loadedBase, wantBase) == 0;
}

// Collects readable load segments from one ELF module
void FillModuleFromPhdr(dl_phdr_info* info, ModuleInfo& out)
{
    uintptr_t minAddr = UINTPTR_MAX;
    uintptr_t maxAddr = 0;
    out.Segments.clear();

    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        auto* segBase = reinterpret_cast<unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        size_t segSize = static_cast<size_t>(ph.p_memsz);
        out.Segments.push_back({ segBase, segSize });

        uintptr_t start = reinterpret_cast<uintptr_t>(segBase);
        uintptr_t end = start + segSize;
        minAddr = std::min(minAddr, start);
        maxAddr = std::max(maxAddr, end);
    }

    if (minAddr != UINTPTR_MAX && maxAddr > minAddr)
    {
        out.Base = reinterpret_cast<unsigned char*>(minAddr);
        out.Size = static_cast<size_t>(maxAddr - minAddr);
    }
}

struct FindByNameCtx
{
    const char* Name = nullptr;
    ModuleInfo Result;
};

// Selects an ELF module by basename
int FindByNameCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<FindByNameCtx*>(data);
    if (!NameMatches(info->dlpi_name, ctx->Name)) return 0;

    FillModuleFromPhdr(info, ctx->Result);
    return ctx->Result ? 1 : 0;
}

struct FindByAddressCtx
{
    uintptr_t Address = 0;
    ModuleInfo Result;
};

// Selects the ELF module containing an address
int FindByAddressCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<FindByAddressCtx*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        uintptr_t start = info->dlpi_addr + ph.p_vaddr;
        uintptr_t end = start + ph.p_memsz;
        if (ctx->Address >= start && ctx->Address < end)
        {
            FillModuleFromPhdr(info, ctx->Result);
            return ctx->Result ? 1 : 0;
        }
    }
    return 0;
}
#endif
} // namespace

// Loads and parses a gamedata JSON object
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

// Returns the gamedata key for the current platform
const char* PlatformName()
{
#if defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

// Returns one platform-specific signature string
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

// Parses signature bytes and wildcard positions
bool ParseSigString(const std::string& sigStr, std::vector<uint8_t>& outBytes, std::vector<bool>& outWild)
{
    outBytes.clear();
    outWild.clear();
    const char* p = sigStr.c_str();
    while (*p)
    {
        if (*p == ' ')
        {
            ++p;
            continue;
        }
        if (*p == '?')
        {
            outBytes.push_back(0);
            outWild.push_back(true);
            ++p;
            if (*p == '?') ++p;
            continue;
        }
        char* end = nullptr;
        unsigned long v = std::strtoul(p, &end, 16);
        if (end == p || end - p > 2 || v > 0xFF) return false;
        outBytes.push_back(static_cast<uint8_t>(v));
        outWild.push_back(false);
        p = end;
    }
    return !outBytes.empty();
}

// Finds the first matching byte pattern in module segments
void* FindPatternIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild)
{
    if (!module || pattern.empty() || pattern.size() != wild.size()) return nullptr;

    const size_t plen = pattern.size();
    for (const ModuleSegment& segment : module.Segments)
    {
        if (!segment.Base || segment.Size < plen) continue;

        for (size_t i = 0; i + plen <= segment.Size; ++i)
        {
            bool match = true;
            for (size_t j = 0; j < plen; ++j)
            {
                if (!wild[j] && segment.Base[i + j] != pattern[j])
                {
                    match = false;
                    break;
                }
            }
            if (match) return segment.Base + i;
        }
    }
    return nullptr;
}

// Resolves a loaded module by basename
ModuleInfo ModuleFromName(const char* moduleName)
{
#if defined(_WIN32)
    return ModuleFromHandle(GetModuleHandleA(moduleName));
#else
    FindByNameCtx ctx{};
    ctx.Name = moduleName;
    dl_iterate_phdr(FindByNameCallback, &ctx);
    return ctx.Result;
#endif
}

// Resolves the module owning an interface virtual table
ModuleInfo ModuleFromInterfacePtr(void* interfacePtr)
{
    if (!interfacePtr) return {};
    void* vtable = *reinterpret_cast<void**>(interfacePtr);
    if (!vtable) return {};

#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(vtable, &mbi, sizeof(mbi))) return {};
    if (mbi.Type != MEM_IMAGE) return {};
    return ModuleFromHandle(reinterpret_cast<HMODULE>(mbi.AllocationBase));
#else
    FindByAddressCtx ctx{};
    ctx.Address = reinterpret_cast<uintptr_t>(vtable);
    dl_iterate_phdr(FindByAddressCallback, &ctx);
    return ctx.Result;
#endif
}

// Resolves one named gamedata signature in a module
void* ResolveSig(const nlohmann::json& gamedata, const ModuleInfo& module, const char* name, char* errorOut, size_t errorOutLen)
{
    std::string sig = FindPlatformSig(gamedata, name);
    if (sig.empty())
    {
        SetError(errorOut, errorOutLen, "gamedata missing '%s.signatures.%s'", name, PlatformName());
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    std::vector<bool> wild;
    if (!ParseSigString(sig, bytes, wild))
    {
        SetError(errorOut, errorOutLen, "failed to parse '%s' sig: '%s'", name, sig.c_str());
        return nullptr;
    }
    void* addr = FindPatternIn(module, bytes, wild);
    if (!addr)
    {
        SetError(errorOut, errorOutLen, "sig '%s' not found in target module", name);
        return nullptr;
    }
    return addr;
}

// Read gamedata[name].offsets.<platform>; defVal if entry/key missing or not integer
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
} // namespace cs2bv::sig
