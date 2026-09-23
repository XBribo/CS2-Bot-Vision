// Additional door occlusion for otherwise visible bot sight lines

#pragma once // NOLINT(portability-avoid-pragma-once)

#include "core/memory_module.h"

#include <nlohmann/json.hpp>

#include <cstdint>

class CEntityInstance;

namespace cs2bv::door_vision {
// Resolves the native shape trace without changing door collision attributes.
bool Install(const nlohmann::json& gamedata, const modules::ModuleInfo& serverModule);

// Clears the native trace after visibility hooks have been removed.
void Remove();

// Reports whether door collision queries are available.
bool IsReady();

// Tests remaining door geometry rather than its BLOCK_LOS flag or bounding box.
bool IsLineBlocked(const float from[3], const float to[3], CEntityInstance* ignore = nullptr);

// Returns the number of sight lines blocked by a door.
int64_t GetBlockedCount();
} // namespace cs2bv::door_vision
