#pragma once
#include "core/cs2_sdk/src/core/memory_module.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <utility>

namespace cs2bv::smoke_density {
bool Install(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& server, void*& visibilityTarget, char* error, size_t maxLength);
void Remove();
float Density(const float* from, const float* to, bool corrected);
bool Ready();
bool ListResolved();
const char* Status();
// Retained HE and bullet counts at the same game time.
std::pair<int, int> ActiveEffectCounts();
void EnableBullets(bool enabled);
bool BulletsEnabled();
void EnableGeometryCache(bool enabled);
bool GeometryCacheEnabled();
uint64_t CacheHits();
uint64_t CacheMisses();
} // namespace cs2bv::smoke_density
