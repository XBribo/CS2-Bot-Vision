// Bullet trace capture and temporary smoke tunnels

#include "BulletVision.h"

#include "SmokeVision/SmokeVision.h"
#include "game_time.h"
#include "hook.h"
#include "memory.h"
#include "platform.h"
#include "raytrace_iface.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace cs2bv::BulletVision {
struct BulletHole
{
    float start[3];
    float end[3];
    float startTime;
    float radius;
};

struct WeaponDefinitionCacheEntry
{
    void* shooter = nullptr;
    float sampleTime = -1.0f;
    int definitionIndex = -1;
};

using PelletTraceFn = __int64(__fastcall*)(__int64 a1,
                                           void* a2,
                                           __int64 a3,
                                           float a4,
                                           float a5,
                                           int a6,
                                           unsigned char a7,
                                           int a8,
                                           int a9,
                                           float a10,
                                           __int64 a11,
                                           int* a12,
                                           float a13,
                                           float a14,
                                           __int64 a15,
                                           __int64 a16,
                                           int a17,
                                           int a18,
                                           void* a19,
                                           __int64 a20);
using GetSlotFn = void*(__fastcall*)(void* weaponServices, int slot, unsigned int position);

static constexpr const char* kPelletTraceName = "BulletPelletTrace";
static constexpr const char* kGetSlotName = "CCSPlayer_WeaponServices::GetSlot";
static constexpr size_t kMaxBulletHoles = 64;
static constexpr size_t kWeaponCacheSize = 64;

static PelletTraceFn g_originalPelletTrace = nullptr;
static GetSlotFn g_getSlot = nullptr;
static Hook g_pelletTraceHook;
static rt::CRayTraceInterface* g_rayTrace = nullptr;
static int g_rayTraceReturnCode = -999;

static int g_weaponServicesOffset = 0xA30;
static int g_activeWeaponOffset = 0x60;
static int g_itemDefinitionIndexOffset = 0xA00;
static int g_entityIdentityOffset = 0x10;
static int g_entityHandleOffset = 0x10;

static std::mutex g_holeMutex;
static std::vector<BulletHole> g_holes;
static std::atomic<int> g_radiusMilli{ 12000 };
static std::atomic<int> g_shotgunRadiusMilli{ 28000 };
static std::atomic<int> g_durationMilli{ 150 };
static std::atomic<int> g_rangeMilli{ 8192000 };
static std::atomic<int> g_holesEnabled{ 1 };

static thread_local WeaponDefinitionCacheEntry g_weaponCache[kWeaponCacheSize];
static std::atomic<int> g_lastWeaponDefinition{ -1 };
static std::atomic<int> g_lastWeaponShotgun{ 0 };
static std::atomic<long long> g_bulletCount{ 0 };
static std::atomic<long long> g_traceAttempts{ 0 };
static std::atomic<long long> g_traceHits{ 0 };
static std::mutex g_lastBulletMutex;
static float g_lastBulletSource[3] = { 0.0f, 0.0f, 0.0f };
static float g_lastBulletAngles[3] = { 0.0f, 0.0f, 0.0f };
static float g_lastBulletDirection[3] = { 0.0f, 0.0f, 0.0f };

// Mixes a pointer for fixed-size cache indexing
static uint64_t MixPointerValue(uintptr_t value)
{
    uint64_t key = static_cast<uint64_t>(value);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return key;
}

// Checks whether an item definition belongs to a shotgun
static bool IsShotgunDefinition(int definitionIndex)
{
    return definitionIndex == 25 || definitionIndex == 27 || definitionIndex == 29 || definitionIndex == 35;
}

