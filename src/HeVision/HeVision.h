// HE grenade smoke-hole capture and state

#pragma once // NOLINT(portability-avoid-pragma-once)

#include "sig_scan.h"

#include <nlohmann/json.hpp>

namespace cs2bv::he_vision {
using DensitySamplerFn = float (*)(const float* from, const float* to);

// Resolves and installs the optional HE detonation hook
bool Install(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule);

// Removes the HE detonation hook and clears runtime state
void Remove();

// Records an HE detonation as an active smoke hole
void OnDetonate(float x, float y, float z);

// Applies the HE smoke-hole model to native line density
float AdjustDensity(const float* from, const float* to, float density, DensitySamplerFn sampler);

// Sets the initial HE hole radius
void SetRadius(float value);

// Returns the initial HE hole radius
float GetRadius();

// Sets the HE hole lifetime
void SetDuration(float value);

// Returns the HE hole lifetime
float GetDuration();

// Returns the number of retained HE holes
int GetActiveCount();

// Overrides the legacy listener diagnostic state
void SetListenerStatus(bool managerResolved, bool listenerAdded);

// Returns the HE hook or listener diagnostic state
const char* GetListenerStatus();
} // namespace cs2bv::he_vision
