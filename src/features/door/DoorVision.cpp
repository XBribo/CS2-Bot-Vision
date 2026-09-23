// Additional door occlusion for otherwise visible bot sight lines

#include "DoorVision.h"

#include "core/gameconfig.h"
#include "core/log.h"
#include "platform.h"

#include <entity2/entityinstance.h>
#include <bspflags.h>
#include <gametrace.h>
#include <mathlib/vector.h>
#include <ray.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace cs2bv::door_vision {
namespace {
using TraceShapeFn = void(CS2BV_FASTCALL*)(
    const void* self, const Ray_t& ray, const Vector& start, const Vector& end, CTraceFilter* filter, CGameTrace* trace);

TraceShapeFn g_traceShape = nullptr;
std::atomic<int64_t> g_blockedCount{ 0 };

// Recognizes moving door entities, including the breakable rotating-door class.
bool IsDoor(CEntityInstance* entity)
{
    if (!entity || !entity->m_pEntity) return false;
    const char* name = entity->GetClassname();
    return name && (std::strcmp(name, "prop_door_rotating") == 0 || std::strcmp(name, "func_door") == 0 ||
                    std::strcmp(name, "func_door_rotating") == 0);
}

class DoorTraceFilter final : public CTraceFilter
{
    CEntityInstance* m_ignore;

  public:
    // Uses bullet-collidable geometry without testing players, NPCs, or the static world.
    explicit DoorTraceFilter(CEntityInstance* ignore) : CTraceFilter(MASK_SHOT, COLLISION_GROUP_DEFAULT, true), m_ignore(ignore)
    {
        m_nInteractsExclude = CONTENTS_PLAYER | CONTENTS_NPC;
        m_nObjectSetMask = RNQUERY_OBJECTS_KEYFRAMED | RNQUERY_OBJECTS_DYNAMIC;
    }

    // Leaves all non-door visibility decisions to the original bot query.
    bool ShouldHitEntity(CEntityInstance* entity) override { return entity != m_ignore && IsDoor(entity); }
};
} // namespace

// Resolves the same native shape-trace interface used by the existing HE module.
bool Install(const nlohmann::json& gamedata, const modules::ModuleInfo& serverModule)
{
    g_traceShape = nullptr;
    g_blockedCount.store(0, std::memory_order_relaxed);

    const CGameTrace probe;
    const auto base = reinterpret_cast<uintptr_t>(&probe);
    if (reinterpret_cast<uintptr_t>(&probe.m_vStartPos) - base != 0x78 ||
        reinterpret_cast<uintptr_t>(&probe.m_vEndPos) - base != 0x84 ||
        reinterpret_cast<uintptr_t>(&probe.m_flFraction) - base != 0xAC)
    {
        BV_LOG_WARN("Door vision disabled: CGameTrace layout changed");
        return false;
    }

    char error[256]{};
    void** table = modules::ResolveVirtualTable(serverModule, "CNavPhysicsInterface", error, sizeof(error));
    const int index = gameconfig::ResolveOffset(gamedata, "CNavPhysicsInterface::TraceShape", -1);
    if (!table || index < 0 || index >= 64 || !modules::IsExecutableAddress(table[index]))
    {
        BV_LOG_WARN("Door vision disabled: native shape trace unavailable (%s, slot=%d)", error, index);
        return false;
    }
    g_traceShape = reinterpret_cast<TraceShapeFn>(table[index]);
    BV_LOG_DEBUG("Door geometry trace resolved");
    return true;
}

// Stops accepting traces after all visibility callbacks have finished.
void Remove() { g_traceShape = nullptr; }

// Reports native trace availability independently of the visibility hook.
bool IsReady() { return g_traceShape != nullptr; }

// Rejects only actual door hits; openings remain available to the original body-part scan.
bool IsLineBlocked(const float from[3], const float to[3], CEntityInstance* ignore)
{
    if (!g_traceShape || !from || !to) return false;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(from[axis]) || !std::isfinite(to[axis])) return false;
    }

    const Vector start(from[0], from[1], from[2]);
    const Vector end(to[0], to[1], to[2]);
    Ray_t ray;
    DoorTraceFilter filter(ignore);
    CGameTrace trace;
    g_traceShape(nullptr, ray, start, end, &filter, &trace);
    const bool blocked = trace.DidHit() && filter.ShouldHitEntity(trace.m_pEnt);
    if (blocked) g_blockedCount.fetch_add(1, std::memory_order_relaxed);
    return blocked;
}

// Returns all door-blocked sight lines since installation.
int64_t GetBlockedCount() { return g_blockedCount.load(std::memory_order_relaxed); }
} // namespace cs2bv::door_vision
