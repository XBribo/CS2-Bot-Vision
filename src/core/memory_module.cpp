//

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "core/memory_module.h"

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
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <tier0/platform.h>
#include "metamod_oslink.h"
#ifdef snprintf
#undef snprintf
#endif

namespace cs2bv::modules {
namespace {
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
// Adds one mapped ELF segment and updates the module bounds.
void AddSegment(ModuleInfo& module, uintptr_t address, size_t size)
{
    if (size == 0) return;

    auto* base = reinterpret_cast<unsigned char*>(address);
    module.segments.push_back({ .base = base, .size = size });

    if (!module.base || address < reinterpret_cast<uintptr_t>(module.base)) module.base = base;

    const uintptr_t end = address + size;
    const uintptr_t currentEnd = reinterpret_cast<uintptr_t>(module.base) + module.size;
    if (end > currentEnd) module.size = static_cast<size_t>(end - reinterpret_cast<uintptr_t>(module.base));
}

// Resolves ELF load segments directly from the handle's link_map.
bool FillModuleFromHandle(HINSTANCE handle, ModuleInfo& image, ModuleInfo& code)
{
    if (!handle) return false;

    link_map* linkMap = nullptr;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &linkMap) != 0 || !linkMap || !linkMap->l_name || !linkMap->l_name[0]) return false;

    const int fileDescriptor = open(linkMap->l_name, O_RDONLY);
    if (fileDescriptor == -1) return false;

    struct stat fileStatus{};
    if (fstat(fileDescriptor, &fileStatus) != 0 || fileStatus.st_size <= 0)
    {
        close(fileDescriptor);
        return false;
    }

    const size_t fileSize = static_cast<size_t>(fileStatus.st_size);
    void* mappedFile = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fileDescriptor, 0);
    if (mappedFile == MAP_FAILED)
    {
        close(fileDescriptor);
        return false;
    }

    if (fileSize < sizeof(ElfW(Ehdr)))
    {
        munmap(mappedFile, fileSize);
        close(fileDescriptor);
        return false;
    }

    auto* elfHeader = static_cast<ElfW(Ehdr)*>(mappedFile);
    const size_t programHeaderOffset = static_cast<size_t>(elfHeader->e_phoff);
    const size_t programHeaderSize = static_cast<size_t>(elfHeader->e_phnum) * elfHeader->e_phentsize;
    const bool validElf = std::memcmp(elfHeader->e_ident, ELFMAG, SELFMAG) == 0 &&
                          elfHeader->e_ident[EI_CLASS] == ELFCLASS64 &&
                          programHeaderOffset <= fileSize && programHeaderSize <= fileSize - programHeaderOffset;
    if (!validElf)
    {
        munmap(mappedFile, fileSize);
        close(fileDescriptor);
        return false;
    }

    auto* programHeaders = reinterpret_cast<ElfW(Phdr)*>(static_cast<unsigned char*>(mappedFile) + programHeaderOffset);
    for (int i = 0; i < elfHeader->e_phnum; ++i)
    {
        const ElfW(Phdr)& programHeader = programHeaders[i];
        if (programHeader.p_type != PT_LOAD || programHeader.p_memsz == 0) continue;

        const uintptr_t address = static_cast<uintptr_t>(linkMap->l_addr + programHeader.p_vaddr);
        const size_t size = static_cast<size_t>(programHeader.p_memsz);
        AddSegment(image, address, size);
        if ((programHeader.p_flags & PF_X) != 0) AddSegment(code, address, size);
    }

    munmap(mappedFile, fileSize);
    close(fileDescriptor);
    return static_cast<bool>(image);
}
#endif
} // namespace

// Opens one game module from its explicit game-relative path.
CModule::CModule(const char* relativeDirectory, const char* moduleName)
{
    if (!relativeDirectory || !moduleName || !moduleName[0]) return;

    const char* gameDirectory = Plat_GetGameDirectory();
    if (!gameDirectory || !gameDirectory[0]) return;

    m_path = std::string(gameDirectory) + relativeDirectory + kModulePrefix + moduleName + kModuleExtension;
    m_hModule = dlmount(m_path.c_str());
    if (!m_hModule) return;

#ifdef _WIN32
    m_image = ModuleFromHandle(reinterpret_cast<HMODULE>(m_hModule));
    m_code = ModuleSectionFromHandle(reinterpret_cast<HMODULE>(m_hModule), ".text");
#else
    if (!FillModuleFromHandle(static_cast<HINSTANCE>(m_hModule), m_image, m_code))
    {
        dlclose(m_hModule);
        m_hModule = nullptr;
    }
#endif
}

CModule* engine = nullptr;
CModule* server = nullptr;

// Loads the engine and server modules once for signature resolution.
void Initialize()
{
    if (!engine) engine = new CModule(kRootBin, "engine2");
    if (!server) server = new CModule(kGameBin, "server");
}





// Returns one platform-specific signature string


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
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;

    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    char line[512] = {};
    uintptr_t start = 0;
    uintptr_t end = 0;
    char permissions[5] = {};
    while (std::fgets(line, sizeof(line), maps))
    {
        if (std::sscanf(line, "%lx-%lx %4s", &start, &end, permissions) == 3 && target >= start && target < end)
        {
            std::fclose(maps);
            return permissions[2] == 'x';
        }
    }

    std::fclose(maps);
    return false;
#endif
}




} // namespace cs2bv::modules
