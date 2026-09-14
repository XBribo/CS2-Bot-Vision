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
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace cs2bv::memory {
namespace {
std::array<std::atomic<int64_t>, static_cast<size_t>(FailureDomain::Count)> g_failures{};
#ifndef _WIN32
std::atomic<int> g_lastReadError{ 0 };
#else
// Reuses only the last readable region within a single read operation.
bool IsReadableRange(const void* address, size_t size, uintptr_t& regionStart, uintptr_t& regionEnd)
{
    if (!address || size == 0) return false;
    auto cursor = reinterpret_cast<uintptr_t>(address);
    if (size > UINTPTR_MAX - cursor) return false;
    const uintptr_t end = cursor + size;
    while (cursor < end)
    {
        if (cursor >= regionStart && cursor < regionEnd)
        {
            cursor = regionEnd;
            continue;
        }

        MEMORY_BASIC_INFORMATION memoryInfo{};
        const void* cursorAddress = reinterpret_cast<const void*>(cursor); // NOLINT(performance-no-int-to-ptr)
        if (VirtualQuery(cursorAddress, &memoryInfo, sizeof(memoryInfo)) == 0) return false;
        if (memoryInfo.State != MEM_COMMIT || (memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const DWORD protection = memoryInfo.Protect & 0xFF;
        const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                              protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
                              protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable) return false;

        const auto start = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
        if (memoryInfo.RegionSize > UINTPTR_MAX - start || start + memoryInfo.RegionSize <= cursor) return false;
        regionStart = start;
        regionEnd = start + memoryInfo.RegionSize;
        cursor = regionEnd;
    }
    return true;
}
#endif
} // namespace

#ifndef _WIN32
// Reads the whole range in one syscall without dereferencing the source in user space
bool ReadBytes(const void* address, void* out, size_t size)
{
    if (!address || !out || size == 0 || size > UINTPTR_MAX - reinterpret_cast<uintptr_t>(address)) return false;

    static const pid_t selfPid = getpid();
    iovec local{ out, size };
    iovec remote{ const_cast<void*>(address), size };
    const ssize_t copied = process_vm_readv(selfPid, &local, 1, &remote, 1, 0);
    if (copied >= 0 && static_cast<size_t>(copied) == size) return true;

    g_lastReadError.store(copied < 0 ? errno : EFAULT, std::memory_order_relaxed);
    return false;
}

// Rejects unsupported or restricted Linux environments before registering plugin state
bool Initialize(char* error, size_t maxLength)
{
    const unsigned char probe = 0x5A;
    unsigned char copied = 0;
    if (ReadBytes(&probe, &copied, sizeof(probe)) && copied == probe) return true;

    RecordFailure(FailureDomain::Install);
    const int readError = g_lastReadError.load(std::memory_order_relaxed);
    if (error && maxLength > 0)
    {
        std::snprintf(error, maxLength, "process_vm_readv self-read failed (errno=%d: %s); check kernel/seccomp policy", readError,
                      std::strerror(readError));
    }
    return false;
}
#endif

// Batches independent reads without retaining addresses or page permissions.
bool ReadPairBytes(const void* first, void* firstOut, size_t firstSize, const void* second, void* secondOut, size_t secondSize)
{
    if (!first || !second || !firstOut || !secondOut || firstSize == 0 || secondSize == 0 ||
        firstSize > UINTPTR_MAX - reinterpret_cast<uintptr_t>(first) ||
        secondSize > UINTPTR_MAX - reinterpret_cast<uintptr_t>(second) || secondSize > SIZE_MAX - firstSize)
        return false;
#ifdef _WIN32
    uintptr_t regionStart = 0;
    uintptr_t regionEnd = 0;
    if (!IsReadableRange(first, firstSize, regionStart, regionEnd) || !IsReadableRange(second, secondSize, regionStart, regionEnd))
        return false;
    std::memcpy(firstOut, first, firstSize);
    std::memcpy(secondOut, second, secondSize);
    return true;
#else
    static const pid_t selfPid = getpid();
    iovec local[2] = { { firstOut, firstSize }, { secondOut, secondSize } };
    iovec remote[2] = { { const_cast<void*>(first), firstSize }, { const_cast<void*>(second), secondSize } };
    const ssize_t copied = process_vm_readv(selfPid, local, 2, remote, 2, 0);
    if (copied >= 0 && static_cast<size_t>(copied) == firstSize + secondSize) return true;
    g_lastReadError.store(copied < 0 ? errno : EFAULT, std::memory_order_relaxed);
    return false;
#endif
}

// Checks every virtual-memory region covered by an address range
bool IsReadable(const void* address, size_t size)
{
#ifdef _WIN32
    uintptr_t regionStart = 0;
    uintptr_t regionEnd = 0;
    return IsReadableRange(address, size, regionStart, regionEnd);
#else
    if (!address || size == 0) return false;

    auto cursor = reinterpret_cast<uintptr_t>(address);
    if (size > UINTPTR_MAX - cursor) return false;
    const uintptr_t end = cursor + size;

    while (cursor < end)
    {
        // Only standalone probes use page stepping; field reads copy the full range.
        static const uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
        const uintptr_t pageEnd = (cursor & ~(pageSize - 1)) + pageSize;
        const uintptr_t chunkEnd = pageEnd < end ? pageEnd : end;
        unsigned char scratch = 0;
        const void* probe = reinterpret_cast<const void*>(cursor); // NOLINT(performance-no-int-to-ptr)
        if (!ReadBytes(probe, &scratch, sizeof(scratch))) return false;
        cursor = chunkEnd;
    }
    return true;
#endif
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
    static char buffer[224];
    std::snprintf(buffer, sizeof(buffer),
                  "scene=%" PRId64 " bot=%" PRId64 " weapon=%" PRId64 " bullet=%" PRId64 " smoke=%" PRId64 " install=%" PRId64,
                  g_failures[static_cast<size_t>(FailureDomain::Scene)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Bot)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Weapon)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Bullet)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Smoke)].load(std::memory_order_relaxed),
                  g_failures[static_cast<size_t>(FailureDomain::Install)].load(std::memory_order_relaxed));
#ifndef _WIN32
    const size_t written = std::strlen(buffer);
    std::snprintf(buffer + written, sizeof(buffer) - written, " linux_read_errno=%d", g_lastReadError.load(std::memory_order_relaxed));
#endif
    return buffer;
}
} // namespace cs2bv::memory
