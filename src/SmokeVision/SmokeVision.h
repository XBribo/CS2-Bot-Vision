// Smoke visibility hooks and per-bot threshold state

#pragma once

#include "sig_scan.h"

#include <nlohmann/json.hpp>

#include <cstddef>

namespace cs2bv::SmokeVision {
// Installs the required smoke hook and optional per-bot hook
bool Install(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule, char* error, size_t maxLength);

// Removes all smoke visibility hooks
void Remove();

// Reports whether volume-smoke mode is selected
bool IsVolumeMode();

// Reports whether the smoke projectile list was resolved
bool AutoListReady();

// Reports whether the smoke projectile list is nonempty
bool HasSmokeProjectiles();

// Returns engine smoke density for a line or zero when unavailable
float DensityInLine(const float* from, const float* to);

// Returns the number of smoke hook calls
long long GetHitCount();

// Returns the number of lines blocked by density
long long GetBlockedCount();

// Returns the smoke hook diagnostic state
const char* GetHookedStatus();

// Sets the active smoke calculation mode
void SetMode(int mode);

// Returns the active smoke calculation mode
int GetMode();

// Sets the global smoke density threshold
void SetDensityThreshold(float value);

// Returns the global smoke density threshold
float GetDensityThreshold();

// Reports whether the engine density function was resolved
bool DensityFunctionReady();

// Sets or clears a bot-specific density threshold
void SetBotDensityThreshold(int slot, float value);

// Returns a bot threshold or negative one when unset
float GetBotDensityThreshold(int slot);

// Returns the supported bot slot count
int GetMaxBots();

// Returns the last bot slot observed by the per-bot hook
int GetLastBotSlot();

// Reports whether the per-bot visibility hook is active
bool IsVisiblePosHooked();

// Returns the per-bot hook call count
long long GetIsVisiblePosCalls();

// Returns the last controller handle observed by the per-bot hook
unsigned int GetLastControllerHandle();

// Returns the last pawn pointer observed by the per-bot hook
unsigned long long GetLastPawnPointer();

// Adds one player slot to the smoke reveal set
void AddRevealSlot(int slot);

// Removes one player slot from the smoke reveal set
void RemoveRevealSlot(int slot);

// Clears every smoke reveal
void ClearReveals();

// Returns the selected reveal slot mask
unsigned long long GetRevealMask();

// Returns one revealed player's latched controller handle
unsigned int GetRevealHandle(int slot);

// Reports whether the player visibility hook is active
bool IsVisiblePlayerHooked();

// Formats a smoke density test for one line
int TestLos(float fromX, float fromY, float fromZ, float toX, float toY, float toZ, char* buffer, size_t bufferLength);
} // namespace cs2bv::SmokeVision
