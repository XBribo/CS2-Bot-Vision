#pragma once
#include "SmokeEffects.h"
#include <cs2_sdk/memory_module.h>
#include <nlohmann/json_fwd.hpp>

struct Ray_t;
class Vector;
class CTraceFilter;
class CGameTrace;

namespace cs2bv::smoke_bindings {
using GetSmokeDensityInLine = float (*)(const float*, const float*, float*);
using PointDensity = float (*)(void*, const smoke_effects::Vec3*);
using TraceShape = void (*)(const void*, const Ray_t&, const Vector&, const Vector&, CTraceFilter*, CGameTrace*);
using GetSlot = void* (*)(void*, int, unsigned int);

struct Configuration
{
    std::size_t volumeOffset = 0, nextOffset = 0, volumeTimeOffset = 0, volumeOriginOffset = 0;
    std::size_t sourceCountOffset = 0, sourceDataOffset = 0, sourceCapacityOffset = 0;
};
struct Fields
{
    std::size_t effectOrigin = 0, effectTick = 0, smokeTick = 0, didSmoke = 0;
};
struct BulletBindings
{
    std::size_t shooter = 0;
    std::size_t weapons = 0, activeWeapon = 0, attributes = 0, item = 0, definition = 0;
    void *pelletTrace = nullptr, *pelletResult = nullptr;
    PointDensity pointDensity = nullptr;
    GetSlot getSlot = nullptr;
};

struct Bindings
{
    Configuration config;
    Fields fields;
    BulletBindings bullets;
    void *visibility = nullptr, *heDetonate = nullptr, *publish = nullptr;
    void *traverse = nullptr, *initialize = nullptr, *ready = nullptr, *destructor = nullptr;
    GetSmokeDensityInLine density = nullptr;
    TraceShape traceShape = nullptr;
    void* const* smokeListHead = nullptr;
    const void* visitor = nullptr;
};

bool Resolve(const nlohmann::json& data, const cs2_sdk::modules::ModuleInfo& server, Bindings& bindings, char* error, size_t maxLength);
bool ResolveBullets(const nlohmann::json& data, const cs2_sdk::modules::ModuleInfo& server, BulletBindings& bullets, char* error, size_t maxLength);
} // namespace cs2bv::smoke_bindings
