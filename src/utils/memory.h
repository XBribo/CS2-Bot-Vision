// Validated runtime memory reads and diagnostics

#pragma once // NOLINT(portability-avoid-pragma-once)

#include <cstddef>
#include <cstdint>
#include <cstring>

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

// Checks whether a complete address range is readable
bool IsReadable(const void* address, size_t size);

#ifndef _WIN32
// Verifies that kernel-assisted reads are available before installing hooks
bool Initialize(char* error, size_t maxLength);

// Copies from the process; a failed read may partially fill the destination
bool ReadBytes(const void* address, void* out, size_t size);
#endif

// Records one failed read for a runtime subsystem
void RecordFailure(FailureDomain domain);

// Formats all runtime read failure counters
const char* Diagnostics();

// Reads a complete field and preserves the output on failure
template <typename T> bool Read(const void* base, size_t offset, T& out, FailureDomain domain)
{
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
#ifdef _WIN32
    if (!IsReadable(address, sizeof(T)))
#else
    unsigned char buffer[sizeof(T)];
    if (!ReadBytes(address, buffer, sizeof(buffer)))
#endif
    {
        RecordFailure(domain);
        return false;
    }

#ifdef _WIN32
    std::memcpy(static_cast<void*>(&out), address, sizeof(T));
#else
    std::memcpy(static_cast<void*>(&out), buffer, sizeof(T));
#endif
    return true;
}
} // namespace cs2bv::memory
