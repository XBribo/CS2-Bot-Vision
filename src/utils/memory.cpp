#include "memory.h"

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
#endif
} // namespace

bool ReadBytes(const void* address, void* out, size_t size)
{
    if (!address || !out || size == 0 || size > UINTPTR_MAX - reinterpret_cast<uintptr_t>(address)) return false;

#ifdef _WIN32
    return cs2_sdk::memory::ReadBytes(address, out, size);
#else
    errno = 0;
    if (cs2_sdk::memory::ReadBytes(address, out, size)) return true;
    g_lastReadError.store(errno ? errno : EFAULT, std::memory_order_relaxed);
    return false;
#endif
}

#ifndef _WIN32
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

void RecordFailure(FailureDomain domain, [[maybe_unused]] int posixError)
{
#ifndef _WIN32
    if (posixError) g_lastReadError.store(posixError, std::memory_order_relaxed);
#endif
    const auto index = static_cast<size_t>(domain);
    if (index < g_failures.size()) g_failures[index].fetch_add(1, std::memory_order_relaxed);
}

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
