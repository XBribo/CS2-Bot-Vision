#pragma once

#include <khook.hpp>
#include <memory>

namespace cs2bv::hooks {

// Owns a typed KHook registration and synchronously removes it before destruction.
template <typename RETURN, typename... ARGS> class NativeHook
{
    class CheckedFunction : public KHook::Function<RETURN, ARGS...>
    {
      public:
        using KHook::Function<RETURN, ARGS...>::Function;
        // Checks whether KHook accepted the registration.
        bool Registered() const { return this->_associated_hook_id != KHook::INVALID_HOOK; }
    };
    std::unique_ptr<CheckedFunction> m_hook;

  public:
    using Callback = KHook::Return<RETURN> (*)(ARGS...);

    // Installs one function entry without taking ownership of the shared KHook engine.
    bool Install(void* target, Callback pre, Callback post = nullptr)
    {
        if (m_hook || !target || !KHook::__exported__khook) return false;
        m_hook = std::make_unique<CheckedFunction>(pre, post);
        m_hook->Configure(target);
        if (m_hook->Registered()) return true;
        m_hook.reset();
        return false;
    }

    // Waits for active invocations before releasing callback storage.
    void Remove() { m_hook.reset(); }
    // Reports a successfully accepted registration.
    bool Active() const { return m_hook != nullptr; }
};

} // namespace cs2bv::hooks
