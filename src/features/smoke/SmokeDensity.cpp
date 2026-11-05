// SPDX-License-Identifier: AGPL-3.0-only
// Server density with shader-derived HE/bullet effects. XBribo/CS2-Bot-Vision attribution retained.
#include "SmokeDensity.h"
#include "Detour.h"
#include "SmokeTraversal.h"
#include "SmokeGeometry.h"
#include "SmokeSources.h"
#include "core/log.h"
#include "core/game_time.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/cs2_sdk/src/utils/rtti.h"
#include "utils/memory.h"
#include "plugin.h"
#include <thread>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <entity2/entityinstance.h>
#include <engine2/isource2server.h>
#include <gametrace.h>
#include <limits>
#include <optional>
#include <ray.h>
#include <schemasystem/schemasystem.h>
#include <vector>
#include <tier0/dbg.h>
#include <utility>

namespace cs2bv::smoke_density {
namespace {
using smoke_effects::BorrowedTraversal;
using smoke_effects::CellContext;
using smoke_effects::HoleCache;
using smoke_effects::IsFinite;
using smoke_effects::SmokeKey;
using smoke_effects::Traversal;
using smoke_effects::Vec3;
using GetSmokeDensityInLine = float (*)(const float*, const float*, float*);
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
using PointDensity = float (*)(void*, const Vec3*);
using TraceShape = void (*)(const void*, const Ray_t&, const Vector&, const Vector&, CTraceFilter*, CGameTrace*);
#ifdef _WIN32
// The HE factory invokes the deleting destructor with flags (0 or 1).
using GrenadeDestructor = void* (*)(void*, unsigned int);
constexpr const char* kDestructorClass = "CHEGrenadeProjectile";
#else
// Both the HE destructor and the inlined factory deletion call the base D1.
using GrenadeDestructor = void (*)(void*);
constexpr const char* kDestructorClass = "CBaseCSGrenadeProjectile";
#endif
using GetSlot = void* (*)(void*, int, unsigned int);

struct Configuration
{
    std::size_t volumeOffset = 0, nextOffset = 0, volumeTimeOffset = 0, volumeOriginOffset = 0;
    std::size_t sourceCountOffset = 0, sourceDataOffset = 0, sourceCapacityOffset = 0;
    std::size_t pelletShooterOffset = 0;
};
struct Fields
{
    std::size_t effectOrigin = 0, effectTick = 0, smokeTick = 0, didSmoke = 0;
    std::size_t weapons = 0, activeWeapon = 0, attributes = 0, item = 0, definition = 0;
} g_fields;
Configuration g_config;
Detour<HeDetonate> g_heDetour;
Detour<GrenadeDestructor> g_destructorDetour;
Detour<PublishEffect> g_publishDetour;
Detour<PelletTrace> g_pelletDetour;
Detour<PelletResult> g_resultDetour;
Detour<Traversal> g_traversalDetour;
Detour<GetSmokeDensityInLine> g_lineDetour;
struct GameFrameHook : KHook::Virtual<ISource2Server, void, bool, bool, bool>
{
    using Virtual::Virtual;
    bool Attach(ISource2Server* game)
    {
        Add(game);
        std::lock_guard guard(_hooks_stored);
        return !_addr_hook_ids.empty();
    }
};
std::optional<GameFrameHook> g_frameHook;
const void* g_visitorIdentity = nullptr;
Detour<InitializeSmoke> g_initializeDetour;
Detour<ReadySmoke> g_readyDetour;
GetSmokeDensityInLine g_getSmokeDensity = nullptr;
PointDensity g_pointDensity = nullptr;
TraceShape g_traceShape = nullptr;
GetSlot g_getSlot = nullptr;
void* const* g_smokeListHead = nullptr;
const void* g_clockOwner = nullptr;
float g_lastTime = -1;
std::thread::id g_serverThread;
bool g_cacheEnabled = true;
std::atomic<bool> g_bulletsEnabled{ true };
bool g_ready = false;
// Only the server thread logs faults and mutates smoke state.
std::atomic<const char*> g_fault{ nullptr };
bool g_faultReported = false;
HoleCache g_holes;
struct InitialSources
{
    void* entity;
    SmokeKey key;
    Vec3 origin;
    std::vector<Vec3> points;
};
std::vector<InitialSources> g_initialSources;
HoleCache::Snapshot g_effects;
smoke_effects::GeometryCache g_geometry;
float g_effectsTime = std::numeric_limits<float>::quiet_NaN();
std::uint64_t g_effectsGeneration = 0;
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
struct QueryFrame
{
    enum class State { Waiting, Inside, Returned };
    HoleCache::Snapshot effects;
    float now;
    std::uint64_t generation;
    const float* from;
    const float* to;
    float output[3]{};
    State state = State::Waiting;
};
thread_local QueryFrame* g_query = nullptr;
struct QueryScope
{
    QueryFrame* previous = g_query;
    explicit QueryScope(QueryFrame* frame) { g_query = frame; }
    ~QueryScope() { g_query = previous; }
    QueryScope(const QueryScope&) = delete;
    QueryScope& operator=(const QueryScope&) = delete;
};
struct VolumeFrame
{
    const QueryFrame& query;
    void* volume;
    HoleCache::SmokeView effects;
    HoleCache::PreparedView prepared;
    SmokeKey key{};
    std::optional<smoke_effects::GeometryCache::View> geometry;
    VolumeFrame(const QueryFrame& owner, void* volumePtr) : query(owner), volume(volumePtr) {}
    VolumeFrame(const VolumeFrame&) = delete;
    VolumeFrame& operator=(const VolumeFrame&) = delete;
};
void SetError(char* error, std::size_t maxLength, const char* format, ...)
{
    if (!error || maxLength == 0) return;
    va_list args;
    va_start(args, format);
    std::vsnprintf(error, maxLength, format, args);
    va_end(args);
}

constexpr size_t kMaxFieldSpan = 65536;
bool FitsField(int value, size_t width, size_t span)
{
    return value > 0 && width <= span && static_cast<size_t>(value) <= span - width;
}

bool ResolveOffset(const char* type, const char* field, std::size_t width, std::size_t& offset, char* error, std::size_t maxLength)
{
    const int value = cs2_sdk::schema::GetFieldOffset(type, field);
    if (!FitsField(value, width, kMaxFieldSpan))
    {
        SetError(error, maxLength, "required Schema field unavailable: %s::%s", type, field);
        return false;
    }
    offset = static_cast<size_t>(value);
    return true;
}

using memory::IsReadable;
template <typename T> bool ReadAt(const void* base, size_t offset, T& value)
{
    return memory::Read(base, offset, value, memory::FailureDomain::Smoke);
}

bool HasActiveSmoke() { return g_smokeListHead && *g_smokeListHead; }

void* ResolveVirtualSlot(const nlohmann::json& data, const char* type, const char* name, char* error, size_t maxLength)
{
    const int slot = cs2_sdk::gameconfig::FindPlatformOffset(data, name, -1);
    if (slot < 0 || slot >= 64)
    {
        SetError(error, maxLength, "missing or invalid vtable slot for %s", name);
        return nullptr;
    }
    const auto& image = cs2_sdk::modules::server->Image();
    auto** table = cs2_sdk::rtti::ResolveVirtualTable(image, type, error, maxLength);
    if (!table) return nullptr;
    void* entry = nullptr;
    if (!cs2_sdk::modules::ContainsRange(image, table, (static_cast<size_t>(slot) + 1) * sizeof(void*)) ||
        !memory::Read(table, static_cast<size_t>(slot) * sizeof(void*), entry, memory::FailureDomain::Install) ||
        !cs2_sdk::modules::ContainsRange(cs2_sdk::modules::server->Code(), entry, 1) || !cs2_sdk::memory::IsExecutableAddress(entry))
    {
        SetError(error, maxLength, "%s slot %d is unreadable or not executable in server", name, slot);
        return nullptr;
    }
    return entry;
}
const void* ResolveRipTarget(void* site, const std::array<uint8_t, 3>& opcode, const cs2_sdk::modules::ModuleInfo& region,
                             const char* name, char* error, size_t maxLength)
{
    if (!cs2_sdk::modules::ContainsRange(cs2_sdk::modules::server->Code(), site, 7) || std::memcmp(site, opcode.data(), opcode.size()) != 0)
    {
        SetError(error, maxLength, "%s signature is not the expected RIP-relative instruction", name);
        return nullptr;
    }
    int32_t displacement = 0;
    std::memcpy(&displacement, static_cast<const uint8_t*>(site) + 3, sizeof(displacement));
    auto* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(site) + 7 + displacement);
    if (!cs2_sdk::modules::ContainsRange(region, target, sizeof(void*)) || !IsReadable(target, sizeof(void*)))
    {
        SetError(error, maxLength, "%s target outside server region or unreadable", name);
        return nullptr;
    }
    return target;
}
bool LoadConfiguration(const nlohmann::json& data, size_t projectileSize, Configuration& config, char* error, size_t maxLength)
{
    auto read = [&](const char* key, size_t& out, size_t width, size_t span) {
        const int value = cs2_sdk::gameconfig::FindPlatformOffset(data, key, -1);
        if (!FitsField(value, width, span))
        {
            SetError(error, maxLength, "missing or out-of-bounds smoke offset: %s (%d)", key, value);
            return false;
        }
        out = static_cast<size_t>(value);
        return true;
    };
    if (!read("SmokeLayout.volume", config.volumeOffset, 1, projectileSize) ||
        !read("SmokeLayout.next", config.nextOffset, sizeof(void*), projectileSize))
        return false;
    const size_t volumeSpan = projectileSize - config.volumeOffset;
    return read("SmokeLayout.start_time", config.volumeTimeOffset, sizeof(float), volumeSpan) &&
           read("SmokeLayout.origin", config.volumeOriginOffset, sizeof(Vec3), volumeSpan) &&
           read("SmokeLayout.source_count", config.sourceCountOffset, sizeof(int), volumeSpan) &&
           read("SmokeLayout.source_data", config.sourceDataOffset, sizeof(void*), volumeSpan) &&
           read("SmokeLayout.source_capacity", config.sourceCapacityOffset, sizeof(int), volumeSpan) &&
           read("SmokeLayout.shooter", config.pelletShooterOffset, sizeof(void*), kMaxFieldSpan);
}
void ReportFault()
{
    if (std::this_thread::get_id() != g_serverThread || g_faultReported) return;
    if (const char* message = g_fault.load(std::memory_order_relaxed))
    {
        g_faultReported = true;
        Warning("[BotVision] smoke provider unavailable: %s\n", message);
        BV_LOG_ERROR("Smoke density unavailable: %s", message);
    }
}
void Fault(const char* message)
{
    const char* expected = nullptr;
    g_fault.compare_exchange_strong(expected, message, std::memory_order_relaxed);
    ReportFault();
}
bool MainThread() { return std::this_thread::get_id() == g_serverThread; }
game_time::Sample ReadClock()
{
    const auto clock = game_time::Read();
    if (!clock.server) return clock;
    const bool changedServer = clock.server != g_clockOwner;
    const bool hasTime = std::isfinite(clock.time);
    if (changedServer || (hasTime && clock.time < g_lastTime))
    {
        g_holes.Reset();
        // A clock rollback expires effects; a server change also invalidates smoke sources.
        if (changedServer) g_initialSources.clear();
        g_geometry.Reset();
    }
    g_clockOwner = clock.server;
    if (changedServer || hasTime) g_lastTime = clock.time;
    return clock;
}
float Now() { return ReadClock().time; }
bool SmokeIdentity(void* entity, SmokeKey& key)
{
    if (!ReadAt(entity, g_fields.smokeTick, key.effectTick)) return false;
    // The handle serial distinguishes reused entity slots.
    CEntityIdentity* identity = nullptr;
    if (!ReadAt(&static_cast<CEntityInstance*>(entity)->m_pEntity, 0, identity) || !IsReadable(identity, sizeof(CEntityIdentity)))
        return false;
    key.handle = identity->GetRefEHandle().ToInt();
    return key.handle != 0xffffffffu;
}
void CaptureInitialSources(void* volume)
{
    using smoke_effects::SmokeSource;
    // Ready owns the projectile after the smoke build finishes.
    if (!g_readySmoke || static_cast<char*>(g_readySmoke) + g_config.volumeOffset != volume || g_fault) return;
    if (!ReadClock().server) return;
    InitialSources snapshot{};
    snapshot.entity = g_readySmoke;
    int count = 0, capacity = 0;
    const SmokeSource* data = nullptr;
    if (!SmokeIdentity(snapshot.entity, snapshot.key) || snapshot.key.effectTick < 0 ||
        !ReadAt(volume, g_config.volumeOriginOffset, snapshot.origin) || !IsFinite(snapshot.origin) ||
        !ReadAt(volume, g_config.sourceCountOffset, count) || count < 0 || !ReadAt(volume, g_config.sourceCapacityOffset, capacity) ||
        capacity < count || !ReadAt(volume, g_config.sourceDataOffset, data) ||
        (count && !IsReadable(data, static_cast<std::size_t>(count) * sizeof(*data))))
    {
        Fault("invalid initial smoke sources");
        return;
    }
    // Copy world centers before Ready sets DidSmoke.
    snapshot.points.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        SmokeSource source;
        std::memcpy(&source, data + i, sizeof(source));
        Vec3 center;
        if (!smoke_effects::SourceCenter(snapshot.origin, source, center))
        {
            Fault("invalid initial smoke source coordinate");
            return;
        }
        snapshot.points.push_back(center);
    }
    // Refresh source records on creation or address reuse.
    for (auto it = g_initialSources.begin(); it != g_initialSources.end();)
    {
        bool present = false;
        if (it->entity != snapshot.entity)
            for (void* smoke = *g_smokeListHead; smoke;)
            {
                if (smoke == it->entity)
                {
                    present = true;
                    break;
                }
                if (!ReadAt(smoke, g_config.nextOffset, smoke))
                {
                    Fault("invalid smoke list during source retirement");
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
    if (!MainThread() || g_fault) return g_readyDetour.Continue(self);
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
    g_traceShape(nullptr, ray, start, end, &filter, &trace);
    // The client treats entity ID 0x8000 as a blocker.
    std::uint32_t entityId = 0;
    std::memcpy(&entityId, reinterpret_cast<const char*>(&trace) + 0x68, sizeof(entityId));
    return !(trace.DidHit() && entityId == 0x8000);
}
void CaptureHe(void* self)
{
    Vec3 blast;
    int tick = 0;
    if (!ReadAt(self, g_fields.effectTick, tick))
    {
        Fault("unreadable HE effect tick");
        return;
    }
    if (tick < 1) return;
    if (!ReadAt(self, g_fields.effectOrigin, blast) || !IsFinite(blast))
    {
        Fault("invalid published HE effect");
        return;
    }
    // The Windows client timestamps HE in 1/64-second ticks.
    const float eventTime = static_cast<float>(tick) * (1.0f / 64.0f);
    std::vector<SmokeKey> affected;
    for (void* smoke = *g_smokeListHead; smoke;)
    {
        void* next = nullptr;
        bool active = false;
        SmokeKey key;
        if (!ReadAt(smoke, g_config.nextOffset, next) || !ReadAt(smoke, g_fields.didSmoke, active))
        {
            Fault("invalid smoke list");
            return;
        }
        if (active)
        {
            if (!SmokeIdentity(smoke, key))
            {
                Fault("invalid smoke identity");
                return;
            }
            const auto sources = std::find_if(g_initialSources.begin(), g_initialSources.end(), [&](const auto& entry) {
                return entry.entity == smoke && entry.key == key;
            });
            // Ready supplies the initial smoke sources; the smoke-age gate applies during sampling.
            if (sources != g_initialSources.end() && smoke_effects::HeAffectsSources(blast, sources->origin, sources->points, SourceReachable))
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
    if (MainThread()) HeDetonation::OnDestroyed(self);
    else Fault("grenade destroyed off server thread");
    return { KHook::Action::Ignore };
}

KHook::Return<std::int64_t> HookedHeDetonate(void* self)
{
    if (!MainThread() || g_fault) return g_heDetour.Continue(self);
    HeDetonation frame(self);
    Now();
    return g_heDetour.Continue(self);
}
KHook::Return<void> HookedPublish(void* self, void* effect, const Vec3* position)
{
    const auto result = g_publishDetour.Continue(self, effect, position);
    // Publish returns before Detonate releases the projectile.
    if (MainThread() && !g_fault && HeDetonation::current && self == HeDetonation::current->projectile && HasActiveSmoke()) CaptureHe(self);
    return result;
}
float WeaponScale(void* pawn)
{
    void* services = nullptr;
    std::uint32_t active = 0xffffffffu;
    if (!ReadAt(pawn, g_fields.weapons, services) || !services || !ReadAt(services, g_fields.activeWeapon, active) || active == 0xffffffffu)
        return 0;
    // Firearm pellets use the active primary or secondary weapon.
    for (int slot = 0; slot < 2; ++slot)
    {
        auto* weapon = static_cast<CEntityInstance*>(g_getSlot(services, slot, 0xffffffffu));
        if (!weapon || !weapon->m_pEntity || weapon->GetRefEHandle().ToInt() != active) continue;
        std::uint16_t definition = 0;
        if (!ReadAt(weapon, g_fields.attributes + g_fields.item + g_fields.definition, definition)) return 0;
        return definition == 25 || definition == 27 || definition == 29 || definition == 35 ? .25f : 1.0f;
    }
    return 0;
}
bool IntersectsActiveSmoke(Vec3 start, Vec3 end)
{
    for (void* smoke = *g_smokeListHead; smoke;)
    {
        void* next = nullptr;
        bool active = false;
        if (!ReadAt(smoke, g_config.nextOffset, next) || !ReadAt(smoke, g_fields.didSmoke, active))
        {
            Fault("invalid smoke list during bullet capture");
            return false;
        }
        if (active)
        {
            Vec3 origin;
            if (!ReadAt(smoke, g_config.volumeOffset + g_config.volumeOriginOffset, origin) || !IsFinite(origin))
            {
                Fault("invalid smoke grid origin");
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
    if (!g_pellet || g_fault) return result;
    float endFraction = 0;
    Vec3 start, end;
    if (!ReadAt(hit, 0x0c, endFraction) || !ReadAt(trace, 0x78, start) || !ReadAt(trace, 0x84, end) || !std::isfinite(startFraction) ||
        !std::isfinite(endFraction) || !IsFinite(start) || !IsFinite(end))
    {
        Fault("invalid pellet result");
        return result;
    }
    // Penetration contributes only forward trace intervals.
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
    if (MainThread() && !g_fault && g_bulletsEnabled && HasActiveSmoke())
    {
        frame.time = Now();
        // The bullet context stores the shooter pointer.
        void* pawn = nullptr;
        if (!ReadAt(reinterpret_cast<void*>(a1), g_config.pelletShooterOffset, pawn)) Fault("invalid pellet shooter context");
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

void PrepareVolume(VolumeFrame& frame)
{
    SmokeKey key;
    float startTime = 0;
    auto* entity = reinterpret_cast<CEntityInstance*>(static_cast<char*>(frame.volume) - g_config.volumeOffset);
    auto* identity = entity->m_pEntity;
    if (!identity)
    {
        Fault("invalid smoke volume");
        return;
    }
    key.handle = identity->GetRefEHandle().ToInt();
    std::memcpy(&key.effectTick, reinterpret_cast<const char*>(entity) + g_fields.smokeTick, sizeof(key.effectTick));
    std::memcpy(&startTime, static_cast<const char*>(frame.volume) + g_config.volumeTimeOffset, sizeof(startTime));
    if (key.handle == 0xffffffffu || !std::isfinite(startTime))
    {
        Fault("invalid smoke volume");
        return;
    }
    frame.key = key;
    frame.effects = HoleCache::ForSmokeView(frame.query.effects, key, startTime, frame.query.now);
    frame.prepared = frame.effects;
}
void AdjustCell(void* state, CellContext* context, const Vec3* point, std::uint32_t cell, float before, float original)
{
    if (g_fault) return;
    auto& frame = *static_cast<VolumeFrame*>(state);
    if (!std::isfinite(original) || original < 0 || !IsFinite(*point) || cell >= 32768)
    {
        Fault("invalid voxel contribution");
        return;
    }
    if (frame.query.generation != g_holes.Generation()) return;
    const auto geometry = frame.geometry ? frame.geometry->Sample(*point, cell) : smoke_effects::PrepareGeometry(*point, frame.prepared);
    const float adjusted = smoke_effects::EvaluateGeometry(geometry, original, g_pointDensity, frame.volume);
    if (!std::isfinite(adjusted) || adjusted < 0)
    {
        Fault("invalid adjusted density");
        return;
    }
    // The adjustment updates the query-local sum.
    *context->sum = before + adjusted;
}
KHook::Return<float> HookedLine(const float* from, const float* to, float* output)
{
    auto* frame = g_query;
    if (!frame) return { KHook::Action::Ignore };
    if (frame->state != QueryFrame::State::Waiting || output != frame->output || from != frame->from || to != frame->to)
    {
        QueryScope foreign(nullptr);
        return g_lineDetour.Continue(from, to, output);
    }
    // The private output identifies this query; the engine receives a null closest output.
    frame->state = QueryFrame::State::Inside;
    const auto result = g_lineDetour.Continue(from, to, static_cast<float*>(nullptr));
    frame->state = QueryFrame::State::Returned;
    return result;
}
KHook::Return<void> HookedTraversal(void* volume, const Vec3* from, const Vec3* to, void* container)
{
    if (!g_query || g_query->state != QueryFrame::State::Inside || g_fault || static_cast<const void*>(from) != g_query->from ||
        static_cast<const void*>(to) != g_query->to || BorrowedTraversal::Identity(container) != g_visitorIdentity)
        return { KHook::Action::Ignore };
    auto* context = BorrowedTraversal::Context(container);
    if (context->volume != volume || !context->closest || *context->closest) return { KHook::Action::Ignore };

    VolumeFrame frame{ *g_query, volume };
    QueryScope scope(nullptr);
    PrepareVolume(frame);
    if (g_fault || frame.effects.Empty()) return g_traversalDetour.Continue(volume, from, to, container);
    if (g_cacheEnabled) frame.geometry.emplace(g_geometry.Bind(frame.prepared, frame.key, frame.query.now, frame.query.generation));
    BorrowedTraversal borrowed(container, context, &frame, &AdjustCell, BorrowedTraversal::Contribution::ExactCell);
    return g_traversalDetour.Continue(volume, from, to, borrowed.Container());
}
const HoleCache::Snapshot& EffectsAt(float now, std::uint64_t generation)
{
    if (now != g_effectsTime || generation != g_effectsGeneration)
    {
        g_effects = g_holes.Prepare(now);
        if (!g_bulletsEnabled) g_effects.bulletCount = 0;
        g_effectsTime = now;
        g_effectsGeneration = generation;
    }
    return g_effects;
}
void RetireIdleLine()
{
    if (!MainThread()) return;
    if (g_fault) ReportFault();
    if (!g_lineDetour.Enabled() || g_query) return;
    if (g_fault || !HasActiveSmoke()) return g_lineDetour.Disable();
    const float now = Now();
    if (!std::isfinite(now) || EffectsAt(now, g_holes.Generation()).Empty())
        g_lineDetour.Disable();
}
KHook::Return<void> AfterGameFrame(ISource2Server*, bool, bool, bool)
{
    // Idle density hooks are removed at frame end.
    RetireIdleLine();
    return { KHook::Action::Ignore };
}
float QueryDensity(const float* start, const float* end, bool corrected)
{
    if (!MainThread() || !g_ready || g_fault || !start || !end) return std::numeric_limits<float>::quiet_NaN();
    float from[3], to[3];
    std::memcpy(from, start, sizeof(from));
    std::memcpy(to, end, sizeof(to));
    if (!IsFinite({ from[0], from[1], from[2] }) || !IsFinite({ to[0], to[1], to[2] }))
        return std::numeric_limits<float>::quiet_NaN();
    if (!HasActiveSmoke()) return 0.0F;
    const float now = Now();
    if (!std::isfinite(now)) return std::numeric_limits<float>::quiet_NaN();
    const auto generation = g_holes.Generation();
    const auto query = [&](QueryFrame* frame) {
        QueryScope scope(frame);
        const float result = g_getSmokeDensity(from, to, frame ? frame->output : nullptr);
        return g_fault || (frame && frame->state != QueryFrame::State::Returned) ||
                       (corrected && generation != g_holes.Generation()) || !std::isfinite(result) || result < 0.0F
                   ? std::numeric_limits<float>::quiet_NaN()
                   : result;
    };
    if (corrected)
    {
        const auto& effects = EffectsAt(now, generation);
        if (!effects.Empty() && HoleCache::MayAffectRay({ from[0], from[1], from[2] }, { to[0], to[1], to[2] }, effects))
        {
            if (!g_lineDetour.EnsureEnabled())
            {
                Fault("failed to register density hook");
                return std::numeric_limits<float>::quiet_NaN();
            }
            QueryFrame frame{ effects, now, generation, from, to };
            return query(&frame);
        }
    }
    return query(nullptr);
}
} // namespace

bool Install(const nlohmann::json& data, const cs2_sdk::modules::ModuleInfo& server, void*& visibilityTarget, char* error, size_t maxLength)
{
    CGameTrace traceLayout;
    const auto offset = [&](const void* field) {
        return static_cast<const char*>(field) - reinterpret_cast<const char*>(&traceLayout);
    };
    if (offset(&traceLayout.m_vStartPos) != 0x78 || offset(&traceLayout.m_vEndPos) != 0x84 || offset(&traceLayout.m_flFraction) != 0xac ||
        offset(&traceLayout.m_bStartInSolid) != 0xbb)
    {
        SetError(error, maxLength, "SDK trace layout differs from validated game");
        return false;
    }
    auto* scope = g_schemaSystem && g_schemaSystem->SchemaSystemIsReady()
                      ? g_schemaSystem->FindTypeScopeForModule(cs2_sdk::modules::kSchemaServerTypeScope, nullptr) : nullptr;
    auto* projectile = scope ? scope->FindDeclaredClass("CSmokeGrenadeProjectile").Get() : nullptr;
    if (!projectile || projectile->m_nSizeOf <= 0)
    {
        SetError(error, maxLength, "missing CSmokeGrenadeProjectile schema size");
        return false;
    }
    Configuration config;
    if (!LoadConfiguration(data, static_cast<size_t>(projectile->m_nSizeOf), config, error, maxLength)) return false;
    struct Field
    {
        const char *type, *name;
        std::size_t width;
        std::size_t* offset;
    };
    const Field fields[]{
        { "CBaseCSGrenadeProjectile", "m_vecExplodeEffectOrigin", sizeof(Vec3), &g_fields.effectOrigin },
        { "CBaseCSGrenadeProjectile", "m_nExplodeEffectTickBegin", sizeof(int), &g_fields.effectTick },
        { "CSmokeGrenadeProjectile", "m_nSmokeEffectTickBegin", sizeof(int), &g_fields.smokeTick },
        { "CSmokeGrenadeProjectile", "m_bDidSmokeEffect", sizeof(bool), &g_fields.didSmoke },
        { "CBasePlayerPawn", "m_pWeaponServices", sizeof(void*), &g_fields.weapons },
        { "CPlayer_WeaponServices", "m_hActiveWeapon", sizeof(std::uint32_t), &g_fields.activeWeapon },
        { "CEconEntity", "m_AttributeManager", sizeof(void*), &g_fields.attributes },
        { "CAttributeContainer", "m_Item", sizeof(void*), &g_fields.item },
        { "CEconItemView", "m_iItemDefinitionIndex", sizeof(std::uint16_t), &g_fields.definition },
    };
    for (const auto& field : fields)
        if (!ResolveOffset(field.type, field.name, field.width, *field.offset, error, maxLength)) return false;
    // The list link and volume offsets are relative to m_bDidSmokeEffect.
    if (g_fields.didSmoke < 12 || config.nextOffset != g_fields.didSmoke - 12 || config.volumeOffset != g_fields.didSmoke + 84)
    {
        SetError(error, maxLength, "smoke list/volume offsets disagree with m_bDidSmokeEffect");
        return false;
    }
    struct
    {
        void *visibility, *density, *heDetonate, *pelletTrace, *listSite, *publish;
        void *pelletResult, *traverse, *visitorSite, *pointDensity, *getSlot, *initialize, *ready;
    } address{};
    struct Signature
    {
        const char* name;
        void*& entry;
    };
    const Signature required[]{
        { "CBotManager::IsVisibleThroughSmoke", address.visibility },
        { "GetSmokeDensityInLine", address.density },
        { "CHEGrenadeProjectile::Detonate", address.heDetonate },
        { "BulletPelletTrace", address.pelletTrace },
        { "g_AutoList_SmokeProj_Head_Server", address.listSite },
        { "CBaseCSGrenadeProjectile::PublishEffect", address.publish },
        { "BulletPelletTraceResult", address.pelletResult },
        { "SmokeVolume::Traverse", address.traverse },
        { "SmokeVolume::DensityVisitor", address.visitorSite },
        { "SmokeVolume::PointDensity", address.pointDensity },
        { "CCSPlayer_WeaponServices::GetSlot", address.getSlot },
        { "SmokeVolume::InitializeReady", address.initialize },
        { "CSmokeGrenadeProjectile::Ready", address.ready },
    };
    for (const auto& binding : required)
    {
        binding.entry = cs2_sdk::gameconfig::ResolveSig(data, server, binding.name, error, maxLength);
        if (!binding.entry) return false;
    }
#ifdef _WIN32
    constexpr std::array<uint8_t, 3> visitorLea{0x48, 0x8d, 0x0d}; // lea rcx, [visitor vtable]
    const auto& identityRegion = cs2_sdk::modules::server->Image();
#else
    constexpr std::array<uint8_t, 3> visitorLea{0x48, 0x8d, 0x05}; // lea rax, [visitor invoker]
    const auto& identityRegion = server;
#endif
    const auto* visitor = ResolveRipTarget(address.visitorSite, visitorLea, identityRegion, "density visitor", error, maxLength);
    if (!visitor) return false;
    // The TraceShape wrapper supplies the physics object on Linux.
    auto* traceShape = ResolveVirtualSlot(data, "CNavPhysicsInterface", "CNavPhysicsInterface::TraceShape", error, maxLength);
    if (!traceShape) return false;
    auto* destructor = ResolveVirtualSlot(data, kDestructorClass, "GrenadeProjectile::Destructor", error, maxLength);
    if (!destructor) return false;
    auto head = static_cast<void* const*>(ResolveRipTarget(address.listSite, {0x48, 0x8b, 0x1d}, cs2_sdk::modules::server->Image(),
                                                           "smoke list head", error, maxLength));
    if (!head) return false;
    g_config = config;

    g_smokeListHead = head;
    g_getSmokeDensity = reinterpret_cast<GetSmokeDensityInLine>(address.density);
    g_pointDensity = reinterpret_cast<PointDensity>(address.pointDensity);
    g_traceShape = reinterpret_cast<TraceShape>(traceShape);
    g_getSlot = reinterpret_cast<GetSlot>(address.getSlot);
    g_serverThread = std::this_thread::get_id();
    g_clockOwner = nullptr;
    g_lastTime = -1;
    g_fault = nullptr;
    g_faultReported = false;
    g_holes.Reset();
    g_initialSources.clear();
    g_geometry.Reset();
    g_visitorIdentity = visitor;
    if (!g_destructorDetour.Create(destructor, &HookedGrenadeDestructor) || !g_heDetour.Create(address.heDetonate, &HookedHeDetonate) ||
        !g_pelletDetour.Create(address.pelletTrace, &HookedPelletTrace) || !g_publishDetour.Create(address.publish, &HookedPublish) ||
        !g_resultDetour.Create(address.pelletResult, &HookedResult) || !g_traversalDetour.Create(address.traverse, &HookedTraversal) ||
        !g_lineDetour.Create(address.density, &HookedLine) ||
        !g_initializeDetour.Create(address.initialize, &HookedInitializeSmoke) || !g_readyDetour.Create(address.ready, &HookedReady) ||
        !g_destructorDetour.Enable() || !g_initializeDetour.Enable() || !g_readyDetour.Enable() || !g_publishDetour.Enable() ||
        !g_resultDetour.Enable() || !g_traversalDetour.Enable() || !g_heDetour.Enable() || !g_pelletDetour.Enable())
    {
        Remove();
        SetError(error, maxLength, "failed to install smoke effect hooks");
        return false;
    }
    auto* game = static_cast<ISource2Server*>(g_SMAPI->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr));
    if (!game || !g_frameHook.emplace(&ISource2Server::GameFrame, nullptr, &AfterGameFrame).Attach(game))
    {
        g_frameHook.reset();
        BV_LOG_WARN("GameFrame unavailable; idle density hook stays registered");
    }
    visibilityTarget = address.visibility;
    g_ready = true;
    return true;
}
void Remove()
{
    g_ready = false;
    g_frameHook.reset();
    g_pelletDetour.Reset();
    g_heDetour.Reset();
    g_destructorDetour.Reset();
    g_resultDetour.Reset();
    g_publishDetour.Reset();
    g_lineDetour.Reset();
    g_traversalDetour.Reset();
    g_visitorIdentity = nullptr;
    g_readyDetour.Reset();
    g_initializeDetour.Reset();
    g_getSmokeDensity = nullptr;
    g_pointDensity = nullptr;
    g_traceShape = nullptr;
    g_getSlot = nullptr;
    g_smokeListHead = nullptr;
    g_clockOwner = nullptr;
    g_query = nullptr;
    g_pellet = nullptr;
    g_readySmoke = nullptr;
    g_holes.Reset();
    g_initialSources.clear();
    g_geometry.Reset();
}

float Density(const float* from, const float* to, bool corrected) { return QueryDensity(from, to, corrected); }
bool Ready() { return g_ready && !g_fault; }
bool ListResolved() { return g_smokeListHead != nullptr; }
const char* Status() { return g_fault ? g_fault.load() : g_ready ? "native-voxel" : "unavailable"; }
std::pair<int, int> ActiveEffectCounts()
{
    if (!MainThread() || !g_ready) return {};
    const auto effects = g_holes.Prepare(Now());
    return { static_cast<int>(effects.heCount), static_cast<int>(effects.bulletCount) };
}
void EnableBullets(bool enabled)
{
    g_bulletsEnabled = enabled;
    g_effectsTime = std::numeric_limits<float>::quiet_NaN();
    g_geometry.Reset();
}
bool BulletsEnabled() { return g_bulletsEnabled.load(); }
void EnableGeometryCache(bool enabled)
{
    g_cacheEnabled = enabled;
    g_geometry.Reset();
}
bool GeometryCacheEnabled() { return g_cacheEnabled; }
uint64_t CacheHits() { return g_geometry.Hits(); }
uint64_t CacheMisses() { return g_geometry.Misses(); }
} // namespace cs2bv::smoke_density
