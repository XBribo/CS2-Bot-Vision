// BotVision module coordinator and public runtime API

#pragma once // NOLINT(portability-avoid-pragma-once)

#include <cstddef>
#include <cstdint>
#include <string>

namespace cs2bv::bot_vision {
// Smoke hooks are required; HE and bullet hooks may be unavailable.
bool Install(const std::string& gamedataPath, char* error = nullptr, size_t maxLength = 0);

// Removes bullet and HE hooks before the smoke hooks they depend on.
void Remove();

// Sets the HE hole radius
void SetHeRadius(float value);

// Sets the HE hole lifetime
void SetHeDuration(float value);

// Returns the retained HE hole count
int GetActiveBlastCount();

// Returns the HE hook status.
const char* GetHeHookStatus();

// Sets the normal bullet tunnel radius
void SetBulletRadius(float value);

// Sets the shotgun bullet tunnel radius
void SetBulletRadiusShotgun(float value);

// Sets the bullet tunnel lifetime
void SetBulletDuration(float value);

// Enables or disables bullet smoke tunnels
void SetBulletHolesEnabled(bool enabled);

// Reports whether bullet smoke tunnels are enabled
bool GetBulletHolesEnabled();

// Returns the retained bullet tunnel count
int GetActiveBulletHoleCount();

// Returns the smoke hook call count
int64_t GetHitCount();

// Returns the smoke-blocked line count
int64_t GetBlockedCount();

// Returns the smoke auto-list status.
const char* GetAutoListStatus();

// Formats a diagnostic smoke density query
int TestLos(float fromX, float fromY, float fromZ, float toX, float toY, float toZ, char* buffer, size_t bufferLength);

// Sets the smoke calculation mode
void SetSmokeMode(int mode);

// Returns the smoke calculation mode
int GetSmokeMode();

// Sets the global density threshold
void SetDensityThreshold(float value);

// Returns the global density threshold
float GetDensityThreshold();

// Reports whether engine density calculation is available
bool IsDensityFnResolved();

// Returns validated memory read diagnostics
const char* GetSafeReadDiag();

// Sets or clears one bot-specific density threshold
void SetBotDensityThreshold(int slot, float value);

// Returns one bot-specific density threshold
float GetBotDensityThreshold(int slot);

// Returns the supported bot slot count
int GetMaxBots();

// Returns the last bot slot observed by the visibility hook
int GetLastBotSlot();

// Reports whether the per-bot visibility hook is active
bool IsVisiblePosHooked();

// Returns the per-bot visibility hook call count
int64_t GetIsVisiblePosCalls();

// Returns the last observed controller handle
unsigned int GetLastCtrlHandle();

// Returns the last observed pawn pointer
uint64_t GetLastPawnPtr();

// Adds one player slot to the smoke reveal set
void AddRevealSlot(int slot);

// Removes one player slot from the smoke reveal set
void RemoveRevealSlot(int slot);

// Clears every smoke reveal
void ClearReveals();

// Returns the selected reveal slot mask
uint64_t GetRevealMask();

// Returns one revealed player's latched controller handle
unsigned int GetRevealHandle(int slot);

// Reports whether the player visibility hook is active
bool IsVisiblePlayerHooked();
} // namespace cs2bv::bot_vision