// Calculates the squared distance from a point to a segment
static float DistanceSquaredToSegment(const float point[3], const float start[3], const float end[3])
{
    float segment[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
    float offset[3] = { point[0] - start[0], point[1] - start[1], point[2] - start[2] };
    const float lengthSquared = segment[0] * segment[0] + segment[1] * segment[1] + segment[2] * segment[2];
    float amount = lengthSquared > 0.0f ? (offset[0] * segment[0] + offset[1] * segment[1] + offset[2] * segment[2]) / lengthSquared : 0.0f;
    if (amount < 0.0f) amount = 0.0f;
    else if (amount > 1.0f)
        amount = 1.0f;

    float closest[3] = { start[0] + segment[0] * amount, start[1] + segment[1] * amount, start[2] + segment[2] * amount };
    float delta[3] = { point[0] - closest[0], point[1] - closest[1], point[2] - closest[2] };
    return delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
}

// Resolves the active weapon definition through weapon services
static int ActiveWeaponDefinition(void* pawn)
{
    if (!pawn || !g_getSlot) return -1;

    void* weaponServices = nullptr;
    if (!memory::Read(pawn, g_weaponServicesOffset, weaponServices, memory::FailureDomain::Weapon) || !weaponServices) return -1;

    uint32_t activeHandle = 0;
    if (!memory::Read(weaponServices, g_activeWeaponOffset, activeHandle, memory::FailureDomain::Weapon)) return -1;
    if (activeHandle == 0u || activeHandle == 0xFFFFFFFFu) return -1;

    const int activeIndex = static_cast<int>(activeHandle & 0x7FFFu);
    for (int slot = 0; slot <= 4; ++slot)
    {
        const unsigned int maxPosition = slot == 3 ? 8u : 1u;
        for (unsigned int position = 0; position < maxPosition; ++position)
        {
            const unsigned int positionArgument = slot == 3 ? position : 0xFFFFFFFFu;
            void* weapon = g_getSlot(weaponServices, slot, positionArgument);
            if (!weapon) continue;

            void* identity = nullptr;
            if (!memory::Read(weapon, g_entityIdentityOffset, identity, memory::FailureDomain::Weapon) || !identity) continue;

            uint32_t handle = 0;
            if (!memory::Read(identity, g_entityHandleOffset, handle, memory::FailureDomain::Weapon)) continue;
            if (static_cast<int>(handle & 0x7FFFu) != activeIndex) continue;

            uint16_t definitionIndex = 0;
            if (!memory::Read(weapon, g_itemDefinitionIndexOffset, definitionIndex, memory::FailureDomain::Weapon)) return -1;
            return definitionIndex;
        }
    }
    return -1;
}

// Caches one weapon definition for all pellets in the same server time
static int CachedActiveWeaponDefinition(void* shooter)
{
    if (!shooter) return -1;

    const float sampleTime = game_time::Now();
    const size_t index = static_cast<size_t>(MixPointerValue(reinterpret_cast<uintptr_t>(shooter))) & (kWeaponCacheSize - 1);
    WeaponDefinitionCacheEntry& entry = g_weaponCache[index];
    if (sampleTime > 0.0f && entry.shooter == shooter && entry.sampleTime == sampleTime) return entry.definitionIndex;

    const int definitionIndex = ActiveWeaponDefinition(shooter);
    entry.shooter = shooter;
    entry.sampleTime = sampleTime;
    entry.definitionIndex = definitionIndex;
    return definitionIndex;
}

// Calls the original pellet trace with its unmodified arguments
static __int64 CallOriginalPelletTrace(__int64 a1,
                                       void* a2,
                                       __int64 a3,
                                       float a4,
                                       float a5,
                                       int a6,
                                       unsigned char a7,
                                       int a8,
                                       int a9,
                                       float a10,
                                       __int64 a11,
                                       int* a12,
                                       float a13,
                                       float a14,
                                       __int64 a15,
                                       __int64 a16,
                                       int a17,
                                       int a18,
                                       void* a19,
                                       __int64 a20)
{
    return g_originalPelletTrace(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
}

// Captures one pellet path before invoking the engine trace
static __int64 __fastcall HookedPelletTrace(__int64 a1,
                                            void* a2,
                                            __int64 a3,
                                            float a4,
                                            float a5,
                                            int a6,
                                            unsigned char a7,
                                            int a8,
                                            int a9,
                                            float a10,
                                            __int64 a11,
                                            int* a12,
                                            float a13,
                                            float a14,
                                            __int64 a15,
                                            __int64 a16,
                                            int a17,
                                            int a18,
                                            void* a19,
                                            __int64 a20)
{
    g_bulletCount.fetch_add(1, std::memory_order_relaxed);
    if (!GetHolesEnabled() || !SmokeVision::IsVolumeMode() || !g_rayTrace || !SmokeVision::HasSmokeProjectiles() || !a2 || !a3)
    {
        return CallOriginalPelletTrace(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
    }

    float sourceValues[3]{};
    float angleValues[3]{};
    if (!memory::Read(a2, 0, sourceValues, memory::FailureDomain::Bullet) ||
        !memory::Read(reinterpret_cast<const void*>(a3), 0, angleValues, memory::FailureDomain::Bullet))
    {
        return CallOriginalPelletTrace(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
    }

    const float pitch = angleValues[0] * 0.01745329252f;
    const float yaw = angleValues[1] * 0.01745329252f;
    const float roll = angleValues[2] * 0.01745329252f;
    const float sinPitch = std::sin(pitch);
    const float cosPitch = std::cos(pitch);
    const float sinYaw = std::sin(yaw);
    const float cosYaw = std::cos(yaw);
    const float sinRoll = std::sin(roll);
    const float cosRoll = std::cos(roll);

    float forward[3] = { cosPitch * cosYaw, cosPitch * sinYaw, -sinPitch };
    float right[3] = { -sinRoll * sinPitch * cosYaw + cosRoll * sinYaw, -sinRoll * sinPitch * sinYaw - cosRoll * cosYaw,
                       -sinRoll * cosPitch };
    float up[3] = { cosRoll * sinPitch * cosYaw + sinRoll * sinYaw, cosRoll * sinPitch * sinYaw - sinRoll * cosYaw, cosRoll * cosPitch };
    float direction[3] = { forward[0] - right[0] * a13 + up[0] * a14, forward[1] - right[1] * a13 + up[1] * a14,
                           forward[2] - right[2] * a13 + up[2] * a14 };
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (length <= 1e-4f)
    {
        return CallOriginalPelletTrace(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
    }

    const float range = GetRange();
    const float scale = range / length;
    rt::Vector start{ sourceValues[0], sourceValues[1], sourceValues[2] };
    rt::Vector end{ sourceValues[0] + direction[0] * scale, sourceValues[1] + direction[1] * scale,
                    sourceValues[2] + direction[2] * scale };
    float fullEnd[3] = { end.x, end.y, end.z };
    if (SmokeVision::DensityFunctionReady() && SmokeVision::DensityInLine(sourceValues, fullEnd) <= 0.0f)
    {
        return CallOriginalPelletTrace(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
    }

    void* shooter = nullptr;
    if (a1 && !memory::Read(reinterpret_cast<const void*>(a1), 56, shooter, memory::FailureDomain::Bullet)) shooter = nullptr;

    g_traceAttempts.fetch_add(1, std::memory_order_relaxed);
    rt::TraceOptions options;
    rt::TraceResult result;
    const bool traceHit = g_rayTrace->TraceEndShape(&start, &end, shooter, &options, &result);
    float traceEnd[3] = { traceHit ? result.EndPos.x : end.x, traceHit ? result.EndPos.y : end.y, traceHit ? result.EndPos.z : end.z };
    if (traceHit) g_traceHits.fetch_add(1, std::memory_order_relaxed);
    if (traceHit && SmokeVision::DensityFunctionReady() && SmokeVision::DensityInLine(sourceValues, traceEnd) <= 0.0f)
    {
        return CallOriginalPelletTrace(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
    }

    const int definitionIndex = CachedActiveWeaponDefinition(shooter);
    const bool shotgun = IsShotgunDefinition(definitionIndex);
    const float radius = shotgun ? GetShotgunRadius() : GetRadius();
    OnHole(sourceValues, traceEnd, radius);
    g_lastWeaponDefinition.store(definitionIndex, std::memory_order_relaxed);
    g_lastWeaponShotgun.store(shotgun ? 1 : 0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_lastBulletMutex);
        for (int index = 0; index < 3; ++index)
        {
            g_lastBulletSource[index] = sourceValues[index];
            g_lastBulletAngles[index] = angleValues[index];
            g_lastBulletDirection[index] = direction[index];
        }
    }

    return CallOriginalPelletTrace(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
}

// Resolves offsets and installs optional bullet capture facilities
bool Install(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    g_weaponServicesOffset = sig::ResolveOffset(gamedata, "CBasePlayerPawn::m_pWeaponServices", g_weaponServicesOffset);
    g_activeWeaponOffset = sig::ResolveOffset(gamedata, "CPlayer_WeaponServices::m_hActiveWeapon", g_activeWeaponOffset);
    g_itemDefinitionIndexOffset = sig::ResolveOffset(gamedata, "CBasePlayerWeapon::m_iItemDefinitionIndex", g_itemDefinitionIndexOffset);
    g_entityIdentityOffset = sig::ResolveOffset(gamedata, "CEntityInstance::m_pEntity", g_entityIdentityOffset);
    g_entityHandleOffset = g_entityIdentityOffset;

    char pelletError[256] = { 0 };
    void* pelletTarget = sig::ResolveSig(gamedata, serverModule, kPelletTraceName, pelletError, sizeof(pelletError));
    bool installed = false;
    if (pelletTarget &&
        g_pelletTraceHook.Create(pelletTarget, reinterpret_cast<void*>(&HookedPelletTrace),
                                 reinterpret_cast<void**>(&g_originalPelletTrace)) &&
        g_pelletTraceHook.Enable())
    {
        char message[160];
        std::snprintf(message, sizeof(message), "[BotVision] %s @ %p (bullet capture active)\n", kPelletTraceName, pelletTarget);
        platform::DebugOut(message);
        installed = true;
    }
    else
    {
        g_pelletTraceHook.Remove();
        g_originalPelletTrace = nullptr;
        char warning[320];
        std::snprintf(warning, sizeof(warning), "[BotVision] pellet-trace hook failed (%s); bullet holes disabled\n",
                      pelletTarget ? "funchook error" : pelletError);
        platform::DebugOut(warning);
    }

    char getSlotError[256] = { 0 };
    void* getSlotTarget = sig::ResolveSig(gamedata, serverModule, kGetSlotName, getSlotError, sizeof(getSlotError));
    if (getSlotTarget)
    {
        g_getSlot = reinterpret_cast<GetSlotFn>(getSlotTarget);
        char message[160];
        std::snprintf(message, sizeof(message), "[BotVision] %s @ %p (shotgun radius active)\n", kGetSlotName, getSlotTarget);
        platform::DebugOut(message);
    }
    else
    {
        char warning[320];
        std::snprintf(warning, sizeof(warning), "[BotVision] %s; shotgun radius disabled (all bullets use normal radius)\n", getSlotError);
        platform::DebugOut(warning);
    }
    return installed;
}

// Removes the bullet hook and clears runtime state
void Remove()
{
    g_pelletTraceHook.Remove();
    g_originalPelletTrace = nullptr;
    g_getSlot = nullptr;
    std::lock_guard<std::mutex> lock(g_holeMutex);
    g_holes.clear();
}

// Stores the external ray-trace interface and return code
void SetRayTrace(void* rayTrace, int returnCode)
{
    g_rayTrace = static_cast<rt::CRayTraceInterface*>(rayTrace);
    g_rayTraceReturnCode = returnCode;
}

// Checks whether external ray tracing is available
bool RayTraceReady() { return g_rayTrace != nullptr; }

// Adds or replaces one temporary bullet tunnel
void OnHole(const float start[3], const float end[3], float radius)
{
    const float time = game_time::Now();
    std::lock_guard<std::mutex> lock(g_holeMutex);
    if (g_holes.size() < kMaxBulletHoles)
    {
        BulletHole hole{};
        for (int index = 0; index < 3; ++index)
        {
            hole.start[index] = start[index];
            hole.end[index] = end[index];
        }
        hole.startTime = time;
        hole.radius = radius;
        g_holes.push_back(hole);
        return;
    }

    size_t oldest = 0;
    for (size_t index = 1; index < g_holes.size(); ++index)
    {
        if (g_holes[index].startTime < g_holes[oldest].startTime) oldest = index;
    }
    for (int index = 0; index < 3; ++index)
    {
        g_holes[oldest].start[index] = start[index];
        g_holes[oldest].end[index] = end[index];
    }
    g_holes[oldest].startTime = time;
    g_holes[oldest].radius = radius;
}

// Removes expired tunnels and tests the bot eye against them
bool ClearsSegment(const float* from, const float* /*to*/)
{
    const float duration = GetDuration();
    if (duration <= 0.0f) return false;

    const float now = game_time::Now();
    std::lock_guard<std::mutex> lock(g_holeMutex);
    bool cleared = false;
    size_t writeIndex = 0;
    for (size_t index = 0; index < g_holes.size(); ++index)
    {
        const float age = now - g_holes[index].startTime;
        if (age < 0.0f || age >= duration) continue;

        g_holes[writeIndex++] = g_holes[index];
        const float radius = g_holes[index].radius;
        if (radius > 0.0f && DistanceSquaredToSegment(from, g_holes[index].start, g_holes[index].end) <= radius * radius)
        {
            cleared = true;
        }
    }
    g_holes.resize(writeIndex);
    return cleared;
}

// Stores the normal tunnel radius
void SetRadius(float value) { g_radiusMilli.store(static_cast<int>(value * 1000.0f), std::memory_order_relaxed); }

// Returns the normal tunnel radius
float GetRadius() { return g_radiusMilli.load(std::memory_order_relaxed) * 0.001f; }

// Stores the shotgun tunnel radius
void SetShotgunRadius(float value) { g_shotgunRadiusMilli.store(static_cast<int>(value * 1000.0f), std::memory_order_relaxed); }

// Returns the shotgun tunnel radius
float GetShotgunRadius() { return g_shotgunRadiusMilli.load(std::memory_order_relaxed) * 0.001f; }

// Stores the tunnel lifetime
void SetDuration(float value) { g_durationMilli.store(static_cast<int>(value * 1000.0f), std::memory_order_relaxed); }

// Returns the tunnel lifetime
float GetDuration() { return g_durationMilli.load(std::memory_order_relaxed) * 0.001f; }

// Stores the trace range
void SetRange(float value) { g_rangeMilli.store(static_cast<int>(value * 1000.0f), std::memory_order_relaxed); }

// Returns the trace range
float GetRange() { return g_rangeMilli.load(std::memory_order_relaxed) * 0.001f; }

// Stores the bullet tunnel enabled state
void SetHolesEnabled(bool enabled) { g_holesEnabled.store(enabled ? 1 : 0, std::memory_order_relaxed); }

// Returns the bullet tunnel enabled state
bool GetHolesEnabled() { return g_holesEnabled.load(std::memory_order_relaxed) != 0; }

// Returns the retained tunnel count
int GetActiveHoleCount()
{
    std::lock_guard<std::mutex> lock(g_holeMutex);
    return static_cast<int>(g_holes.size());
}

// Returns the pellet hook call count
long long GetBulletCount() { return g_bulletCount.load(std::memory_order_relaxed); }

// Formats the last captured pellet
const char* GetLastBulletInfo()
{
    static char buffer[160];
    std::lock_guard<std::mutex> lock(g_lastBulletMutex);
    std::snprintf(buffer, sizeof(buffer), "src=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f) fwd=(%.2f,%.2f,%.2f)", g_lastBulletSource[0],
                  g_lastBulletSource[1], g_lastBulletSource[2], g_lastBulletAngles[0], g_lastBulletAngles[1], g_lastBulletAngles[2],
                  g_lastBulletDirection[0], g_lastBulletDirection[1], g_lastBulletDirection[2]);
    return buffer;
}

// Formats ray-trace and bullet capture diagnostics
const char* GetDiagnostics()
{
    static char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "rt=%s ret=%d enabled=%d autolist=%s attempts=%lld traceHits=%lld", g_rayTrace ? "OK" : "NULL",
                  g_rayTraceReturnCode, g_holesEnabled.load(std::memory_order_relaxed), SmokeVision::AutoListReady() ? "set" : "NULL",
                  g_traceAttempts.load(std::memory_order_relaxed), g_traceHits.load(std::memory_order_relaxed));
    return buffer;
}

// Formats the most recently resolved active weapon
const char* GetWeaponProbe()
{
    static char buffer[128];
    const int definitionIndex = g_lastWeaponDefinition.load(std::memory_order_relaxed);
    std::snprintf(buffer, sizeof(buffer), "lastDef=%d shotgun=%d shotgunRadius=%.1f normalRadius=%.1f getSlot=%s", definitionIndex,
                  g_lastWeaponShotgun.load(std::memory_order_relaxed), GetShotgunRadius(), GetRadius(), g_getSlot ? "OK" : "NULL");
    return buffer;
}
} // namespace cs2bv::BulletVision
