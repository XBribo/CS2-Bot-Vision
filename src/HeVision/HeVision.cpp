// HE grenade smoke-hole capture and state

#include "HeVision.h"

#include "game_time.h"
#include "hook.h"
#include "memory.h"
#include "platform.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace cs2bv::HeVision
{
    struct HeBlast
    {
        float x;
        float y;
        float z;
        float startTime;
    };

    using HeDetonateFn = __int64(__fastcall *)(void *self);

    static constexpr const char *kHeDetonateName =
        "CHEGrenadeProjectile::Detonate";
    static HeDetonateFn g_originalDetonate = nullptr;
    static Hook g_detonateHook;

    static int g_bodyComponentOffset = 0x30;
    static int g_sceneNodeOffset = 0x8;
    static int g_absOriginOffset = 200;

    static std::mutex g_blastMutex;
    static std::vector<HeBlast> g_blasts;
    static std::atomic<int> g_radiusMilli{75000};
    static std::atomic<int> g_durationMilli{3200};
    static std::string g_listenerStatus = "not_attempted";

    // Calculates the squared distance from a point to a segment
    static float DistanceSquaredToSegment(const float point[3],
                                          const float start[3],
                                          const float end[3])
    {
        float segment[3] = {
            end[0] - start[0],
            end[1] - start[1],
            end[2] - start[2]};
        float offset[3] = {
            point[0] - start[0],
            point[1] - start[1],
            point[2] - start[2]};
        const float lengthSquared =
            segment[0] * segment[0] +
            segment[1] * segment[1] +
            segment[2] * segment[2];
        float amount = lengthSquared > 0.0f
                           ? (offset[0] * segment[0] +
                              offset[1] * segment[1] +
                              offset[2] * segment[2]) /
                                 lengthSquared
                           : 0.0f;
        if (amount < 0.0f)
            amount = 0.0f;
        else if (amount > 1.0f)
            amount = 1.0f;

        float closest[3] = {
            start[0] + segment[0] * amount,
            start[1] + segment[1] * amount,
            start[2] + segment[2] * amount};
        float delta[3] = {
            point[0] - closest[0],
            point[1] - closest[1],
            point[2] - closest[2]};
        return delta[0] * delta[0] +
               delta[1] * delta[1] +
               delta[2] * delta[2];
    }

    // Captures the projectile origin before invoking the original detonation
    static __int64 __fastcall HookedDetonate(void *self)
    {
        if (self)
        {
            uintptr_t bodyComponent = 0;
            if (!memory::Read(self, g_bodyComponentOffset, bodyComponent,
                              memory::FailureDomain::Scene) ||
                !bodyComponent)
                return g_originalDetonate(self);

            uintptr_t sceneNode = 0;
            if (!memory::Read(
                    reinterpret_cast<const void *>(bodyComponent),
                    g_sceneNodeOffset, sceneNode,
                    memory::FailureDomain::Scene))
                return g_originalDetonate(self);

            if (sceneNode)
            {
                float origin[3]{};
                if (!memory::Read(
                        reinterpret_cast<const void *>(sceneNode),
                        g_absOriginOffset, origin,
                        memory::FailureDomain::Scene))
                    return g_originalDetonate(self);
                OnDetonate(origin[0], origin[1], origin[2]);
            }
        }
        return g_originalDetonate(self);
    }

    // Resolves offsets and installs the optional HE detonation hook
    bool Install(const nlohmann::json &gamedata,
                 const sig::ModuleInfo &serverModule)
    {
        g_bodyComponentOffset = sig::ResolveOffset(
            gamedata, "CBaseEntity::m_CBodyComponent",
            g_bodyComponentOffset);
        g_sceneNodeOffset = sig::ResolveOffset(
            gamedata, "CBodyComponent::m_pSceneNode",
            g_sceneNodeOffset);
        g_absOriginOffset = sig::ResolveOffset(
            gamedata, "CGameSceneNode::m_vecAbsOrigin",
            g_absOriginOffset);

        char error[256] = {0};
        void *target = sig::ResolveSig(
            gamedata, serverModule, kHeDetonateName,
            error, sizeof(error));
        if (target &&
            g_detonateHook.Create(
                target, reinterpret_cast<void *>(&HookedDetonate),
                reinterpret_cast<void **>(&g_originalDetonate)) &&
            g_detonateHook.Enable())
        {
            char message[160];
            std::snprintf(
                message, sizeof(message),
                "[BotVision] %s @ %p (HE holes active)\n",
                kHeDetonateName, target);
            platform::DebugOut(message);
            g_listenerStatus = "hook=ok";
            return true;
        }

        g_detonateHook.Remove();
        g_originalDetonate = nullptr;
        char message[320];
        std::snprintf(
            message, sizeof(message),
            "[BotVision] HE detonate hook failed (%s); HE holes disabled\n",
            target ? "funchook error" : error);
        platform::DebugOut(message);
        g_listenerStatus = target ? "hook=FAIL" : "sig=FAIL";
        return false;
    }

    // Removes the HE hook and active blast state
    void Remove()
    {
        g_detonateHook.Remove();
        g_originalDetonate = nullptr;
        std::lock_guard<std::mutex> lock(g_blastMutex);
        g_blasts.clear();
    }

    // Appends a new active HE hole
    void OnDetonate(float x, float y, float z)
    {
        const float time = game_time::Now();
        {
            std::lock_guard<std::mutex> lock(g_blastMutex);
            g_blasts.push_back({x, y, z, time});
        }

        char message[160];
        std::snprintf(
            message, sizeof(message),
            "[BotVision] HE detonate @ (%.1f,%.1f,%.1f) t=%.2f total=%d\n",
            x, y, z, time, GetActiveCount());
        platform::DebugOut(message);
    }

    // Removes expired holes and tests the line against the remaining holes
    bool ClearsSegment(const float *from, const float *to)
    {
        const float duration =
            g_durationMilli.load(std::memory_order_relaxed) * 0.001f;
        const float baseRadius =
            g_radiusMilli.load(std::memory_order_relaxed) * 0.001f;
        if (duration <= 0.0f || baseRadius <= 0.0f)
            return false;

        const float now = game_time::Now();
        std::lock_guard<std::mutex> lock(g_blastMutex);
        bool cleared = false;
        size_t writeIndex = 0;
        for (size_t index = 0; index < g_blasts.size(); ++index)
        {
            const float age = now - g_blasts[index].startTime;
            if (age < 0.0f || age >= duration)
                continue;

            g_blasts[writeIndex++] = g_blasts[index];
            const float radius = baseRadius * (1.0f - age / duration);
            const float center[3] = {
                g_blasts[index].x,
                g_blasts[index].y,
                g_blasts[index].z};
            if (DistanceSquaredToSegment(center, from, to) <= radius * radius)
                cleared = true;
        }
        g_blasts.resize(writeIndex);
        return cleared;
    }

    // Stores the HE hole radius in fixed-point units
    void SetRadius(float value)
    {
        g_radiusMilli.store(
            static_cast<int>(value * 1000.0f),
            std::memory_order_relaxed);
    }

    // Returns the HE hole radius
    float GetRadius()
    {
        return g_radiusMilli.load(std::memory_order_relaxed) * 0.001f;
    }

    // Stores the HE hole lifetime in fixed-point units
    void SetDuration(float value)
    {
        g_durationMilli.store(
            static_cast<int>(value * 1000.0f),
            std::memory_order_relaxed);
    }

    // Returns the HE hole lifetime
    float GetDuration()
    {
        return g_durationMilli.load(std::memory_order_relaxed) * 0.001f;
    }

    // Returns the number of retained HE holes
    int GetActiveCount()
    {
        std::lock_guard<std::mutex> lock(g_blastMutex);
        return static_cast<int>(g_blasts.size());
    }

    // Stores the legacy event-listener diagnostic state
    void SetListenerStatus(bool managerResolved, bool listenerAdded)
    {
        g_listenerStatus = managerResolved
                               ? (listenerAdded
                                      ? "mgr=ok,add=ok"
                                      : "mgr=ok,add=FAIL")
                               : "mgr=NULL";
    }

    // Returns the current HE diagnostic state
    const char *GetListenerStatus()
    {
        return g_listenerStatus.c_str();
    }
}
