// Validated runtime memory reads and diagnostics

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cs2bv::memory
{
    enum class FailureDomain
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
    bool IsReadable(const void *address, size_t size);

    // Records one failed read for a runtime subsystem
    void RecordFailure(FailureDomain domain);

    // Formats all runtime read failure counters
    const char *Diagnostics();

    // Reads a field after validating its complete memory range
    template <typename T>
    bool Read(const void *base, size_t offset, T &out, FailureDomain domain)
    {
        if (!base)
        {
            RecordFailure(domain);
            return false;
        }

        const uintptr_t baseAddress = reinterpret_cast<uintptr_t>(base);
        if (offset > UINTPTR_MAX - baseAddress)
        {
            RecordFailure(domain);
            return false;
        }

        const void *address = reinterpret_cast<const void *>(baseAddress + offset);
        if (!IsReadable(address, sizeof(T)))
        {
            RecordFailure(domain);
            return false;
        }

        std::memcpy(&out, address, sizeof(T));
        return true;
    }
}
