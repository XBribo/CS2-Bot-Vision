#pragma once
#include "SmokeEffects.h"
#include "core/cs2_sdk/src/core/memory_module.h"
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
    std::size_t pelletShooterOffset = 0;
};
struct Fields
{
    std::size_t effectOrigin = 0, effectTick = 0, smokeTick = 0, didSmoke = 0;
    std::size_t weapons = 0, activeWeapon = 0, attributes = 0, item = 0, definition = 0;
};

struct Bindings
{
    Configuration config;
    Fields fields;
    void *visibility = nullptr, *heDetonate = nullptr, *pelletTrace = nullptr, *publish = nullptr;
    void *pelletResult = nullptr, *traverse = nullptr, *initialize = nullptr, *ready = nullptr, *destructor = nullptr;
    GetSmokeDensityInLine density = nullptr;
    PointDensity pointDensity = nullptr;
    TraceShape traceShape = nullptr;
    GetSlot getSlot = nullptr;
    void* const* smokeListHead = nullptr;
    const void* visitor = nullptr;
};

bool Resolve(const nlohmann::json& data, const cs2_sdk::modules::ModuleInfo& server, Bindings& bindings, char* error, size_t maxLength);
} // namespace cs2bv::smoke_bindings
