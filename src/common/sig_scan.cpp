// Signature scanning and gamedata helpers
//
// Signature scanning + gamedata.json loader.

#include "sig_scan.h"

#ifdef _WIN32
#include <Windows.h> // NOLINT(misc-include-cleaner)
#include <libloaderapi.h>
#include <memoryapi.h>
#include <minwindef.h>
#include <psapi.h>
#include <processthreadsapi.h>
#include <winnt.h>
#else
#include <dlfcn.h>
#include <link.h>
#include <strings.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <ios>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace cs2bv::sig {
namespace {
// Returns the final component of a platform path
const char* BaseName(const char* path)
{
    if (!path) return "";
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* base = nullptr;
    if (slash && backslash) base = std::max(slash, backslash);
    else if (slash)
        base = slash;
    else
        base = backslash;
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

// Checks whether a complete range belongs to one selected module segment
bool ContainsRange(const ModuleInfo& module, const void* address, size_t length)
{
    if (!address || length == 0) return false;
    const auto begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = begin + length;
    if (end < begin) return false;

    return std::ranges::any_of(module.segments, [begin, end](const ModuleSegment& segment) {
        const auto segmentBegin = reinterpret_cast<uintptr_t>(segment.base);
        const uintptr_t segmentEnd = segmentBegin + segment.size;
        return segmentEnd >= segmentBegin && begin >= segmentBegin && end <= segmentEnd;
    });
}

// Finds every exact byte sequence in selected module segments
std::vector<void*> FindExactMatches(const ModuleInfo& module, const void* bytes, size_t length)
{
    if (!bytes || length == 0) return {};
    const auto* first = static_cast<const uint8_t*>(bytes);
    std::vector<uint8_t> pattern(first, first + length);
    std::vector<bool> wild(length, false);
    return FindPatternMatchesIn(module, pattern, wild);
}

#ifdef _WIN32
// Resolves module boundaries from a Windows module handle
ModuleInfo ModuleFromHandle(HMODULE handle)
{
    ModuleInfo out;
    if (!handle) return out;

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), handle, &mi, sizeof(mi))) return out;

    out.base = static_cast<unsigned char*>(mi.lpBaseOfDll);
    out.size = static_cast<size_t>(mi.SizeOfImage);
    out.segments.push_back({ .base = out.base, .size = out.size });
    return out;
}

// Resolves one mapped PE section and retains the full image bounds
ModuleInfo ModuleSectionFromHandle(HMODULE handle, const char* sectionName)
{
    ModuleInfo out;
    if (!handle || !sectionName) return out;

    ModuleInfo image = ModuleFromHandle(handle);
    if (!image) return out;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return out;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;

    const size_t nameLength = std::strlen(sectionName);
    if (nameLength == 0 || nameLength > IMAGE_SIZEOF_SHORT_NAME) return out;

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        std::memcpy(name, section->Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::strcmp(name, sectionName) != 0) continue;

        const auto sectionSize = static_cast<size_t>(section->Misc.VirtualSize);
        const auto sectionOffset = static_cast<size_t>(section->VirtualAddress);
        if (sectionSize == 0 || sectionOffset >= image.size || sectionSize > image.size - sectionOffset) return out;

        out.base = image.base;
        out.size = image.size;
        out.segments.push_back({ .base = image.base + sectionOffset, .size = sectionSize });
        return out;
    }
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
    out.segments.clear();

    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        auto* segBase = reinterpret_cast<unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        size_t segSize = static_cast<size_t>(ph.p_memsz);
        out.segments.push_back({ segBase, segSize });

        uintptr_t start = reinterpret_cast<uintptr_t>(segBase);
        uintptr_t end = start + segSize;
        minAddr = std::min(minAddr, start);
        maxAddr = std::max(maxAddr, end);
    }

    if (minAddr != UINTPTR_MAX && maxAddr > minAddr)
    {
        out.base = reinterpret_cast<unsigned char*>(minAddr);
        out.size = static_cast<size_t>(maxAddr - minAddr);
    }
}

