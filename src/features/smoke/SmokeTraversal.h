#pragma once
#include "SmokeEffects.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cs2bv::smoke_effects {
using Traversal = void (*)(void*, const Vec3*, const Vec3*, void*);
using CellInvoke = bool (*)(void*, const Vec3*, const std::uint32_t*);
struct CellContext
{
    bool* found;
    float* sum;
    Vec3** closest;
    void* volume;
};
static_assert(sizeof(void*) == 8 && sizeof(CellContext) == 0x20);
static_assert(offsetof(CellContext, sum) == 8 && offsetof(CellContext, volume) == 0x18);

// Traversal borrows the callback synchronously.
// Win64 uses a vtable.
// libstdc++ uses an invoker and manager.
class BorrowedTraversal
{
  public:
    using Adjust = void (*)(void*, CellContext*, const Vec3*, std::uint32_t, float, float);
    static void* Callable(void* container)
    {
        void* callable;
#ifdef _WIN32
        std::memcpy(&callable, static_cast<char*>(container) + 0x38, sizeof(callable));
#else
        std::memcpy(&callable, container, sizeof(callable));
#endif
        return callable;
    }
    static CellContext* Context(void* container)
    {
#ifdef _WIN32
        return reinterpret_cast<CellContext*>(static_cast<char*>(Callable(container)) + 8);
#else
        return static_cast<CellContext*>(Callable(container));
#endif
    }
    // Dispatch identity lies outside captured state.
    static const void* Identity(void* container)
    {
        const void* id = nullptr;
#ifdef _WIN32
        if (auto* callable = Callable(container)) std::memcpy(&id, callable, sizeof(id));
#else
        std::memcpy(&id, static_cast<char*>(container) + 0x18, sizeof(id));
#endif
        return id;
    }
    BorrowedTraversal(void* originalContainer, CellContext* context, void* state, Adjust adjust)
        : proxy_{ &table_, Callable(originalContainer), nullptr, context, state, adjust }
    {
#ifdef _WIN32
        const void* vtable;
        std::memcpy(&vtable, proxy_.original, sizeof(vtable));
        std::memcpy(&proxy_.invoke, static_cast<const char*>(vtable) + 0x10, sizeof(proxy_.invoke));
#else
        proxy_.original = originalContainer;
        std::memcpy(&proxy_.invoke, static_cast<const char*>(originalContainer) + 0x18, sizeof(proxy_.invoke));
        std::memcpy(&container_.manager, static_cast<const char*>(originalContainer) + 0x10, sizeof(container_.manager));
        container_.invoke = &InvokeData;
#endif
        container_.callable = &proxy_;
    }
    BorrowedTraversal(const BorrowedTraversal&) = delete;
    BorrowedTraversal& operator=(const BorrowedTraversal&) = delete;
    void* Container() { return &container_; }

  private:
    struct VTable
    {
        void* unsupportedBefore[2];
        CellInvoke invoke;
        void* unsupportedAfter[3];
    };
    struct Proxy
    {
        const VTable* vtable;
        void* original;
        CellInvoke invoke;
        CellContext* context;
        void* state;
        Adjust adjust;
    };
    struct ContainerStorage
    {
#ifdef _WIN32
        std::byte unused[0x38]{};
        Proxy* callable = nullptr;
#else
        Proxy* callable = nullptr;
        std::byte unused[8]{};
        void* manager = nullptr;
        CellInvoke invoke = nullptr;
#endif
    };
    static bool Invoke(void* self, const Vec3* point, const std::uint32_t* cell)
    {
        auto& proxy = *static_cast<Proxy*>(self);
        float* const sum = proxy.context->sum;
        const float before = *sum;
        bool result;
        // Null closest output gives independent cell contributions.
        float cellDensity = 0;
        struct RestoreSum
        {
            CellContext& context;
            float* previous;
            ~RestoreSum() { context.sum = previous; }
        };
        {
            RestoreSum restore{ *proxy.context, sum };
            proxy.context->sum = &cellDensity;
            result = proxy.invoke(proxy.original, point, cell);
        }
        // Faults or stale generations keep the engine contribution.
        *sum = before + cellDensity;
        proxy.adjust(proxy.state, proxy.context, point, *cell, before, cellDensity);
        return result;
    }
#ifndef _WIN32
    static bool InvokeData(void* data, const Vec3* point, const std::uint32_t* cell)
    {
        Proxy* proxy;
        std::memcpy(&proxy, data, sizeof(proxy));
        return Invoke(proxy, point, cell);
    }
#endif
    inline static const VTable table_{ { nullptr, nullptr }, &Invoke, { nullptr, nullptr, nullptr } };
    static_assert(offsetof(VTable, invoke) == 0x10 && sizeof(VTable) == 0x30);
#ifdef _WIN32
    static_assert(offsetof(ContainerStorage, callable) == 0x38 && sizeof(ContainerStorage) == 0x40);
#else
    static_assert(offsetof(ContainerStorage, invoke) == 0x18 && sizeof(ContainerStorage) == 0x20);
#endif
    Proxy proxy_;
    ContainerStorage container_;
};
} // namespace cs2bv::smoke_effects
