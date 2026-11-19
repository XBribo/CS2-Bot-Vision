#pragma once
#include <khook.hpp>
#include <memory>
#include <utility>
namespace cs2bv::smoke_density {
template <typename F> class Detour;
template <typename R, typename... Args> class Detour<R (*)(Args...)>
{
    using Function = R (*)(Args...);
    using Callback = KHook::Return<R> (*)(Args...);
    class Registered : public KHook::Function<R, Args...>
    {
      public:
        using KHook::Function<R, Args...>::Function;
        bool Active() const { return this->_associated_hook_id != KHook::INVALID_HOOK; }
        // Busy targets may defer registration.
        void AttachNow(Function target)
        {
            auto* address = reinterpret_cast<void*>(target);
            const auto id =
                KHook::SetupHook(address, this, (void*)this->_KHook_RemovedHook, (void*)this->_KHook_Callback_PRE,
                                 (void*)this->_KHook_Callback_POST, (void*)this->_KHook_MakeReturn, (void*)this->_KHook_MakeOriginalCall,
                                 this->template _copy_stack_size<void*, Args...>(), false);
            if (id == KHook::INVALID_HOOK) return;
            std::lock_guard guard(this->_hooks_stored);
            this->_associated_hook_id = id;
            this->_hooked_addr = address;
            this->_hook_ids.insert(id);
        }
    };
    Function target_ = nullptr;
    Callback callback_ = nullptr;
    std::unique_ptr<Registered> hook_;

    bool Register(bool immediate)
    {
        auto hook = std::make_unique<Registered>(callback_, nullptr);
        if (immediate) hook->AttachNow(target_);
        else
            hook->Configure(target_);
        if (!hook->Active()) return false;
        hook_ = std::move(hook);
        return true;
    }

  public:
    bool Create(void* target, Callback callback)
    {
        if (target_ || !target || !callback) return false;
        target_ = reinterpret_cast<Function>(target);
        callback_ = callback;
        return true;
    }
    bool Enable() { return target_ && !hook_ && Register(false); }
    KHook::Return<R> Continue(Args... args)
    {
        return KHook::Recall(target_, KHook::Return<R>{ KHook::Action::Ignore }, std::forward<Args>(args)...);
    }
    bool Enabled() const { return hook_ != nullptr; }
    bool EnsureEnabled() { return hook_ || (target_ && Register(true)); }
    void Disable() { hook_.reset(); }
    void Reset()
    {
        Disable();
        target_ = nullptr;
        callback_ = nullptr;
    }
};
} // namespace cs2bv::smoke_density
