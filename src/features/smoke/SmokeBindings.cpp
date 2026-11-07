#include "SmokeBindings.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/cs2_sdk/src/utils/rtti.h"
#include "utils/memory.h"
#include "plugin.h"
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <gametrace.h>
#include <schemasystem/schemasystem.h>

namespace cs2bv::smoke_bindings {
namespace {
using memory::IsReadable;
using smoke_effects::Vec3;
#ifdef _WIN32
constexpr const char* kDestructorClass = "CHEGrenadeProjectile";
#else
constexpr const char* kDestructorClass = "CBaseCSGrenadeProjectile";
#endif
void SetError(char* error, std::size_t maxLength, const char* format, ...)
{
    if (!error || maxLength == 0) return;
    va_list args;
    va_start(args, format);
    std::vsnprintf(error, maxLength, format, args);
    va_end(args);
}

constexpr size_t kMaxFieldSpan = 65536;
bool FitsField(int value, size_t width, size_t span) { return value > 0 && width <= span && static_cast<size_t>(value) <= span - width; }

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
const void* ResolveRipTarget(void* site,
                             const std::array<uint8_t, 3>& opcode,
                             const cs2_sdk::modules::ModuleInfo& region,
                             const char* name,
                             char* error,
                             size_t maxLength)
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

} // namespace

bool Resolve(const nlohmann::json& data, const cs2_sdk::modules::ModuleInfo& server, Bindings& bindings, char* error, size_t maxLength)
{
    Bindings resolved;
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
                      ? g_schemaSystem->FindTypeScopeForModule(cs2_sdk::modules::kSchemaServerTypeScope, nullptr)
                      : nullptr;
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
        { "CBaseCSGrenadeProjectile", "m_vecExplodeEffectOrigin", sizeof(Vec3), &resolved.fields.effectOrigin },
        { "CBaseCSGrenadeProjectile", "m_nExplodeEffectTickBegin", sizeof(int), &resolved.fields.effectTick },
        { "CSmokeGrenadeProjectile", "m_nSmokeEffectTickBegin", sizeof(int), &resolved.fields.smokeTick },
        { "CSmokeGrenadeProjectile", "m_bDidSmokeEffect", sizeof(bool), &resolved.fields.didSmoke },
        { "CBasePlayerPawn", "m_pWeaponServices", sizeof(void*), &resolved.fields.weapons },
        { "CPlayer_WeaponServices", "m_hActiveWeapon", sizeof(std::uint32_t), &resolved.fields.activeWeapon },
        { "CEconEntity", "m_AttributeManager", sizeof(void*), &resolved.fields.attributes },
        { "CAttributeContainer", "m_Item", sizeof(void*), &resolved.fields.item },
        { "CEconItemView", "m_iItemDefinitionIndex", sizeof(std::uint16_t), &resolved.fields.definition },
    };
    for (const auto& field : fields)
        if (!ResolveOffset(field.type, field.name, field.width, *field.offset, error, maxLength)) return false;
    // List and volume offsets follow m_bDidSmokeEffect.
    if (resolved.fields.didSmoke < 12 || config.nextOffset != resolved.fields.didSmoke - 12 ||
        config.volumeOffset != resolved.fields.didSmoke + 84)
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
        { "CBotManager::IsVisibleThroughSmoke", address.visibility }, { "GetSmokeDensityInLine", address.density },
        { "CHEGrenadeProjectile::Detonate", address.heDetonate },     { "BulletPelletTrace", address.pelletTrace },
        { "g_AutoList_SmokeProj_Head_Server", address.listSite },     { "CBaseCSGrenadeProjectile::PublishEffect", address.publish },
        { "BulletPelletTraceResult", address.pelletResult },          { "SmokeVolume::Traverse", address.traverse },
        { "SmokeVolume::DensityVisitor", address.visitorSite },       { "SmokeVolume::PointDensity", address.pointDensity },
        { "CCSPlayer_WeaponServices::GetSlot", address.getSlot },     { "SmokeVolume::InitializeReady", address.initialize },
        { "CSmokeGrenadeProjectile::Ready", address.ready },
    };
    for (const auto& binding : required)
    {
        binding.entry = cs2_sdk::gameconfig::ResolveSig(data, server, binding.name, error, maxLength);
        if (!binding.entry) return false;
    }
#ifdef _WIN32
    constexpr std::array<uint8_t, 3> visitorLea{ 0x48, 0x8d, 0x0d }; // lea rcx, [visitor vtable]
    const auto& identityRegion = cs2_sdk::modules::server->Image();
#else
    constexpr std::array<uint8_t, 3> visitorLea{ 0x48, 0x8d, 0x05 }; // lea rax, [visitor invoker]
    const auto& identityRegion = server;
#endif
    const auto* visitor = ResolveRipTarget(address.visitorSite, visitorLea, identityRegion, "density visitor", error, maxLength);
    if (!visitor) return false;
    // Linux TraceShape supplies the physics object.
    auto* traceShape = ResolveVirtualSlot(data, "CNavPhysicsInterface", "CNavPhysicsInterface::TraceShape", error, maxLength);
    if (!traceShape) return false;
    auto* destructor = ResolveVirtualSlot(data, kDestructorClass, "GrenadeProjectile::Destructor", error, maxLength);
    if (!destructor) return false;
    auto head = static_cast<void* const*>(
        ResolveRipTarget(address.listSite, { 0x48, 0x8b, 0x1d }, cs2_sdk::modules::server->Image(), "smoke list head", error, maxLength));
    if (!head) return false;

    resolved.config = config;
    resolved.visibility = address.visibility;
    resolved.heDetonate = address.heDetonate;
    resolved.pelletTrace = address.pelletTrace;
    resolved.publish = address.publish;
    resolved.pelletResult = address.pelletResult;
    resolved.traverse = address.traverse;
    resolved.initialize = address.initialize;
    resolved.ready = address.ready;
    resolved.destructor = destructor;
    resolved.density = reinterpret_cast<GetSmokeDensityInLine>(address.density);
    resolved.pointDensity = reinterpret_cast<PointDensity>(address.pointDensity);
    resolved.traceShape = reinterpret_cast<TraceShape>(traceShape);
    resolved.getSlot = reinterpret_cast<GetSlot>(address.getSlot);
    resolved.smokeListHead = head;
    resolved.visitor = visitor;
    bindings = resolved;
    return true;
}
} // namespace cs2bv::smoke_bindings
