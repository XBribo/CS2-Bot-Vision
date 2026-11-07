#pragma once // NOLINT(portability-avoid-pragma-once)

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace cs2bv::bot_vision {
bool Install(const std::string& gamedataPath, char* error = nullptr, size_t maxLength = 0);
void Remove();

// HE and bullet counts share a timestamp.
std::pair<int, int> GetActiveEffectCounts();
const char* GetModelStatus();
void SetGeometryCacheEnabled(bool enabled);
bool GetGeometryCacheEnabled();
uint64_t GetGeometryCacheHits();
uint64_t GetGeometryCacheMisses();
void SetBulletHolesEnabled(bool enabled);
bool GetBulletHolesEnabled();

int64_t GetHitCount();

int64_t GetBlockedCount();
const char* GetAutoListStatus();

// Returns bytes written, excluding the terminating NUL.
int TestLos(float fromX, float fromY, float fromZ, float toX, float toY, float toZ, char* buffer, size_t bufferLength);
void SetSmokeMode(int mode);
int GetSmokeMode();
void SetDensityThreshold(float value);
float GetDensityThreshold();
bool IsDensityFnResolved();
const char* GetSafeReadDiag();
void SetBotDensityThreshold(int slot, float value);
float GetBotDensityThreshold(int slot);
int GetMaxBots();
int GetLastBotSlot();
bool IsVisiblePosHooked();

int64_t GetIsVisiblePosCalls();
unsigned int GetLastCtrlHandle();

uint64_t GetLastPawnPtr();
void AddRevealSlot(int slot);
void RemoveRevealSlot(int slot);
void ClearReveals();

uint64_t GetRevealMask();
unsigned int GetRevealHandle(int slot);
bool IsVisiblePlayerHooked();
} // namespace cs2bv::bot_vision
