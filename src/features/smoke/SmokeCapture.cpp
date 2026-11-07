#include "SmokeCapture.h"
#include "Detour.h"
#include "SmokeSources.h"
#include "utils/memory.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <entity2/entityinstance.h>
#include <gametrace.h>
#include <ray.h>
#include <vector>
#include <utility>

namespace cs2bv::smoke_capture {
namespace {
using smoke_density::Detour;
using smoke_effects::HoleCache;
using smoke_effects::IsFinite;
using smoke_effects::SmokeKey;
using smoke_effects::Vec3;
using HeDetonate = std::int64_t (*)(void*);
using PublishEffect = void (*)(void*, void*, const Vec3*);
using InitializeSmoke = void (*)(void*, const Vec3*, float);
using ReadySmoke = int64_t (*)(void*);
using PelletTrace = std::int64_t (*)(std::int64_t,
#ifdef _WIN32
                                     void*,
#else
                                     double,
                                     float,
#endif
                                     std::int64_t,
                                     float,
                                     float,
                                     int,
                                     unsigned char,
                                     int,
                                     int,
                                     float,
                                     std::int64_t,
                                     int*,
                                     float,
                                     float,
                                     std::int64_t,
                                     std::int64_t,
                                     int,
                                     int,
                                     void*,
                                     std::int64_t);
using PelletResult = std::int64_t (*)(void*, void*, float, void*);
#ifdef _WIN32
// The HE factory passes destructor flags 0 or 1.
using GrenadeDestructor = void* (*)(void*, unsigned int);
#else
// HE destruction and factory deletion call base D1.
using GrenadeDestructor = void (*)(void*);
#endif

const smoke_bindings::Bindings* g_bindings = nullptr;
Runtime g_runtime{};
HoleCache g_holes;
std::atomic<bool> g_bulletsEnabled{ true };
Detour<HeDetonate> g_heDetour;
Detour<GrenadeDestructor> g_destructorDetour;
Detour<PublishEffect> g_publishDetour;
Detour<PelletTrace> g_pelletDetour;
Detour<PelletResult> g_resultDetour;
Detour<InitializeSmoke> g_initializeDetour;
Detour<ReadySmoke> g_readyDetour;
struct InitialSources
{
    void* entity;
    SmokeKey key;
    Vec3 origin;
    std::vector<Vec3> points;
};
std::vector<InitialSources> g_initialSources;
struct HeDetonation
{
    inline static thread_local HeDetonation* current = nullptr;
    HeDetonation* previous = current;
    void* projectile;
    explicit HeDetonation(void* self) : projectile(self) { current = this; }
    ~HeDetonation() { current = previous; }
    HeDetonation(const HeDetonation&) = delete;
    HeDetonation& operator=(const HeDetonation&) = delete;

