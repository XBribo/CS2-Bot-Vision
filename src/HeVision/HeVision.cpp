// HE grenade smoke-hole capture and state

#include "HeVision.h"

#include "SmokeVision/SmokeVision.h"
#include "game_time.h"
#include "hook.h"
#include "memory.h"
#include "platform.h"
#include "schema_resolver.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cs2bv::HeVision {
struct HeBlast
{
    float x;
    float y;
    float z;
    float startTime;
};

struct HeInfluence
{
    HeBlast blast;
    float age;
    float begin;
    float end;
};

using HeDetonateFn = __int64(__fastcall*)(void* self);

static constexpr const char* kHeDetonateName = "CHEGrenadeProjectile::Detonate";
static constexpr size_t kMaxHeBlasts = 5;
static constexpr int kDensitySlices = 5;
static HeDetonateFn g_originalDetonate = nullptr;
static Hook g_detonateHook;

static int g_bodyComponentOffset = -1;
static int g_sceneNodeOffset = -1;
static int g_absOriginOffset = -1;

static std::mutex g_blastMutex;
static std::vector<HeBlast> g_blasts;
static std::atomic<int> g_radiusMilli{ 250000 };
static std::atomic<int> g_durationMilli{ 5000 };
static std::string g_listenerStatus = "not_attempted";

