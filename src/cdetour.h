// cdetour.h
//
// Thin funchook wrapper for inline detours (cross-platform: Windows + Linux x64).
// Target address comes from the existing sig layer; this only owns the funchook handle.

#pragma once

#include <funchook.h>

namespace cs2bv::hooks
{
    // Inline detour over a resolved address. T = hooked function signature
    template <typename T>
    class CDetour
    {
    public:
        explicit CDetour(const char *name) : m_name(name) {}
        ~CDetour() { Free(); }

        CDetour(const CDetour &) = delete;
        CDetour &operator=(const CDetour &) = delete;

        /* Prepare a detour at `target`; writes the trampoline (orig caller) into *origOut.
           Returns false on alloc/prepare failure */
        bool Create(void *target, void *detour, void **origOut)
        {
            if (!target || m_hook)
                return false;
            m_hook = funchook_create();
            if (!m_hook)
                return false;
            *origOut = target; // funchook_prepare rewrites this to the trampoline
            if (funchook_prepare(m_hook, origOut, detour) != FUNCHOOK_ERROR_SUCCESS)
            {
                funchook_destroy(m_hook);
                m_hook = nullptr;
                return false;
            }
            return true;
        }

        // Arm the detour
        bool Enable()
        {
            if (!m_hook || m_installed)
                return false;
            if (funchook_install(m_hook, 0) != FUNCHOOK_ERROR_SUCCESS)
                return false;
            m_installed = true;
            return true;
        }

        // Disarm + free the funchook handle
        void Free()
        {
            if (!m_hook)
                return;
            if (m_installed)
            {
                funchook_uninstall(m_hook, 0);
                m_installed = false;
            }
            funchook_destroy(m_hook);
            m_hook = nullptr;
        }

        const char *Name() const { return m_name; }
        bool Installed() const { return m_installed; }

    private:
        const char *m_name = nullptr;
        funchook_t *m_hook = nullptr;
        bool m_installed = false;
    };
} // namespace cs2bv::hooks