    static void OnDestroyed(void* self)
    {
        // Nested detonations can destroy an outer projectile.
        for (auto* frame = current; frame; frame = frame->previous)
            if (frame->projectile == self) frame->projectile = nullptr;
        g_holes.RemoveHe(self);
    }
};
thread_local void* g_readySmoke = nullptr;
struct PelletFrame
{
    float time, scale;
    Vec3 lastStart{}, lastEnd{};
    bool hasLast = false;
};
thread_local PelletFrame* g_pellet = nullptr;

using memory::IsReadable;
template <typename T> bool ReadAt(const void* base, size_t offset, T& value)
{
    return memory::Read(base, offset, value, memory::FailureDomain::Smoke);
}
bool HasActiveSmoke() { return g_bindings->smokeListHead && *g_bindings->smokeListHead; }
bool SmokeIdentity(void* entity, SmokeKey& key)
{
    if (!ReadAt(entity, g_bindings->fields.smokeTick, key.effectTick)) return false;
    // Handle serials distinguish reused slots.
    CEntityIdentity* identity = nullptr;
    if (!ReadAt(&static_cast<CEntityInstance*>(entity)->m_pEntity, 0, identity) || !IsReadable(identity, sizeof(CEntityIdentity)))
        return false;
    key.handle = identity->GetRefEHandle().ToInt();
    return key.handle != 0xffffffffu;
}
void CaptureInitialSources(void* volume)
{
    using smoke_effects::SmokeSource;
    // Ready identifies the owning projectile.
    if (!g_readySmoke || static_cast<char*>(g_readySmoke) + g_bindings->config.volumeOffset != volume || g_runtime.faulted()) return;
    if (!g_runtime.readClock().server) return;
    InitialSources snapshot{};
    snapshot.entity = g_readySmoke;
    int count = 0, capacity = 0;
    const SmokeSource* data = nullptr;
    if (!SmokeIdentity(snapshot.entity, snapshot.key) || snapshot.key.effectTick < 0 ||
        !ReadAt(volume, g_bindings->config.volumeOriginOffset, snapshot.origin) || !IsFinite(snapshot.origin) ||
        !ReadAt(volume, g_bindings->config.sourceCountOffset, count) || count < 0 ||
        !ReadAt(volume, g_bindings->config.sourceCapacityOffset, capacity) || capacity < count ||
        !ReadAt(volume, g_bindings->config.sourceDataOffset, data) ||
        (count && !IsReadable(data, static_cast<std::size_t>(count) * sizeof(*data))))
    {
        g_runtime.fault("invalid initial smoke sources");
        return;
    }
    // Copy centers before Ready sets DidSmoke.
    snapshot.points.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        SmokeSource source;
        std::memcpy(&source, data + i, sizeof(source));
        Vec3 center;
        if (!smoke_effects::SourceCenter(snapshot.origin, source, center))
        {
            g_runtime.fault("invalid initial smoke source coordinate");
            return;
        }
        snapshot.points.push_back(center);
    }
    // Retire missing or reused projectiles.
    for (auto it = g_initialSources.begin(); it != g_initialSources.end();)
    {
        bool present = false;
        if (it->entity != snapshot.entity)
            for (void* smoke = *g_bindings->smokeListHead; smoke;)
            {
                if (smoke == it->entity)
                {
                    present = true;
                    break;
                }
                if (!ReadAt(smoke, g_bindings->config.nextOffset, smoke))
                {
                    g_runtime.fault("invalid smoke list during source retirement");
                    return;
                }
            }
        if (present) ++it;
        else
            it = g_initialSources.erase(it);
    }
    g_initialSources.push_back(std::move(snapshot));
}
KHook::Return<void> HookedInitializeSmoke(void* volume, const Vec3* origin, float startTime)
{
    const auto result = g_initializeDetour.Continue(volume, origin, startTime);
    CaptureInitialSources(volume);
    return result;
}
KHook::Return<int64_t> HookedReady(void* self)
{
    if (!g_runtime.mainThread() || g_runtime.faulted()) return g_readyDetour.Continue(self);
    void* previous = g_readySmoke;
    g_readySmoke = self;
    const auto result = g_readyDetour.Continue(self);
    g_readySmoke = previous;
    return result;
}
bool SourceReachable(Vec3 blast, Vec3 source)
{
    Ray_t ray;
    Vector start(blast.x, blast.y, blast.z), end(source.x, source.y, source.z);
    CTraceFilter filter(0x2001, 4, true);
    CGameTrace trace;
    g_bindings->traceShape(nullptr, ray, start, end, &filter, &trace);
    // Entity ID 0x8000 blocks client traces.
    std::uint32_t entityId = 0;
    std::memcpy(&entityId, reinterpret_cast<const char*>(&trace) + 0x68, sizeof(entityId));
    return !(trace.DidHit() && entityId == 0x8000);
}
void CaptureHe(void* self)
{
    Vec3 blast;
    int tick = 0;
    if (!ReadAt(self, g_bindings->fields.effectTick, tick))
    {
        g_runtime.fault("unreadable HE effect tick");
        return;
    }
    if (tick < 1) return;
    if (!ReadAt(self, g_bindings->fields.effectOrigin, blast) || !IsFinite(blast))
    {
        g_runtime.fault("invalid published HE effect");
        return;
    }
    // Windows HE timestamps use 1/64-second ticks.
    const float eventTime = static_cast<float>(tick) * (1.0f / 64.0f);
    std::vector<SmokeKey> affected;
    for (void* smoke = *g_bindings->smokeListHead; smoke;)
    {
        void* next = nullptr;
        bool active = false;
        SmokeKey key;
        if (!ReadAt(smoke, g_bindings->config.nextOffset, next) || !ReadAt(smoke, g_bindings->fields.didSmoke, active))
        {
            g_runtime.fault("invalid smoke list");
            return;
        }
        if (active)
        {
            if (!SmokeIdentity(smoke, key))
            {
                g_runtime.fault("invalid smoke identity");
                return;
            }
            const auto sources = std::find_if(g_initialSources.begin(), g_initialSources.end(), [&](const auto& entry) {
                return entry.entity == smoke && entry.key == key;
            });
            // Ready records sources. Sampling checks smoke age.
            if (sources != g_initialSources.end() &&
                smoke_effects::HeAffectsSources(blast, sources->origin, sources->points, SourceReachable))
                affected.push_back(key);
        }
        smoke = next;
    }
    g_holes.AddHe(blast, eventTime, std::move(affected), self);
}
#ifdef _WIN32
KHook::Return<void*> HookedGrenadeDestructor(void* self, unsigned int)
#else
KHook::Return<void> HookedGrenadeDestructor(void* self)
#endif
{
    if (g_runtime.mainThread()) HeDetonation::OnDestroyed(self);
    else
        g_runtime.fault("grenade destroyed off server thread");
    return { KHook::Action::Ignore };
}

KHook::Return<std::int64_t> HookedHeDetonate(void* self)
{
    if (!g_runtime.mainThread() || g_runtime.faulted()) return g_heDetour.Continue(self);
    HeDetonation frame(self);
    g_runtime.readClock().time;
    return g_heDetour.Continue(self);
}
KHook::Return<void> HookedPublish(void* self, void* effect, const Vec3* position)
{
    const auto result = g_publishDetour.Continue(self, effect, position);
    // Publish finishes before projectile release.
    if (g_runtime.mainThread() && !g_runtime.faulted() && HeDetonation::current && self == HeDetonation::current->projectile &&
        HasActiveSmoke())
        CaptureHe(self);
    return result;
}
float WeaponScale(void* pawn)
{
    void* services = nullptr;
    std::uint32_t active = 0xffffffffu;
    if (!ReadAt(pawn, g_bindings->fields.weapons, services) || !services || !ReadAt(services, g_bindings->fields.activeWeapon, active) ||
        active == 0xffffffffu)
        return 0;
    // Pellets use the active primary or secondary weapon.
    for (int slot = 0; slot < 2; ++slot)
    {
        auto* weapon = static_cast<CEntityInstance*>(g_bindings->getSlot(services, slot, 0xffffffffu));
        if (!weapon || !weapon->m_pEntity || weapon->GetRefEHandle().ToInt() != active) continue;
        std::uint16_t definition = 0;
        if (!ReadAt(weapon, g_bindings->fields.attributes + g_bindings->fields.item + g_bindings->fields.definition, definition)) return 0;
        return definition == 25 || definition == 27 || definition == 29 || definition == 35 ? .25f : 1.0f;
    }
    return 0;
}
bool IntersectsActiveSmoke(Vec3 start, Vec3 end)
{
    for (void* smoke = *g_bindings->smokeListHead; smoke;)
    {
        void* next = nullptr;
        bool active = false;
        if (!ReadAt(smoke, g_bindings->config.nextOffset, next) || !ReadAt(smoke, g_bindings->fields.didSmoke, active))
        {
            g_runtime.fault("invalid smoke list during bullet capture");
            return false;
        }
        if (active)
        {
            Vec3 origin;
            if (!ReadAt(smoke, g_bindings->config.volumeOffset + g_bindings->config.volumeOriginOffset, origin) || !IsFinite(origin))
            {
                g_runtime.fault("invalid smoke grid origin");
                return false;
            }
            if (HoleCache::IntersectsVolume(start, end, origin)) return true;
        }
        smoke = next;
    }
    return false;
}
KHook::Return<std::int64_t> HookedResult(void* data, void* trace, float startFraction, void* hit)
{
    const auto result = g_resultDetour.Continue(data, trace, startFraction, hit);
    if (!g_pellet || g_runtime.faulted()) return result;
    float endFraction = 0;
    Vec3 start, end;
    if (!ReadAt(hit, 0x0c, endFraction) || !ReadAt(trace, 0x78, start) || !ReadAt(trace, 0x84, end) || !std::isfinite(startFraction) ||
        !std::isfinite(endFraction) || !IsFinite(start) || !IsFinite(end))
    {
        g_runtime.fault("invalid pellet result");
        return result;
    }
    // Keep forward penetration intervals.
    if (!(endFraction > startFraction)) return result;
    if (g_pellet->hasLast && std::memcmp(&start, &g_pellet->lastStart, sizeof(start)) == 0 &&
        std::memcmp(&end, &g_pellet->lastEnd, sizeof(end)) == 0)
        return result;
    g_pellet->lastStart = start;
    g_pellet->lastEnd = end;
    g_pellet->hasLast = true;
    if (!IntersectsActiveSmoke(start, end)) return result;
    g_holes.AddBullet(start, end, g_pellet->time, g_pellet->scale);
    return result;
}
KHook::Return<std::int64_t> HookedPelletTrace(std::int64_t a1,
#ifdef _WIN32
                                              void* a2,
#else
                                              double sourceXY,
                                              float sourceZ,
#endif
                                              std::int64_t a3,
                                              float a4,
                                              float a5,
                                              int a6,
                                              unsigned char a7,
                                              int a8,
                                              int a9,
                                              float a10,
                                              std::int64_t a11,
                                              int* a12,
                                              float a13,
                                              float a14,
                                              std::int64_t a15,
                                              std::int64_t a16,
                                              int a17,
                                              int a18,
                                              void* a19,
                                              std::int64_t a20)
{
    PelletFrame* previous = g_pellet;
    PelletFrame frame{};
    g_pellet = nullptr;
    if (g_runtime.mainThread() && !g_runtime.faulted() && g_bulletsEnabled && HasActiveSmoke())
    {
        frame.time = g_runtime.readClock().time;
        void* pawn = nullptr;
        if (!ReadAt(reinterpret_cast<void*>(a1), g_bindings->config.pelletShooterOffset, pawn))
            g_runtime.fault("invalid pellet shooter context");
        else
            frame.scale = WeaponScale(pawn);
        if (std::isfinite(frame.time) && frame.scale > 0) g_pellet = &frame;
    }
    const auto result = g_pelletDetour.Continue(a1,
#ifdef _WIN32
                                                a2,
#else
                                                sourceXY, sourceZ,
#endif
                                                a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
    g_pellet = previous;
    return result;
}

} // namespace

void Initialize(const smoke_bindings::Bindings& bindings, Runtime runtime)
{
    g_bindings = &bindings;
    g_runtime = runtime;
    ResetEffects(true);
}
bool CreateEventHooks()
{
    return g_destructorDetour.Create(g_bindings->destructor, &HookedGrenadeDestructor) &&
           g_heDetour.Create(g_bindings->heDetonate, &HookedHeDetonate) &&
           g_pelletDetour.Create(g_bindings->pelletTrace, &HookedPelletTrace) &&
           g_publishDetour.Create(g_bindings->publish, &HookedPublish) && g_resultDetour.Create(g_bindings->pelletResult, &HookedResult);
}
bool CreateSourceHooks()
{
    return g_initializeDetour.Create(g_bindings->initialize, &HookedInitializeSmoke) &&
           g_readyDetour.Create(g_bindings->ready, &HookedReady);
}
bool EnableObservers()
{
    return g_destructorDetour.Enable() && g_initializeDetour.Enable() && g_readyDetour.Enable() && g_publishDetour.Enable() &&
           g_resultDetour.Enable();
}
bool EnableEmitters() { return g_heDetour.Enable() && g_pelletDetour.Enable(); }
void RemoveEventHooks()
{
    g_pelletDetour.Reset();
    g_heDetour.Reset();
    g_destructorDetour.Reset();
    g_resultDetour.Reset();
    g_publishDetour.Reset();
}
void RemoveSourceHooks()
{
    g_readyDetour.Reset();
    g_initializeDetour.Reset();
}
void Clear()
{
    g_pellet = nullptr;
    g_readySmoke = nullptr;
    ResetEffects(true);
    g_bindings = nullptr;
    g_runtime = {};
}
void ResetEffects(bool clearSources)
{
    g_holes.Reset();
    if (clearSources) g_initialSources.clear();
}
const HoleCache& Effects() { return g_holes; }
void EnableBullets(bool enabled) { g_bulletsEnabled = enabled; }
bool BulletsEnabled() { return g_bulletsEnabled.load(); }
} // namespace cs2bv::smoke_capture