// Clamps a scalar to the normalized range
static float Saturate(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

// Evaluates the shader smoothstep polynomial
static float SmoothStep(float value)
{
    const float amount = Saturate(value);
    return amount * amount * (3.0f - 2.0f * amount);
}

// Evaluates the HE density multiplier at one smoke point
static float DensityScale(float distance, float age, float radius, float duration)
{
    const float radiusScale = radius / 250.0f;
    const float impulse = std::pow(1.0f - SmoothStep(age * 0.5f), 128.0f);
    const float radial = SmoothStep((distance + impulse * radius - 200.0f * radiusScale) / (40.0f * radiusScale));
    const float recoveryStart = duration * 0.1f;
    const float recoveryLength = std::max(duration - recoveryStart, 0.001f);
    const float recovery = std::pow(SmoothStep((age - recoveryStart) / recoveryLength), 1.8f);
    return 0.02f + 0.98f * std::max(radial, recovery);
}

// Captures the projectile origin before invoking the original detonation
static __int64 __fastcall HookedDetonate(void* self)
{
    if (self)
    {
        uintptr_t bodyComponent = 0;
        if (!memory::Read(self, g_bodyComponentOffset, bodyComponent, memory::FailureDomain::Scene) || !bodyComponent)
            return g_originalDetonate(self);

        uintptr_t sceneNode = 0;
        if (!memory::Read(reinterpret_cast<const void*>(bodyComponent), g_sceneNodeOffset, sceneNode, memory::FailureDomain::Scene))
            return g_originalDetonate(self);

        if (sceneNode)
        {
            float origin[3]{};
            if (!memory::Read(reinterpret_cast<const void*>(sceneNode), g_absOriginOffset, origin, memory::FailureDomain::Scene))
                return g_originalDetonate(self);
            OnDetonate(origin[0], origin[1], origin[2]);
        }
    }
    return g_originalDetonate(self);
}

// Resolves offsets and installs the optional HE detonation hook
bool Install(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    g_bodyComponentOffset = schema::GetFieldOffset("CBaseEntity", "m_CBodyComponent");
    g_sceneNodeOffset = schema::GetFieldOffset("CBodyComponent", "m_pSceneNode");
    g_absOriginOffset = schema::GetFieldOffset("CGameSceneNode", "m_vecAbsOrigin");
    if (g_bodyComponentOffset < 0 || g_sceneNodeOffset < 0 || g_absOriginOffset < 0)
    {
        platform::DebugOut("[BotVision] HE offsets unavailable from schema; HE holes disabled\n");
        g_listenerStatus = "schema=FAIL";
        return false;
    }

    char error[256] = { 0 };
    void* target = sig::ResolveSig(gamedata, serverModule, kHeDetonateName, error, sizeof(error));
    if (target && g_detonateHook.Create(target, reinterpret_cast<void*>(&HookedDetonate), reinterpret_cast<void**>(&g_originalDetonate)) &&
        g_detonateHook.Enable())
    {
        g_listenerStatus = "hook=ok";
        return true;
    }

    g_detonateHook.Remove();
    g_originalDetonate = nullptr;
    char message[320];
    std::snprintf(message, sizeof(message), "[BotVision] HE detonate hook failed (%s); HE holes disabled\n",
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
    const float point[3] = { x, y, z };
    if (!SmokeVision::HasSmokeNearPoint(point, GetRadius())) return;

    {
        std::lock_guard<std::mutex> lock(g_blastMutex);
        if (g_blasts.size() == kMaxHeBlasts) g_blasts.erase(g_blasts.begin());
        g_blasts.push_back({ x, y, z, time });
    }

    char message[160];
    std::snprintf(message, sizeof(message), "[BotVision] HE detonate @ (%.1f,%.1f,%.1f) t=%.2f total=%d\n", x, y, z, time,
                  GetActiveCount());
    platform::DebugOut(message);
}

// Applies active HE records to native density inside each blast chord
float AdjustDensity(const float* from, const float* to, float density, DensitySamplerFn sampler)
{
    const float duration = g_durationMilli.load(std::memory_order_relaxed) * 0.001f;
    const float radius = g_radiusMilli.load(std::memory_order_relaxed) * 0.001f;
    if (!from || !to || !sampler || density <= 0.0f || duration <= 0.0f || radius <= 0.0f) return density;

    const float line[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    const float lineLengthSquared = line[0] * line[0] + line[1] * line[1] + line[2] * line[2];
    if (lineLengthSquared <= 0.001f) return density;

    const float now = game_time::Now();
    std::lock_guard<std::mutex> lock(g_blastMutex);
    std::vector<HeInfluence> influences;
    influences.reserve(g_blasts.size());
    size_t writeIndex = 0;
    for (size_t index = 0; index < g_blasts.size(); ++index)
    {
        const HeBlast blast = g_blasts[index];
        const float age = now - blast.startTime;
        if (age < 0.0f || age >= duration) continue;

        g_blasts[writeIndex++] = blast;
        const float center[3] = { blast.x, blast.y, blast.z };
        const float offset[3] = { center[0] - from[0], center[1] - from[1], center[2] - from[2] };
        const float closestAmount = Saturate((offset[0] * line[0] + offset[1] * line[1] + offset[2] * line[2]) / lineLengthSquared);
        const float closest[3] = { from[0] + line[0] * closestAmount, from[1] + line[1] * closestAmount,
                                   from[2] + line[2] * closestAmount };
        const float delta[3] = { closest[0] - center[0], closest[1] - center[1], closest[2] - center[2] };
        const float distanceSquared = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];

        const float impulse = std::pow(1.0f - SmoothStep(age * 0.5f), 128.0f);
        const float effectRadius = radius * (0.96f - impulse);
        if (effectRadius <= 0.0f || distanceSquared >= effectRadius * effectRadius) continue;

        const float halfAmount = std::sqrt(effectRadius * effectRadius - distanceSquared) / std::sqrt(lineLengthSquared);
        const float begin = Saturate(closestAmount - halfAmount);
        const float end = Saturate(closestAmount + halfAmount);
        if (end > begin) influences.push_back({ blast, age, begin, end });
    }
    g_blasts.resize(writeIndex);
    if (influences.empty()) return density;

    std::sort(influences.begin(), influences.end(), [](const HeInfluence& left, const HeInfluence& right)
    {
        return left.begin < right.begin;
    });

    std::vector<std::pair<float, float>> intervals;
    intervals.reserve(influences.size());
    for (const HeInfluence& influence : influences)
    {
        if (intervals.empty() || influence.begin > intervals.back().second + 0.0001f)
        {
            intervals.emplace_back(influence.begin, influence.end);
        }
        else
        {
            intervals.back().second = std::max(intervals.back().second, influence.end);
        }
    }

    float sampledDensity = 0.0f;
    float weightedDensity = 0.0f;
    for (const std::pair<float, float>& interval : intervals)
    {
        for (int slice = 0; slice < kDensitySlices; ++slice)
        {
            const float sliceBeginAmount = interval.first + (interval.second - interval.first) * (static_cast<float>(slice) / kDensitySlices);
            const float sliceEndAmount =
                interval.first + (interval.second - interval.first) * (static_cast<float>(slice + 1) / kDensitySlices);
            const float midpointAmount = (sliceBeginAmount + sliceEndAmount) * 0.5f;
            float sliceBegin[3] = { from[0] + line[0] * sliceBeginAmount, from[1] + line[1] * sliceBeginAmount,
                                    from[2] + line[2] * sliceBeginAmount };
            float sliceEnd[3] = { from[0] + line[0] * sliceEndAmount, from[1] + line[1] * sliceEndAmount,
                                  from[2] + line[2] * sliceEndAmount };
            const float midpoint[3] = { from[0] + line[0] * midpointAmount, from[1] + line[1] * midpointAmount,
                                        from[2] + line[2] * midpointAmount };

            float minimumScale = 1.0f;
            for (const HeInfluence& influence : influences)
            {
                if (midpointAmount < influence.begin || midpointAmount > influence.end) continue;

                const float delta[3] = { midpoint[0] - influence.blast.x, midpoint[1] - influence.blast.y,
                                         midpoint[2] - influence.blast.z };
                const float distance = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
                minimumScale = std::min(minimumScale, DensityScale(distance, influence.age, radius, duration));
            }
            if (minimumScale >= 0.999f) continue;

            const float sliceDensity = std::max(sampler(sliceBegin, sliceEnd), 0.0f);
            sampledDensity += sliceDensity;
            weightedDensity += sliceDensity * minimumScale;
        }
    }

    if (sampledDensity <= 0.0001f) return density;

    const float averageScale = std::clamp(weightedDensity / sampledDensity, 0.02f, 1.0f);
    const float affectedDensity = std::min(sampledDensity, density);
    return std::max(density - affectedDensity * (1.0f - averageScale), density * 0.02f);
}

// Stores the HE hole radius in fixed-point units
void SetRadius(float value) { g_radiusMilli.store(static_cast<int>(value * 1000.0f), std::memory_order_relaxed); }

// Returns the HE hole radius
float GetRadius() { return g_radiusMilli.load(std::memory_order_relaxed) * 0.001f; }

// Stores the HE hole lifetime in fixed-point units
void SetDuration(float value) { g_durationMilli.store(static_cast<int>(value * 1000.0f), std::memory_order_relaxed); }

// Returns the HE hole lifetime
float GetDuration() { return g_durationMilli.load(std::memory_order_relaxed) * 0.001f; }

// Returns the number of retained HE holes
int GetActiveCount()
{
    std::lock_guard<std::mutex> lock(g_blastMutex);
    return static_cast<int>(g_blasts.size());
}

// Stores the legacy event-listener diagnostic state
void SetListenerStatus(bool managerResolved, bool listenerAdded)
{
    g_listenerStatus = managerResolved ? (listenerAdded ? "mgr=ok,add=ok" : "mgr=ok,add=FAIL") : "mgr=NULL";
}

// Returns the current HE diagnostic state
const char* GetListenerStatus() { return g_listenerStatus.c_str(); }
} // namespace cs2bv::HeVision
