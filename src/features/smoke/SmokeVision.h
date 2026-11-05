#pragma once // NOLINT(portability-avoid-pragma-once)

#include "core/cs2_sdk/src/core/memory_module.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>

namespace cs2bv::smoke_vision {
bool Install(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule, char* error, size_t maxLength);
void Remove();
bool IsVolumeMode();

int64_t GetHitCount();

int64_t GetBlockedCount();
const char* GetAutoListStatus();
void SetMode(int mode);
int GetMode();
void SetDensityThreshold(float value);
float GetDensityThreshold();
bool DensityFunctionReady();
void SetBotDensityThreshold(int slot, float value);

// An unset override returns -1.
float GetBotDensityThreshold(int slot);
int GetMaxBots();
int GetLastBotSlot();
bool IsVisiblePosHooked();

int64_t GetIsVisiblePosCalls();
unsigned int GetLastControllerHandle();

uint64_t GetLastPawnPointer();
void AddRevealSlot(int slot);
void RemoveRevealSlot(int slot);
void ClearReveals();

uint64_t GetRevealMask();
unsigned int GetRevealHandle(int slot);
bool IsVisiblePlayerHooked();

// Returns bytes written, excluding the terminating NUL.
int TestLos(float fromX, float fromY, float fromZ, float toX, float toY, float toZ, char* buffer, size_t bufferLength);
} // namespace cs2bv::smoke_vision
