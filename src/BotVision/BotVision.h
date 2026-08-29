// BotVision module coordinator and public runtime API

#pragma once // NOLINT(portability-avoid-pragma-once)

#include <cstddef>
#include <cstdint>
#include <string>

namespace cs2bv::bot_vision {
// Loads gamedata and installs all vision modules
bool Install(const std::string& gamedataPath, void* serverInterface, char* error = nullptr, size_t maxLength = 0);

// Removes all installed vision modules
void Remove();

// Stores the engine interface used for server time
void SetEngine(void* engine);

// Records an HE detonation
void OnHeDetonate(float x, float y, float z);

// Sets the HE hole radius
void SetHeRadius(float value);

// Returns the HE hole radius
float GetHeRadius();

// Sets the HE hole lifetime
void SetHeDuration(float value);

// Returns the HE hole lifetime
float GetHeDuration();

// Returns the retained HE hole count
int GetActiveBlastCount();

// Overrides the legacy HE listener diagnostic
void SetHeListenerStatus(bool managerResolved, bool listenerAdded);

// Returns the HE hook or listener diagnostic
const char* GetHeListenerStatus();

// Records one bullet smoke tunnel
void OnBulletHole(const float start[3], const float end[3], float radius);

// Sets the normal bullet tunnel radius
void SetBulletRadius(float value);

// Returns the normal bullet tunnel radius
float GetBulletRadius();

// Sets the shotgun bullet tunnel radius
void SetBulletRadiusShotgun(float value);

// Returns the shotgun bullet tunnel radius
float GetBulletRadiusShotgun();

// Returns the active weapon probe
const char* GetWeaponProbe();

// Sets the bullet tunnel lifetime
void SetBulletDuration(float value);

// Returns the bullet tunnel lifetime
float GetBulletDuration();

// Enables or disables bullet smoke tunnels
void SetBulletHolesEnabled(bool enabled);

// Reports whether bullet smoke tunnels are enabled
bool GetBulletHolesEnabled();

// Returns the retained bullet tunnel count
int GetActiveBulletHoleCount();

// Returns bullet capture diagnostics
const char* GetBulletDiag();

// Returns the pellet hook call count
int64_t GetBulletCount();

// Returns the most recently captured pellet
const char* GetLastBulletInfo();

// Returns the smoke hook call count
int64_t GetHitCount();

// Returns the smoke-blocked line count
int64_t GetBlockedCount();

// Reports whether the smoke auto-list was resolved
bool IsHookedActive();

// Returns the smoke hook diagnostic
const char* GetHookedStatus();

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
