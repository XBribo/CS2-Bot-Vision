#include "SmokeDensity.h"
#include "SmokeCapture.h"
#include "SmokeBindings.h"
#include "Detour.h"
#include "SmokeTraversal.h"
#include "SmokeGeometry.h"
#include "core/log.h"
#include "core/game_time.h"
#include "plugin.h"
#include <thread>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <entity2/entityinstance.h>
#include <engine2/isource2server.h>
#include <limits>
#include <optional>
#include <tier0/dbg.h>
#include <utility>

namespace cs2bv::smoke_density {
namespace {
using smoke_bindings::GetSmokeDensityInLine;
using smoke_effects::BorrowedTraversal;
using smoke_effects::CellContext;
using smoke_effects::HoleCache;
using smoke_effects::IsFinite;
using smoke_effects::SmokeKey;
using smoke_effects::Traversal;
using smoke_effects::Vec3;
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
smoke_bindings::Bindings g_bindings;
const void* g_clockOwner = nullptr;
float g_lastTime = -1;
std::thread::id g_serverThread;
bool g_cacheEnabled = true;
bool g_ready = false;
// Only the server thread logs faults and changes smoke state.
std::atomic<const char*> g_fault{ nullptr };
bool g_faultReported = false;
const HoleCache& g_holes = smoke_capture::Effects();
HoleCache::Snapshot g_effects;
smoke_effects::GeometryCache g_geometry;
float g_effectsTime = std::numeric_limits<float>::quiet_NaN();
std::uint64_t g_effectsGeneration = 0;
struct QueryFrame
{
    enum class State
    {
        Waiting,
        Inside,
        Returned
    };
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
bool HasActiveSmoke() { return g_bindings.smokeListHead && *g_bindings.smokeListHead; }
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
bool HasFault() { return g_fault.load() != nullptr; }
game_time::Sample ReadClock()
{
    const auto clock = game_time::Read();
    if (!clock.server) return clock;
    const bool changedServer = clock.server != g_clockOwner;
    const bool hasTime = std::isfinite(clock.time);
    if (changedServer || (hasTime && clock.time < g_lastTime))
    {
        // Clock rollback clears effects. Server changes also clear sources.
        smoke_capture::ResetEffects(changedServer);
        g_geometry.Reset();
    }
    g_clockOwner = clock.server;
    if (changedServer || hasTime) g_lastTime = clock.time;
    return clock;
}
float Now() { return ReadClock().time; }
void PrepareVolume(VolumeFrame& frame)
{
    SmokeKey key;
    float startTime = 0;
    auto* entity = reinterpret_cast<CEntityInstance*>(static_cast<char*>(frame.volume) - g_bindings.config.volumeOffset);
    auto* identity = entity->m_pEntity;
    if (!identity)
    {
        Fault("invalid smoke volume");
        return;
    }
    key.handle = identity->GetRefEHandle().ToInt();
    std::memcpy(&key.effectTick, reinterpret_cast<const char*>(entity) + g_bindings.fields.smokeTick, sizeof(key.effectTick));
    std::memcpy(&startTime, static_cast<const char*>(frame.volume) + g_bindings.config.volumeTimeOffset, sizeof(startTime));
    if (key.handle == 0xffffffffu || !std::isfinite(startTime))
    {
        Fault("invalid smoke volume");
        return;
    }
    frame.key = key;
    frame.effects = HoleCache::ForSmokeView(frame.query.effects, key, startTime, frame.query.now);
    frame.prepared = frame.effects;
}
template <bool DirectHe = false>
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
    float adjusted;
    if constexpr (DirectHe)
        adjusted = HoleCache::HeContribution(original, HoleCache::HeFactor(HoleCache::IdentityBullet(*point).position, frame.prepared));
    else
    {
        const auto geometry =
            frame.geometry ? frame.geometry->Sample(*point, cell) : smoke_effects::PrepareGeometry(*point, frame.prepared);
        adjusted = smoke_effects::EvaluateGeometry(geometry, original, g_bindings.pointDensity, frame.volume);
    }
    if (!std::isfinite(adjusted) || adjusted < 0)
    {
        Fault("invalid adjusted density");
        return;
    }
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
    // Private output marks this query. The engine receives nullptr.
    frame->state = QueryFrame::State::Inside;
    const auto result = g_lineDetour.Continue(from, to, static_cast<float*>(nullptr));
    frame->state = QueryFrame::State::Returned;
    return result;
}
KHook::Return<void> HookedTraversal(void* volume, const Vec3* from, const Vec3* to, void* container)
{
    if (!g_query || g_query->state != QueryFrame::State::Inside || g_fault || static_cast<const void*>(from) != g_query->from ||
        static_cast<const void*>(to) != g_query->to || BorrowedTraversal::Identity(container) != g_bindings.visitor)
        return { KHook::Action::Ignore };
    auto* context = BorrowedTraversal::Context(container);
    if (context->volume != volume || !context->closest || *context->closest) return { KHook::Action::Ignore };

    VolumeFrame frame{ *g_query, volume };
    QueryScope scope(nullptr);
    PrepareVolume(frame);
    if (g_fault || frame.effects.Empty()) return g_traversalDetour.Continue(volume, from, to, container);
    if (g_cacheEnabled && g_geometry.ShouldCache(frame.prepared, frame.key, frame.query.now, frame.query.generation))
        frame.geometry.emplace(g_geometry.Bind(frame.prepared, frame.key, frame.query.now, frame.query.generation));
    const auto adjust = !frame.geometry && frame.prepared.bullets.empty() ? &AdjustCell<true> : &AdjustCell<>;
    BorrowedTraversal borrowed(container, context, &frame, adjust);
    return g_traversalDetour.Continue(volume, from, to, borrowed.Container());
}
const HoleCache::Snapshot& EffectsAt(float now, std::uint64_t generation)
{
    if (now != g_effectsTime || generation != g_effectsGeneration)
    {
        g_effects = g_holes.Prepare(now);
        if (!smoke_capture::BulletsEnabled()) g_effects.bulletCount = 0;
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
    if (!std::isfinite(now) || EffectsAt(now, g_holes.Generation()).Empty()) g_lineDetour.Disable();
}
KHook::Return<void> AfterGameFrame(ISource2Server*, bool, bool, bool)
{
    // Remove idle density hooks at frame end.
    RetireIdleLine();
    return { KHook::Action::Ignore };
}
float QueryDensity(const float* start, const float* end, bool corrected)
{
    if (!MainThread() || !g_ready || g_fault || !start || !end) return std::numeric_limits<float>::quiet_NaN();
    float from[3], to[3];
    std::memcpy(from, start, sizeof(from));
    std::memcpy(to, end, sizeof(to));
    if (!IsFinite({ from[0], from[1], from[2] }) || !IsFinite({ to[0], to[1], to[2] })) return std::numeric_limits<float>::quiet_NaN();
    if (!HasActiveSmoke()) return 0.0F;
    const float now = Now();
    if (!std::isfinite(now)) return std::numeric_limits<float>::quiet_NaN();
    const auto generation = g_holes.Generation();
    const auto query = [&](QueryFrame* frame) {
        QueryScope scope(frame);
        const float result = g_bindings.density(from, to, frame ? frame->output : nullptr);
        return g_fault || (frame && frame->state != QueryFrame::State::Returned) || (corrected && generation != g_holes.Generation()) ||
                       !std::isfinite(result) || result < 0.0F
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
    if (!smoke_bindings::Resolve(data, server, g_bindings, error, maxLength)) return false;
    g_serverThread = std::this_thread::get_id();
    g_clockOwner = nullptr;
    g_lastTime = -1;
    g_fault = nullptr;
    g_faultReported = false;
    smoke_capture::Initialize(g_bindings, { &MainThread, &HasFault, &Fault, &ReadClock });
    g_geometry.Reset();
    if (!smoke_capture::CreateEventHooks() || !g_traversalDetour.Create(g_bindings.traverse, &HookedTraversal) ||
        !g_lineDetour.Create(reinterpret_cast<void*>(g_bindings.density), &HookedLine) || !smoke_capture::CreateSourceHooks() ||
        !smoke_capture::EnableObservers() || !g_traversalDetour.Enable() || !smoke_capture::EnableEmitters())
    {
        Remove();
        if (error && maxLength) std::snprintf(error, maxLength, "failed to install smoke effect hooks");
        return false;
    }
    auto* game = static_cast<ISource2Server*>(g_SMAPI->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr));
    if (!game || !g_frameHook.emplace(&ISource2Server::GameFrame, nullptr, &AfterGameFrame).Attach(game))
    {
        g_frameHook.reset();
        BV_LOG_WARN("GameFrame unavailable; idle density hook stays registered");
    }
    visibilityTarget = g_bindings.visibility;
    g_ready = true;
    return true;
}
void Remove()
{
    g_ready = false;
    g_frameHook.reset();
    smoke_capture::RemoveEventHooks();
    g_lineDetour.Reset();
    g_traversalDetour.Reset();
    smoke_capture::RemoveSourceHooks();
    g_bindings = {};
    g_clockOwner = nullptr;
    g_query = nullptr;
    smoke_capture::Clear();
    g_geometry.Reset();
}

float Density(const float* from, const float* to, bool corrected) { return QueryDensity(from, to, corrected); }
bool Ready() { return g_ready && !g_fault; }
bool ListResolved() { return g_bindings.smokeListHead != nullptr; }
const char* Status() { return g_fault ? g_fault.load() : g_ready ? "native-voxel" : "unavailable"; }
std::pair<int, int> ActiveEffectCounts()
{
    if (!MainThread() || !g_ready) return {};
    const auto effects = g_holes.Prepare(Now());
    return { static_cast<int>(effects.heCount), static_cast<int>(effects.bulletCount) };
}
void EnableBullets(bool enabled)
{
    smoke_capture::EnableBullets(enabled);
    g_effectsTime = std::numeric_limits<float>::quiet_NaN();
    g_geometry.Reset();
}
bool BulletsEnabled() { return smoke_capture::BulletsEnabled(); }
void EnableGeometryCache(bool enabled)
{
    g_cacheEnabled = enabled;
    g_geometry.Reset();
}
bool GeometryCacheEnabled() { return g_cacheEnabled; }
uint64_t CacheHits() { return g_geometry.Hits(); }
uint64_t CacheMisses() { return g_geometry.Misses(); }
} // namespace cs2bv::smoke_density
