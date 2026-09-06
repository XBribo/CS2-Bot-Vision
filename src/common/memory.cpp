// Validated runtime memory reads and diagnostics

#include "memory.h"

#ifdef _WIN32
#include <Windows.h> // NOLINT(misc-include-cleaner)
#include <memoryapi.h>
#include <minwindef.h>
#include <winnt.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace cs2bv::memory {
namespace {
std::array<std::atomic<int64_t>, static_cast<size_t>(FailureDomain::Count)> g_failures{};
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
        // Probe the page through the kernel instead of parsing /proc/self/maps.
        // process_vm_readv() on our own pid copies from the target range and
        // fails with EFAULT (returns -1) when any byte of it is unmapped or not
        // readable, without faulting the caller. It costs one syscall per page.
        // The previous implementation opened /proc/self/maps and scanned every
        // line with fgets()+sscanf() on each one-entry-cache miss; the IsVisible
        // hooks call this hundreds of times per tick with rotating pointers, so
        // that scan dominated the whole server frame.
        static const pid_t selfPid = getpid();
        static const uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
        const uintptr_t pageEnd = (cursor & ~(pageSize - 1)) + pageSize;
        const uintptr_t chunkEnd = pageEnd < end ? pageEnd : end;
        unsigned char scratch = 0;
        iovec local{&scratch, 1};
        iovec remote{reinterpret_cast<void*>(cursor), 1}; // NOLINT(performance-no-int-to-ptr)
        if (process_vm_readv(selfPid, &local, 1, &remote, 1, 0) != 1) return false;
        cursor = chunkEnd;
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
    std::snprintf(buffer, sizeof(buffer),
                  "scene=%" PRId64 " bot=%" PRId64 " weapon=%" PRId64 " bullet=%" PRId64 " smoke=%" PRId64 " install=%" PRId64,
                  g_failures[static_cast<size_t>(FailureDomain::Scene)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Bot)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Weapon)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Bullet)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Smoke)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Install)].load(std::memory_order_relaxed));
    return buffer;
}
} // namespace cs2bv::memory
