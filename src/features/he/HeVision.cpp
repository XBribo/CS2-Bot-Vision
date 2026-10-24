#include "core/log.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
// HE grenade smoke-hole capture and state

#include "HeVision.h"

#include "features/smoke/SmokeVision.h"
#include "game_time.h"
#include "core/cs2_sdk/src/utils/hooks.h"
#include "memory.h"
#include "core/cs2_sdk/src/utils/platform.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/cs2_sdk/src/core/memory_module.h"

#include <nlohmann/json.hpp>
#include <tier0/dbg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace cs2bv::he_vision {
namespace {
struct HeBlast
{
    float x;
    float y;
    float z;
    float startTime;
};

constexpr const char* kHeDetonateName = "CHEGrenadeProjectile::Detonate";
cs2_sdk::hooks::NativeHook<int64_t, void*> g_detonateHook;

int g_bodyComponentOffset = -1;
int g_sceneNodeOffset = -1;
int g_absOriginOffset = -1;

std::mutex g_blastMutex;
std::vector<HeBlast> g_blasts;
std::atomic<int> g_radiusMilli{ 250000 };
std::atomic<int> g_durationMilli{ 5000 };
const char* g_hookStatus = "not_attempted";

// Clamps a scalar to the normalized range
float Saturate(float value) { return std::clamp(value, 0.0F, 1.0F); }

// Evaluates the shader smoothstep polynomial
float SmoothStep(float value)
{
    const float amount = Saturate(value);
    return amount * amount * (3.0F - 2.0F * amount);
}

// Captures the projectile origin before invoking the original detonation
KHook::Return<int64_t> HookedDetonate(void* self) noexcept
{
    if (self)
    {
        uintptr_t bodyComponent = 0;
        if (!memory::Read(self, g_bodyComponentOffset, bodyComponent, memory::FailureDomain::Scene) || !bodyComponent)
            return { KHook::Action::Ignore };

        uintptr_t sceneNode = 0;
        const void* bodyAddress = reinterpret_cast<const void*>(bodyComponent); // NOLINT(performance-no-int-to-ptr)
        if (!memory::Read(bodyAddress, g_sceneNodeOffset, sceneNode, memory::FailureDomain::Scene)) return { KHook::Action::Ignore };

        if (sceneNode)
        {
            float origin[3]{};
            const void* sceneAddress = reinterpret_cast<const void*>(sceneNode); // NOLINT(performance-no-int-to-ptr)
            if (!memory::Read(sceneAddress, g_absOriginOffset, origin, memory::FailureDomain::Scene)) return { KHook::Action::Ignore };
            OnDetonate(origin[0], origin[1], origin[2]);
        }
    }
    return { KHook::Action::Ignore };
}

} // namespace

// Resolves offsets and installs the optional HE detonation hook
bool Install(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    const cs2_sdk::schema::FieldBinding fields[] = {
        { &g_bodyComponentOffset, "CBaseEntity", "m_CBodyComponent" },
        { &g_sceneNodeOffset, "CBodyComponent", "m_pSceneNode" },
        { &g_absOriginOffset, "CGameSceneNode", "m_vecAbsOrigin" },
    };
    if (!cs2_sdk::schema::ResolveFields(fields))
    {
        BV_LOG_WARN("HE offsets unavailable from schema; HE holes disabled");
        g_hookStatus = "schema=FAIL";
        return false;
    }

    char error[256] = { 0 };
    void* target = cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, kHeDetonateName, error, sizeof(error));
    if (target && g_detonateHook.Install(target, &HookedDetonate))
    {
        g_hookStatus = "hook=ok";
        return true;
    }

    g_detonateHook.Remove();
    BV_LOG_WARN("HE detonate hook failed (%s); HE holes disabled", target ? "KHook error" : error);
    g_hookStatus = target ? "hook=FAIL" : "sig=FAIL";
    return false;
}

// Removes the HE hook and active blast state
void Remove()
{
    g_detonateHook.Remove();
    std::scoped_lock lock(g_blastMutex);
    g_blasts.clear();
}

// Appends a new active HE hole
void OnDetonate(float x, float y, float z)
{
    const float time = game_time::Now();
    const float point[3] = { x, y, z };
    if (!smoke_vision::HasSmokeNearPoint(point, GetRadius())) return;

    {
        std::scoped_lock lock(g_blastMutex);
        if (g_blasts.size() == kMaxInfluences) g_blasts.erase(g_blasts.begin());
        g_blasts.push_back({ .x = x, .y = y, .z = z, .startTime = time });
    }

    BV_LOG_DEBUG("HE detonate @ (%.1f,%.1f,%.1f) t=%.2f total=%d", x, y, z, time, GetActiveCount());
}

