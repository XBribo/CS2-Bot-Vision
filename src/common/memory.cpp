// Validated runtime memory reads and diagnostics

#include "memory.h"

#ifdef _WIN32
#include <Windows.h> // NOLINT(misc-include-cleaner)
#include <memoryapi.h>
#include <minwindef.h>
#include <winnt.h>
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace cs2bv::memory {
namespace {
std::array<std::atomic<int64_t>, static_cast<size_t>(FailureDomain::Count)> g_failures{};

#ifndef _WIN32
static thread_local uintptr_t g_cachedRegionStart = 0;
static thread_local uintptr_t g_cachedRegionEnd = 0;
static thread_local bool g_cachedRegionReadable = false;
#endif
} // namespace

// Checks every virtual-memory region covered by an address range
bool IsReadable(const void* address, size_t size)
{
    if (!address || size == 0) return false;

    auto cursor = reinterpret_cast<uintptr_t>(address);
    if (size > UINTPTR_MAX - cursor) return false;
    const uintptr_t end = cursor + size;

    while (cursor < end)
    {
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION memoryInfo{};
        const void* cursorAddress = reinterpret_cast<const void*>(cursor); // NOLINT(performance-no-int-to-ptr)
        if (VirtualQuery(cursorAddress, &memoryInfo, sizeof(memoryInfo)) == 0) return false;
        if (memoryInfo.State != MEM_COMMIT || (memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;

        const DWORD protection = memoryInfo.Protect & 0xFF;
        const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                              protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
                              protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable) return false;

        const auto regionStart = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
        if (memoryInfo.RegionSize > UINTPTR_MAX - regionStart) return false;
        const uintptr_t regionEnd = regionStart + memoryInfo.RegionSize;
        if (regionEnd <= cursor) return false;
        cursor = regionEnd;
#else
        if (cursor >= g_cachedRegionStart && cursor < g_cachedRegionEnd)
        {
            if (!g_cachedRegionReadable) return false;
            cursor = g_cachedRegionEnd;
            continue;
        }

        FILE* maps = std::fopen("/proc/self/maps", "r");
        if (!maps) return false;

        uintptr_t regionStart = 0;
        uintptr_t regionEnd = 0;
        bool readable = false;
        char permissions[5] = {};
        char line[256] = {};
        while (std::fgets(line, sizeof(line), maps))
        {
            uint64_t parsedStart = 0;
            uint64_t parsedEnd = 0;
            if (std::sscanf(line, "%llx-%llx %4s", &parsedStart, &parsedEnd, permissions) != 3) continue;
            if (cursor < static_cast<uintptr_t>(parsedStart) || cursor >= static_cast<uintptr_t>(parsedEnd)) continue;

            regionStart = static_cast<uintptr_t>(parsedStart);
            regionEnd = static_cast<uintptr_t>(parsedEnd);
            readable = permissions[0] == 'r';
            break;
        }
        std::fclose(maps);
        if (!readable || regionEnd <= cursor) return false;
        g_cachedRegionStart = regionStart;
        g_cachedRegionEnd = regionEnd;
        g_cachedRegionReadable = readable;
        cursor = regionEnd;
#endif
    }
    return true;
}

// Increments a subsystem failure counter
void RecordFailure(FailureDomain domain)
{
    const auto index = static_cast<size_t>(domain);
    if (index < g_failures.size()) g_failures[index].fetch_add(1, std::memory_order_relaxed);
}

// Formats the current subsystem counters
const char* Diagnostics()
{
    static char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "scene=%lld bot=%lld weapon=%lld bullet=%lld smoke=%lld install=%lld",
                  g_failures[static_cast<size_t>(FailureDomain::Scene)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Bot)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Weapon)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Bullet)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Smoke)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Install)].load(std::memory_order_relaxed));
    return buffer;
}
} // namespace cs2bv::memory
