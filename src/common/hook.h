// Inline hook wrapper over funchook

#pragma once // NOLINT(portability-avoid-pragma-once)

#include <funchook.h>

namespace cs2bv {
// Owns one prepared or installed funchook detour
class Hook
{
  public:
    // Creates an empty hook owner
    Hook() = default;

    // Removes the owned hook
    ~Hook() { Remove(); }

    // Prevents duplicate ownership of a hook handle
    Hook(const Hook&) = delete;

    // Prevents duplicate ownership through assignment
    Hook& operator=(const Hook&) = delete;

    // Prepares a detour and publishes its trampoline
    bool Create(void* target, void* detour, void** original)
    {
        if (m_hook || !target || !detour || !original) return false;

        m_hook = funchook_create();
        if (!m_hook) return false;

        *original = target;
        if (funchook_prepare(m_hook, original, detour) != FUNCHOOK_ERROR_SUCCESS)
        {
            funchook_destroy(m_hook);
            m_hook = nullptr;
            return false;
        }
        return true;
    }

    // Installs the prepared detour
    bool Enable()
    {
        if (!m_hook || m_installed) return false;
        if (funchook_install(m_hook, 0) != FUNCHOOK_ERROR_SUCCESS) return false;
        m_installed = true;
        return true;
    }

    // Uninstalls and destroys the detour
    void Remove()
    {
        if (!m_hook) return;
        if (m_installed)
        {
            funchook_uninstall(m_hook, 0);
            m_installed = false;
        }
        funchook_destroy(m_hook);
        m_hook = nullptr;
    }

    // Reports whether the detour is installed
    bool Active() const { return m_installed; }

  private:
    funchook_t* m_hook = nullptr;
    bool m_installed = false;
};
} // namespace cs2bv
