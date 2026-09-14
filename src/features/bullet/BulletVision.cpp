#include "core/log.h"
#include "core/gameconfig.h"
// Bullet trace capture and temporary smoke tunnels

#include "BulletVision.h"

#include "features/smoke/SmokeVision.h"
#include "game_time.h"
#include "hooks.h"
#include "memory.h"
#include "platform.h"
#include "core/cs2_sdk/schema.h"
#include "core/memory_module.h"

#include <entity2/entityinstance.h>
#include <entityhandle.h>
#include <const.h>
#include <gametrace.h>
#include <mathlib/vector.h>
#include <ray.h>
#include <tier0/dbg.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace cs2bv::bullet_vision {
namespace {
struct BulletHole
{
    float start[3];
    float end[3];
    float startTime;
    float radius;
};

struct BulletInfluence
{
    BulletHole hole;
    float age;
    float begin;
    float end;
};

struct WeaponDefinitionCacheEntry
{
    void* shooter = nullptr;
    float sampleTime = -1.0F;
    int definitionIndex = -1;
};

using PelletTraceHook = hooks::NativeHook<int64_t,
                                          int64_t,
#ifdef _WIN32
                                          void*,
#else
                                          // SysV passes the by-value Vector in XMM0 (xy) and XMM1 (z).
                                          double,
                                          float,
#endif
                                          int64_t,
                                          float,
                                          float,
                                          int,
                                          unsigned char,
                                          int,
                                          int,
                                          float,
                                          int64_t,
                                          void*,
                                          float,
                                          float,
                                          int64_t,
                                          int64_t,
                                          int,
                                          int,
                                          void*,
                                          int64_t>;
using PelletResultHook = hooks::NativeHook<int64_t, void*, void*, float, void*>;
using TraceShapeFn = void(CS2BV_FASTCALL*)(
    const void* self, const Ray_t& ray, const Vector& start, const Vector& end, CTraceFilter* filter, CGameTrace* trace);
using GetSlotFn = void*(CS2BV_FASTCALL*)(void* weaponServices, int slot, unsigned int position);

constexpr const char* kPelletTraceName = "BulletPelletTrace";
constexpr const char* kPelletResultName = "BulletPelletTraceResult";
constexpr const char* kTraceShapeName = "CNavPhysicsInterface::TraceShape";
constexpr const char* kGetSlotName = "CCSPlayer_WeaponServices::GetSlot";
constexpr size_t kMaxBulletHoles = 16;
constexpr size_t kWeaponCacheSize = 64;
constexpr size_t kNativeTraceStartOffset = 0x78;
constexpr size_t kNativeTraceEndOffset = 0x84;
constexpr int kDensitySlices = 5;

TraceShapeFn g_traceShape = nullptr;
GetSlotFn g_getSlot = nullptr;
PelletTraceHook g_pelletTraceHook;
PelletResultHook g_pelletResultHook;
struct PelletFrame
{
    int64_t shooter;
    bool captureRequested = false;
    bool traceReady = false;
    float source[3]{};
    float end[3]{};
    float direction[3]{};
    float shotAngles[3]{};
};
struct PelletResultFrame
{
    void* trace;
    size_t pelletDepth;
};
thread_local std::vector<PelletFrame> g_pelletFrames;
thread_local std::vector<PelletResultFrame> g_pelletResultFrames;
void** g_navPhysicsVtable = nullptr;

int g_weaponServicesOffset = -1;
int g_activeWeaponOffset = -1;
int g_itemDefinitionIndexOffset = -1;

std::mutex g_holeMutex;
std::vector<BulletHole> g_holes;
std::atomic<int> g_radiusMilli{ 20000 };
std::atomic<int> g_shotgunRadiusMilli{ 80000 };
std::atomic<int> g_durationMilli{ 1000 };
std::atomic<int> g_holesEnabled{ 1 };

thread_local WeaponDefinitionCacheEntry g_weaponCache[kWeaponCacheSize];
std::atomic<int> g_lastWeaponDefinition{ -1 };
std::atomic<int> g_lastWeaponShotgun{ 0 };
std::atomic<int64_t> g_bulletCount{ 0 };
std::atomic<int64_t> g_nativeResultCount{ 0 };
std::atomic<int64_t> g_missingResultCount{ 0 };
std::atomic<int64_t> g_skippedModeCount{ 0 };
std::atomic<int64_t> g_skippedNoSmokeCount{ 0 };
std::atomic<int64_t> g_heTraceAttempts{ 0 };
std::atomic<int64_t> g_heTraceHits{ 0 };
std::mutex g_lastBulletMutex;
float g_lastBulletSource[3] = { 0.0F, 0.0F, 0.0F };
float g_lastBulletEnd[3] = { 0.0F, 0.0F, 0.0F };
float g_lastBulletAngles[3] = { 0.0F, 0.0F, 0.0F };
float g_lastBulletDirection[3] = { 0.0F, 0.0F, 0.0F };

// Mixes a pointer for fixed-size cache indexing
uint64_t MixPointerValue(uintptr_t value)
{
    auto key = static_cast<uint64_t>(value);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return key;
}

// Checks whether an item definition belongs to a shotgun
bool IsShotgunDefinition(int definitionIndex)
{
    return definitionIndex == 25 || definitionIndex == 27 || definitionIndex == 29 || definitionIndex == 35;
}

// Calculates the squared distance from a point to a segment
float DistanceSquaredToSegment(const float point[3], const float start[3], const float end[3])
{
    float segment[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
    float offset[3] = { point[0] - start[0], point[1] - start[1], point[2] - start[2] };
    const float lengthSquared = (segment[0] * segment[0]) + (segment[1] * segment[1]) + (segment[2] * segment[2]);
    float amount = lengthSquared > 0.0F ? (offset[0] * segment[0] + offset[1] * segment[1] + offset[2] * segment[2]) / lengthSquared : 0.0F;
    if (amount < 0.0F) amount = 0.0F;
    else if (amount > 1.0F)
        amount = 1.0F;

    float closest[3] = { start[0] + (segment[0] * amount), start[1] + (segment[1] * amount), start[2] + (segment[2] * amount) };
    float delta[3] = { point[0] - closest[0], point[1] - closest[1], point[2] - closest[2] };
    return (delta[0] * delta[0]) + (delta[1] * delta[1]) + (delta[2] * delta[2]);
}

// Clamps a scalar to the normalized range
float Saturate(float value) { return std::clamp(value, 0.0F, 1.0F); }

// Evaluates the shader smoothstep polynomial
float SmoothStep(float value)
{
    const float amount = Saturate(value);
    return amount * amount * (3.0F - 2.0F * amount);
}

// Finds the closest parameters between two finite segments
float ClosestSegmentParameters(const float firstStart[3],
                               const float firstEnd[3],
                               const float secondStart[3],
                               const float secondEnd[3],
                               float& firstAmount,
                               float& secondAmount)
{
    const float first[3] = { firstEnd[0] - firstStart[0], firstEnd[1] - firstStart[1], firstEnd[2] - firstStart[2] };
    const float second[3] = { secondEnd[0] - secondStart[0], secondEnd[1] - secondStart[1], secondEnd[2] - secondStart[2] };
    const float offset[3] = { firstStart[0] - secondStart[0], firstStart[1] - secondStart[1], firstStart[2] - secondStart[2] };
    const float firstLengthSquared = (first[0] * first[0]) + (first[1] * first[1]) + (first[2] * first[2]);
    const float secondLengthSquared = (second[0] * second[0]) + (second[1] * second[1]) + (second[2] * second[2]);
    const float cross = (first[0] * second[0]) + (first[1] * second[1]) + (first[2] * second[2]);
    const float firstOffset = (first[0] * offset[0]) + (first[1] * offset[1]) + (first[2] * offset[2]);
    const float secondOffset = (second[0] * offset[0]) + (second[1] * offset[1]) + (second[2] * offset[2]);
    const float denominator = (firstLengthSquared * secondLengthSquared) - (cross * cross);

    firstAmount = denominator > 0.001F ? Saturate((cross * secondOffset - firstOffset * secondLengthSquared) / denominator) : 0.0F;
    secondAmount = secondLengthSquared > 0.001F ? Saturate((cross * firstAmount + secondOffset) / secondLengthSquared) : 0.0F;
    firstAmount = firstLengthSquared > 0.001F ? Saturate((cross * secondAmount - firstOffset) / firstLengthSquared) : 0.0F;

    const float firstPoint[3] = { firstStart[0] + (first[0] * firstAmount), firstStart[1] + (first[1] * firstAmount),
                                  firstStart[2] + (first[2] * firstAmount) };
    const float secondPoint[3] = { secondStart[0] + (second[0] * secondAmount), secondStart[1] + (second[1] * secondAmount),
                                   secondStart[2] + (second[2] * secondAmount) };
    const float delta[3] = { firstPoint[0] - secondPoint[0], firstPoint[1] - secondPoint[1], firstPoint[2] - secondPoint[2] };
    return (delta[0] * delta[0]) + (delta[1] * delta[1]) + (delta[2] * delta[2]);
}

// Resolves the active weapon definition through weapon services
int ActiveWeaponDefinition(void* pawn)
{
    if (!pawn || !g_getSlot || g_weaponServicesOffset < 0 || g_activeWeaponOffset < 0 || g_itemDefinitionIndexOffset < 0) return -1;

    void* weaponServices = nullptr;
    if (!memory::Read(pawn, g_weaponServicesOffset, weaponServices, memory::FailureDomain::Weapon) || !weaponServices) return -1;

    uint32_t activeHandle = 0;
    if (!memory::Read(weaponServices, g_activeWeaponOffset, activeHandle, memory::FailureDomain::Weapon)) return -1;
    if (activeHandle == 0U || activeHandle == 0xFFFFFFFFU) return -1;

    const int activeIndex = static_cast<int>(activeHandle & 0x7FFFU);
    for (int slot = 0; slot <= 4; ++slot)
    {
        const unsigned int maxPosition = slot == 3 ? 8U : 1U;
        for (unsigned int position = 0; position < maxPosition; ++position)
        {
            const unsigned int positionArgument = slot == 3 ? position : 0xFFFFFFFFU;
            void* weapon = g_getSlot(weaponServices, slot, positionArgument);
            if (!weapon) continue;

            const CEntityHandle handle = static_cast<CEntityInstance*>(weapon)->GetRefEHandle();
            if (handle.GetEntryIndex() != activeIndex) continue;

            uint16_t definitionIndex = 0;
            if (!memory::Read(weapon, g_itemDefinitionIndexOffset, definitionIndex, memory::FailureDomain::Weapon)) return -1;
            return definitionIndex;
        }
    }
    return -1;
}

// Caches one weapon definition for all pellets in the same server time
int CachedActiveWeaponDefinition(void* shooter)
{
    if (!shooter) return -1;

    const float sampleTime = game_time::Now();
    const size_t index = static_cast<size_t>(MixPointerValue(reinterpret_cast<uintptr_t>(shooter))) & (kWeaponCacheSize - 1);
    WeaponDefinitionCacheEntry& entry = g_weaponCache[index];
    if (sampleTime > 0.0F && entry.shooter == shooter && entry.sampleTime == sampleTime) return entry.definitionIndex;

    const int definitionIndex = ActiveWeaponDefinition(shooter);
    entry.shooter = shooter;
    entry.sampleTime = sampleTime;
    entry.definitionIndex = definitionIndex;
    return definitionIndex;
}

// Validates and normalizes one captured pellet segment.
bool NormalizePellet(const float (&source)[3], const float (&end)[3], float (&direction)[3])
{
    for (int index = 0; index < 3; ++index)
    {
        if (!std::isfinite(source[index]) || !std::isfinite(end[index])) return false;
        direction[index] = end[index] - source[index];
    }
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (!std::isfinite(length) || length <= 1e-4F) return false;
    for (float& component : direction)
        component /= length;
    return true;
}

// Saves the output pointer before native code can overwrite its argument registers.
KHook::Return<int64_t> PelletResultPre(void* traceData, void* trace, float startFraction, void* hit) noexcept
{
    g_pelletResultFrames.push_back({ trace, g_pelletFrames.size() });
    return { KHook::Action::Ignore };
}

// Copies the first valid native segment while its owning pellet call is active.
KHook::Return<int64_t> PelletResultPost(void* traceData, void* trace, float startFraction, void* hit) noexcept
{
    const PelletResultFrame result = g_pelletResultFrames.back();
    g_pelletResultFrames.pop_back();
    if (result.pelletDepth == 0 || result.pelletDepth != g_pelletFrames.size()) return { KHook::Action::Ignore };

    PelletFrame& frame = g_pelletFrames.back();
    if (!frame.captureRequested || frame.traceReady) return { KHook::Action::Ignore };
    frame.traceReady = memory::Read(result.trace, kNativeTraceStartOffset, frame.source, memory::FailureDomain::Bullet) &&
                       memory::Read(result.trace, kNativeTraceEndOffset, frame.end, memory::FailureDomain::Bullet) &&
                       NormalizePellet(frame.source, frame.end, frame.direction);
    return { KHook::Action::Ignore };
}

// Starts a pellet scope without requiring the optional native result vector.
KHook::Return<int64_t> PelletTracePre(int64_t a1,
#ifdef _WIN32
                                      void* a2,
#else
                                      double sourceXY,
                                      float sourceZ,
#endif
                                      int64_t a3,
                                      float a4,
                                      float a5,
                                      int a6,
                                      unsigned char a7,
                                      int a8,
                                      int a9,
                                      float a10,
                                      int64_t a11,
                                      void* a12,
                                      float a13,
                                      float a14,
                                      int64_t a15,
                                      int64_t a16,
                                      int a17,
                                      int a18,
                                      void* a19,
                                      int64_t a20) noexcept
{
    g_bulletCount.fetch_add(1, std::memory_order_relaxed);
    PelletFrame frame{ a1 };
    if (GetHolesEnabled())
    {
        if (!smoke_vision::IsVolumeMode()) g_skippedModeCount.fetch_add(1, std::memory_order_relaxed);
        else if (!smoke_vision::HasSmokeProjectiles())
            g_skippedNoSmokeCount.fetch_add(1, std::memory_order_relaxed);
        else
            frame.captureRequested = true;
    }
    if (frame.captureRequested)
    {
        memory::Read(reinterpret_cast<const void*>(a3), 0, frame.shotAngles, memory::FailureDomain::Bullet);
    }
    g_pelletFrames.push_back(frame);
    return { KHook::Action::Ignore };
}

// Publishes the native segment captured during this pellet call, without retracing.
KHook::Return<int64_t> HookedPelletTrace(int64_t a1,
#ifdef _WIN32
                                         void* a2,
#else
                                         double sourceXY,
                                         float sourceZ,
#endif
                                         int64_t a3,
                                         float a4,
                                         float a5,
                                         int a6,
                                         unsigned char a7,
                                         int a8,
                                         int a9,
                                         float a10,
                                         int64_t a11,
                                         void* a12,
                                         float a13,
                                         float a14,
                                         int64_t a15,
                                         int64_t a16,
                                         int a17,
                                         int a18,
                                         void* a19,
                                         int64_t a20) noexcept
{
    const PelletFrame frame = g_pelletFrames.back();
    g_pelletFrames.pop_back();
    if (!frame.captureRequested) return { KHook::Action::Ignore };
    if (!frame.traceReady)
    {
        g_missingResultCount.fetch_add(1, std::memory_order_relaxed);
        return { KHook::Action::Ignore };
    }
    g_nativeResultCount.fetch_add(1, std::memory_order_relaxed);

    void* shooter = nullptr;
    if (frame.shooter)
    {
        const void* shooterAddress = reinterpret_cast<const void*>(frame.shooter); // NOLINT(performance-no-int-to-ptr)
        if (!memory::Read(shooterAddress, 56, shooter, memory::FailureDomain::Bullet)) shooter = nullptr;
    }

    if (smoke_vision::DensityFunctionReady() && smoke_vision::DensityInLine(frame.source, frame.end) <= 0.0F)
        return { KHook::Action::Ignore };

    const int definitionIndex = CachedActiveWeaponDefinition(shooter);
    const bool shotgun = IsShotgunDefinition(definitionIndex);
    const float radius = shotgun ? GetShotgunRadius() : GetRadius();
    OnHole(frame.source, frame.end, radius);
    g_lastWeaponDefinition.store(definitionIndex, std::memory_order_relaxed);
    g_lastWeaponShotgun.store(shotgun ? 1 : 0, std::memory_order_relaxed);
    {
        std::scoped_lock lock(g_lastBulletMutex);
        for (int index = 0; index < 3; ++index)
        {
            g_lastBulletSource[index] = frame.source[index];
            g_lastBulletEnd[index] = frame.end[index];
            g_lastBulletAngles[index] = frame.shotAngles[index];
            g_lastBulletDirection[index] = frame.direction[index];
        }
    }

    return { KHook::Action::Ignore };
}

} // namespace

// Resolves offsets and installs optional bullet capture facilities
bool Install(const nlohmann::json& gamedata, const modules::ModuleInfo& serverModule)
{
    // Checks SDK member offsets on a real object without non-standard offsetof.
    const CGameTrace layoutProbe;
    const auto traceAddress = reinterpret_cast<uintptr_t>(&layoutProbe);
    if (reinterpret_cast<uintptr_t>(&layoutProbe.m_vStartPos) - traceAddress != kNativeTraceStartOffset ||
        reinterpret_cast<uintptr_t>(&layoutProbe.m_vEndPos) - traceAddress != kNativeTraceEndOffset)
    {
        BV_LOG_ERROR("CGameTrace layout changed; bullet capture and HE traces disabled");
        return false;
    }

    char traceError[256] = { 0 };
    g_navPhysicsVtable = modules::ResolveVirtualTable(serverModule, "CNavPhysicsInterface", traceError, sizeof(traceError));
    const int traceShapeOffset = gameconfig::ResolveOffset(gamedata, kTraceShapeName, -1);
    if (g_navPhysicsVtable && traceShapeOffset >= 0 && traceShapeOffset < 64 &&
        modules::IsExecutableAddress(g_navPhysicsVtable[traceShapeOffset]))
    {
        g_traceShape = reinterpret_cast<TraceShapeFn>(g_navPhysicsVtable[traceShapeOffset]);
    }
    else
    {
        g_traceShape = nullptr;
        g_navPhysicsVtable = nullptr;
        const char* reason = traceShapeOffset < 0 ? "gamedata offset unavailable" : traceError;
        if (reason[0] == '\0') reason = "vtable slot is not executable";
        BV_LOG_WARN("Native trace unavailable (%s); HE smoke holes disabled", reason);
    }

    g_weaponServicesOffset = schema::GetFieldOffset("CBasePlayerPawn", "m_pWeaponServices");
    g_activeWeaponOffset = schema::GetFieldOffset("CPlayer_WeaponServices", "m_hActiveWeapon");
    const int attributeManagerOffset = schema::GetFieldOffset("CBasePlayerWeapon", "m_AttributeManager");
    const int itemOffset = schema::GetFieldOffset("CAttributeContainer", "m_Item");
    const int definitionIndexOffset = schema::GetFieldOffset("CEconItemView", "m_iItemDefinitionIndex");
    g_itemDefinitionIndexOffset = attributeManagerOffset >= 0 && itemOffset >= 0 && definitionIndexOffset >= 0
                                      ? attributeManagerOffset + itemOffset + definitionIndexOffset
                                      : -1;

    char pelletError[256] = { 0 };
    void* pelletTarget = gameconfig::ResolveSig(gamedata, serverModule, kPelletTraceName, pelletError, sizeof(pelletError));
    char resultError[256] = { 0 };
    void* resultTarget = gameconfig::ResolveSig(gamedata, serverModule, kPelletResultName, resultError, sizeof(resultError));
    bool installed = false;
    if (!resultTarget || !g_pelletResultHook.Install(resultTarget, &PelletResultPre, &PelletResultPost))
    {
        BV_LOG_WARN("Pellet-result hook failed (%s); bullet holes disabled", resultTarget ? "KHook error" : resultError);
    }
    else if (pelletTarget && g_pelletTraceHook.Install(pelletTarget, &PelletTracePre, &HookedPelletTrace))
    {
        installed = true;
    }
    else
    {
        g_pelletTraceHook.Remove();
        g_pelletResultHook.Remove();
        BV_LOG_WARN("Pellet-trace hook failed (%s); bullet holes disabled", pelletTarget ? "KHook error" : pelletError);
    }

    char getSlotError[256] = { 0 };
    const bool weaponOffsetsReady = g_weaponServicesOffset >= 0 && g_activeWeaponOffset >= 0 && g_itemDefinitionIndexOffset >= 0;
    void* getSlotTarget =
        weaponOffsetsReady ? gameconfig::ResolveSig(gamedata, serverModule, kGetSlotName, getSlotError, sizeof(getSlotError)) : nullptr;
    if (getSlotTarget)
    {
        g_getSlot = reinterpret_cast<GetSlotFn>(getSlotTarget);
    }
    else
    {
        const char* reason = weaponOffsetsReady ? getSlotError : "weapon schema offset unavailable";
        BV_LOG_WARN("%s; shotgun radius disabled (all bullets use normal radius)", reason);
    }
    return installed;
}

// Stops pellet scopes before removing their result capture hook.
void Remove()
{
    g_pelletTraceHook.Remove();
    g_pelletResultHook.Remove();
    g_traceShape = nullptr;
    g_navPhysicsVtable = nullptr;
    g_getSlot = nullptr;
    std::scoped_lock lock(g_holeMutex);
    g_holes.clear();
}

// Tests a line with the HE-to-smoke collision mask
bool IsLineUnobstructed(const float* from, const float* to)
{
    if (!from || !to || !g_traceShape) return false;

    Ray_t ray;
    Vector start(from[0], from[1], from[2]);
    Vector end(to[0], to[1], to[2]);
    CTraceFilter filter(8193, COLLISION_GROUP_DEFAULT, true);
    CGameTrace trace;
    g_heTraceAttempts.fetch_add(1, std::memory_order_relaxed);
    g_traceShape(nullptr, ray, start, end, &filter, &trace);
    if (trace.DidHit()) g_heTraceHits.fetch_add(1, std::memory_order_relaxed);
    return !trace.DidHit() || trace.m_flFraction >= 0.999F;
}

// Adds or replaces one temporary bullet tunnel
void OnHole(const float start[3], const float end[3], float radius)
{
    const float time = game_time::Now();
    std::scoped_lock lock(g_holeMutex);
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

// Applies active bullet records to native density near each tunnel
float AdjustDensity(const float* from, const float* to, float density, DensitySamplerFn sampler)
{
    const float duration = GetDuration();
    if (!from || !to || !sampler || density <= 0.0F || duration <= 0.0F) return density;

    const float line[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    const float lineLengthSquared = (line[0] * line[0]) + (line[1] * line[1]) + (line[2] * line[2]);
    if (lineLengthSquared <= 0.001F) return density;
    const float lineLength = std::sqrt(lineLengthSquared);

    const float now = game_time::Now();
    std::scoped_lock lock(g_holeMutex);
    std::vector<BulletInfluence> influences;
    influences.reserve(g_holes.size());
    size_t writeIndex = 0;
    for (size_t index = 0; index < g_holes.size(); ++index)
    {
        const BulletHole holeRecord = g_holes[index];
        const float age = now - holeRecord.startTime;
        if (age < 0.0F || age >= duration) continue;

        g_holes[writeIndex++] = holeRecord;
        const float radius = holeRecord.radius;
        if (radius <= 0.0F) continue;

        BulletHole shaderHole = holeRecord;
        const float travelAmount = std::min(age * 10.0F, 1.0F);
        for (int axis = 0; axis < 3; ++axis)
        {
            shaderHole.end[axis] = holeRecord.start[axis] + ((holeRecord.end[axis] - holeRecord.start[axis]) * travelAmount);
        }

        float lineAmount = 0.0F;
        float holeAmount = 0.0F;
        const float distanceSquared = ClosestSegmentParameters(from, to, shaderHole.start, shaderHole.end, lineAmount, holeAmount);
        if (distanceSquared >= radius * radius) continue;

        const float hole[3] = { shaderHole.end[0] - shaderHole.start[0], shaderHole.end[1] - shaderHole.start[1],
                                shaderHole.end[2] - shaderHole.start[2] };
        const float holeLengthSquared = (hole[0] * hole[0]) + (hole[1] * hole[1]) + (hole[2] * hole[2]);
        const float directionDot =
            holeLengthSquared > 0.001F
                ? std::clamp((line[0] * hole[0] + line[1] * hole[1] + line[2] * hole[2]) / (lineLength * std::sqrt(holeLengthSquared)),
                             -1.0F, 1.0F)
                : 0.0F;

        float begin = 0.0F;
        float end = 0.0F;
        if (std::abs(directionDot) > 0.95F)
        {
            const float startOffset[3] = { shaderHole.start[0] - from[0], shaderHole.start[1] - from[1], shaderHole.start[2] - from[2] };
            const float endOffset[3] = { shaderHole.end[0] - from[0], shaderHole.end[1] - from[1], shaderHole.end[2] - from[2] };
            const float startAmount = (startOffset[0] * line[0] + startOffset[1] * line[1] + startOffset[2] * line[2]) / lineLengthSquared;
            const float endAmount = (endOffset[0] * line[0] + endOffset[1] * line[1] + endOffset[2] * line[2]) / lineLengthSquared;
            const float padding = std::sqrt((radius * radius) - distanceSquared) / lineLength;
            begin = Saturate(std::min(startAmount, endAmount) - padding);
            end = Saturate(std::max(startAmount, endAmount) + padding);
        }
        else
        {
            const float sine = std::sqrt(std::max(1.0F - (directionDot * directionDot), 0.0F));
            const float halfAmount = std::sqrt((radius * radius) - distanceSquared) / (lineLength * sine);
            begin = Saturate(lineAmount - halfAmount);
            end = Saturate(lineAmount + halfAmount);
        }

        if (end > begin) influences.push_back({ .hole = shaderHole, .age = age, .begin = begin, .end = end });
    }
    g_holes.resize(writeIndex);
    if (influences.empty()) return density;

    std::ranges::sort(influences, [](const BulletInfluence& left, const BulletInfluence& right) {
        return left.begin < right.begin;
    });

    std::vector<std::pair<float, float>> intervals;
    intervals.reserve(influences.size());
    for (const BulletInfluence& influence : influences)
    {
        if (intervals.empty() || influence.begin > intervals.back().second + 0.0001F)
        {
            intervals.emplace_back(influence.begin, influence.end);
        }
        else
        {
            intervals.back().second = std::max(intervals.back().second, influence.end);
        }
    }

    float sampledDensity = 0.0F;
    float weightedDensity = 0.0F;
    for (const std::pair<float, float>& interval : intervals)
    {
        for (int slice = 0; slice < kDensitySlices; ++slice)
        {
            const float sliceBeginAmount =
                interval.first + ((interval.second - interval.first) * (static_cast<float>(slice) / kDensitySlices));
            const float sliceEndAmount =
                interval.first + ((interval.second - interval.first) * (static_cast<float>(slice + 1) / kDensitySlices));
            const float midpointAmount = (sliceBeginAmount + sliceEndAmount) * 0.5F;
            float sliceBegin[3] = { from[0] + (line[0] * sliceBeginAmount), from[1] + (line[1] * sliceBeginAmount),
                                    from[2] + (line[2] * sliceBeginAmount) };
            float sliceEnd[3] = { from[0] + (line[0] * sliceEndAmount), from[1] + (line[1] * sliceEndAmount),
                                  from[2] + (line[2] * sliceEndAmount) };
            const float midpoint[3] = { from[0] + (line[0] * midpointAmount), from[1] + (line[1] * midpointAmount),
                                        from[2] + (line[2] * midpointAmount) };

            float maximumStrength = 0.0F;
            for (const BulletInfluence& influence : influences)
            {
                if (midpointAmount < influence.begin || midpointAmount > influence.end) continue;

                const float distance = std::sqrt(DistanceSquaredToSegment(midpoint, influence.hole.start, influence.hole.end));
                const float endpointDelta[3] = { midpoint[0] - influence.hole.end[0], midpoint[1] - influence.hole.end[1],
                                                 midpoint[2] - influence.hole.end[2] };
                const float endpointDistance = std::sqrt((endpointDelta[0] * endpointDelta[0]) + (endpointDelta[1] * endpointDelta[1]) +
                                                         (endpointDelta[2] * endpointDelta[2]));
                const float normalizedDistance = distance / influence.hole.radius;
                const float endpointFade = std::min(endpointDistance * 0.01F, 1.0F);
                const float strength = SmoothStep(1.0F - Saturate(normalizedDistance - endpointFade + 1.0F + (influence.age / duration)));
                maximumStrength = std::max(maximumStrength, strength);
            }
            if (maximumStrength <= 0.001F) continue;

            const float sliceDensity = std::max(sampler(sliceBegin, sliceEnd), 0.0F);
            const float deformationStrength = maximumStrength * maximumStrength * maximumStrength;
            sampledDensity += sliceDensity;
            weightedDensity += sliceDensity * (1.0F - deformationStrength);
        }
    }

    if (sampledDensity <= 0.0001F) return density;

    const float averageScale = Saturate(weightedDensity / sampledDensity);
    const float affectedDensity = std::min(sampledDensity, density);
    return std::max(density - (affectedDensity * (1.0F - averageScale)), 0.0F);
}

// Stores the normal tunnel radius
void SetRadius(float value) { g_radiusMilli.store(static_cast<int>(value * 1000.0F), std::memory_order_relaxed); }

// Returns the normal tunnel radius
float GetRadius() { return g_radiusMilli.load(std::memory_order_relaxed) * 0.001F; }

// Stores the shotgun tunnel radius
void SetShotgunRadius(float value) { g_shotgunRadiusMilli.store(static_cast<int>(value * 1000.0F), std::memory_order_relaxed); }

// Returns the shotgun tunnel radius
float GetShotgunRadius() { return g_shotgunRadiusMilli.load(std::memory_order_relaxed) * 0.001F; }

// Stores the tunnel lifetime
void SetDuration(float value) { g_durationMilli.store(static_cast<int>(value * 1000.0F), std::memory_order_relaxed); }

// Returns the tunnel lifetime
float GetDuration() { return g_durationMilli.load(std::memory_order_relaxed) * 0.001F; }

// Stores the bullet tunnel enabled state
void SetHolesEnabled(bool enabled) { g_holesEnabled.store(enabled ? 1 : 0, std::memory_order_relaxed); }

// Returns the bullet tunnel enabled state
bool GetHolesEnabled() { return g_holesEnabled.load(std::memory_order_relaxed) != 0; }

// Returns the retained tunnel count
int GetActiveHoleCount()
{
    std::scoped_lock lock(g_holeMutex);
    return static_cast<int>(g_holes.size());
}

// Returns the pellet hook call count
int64_t GetBulletCount() { return g_bulletCount.load(std::memory_order_relaxed); }

// Formats the last captured pellet
const char* GetLastBulletInfo()
{
    static char buffer[224];
    std::scoped_lock lock(g_lastBulletMutex);
    std::snprintf(buffer, sizeof(buffer), "src=(%.1f,%.1f,%.1f) end=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f) fwd=(%.2f,%.2f,%.2f)",
                  g_lastBulletSource[0], g_lastBulletSource[1], g_lastBulletSource[2],
                  g_lastBulletEnd[0], g_lastBulletEnd[1], g_lastBulletEnd[2],
                  g_lastBulletAngles[0], g_lastBulletAngles[1], g_lastBulletAngles[2],
                  g_lastBulletDirection[0], g_lastBulletDirection[1], g_lastBulletDirection[2]);
    return buffer;
}

// Formats native trace and bullet capture diagnostics
const char* GetDiagnostics()
{
    static char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "enabled=%d hook=%s captured=%" PRId64 " missing=%" PRId64,
                  g_holesEnabled.load(std::memory_order_relaxed),
                  g_pelletTraceHook.Active() && g_pelletResultHook.Active() ? "ON" : "OFF",
                  g_nativeResultCount.load(std::memory_order_relaxed), g_missingResultCount.load(std::memory_order_relaxed));
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
} // namespace cs2bv::bullet_vision
