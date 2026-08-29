// Bullet trace capture and temporary smoke tunnels

#pragma once // NOLINT(portability-avoid-pragma-once)

#include "sig_scan.h"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace cs2bv::bullet_vision {
using DensitySamplerFn = float (*)(const float* from, const float* to);

// Resolves and installs optional bullet capture facilities
bool Install(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule);

// Removes the bullet hook and clears runtime state
void Remove();

// Tests a line with the HE-to-smoke collision mask
bool IsLineUnobstructed(const float* from, const float* to);

// Records one temporary bullet smoke tunnel
void OnHole(const float start[3], const float end[3], float radius);

// Applies the bullet tunnel model to native line density
float AdjustDensity(const float* from, const float* to, float density, DensitySamplerFn sampler);

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

// Returns the number of captured pellet hook calls
int64_t GetBulletCount();

// Formats the most recently captured pellet
const char* GetLastBulletInfo();

// Formats bullet capture diagnostics
const char* GetDiagnostics();

// Formats the active weapon probe
const char* GetWeaponProbe();
} // namespace cs2bv::bullet_vision
