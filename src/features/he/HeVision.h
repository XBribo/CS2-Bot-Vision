// HE grenade smoke-hole capture and state

#pragma once // NOLINT(portability-avoid-pragma-once)

#include "core/cs2_sdk/src/core/memory_module.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>

namespace cs2bv::he_vision {
constexpr size_t kMaxInfluences = 5;

struct Influence
{
    float center[3];
    float radius;
    float impulse;
    float recovery;
    float begin;
    float finish;
};

// Resolves and installs the optional HE detonation hook
bool Install(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule);

// Removes the HE detonation hook and clears runtime state
void Remove();

// Records an HE detonation as an active smoke hole
void OnDetonate(float x, float y, float z);

// Collects active blasts along a line.
size_t CollectInfluences(const float* from, const float* to, float now, std::span<Influence, kMaxInfluences> influences);

// Returns the blast density scale at a point.
float DensityScale(const float* point, const Influence& influence);

// Sets the initial HE hole radius
void SetRadius(float value);

// Returns the initial HE hole radius
float GetRadius();

// Sets the HE hole lifetime
void SetDuration(float value);

// Returns the number of retained HE holes
int GetActiveCount();

// Returns the HE hook status.
const char* GetHookStatus();
} // namespace cs2bv::he_vision