// Collects executable load segments from one ELF module
void FillCodeModuleFromPhdr(dl_phdr_info* info, ModuleInfo& out)
{
    FillModuleFromPhdr(info, out);
    out.segments.clear();
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0 || (ph.p_flags & PF_X) == 0) continue;

        auto* segmentBase = reinterpret_cast<unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        out.segments.push_back({ segmentBase, static_cast<size_t>(ph.p_memsz) });
    }
    if (out.segments.empty()) out = {};
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

// Resolves executable segments for a module basename
int FindCodeByNameCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<FindByNameCtx*>(data);
    if (!NameMatches(info->dlpi_name, ctx->Name)) return 0;

    FillCodeModuleFromPhdr(info, ctx->Result);
    return ctx->Result ? 1 : 0;
}

struct FindByAddressCtx
{
    uintptr_t Address = 0;
    ModuleInfo Result;
};

struct FindExecutableAddressCtx
{
    uintptr_t Address = 0;
    bool Found = false;
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

// Checks executable ELF load segments for one address
int FindExecutableAddressCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<FindExecutableAddressCtx*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0 || (ph.p_flags & PF_X) == 0) continue;

        const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
        const uintptr_t end = start + ph.p_memsz;
        if (ctx->Address >= start && ctx->Address < end)
        {
            ctx->Found = true;
            return 1;
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
#ifdef _WIN32
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
        const auto v = std::strtoul(p, &end, 16);
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
    for (const ModuleSegment& segment : module.segments)
    {
        if (!segment.base || segment.size < plen) continue;

        for (size_t i = 0; i + plen <= segment.size; ++i)
        {
            bool match = true;
            for (size_t j = 0; j < plen; ++j)
            {
                if (!wild[j] && segment.base[i + j] != pattern[j])
                {
                    match = false;
                    break;
                }
            }
            if (match) return segment.base + i;
        }
    }
    return nullptr;
}

// Finds every pattern match in the selected module segments
std::vector<void*> FindPatternMatchesIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild)
{
    std::vector<void*> matches;
    if (!module || pattern.empty() || pattern.size() != wild.size()) return matches;

    const size_t patternLength = pattern.size();
    for (const ModuleSegment& segment : module.segments)
    {
        if (!segment.base || segment.size < patternLength) continue;

        for (size_t i = 0; i + patternLength <= segment.size; ++i)
        {
            bool match = true;
            for (size_t j = 0; j < patternLength; ++j)
            {
                if (!wild[j] && segment.base[i + j] != pattern[j])
                {
                    match = false;
                    break;
                }
            }
            if (match) matches.push_back(segment.base + i);
        }
    }
    return matches;
}

// Resolves a loaded module by basename
ModuleInfo ModuleFromName(const char* moduleName)
{
#ifdef _WIN32
    return ModuleFromHandle(GetModuleHandleA(moduleName));
#else
    FindByNameCtx ctx{};
    ctx.Name = moduleName;
    dl_iterate_phdr(FindByNameCallback, &ctx);
    return ctx.Result;
#endif
}

// Resolves executable code ranges from a loaded module
ModuleInfo ModuleCodeFromName(const char* moduleName)
{
#ifdef _WIN32
    return ModuleSectionFromHandle(GetModuleHandleA(moduleName), ".text");
#else
    FindByNameCtx ctx{};
    ctx.Name = moduleName;
    dl_iterate_phdr(FindCodeByNameCallback, &ctx);
    return ctx.Result;
#endif
}

