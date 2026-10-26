#pragma once // NOLINT(portability-avoid-pragma-once)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "core/cs2_sdk/src/utils/memory.h"

namespace cs2bv::memory {
enum class FailureDomain : uint8_t
{
    Scene,
    Bot,
    Weapon,
    Bullet,
    Smoke,
    Install,
    Count
};

// Checks current readability; object lifetime is caller-managed.
using cs2_sdk::memory::IsReadable;

#ifndef _WIN32
// Verifies kernel-assisted reads before hook installation
bool Initialize(char* error, size_t maxLength);
#endif

// Reads bytes and retains Linux errno; failure may partially fill out.
bool ReadBytes(const void* address, void* out, size_t size);

// Records a failed read and optional POSIX error.
void RecordFailure(FailureDomain domain, int posixError = 0);

// Returns a borrowed buffer, overwritten on the next call.
const char* Diagnostics();

// Reads a complete field and preserves the output on failure
template <typename T> bool Read(const void* base, size_t offset, T& out, FailureDomain domain)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (!base)
    {
        RecordFailure(domain);
        return false;
    }

    const auto baseAddress = reinterpret_cast<uintptr_t>(base);
    if (offset > UINTPTR_MAX - baseAddress)
    {
        RecordFailure(domain);
        return false;
    }

    const void* address = reinterpret_cast<const void*>(baseAddress + offset); // NOLINT(performance-no-int-to-ptr)
    unsigned char buffer[sizeof(T)];
    if (!ReadBytes(address, buffer, sizeof(buffer)))
    {
        RecordFailure(domain);
        return false;
    }

    std::memcpy(static_cast<void*>(&out), buffer, sizeof(T));
    return true;
}
} // namespace cs2bv::memory
