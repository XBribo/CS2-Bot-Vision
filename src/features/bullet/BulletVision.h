// Bullet trace capture and temporary smoke tunnels

#pragma once // NOLINT(portability-avoid-pragma-once)

#include "core/cs2_sdk/src/core/memory_module.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>

namespace cs2bv::bullet_vision {
constexpr size_t kMaxInfluences = 16;

struct Influence
{
    float start[3];
    float end[3];
    float radius;
    float begin;
    float finish;
};

// Resolves and installs optional bullet capture facilities
bool Install(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule);

// Removes the bullet hook and clears runtime state
void Remove();

// Tests a line with the HE-to-smoke collision mask
bool IsLineUnobstructed(const float* from, const float* to);

// Records one temporary bullet smoke tunnel
void OnHole(const float start[3], const float end[3], float radius);

// Collects active tunnels along a line.
size_t CollectInfluences(const float* from, const float* to, float now, std::span<Influence, kMaxInfluences> influences);

// Returns the tunnel density scale at a point.
float DensityScale(const float* point, const Influence& influence);

// Sets the normal bullet tunnel radius
void SetRadius(float value);

// Returns the normal bullet tunnel radius
float GetRadius();

// Sets the shotgun bullet tunnel radius
void SetShotgunRadius(float value);

// Returns the shotgun bullet tunnel radius
float GetShotgunRadius();

// Sets the bullet tunnel lifetime
void SetDuration(float value);

// Returns the bullet tunnel lifetime
float GetDuration();

// Enables or disables bullet smoke tunnels
void SetHolesEnabled(bool enabled);

// Reports whether bullet smoke tunnels are enabled
bool GetHolesEnabled();

// Returns the number of retained bullet tunnels
int GetActiveHoleCount();

} // namespace cs2bv::bullet_vision