// Resolves the module owning an interface virtual table
ModuleInfo ModuleFromInterfacePtr(void* interfacePtr)
{
    if (!interfacePtr) return {};
    void* vtable = *reinterpret_cast<void**>(interfacePtr);
    if (!vtable) return {};

#ifdef _WIN32
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

// Resolves a polymorphic class vtable from platform RTTI records
void** ResolveVirtualTable(const ModuleInfo& module, const char* className, char* errorOut, size_t errorOutLen)
{
    if (!module || !className || !className[0])
    {
        SetError(errorOut, errorOutLen, "%s", "invalid RTTI vtable request");
        return nullptr;
    }

#ifdef _WIN32
    auto* const moduleHandle = reinterpret_cast<HMODULE>(module.base);
    const ModuleInfo data = ModuleSectionFromHandle(moduleHandle, ".data");
    const ModuleInfo rdata = ModuleSectionFromHandle(moduleHandle, ".rdata");
    if (!data || !rdata)
    {
        SetError(errorOut, errorOutLen, "PE RTTI sections unavailable for '%s'", className);
        return nullptr;
    }

    std::string decoratedName = ".?AV" + std::string(className) + "@@";
    decoratedName.push_back('\0');
    const auto nameMatches = FindExactMatches(data, decoratedName.data(), decoratedName.size());
    for (void* nameMatch : nameMatches)
    {
        auto* nameAddress = static_cast<unsigned char*>(nameMatch);
        if (!ContainsRange(data, nameAddress - 0x10, 0x10 + decoratedName.size())) continue;

        const auto* typeDescriptor = nameAddress - 0x10;
        const auto descriptorOffset = static_cast<uintptr_t>(typeDescriptor - module.base);
        if (descriptorOffset > UINT32_MAX) continue;
        const auto descriptorRva = static_cast<uint32_t>(descriptorOffset);
        for (void* descriptorMatch : FindExactMatches(rdata, &descriptorRva, sizeof(descriptorRva)))
        {
            auto* descriptorReference = static_cast<unsigned char*>(descriptorMatch);
            if (!ContainsRange(rdata, descriptorReference - 0x0C, 0x18)) continue;

            auto* locator = descriptorReference - 0x0C;
            int32_t signature = 0;
            int32_t vtableOffset = 0;
            std::memcpy(&signature, locator, sizeof(signature));
            std::memcpy(&vtableOffset, locator + sizeof(signature), sizeof(vtableOffset));
            if (signature != 1 || vtableOffset != 0) continue;

            const auto locatorAddress = reinterpret_cast<uintptr_t>(locator);
            for (void* locatorMatch : FindExactMatches(rdata, &locatorAddress, sizeof(locatorAddress)))
            {
                auto* vtable = reinterpret_cast<void**>(static_cast<unsigned char*>(locatorMatch) + sizeof(void*));
                if (!ContainsRange(rdata, static_cast<const void*>(vtable), sizeof(void*)) || !ContainsRange(module, vtable[0], 1) ||
                    !IsExecutableAddress(vtable[0]))
                {
                    continue;
                }
                return vtable;
            }
        }
    }
#else
    std::string decoratedName = std::to_string(std::strlen(className)) + className;
    decoratedName.push_back('\0');
    for (void* nameMatch : FindExactMatches(module, decoratedName.data(), decoratedName.size()))
    {
        const uintptr_t nameAddress = reinterpret_cast<uintptr_t>(nameMatch);
        for (void* nameReferenceMatch : FindExactMatches(module, &nameAddress, sizeof(nameAddress)))
        {
            auto* nameReference = static_cast<unsigned char*>(nameReferenceMatch);
            if (!ContainsRange(module, nameReference - sizeof(void*), sizeof(void*) * 2)) continue;

            const uintptr_t typeInfoAddress = reinterpret_cast<uintptr_t>(nameReference - sizeof(void*));
            for (void* typeInfoMatch : FindExactMatches(module, &typeInfoAddress, sizeof(typeInfoAddress)))
            {
                auto* typeInfoReference = static_cast<unsigned char*>(typeInfoMatch);
                if (!ContainsRange(module, typeInfoReference - sizeof(ptrdiff_t), sizeof(void*) * 3)) continue;

                ptrdiff_t offsetToTop = -1;
                std::memcpy(&offsetToTop, typeInfoReference - sizeof(offsetToTop), sizeof(offsetToTop));
                auto* vtable = reinterpret_cast<void**>(typeInfoReference + sizeof(void*));
                if (offsetToTop != 0 || !ContainsRange(module, vtable[0], 1) || !IsExecutableAddress(vtable[0])) continue;
                return vtable;
            }
        }
    }
#endif

    SetError(errorOut, errorOutLen, "RTTI vtable unavailable for '%s'", className);
    return nullptr;
}

// Checks whether an address belongs to executable image memory
bool IsExecutableAddress(const void* address)
{
    if (!address) return false;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0) return false;
    const DWORD protection = memory.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
#else
    FindExecutableAddressCtx ctx{};
    ctx.Address = reinterpret_cast<uintptr_t>(address);
    dl_iterate_phdr(FindExecutableAddressCallback, &ctx);
    return ctx.Found;
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