// Collects active blasts along a line.
size_t CollectInfluences(const float* from, const float* to, float now, std::span<Influence, kMaxInfluences> influences)
{
    const float duration = g_durationMilli.load(std::memory_order_relaxed) * 0.001F;
    const float radius = g_radiusMilli.load(std::memory_order_relaxed) * 0.001F;
    if (duration <= 0.0F || radius <= 0.0F) return 0;

    const float line[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    const float lineLengthSquared = (line[0] * line[0]) + (line[1] * line[1]) + (line[2] * line[2]);
    if (lineLengthSquared <= 0.001F) return 0;

    const float recoveryStart = duration * 0.1F;
    const float recoveryLength = std::max(duration - recoveryStart, 0.001F);
    std::scoped_lock lock(g_blastMutex);
    size_t influenceCount = 0;
    size_t writeIndex = 0;
    for (size_t index = 0; index < g_blasts.size(); ++index)
    {
        const HeBlast blast = g_blasts[index];
        const float age = now - blast.startTime;
        if (age < 0.0F || age >= duration) continue;

        g_blasts[writeIndex++] = blast;
        const float center[3] = { blast.x, blast.y, blast.z };
        const float offset[3] = { center[0] - from[0], center[1] - from[1], center[2] - from[2] };
        const float closestAmount = Saturate((offset[0] * line[0] + offset[1] * line[1] + offset[2] * line[2]) / lineLengthSquared);
        const float closest[3] = { from[0] + (line[0] * closestAmount), from[1] + (line[1] * closestAmount),
                                   from[2] + (line[2] * closestAmount) };
        const float delta[3] = { closest[0] - center[0], closest[1] - center[1], closest[2] - center[2] };
        const float distanceSquared = (delta[0] * delta[0]) + (delta[1] * delta[1]) + (delta[2] * delta[2]);

        const float impulse = std::pow(1.0F - SmoothStep(age * 0.5F), 128.0F);
        const float effectRadius = radius * (0.96F - impulse);
        if (effectRadius <= 0.0F || distanceSquared >= effectRadius * effectRadius) continue;

        const float halfAmount = std::sqrt((effectRadius * effectRadius) - distanceSquared) / std::sqrt(lineLengthSquared);
        const float begin = Saturate(closestAmount - halfAmount);
        const float end = Saturate(closestAmount + halfAmount);
        if (end > begin)
        {
            const float recovery = std::pow(SmoothStep((age - recoveryStart) / recoveryLength), 1.8F);
            influences[influenceCount++] = { .center = { blast.x, blast.y, blast.z },
                                             .radius = radius,
                                             .impulse = impulse,
                                             .recovery = recovery,
                                             .begin = begin,
                                             .finish = end };
        }
    }
    g_blasts.resize(writeIndex);
    return influenceCount;
}

// Returns the blast density scale at a point.
float DensityScale(const float* point, const Influence& influence)
{
    const float delta[3] = { point[0] - influence.center[0], point[1] - influence.center[1], point[2] - influence.center[2] };
    const float distance = std::sqrt((delta[0] * delta[0]) + (delta[1] * delta[1]) + (delta[2] * delta[2]));
    const float radiusScale = influence.radius / 250.0F;
    const float radial = SmoothStep((distance + influence.impulse * influence.radius - 200.0F * radiusScale) / (40.0F * radiusScale));
    return 0.02F + (0.98F * std::max(radial, influence.recovery));
}

// Stores the HE hole radius in fixed-point units
void SetRadius(float value) { g_radiusMilli.store(static_cast<int>(value * 1000.0F), std::memory_order_relaxed); }

// Returns the HE hole radius
float GetRadius() { return g_radiusMilli.load(std::memory_order_relaxed) * 0.001F; }

// Stores the HE hole lifetime in fixed-point units
void SetDuration(float value) { g_durationMilli.store(static_cast<int>(value * 1000.0F), std::memory_order_relaxed); }

// Returns the number of retained HE holes
int GetActiveCount()
{
    std::scoped_lock lock(g_blastMutex);
    return static_cast<int>(g_blasts.size());
}

// Returns the HE hook status.
const char* GetHookStatus() { return g_hookStatus; }
} // namespace cs2bv::he_vision
