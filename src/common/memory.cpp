// Validated runtime memory reads and diagnostics

#include "memory.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace cs2bv::memory
{
    static std::array<std::atomic<long long>,
                      static_cast<size_t>(FailureDomain::Count)>
        g_failures{};

    // Checks every virtual-memory region covered by an address range
    bool IsReadable(const void *address, size_t size)
    {
        if (!address || size == 0)
            return false;

        uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
        if (size > UINTPTR_MAX - cursor)
            return false;
        const uintptr_t end = cursor + size;

        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION memoryInfo{};
            if (VirtualQuery(reinterpret_cast<const void *>(cursor), &memoryInfo,
                             sizeof(memoryInfo)) == 0)
                return false;
            if (memoryInfo.State != MEM_COMMIT ||
                (memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
                return false;

            const DWORD protection = memoryInfo.Protect & 0xFF;
            const bool readable = protection == PAGE_READONLY ||
                                  protection == PAGE_READWRITE ||
                                  protection == PAGE_WRITECOPY ||
                                  protection == PAGE_EXECUTE_READ ||
                                  protection == PAGE_EXECUTE_READWRITE ||
                                  protection == PAGE_EXECUTE_WRITECOPY;
            if (!readable)
                return false;

            const uintptr_t regionStart =
                reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
            if (memoryInfo.RegionSize > UINTPTR_MAX - regionStart)
                return false;
            const uintptr_t regionEnd = regionStart + memoryInfo.RegionSize;
            if (regionEnd <= cursor)
                return false;
            cursor = regionEnd;
        }
        return true;
    }

    // Increments a subsystem failure counter
    void RecordFailure(FailureDomain domain)
    {
        const size_t index = static_cast<size_t>(domain);
        if (index < g_failures.size())
            g_failures[index].fetch_add(1, std::memory_order_relaxed);
    }

    // Formats the current subsystem counters
    const char *Diagnostics()
    {
        static char buffer[192];
        std::snprintf(
            buffer, sizeof(buffer),
            "scene=%lld bot=%lld weapon=%lld bullet=%lld smoke=%lld install=%lld",
            g_failures[static_cast<size_t>(FailureDomain::Scene)].load(
                std::memory_order_relaxed),
            g_failures[static_cast<size_t>(FailureDomain::Bot)].load(
                std::memory_order_relaxed),
            g_failures[static_cast<size_t>(FailureDomain::Weapon)].load(
                std::memory_order_relaxed),
            g_failures[static_cast<size_t>(FailureDomain::Bullet)].load(
                std::memory_order_relaxed),
            g_failures[static_cast<size_t>(FailureDomain::Smoke)].load(
                std::memory_order_relaxed),
            g_failures[static_cast<size_t>(FailureDomain::Install)].load(
                std::memory_order_relaxed));
        return buffer;
    }
}
