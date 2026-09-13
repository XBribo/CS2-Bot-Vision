#include "core/config.h"
#include "core/log.h"
#include "core/gameconfig.h"
// BotVision module coordinator and public runtime API

#include "BotVision.h"

#include "features/bullet/BulletVision.h"
#include "features/he/HeVision.h"
#include "features/smoke/SmokeVision.h"
#include "game_time.h"
#include "memory.h"
#include "core/cs2_sdk/schema.h"
#include "core/memory_module.h"

#include <nlohmann/json.hpp>
#include <tier0/dbg.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace cs2bv::bot_vision {
// Loads gamedata and installs the required and optional modules
bool Install(const std::string& gamedataPath, void* serverInterface, char* error, size_t maxLength)
{
    nlohmann::json gamedata;
    if (!gameconfig::LoadGamedata(gamedataPath.c_str(), gamedata))
    {
        const char* format = "failed to read/parse gamedata.json at %s";
        if (error && maxLength > 0)
        {
            std::snprintf(error, maxLength, format, gamedataPath.c_str());
        }
        char message[512];
        std::snprintf(message, sizeof(message), "[BotVision] failed to read/parse gamedata.json at %s\n", gamedataPath.c_str());
        BV_LOG_INFO("%s", message);
        return false;
    }

    config::LoadStartupConfig(gamedataPath);

    modules::ModuleInfo serverModule = modules::ModuleFromInterfacePtr(serverInterface);
    if (!serverModule)
    {
#ifdef _WIN32
        serverModule = modules::ModuleFromName("server.dll");
#else
        serverModule = modules::ModuleFromName("libserver.so");
#endif
    }
    if (!serverModule)
    {
        if (error && maxLength > 0)
        {
            std::snprintf(error, maxLength, "could not resolve CS2 server module from interface ptr=%p", serverInterface);
        }
        char message[256];
        std::snprintf(message, sizeof(message), "[BotVision] could not resolve CS2 server module from interface ptr=%p\n", serverInterface);
        BV_LOG_INFO("%s", message);
        return false;
    }

    if (!schema::Init())
    {
        BV_LOG_WARN("%s", "[BotVision] WARN: SchemaSystem unavailable; optional features disabled\n");
    }

    if (!smoke_vision::Install(gamedata, serverModule, error, maxLength)) return false;

    he_vision::Install(gamedata, serverModule);
    bullet_vision::Install(gamedata, serverModule);
    return true;
}

// Removes modules in reverse dependency order
void Remove()
{
    bullet_vision::Remove();
    he_vision::Remove();
    smoke_vision::Remove();

    char message[160];
    std::snprintf(message, sizeof(message), "[BotVision] removed: hits=%lld blocked=%lld\n", GetHitCount(), GetBlockedCount());
    BV_LOG_INFO("%s", message);
}

// Stores the engine interface for shared server time
void SetEngine(void* engine) { game_time::SetEngine(engine); }

// Forwards an HE detonation to HE state
void OnHeDetonate(float x, float y, float z) { he_vision::OnDetonate(x, y, z); }

// Sets the HE hole radius
void SetHeRadius(float value) { he_vision::SetRadius(value); }

// Returns the HE hole radius
float GetHeRadius() { return he_vision::GetRadius(); }

// Sets the HE hole lifetime
void SetHeDuration(float value) { he_vision::SetDuration(value); }

// Returns the HE hole lifetime
float GetHeDuration() { return he_vision::GetDuration(); }

// Returns the active HE hole count
int GetActiveBlastCount() { return he_vision::GetActiveCount(); }

// Stores the legacy HE listener diagnostic
void SetHeListenerStatus(bool managerResolved, bool listenerAdded) { he_vision::SetListenerStatus(managerResolved, listenerAdded); }

// Returns the HE diagnostic state
const char* GetHeListenerStatus() { return he_vision::GetListenerStatus(); }

// Forwards a bullet tunnel to bullet state
void OnBulletHole(const float start[3], const float end[3], float radius) { bullet_vision::OnHole(start, end, radius); }

// Sets the normal bullet tunnel radius
void SetBulletRadius(float value) { bullet_vision::SetRadius(value); }

// Returns the normal bullet tunnel radius
float GetBulletRadius() { return bullet_vision::GetRadius(); }

// Sets the shotgun bullet tunnel radius
void SetBulletRadiusShotgun(float value) { bullet_vision::SetShotgunRadius(value); }

// Returns the shotgun bullet tunnel radius
float GetBulletRadiusShotgun() { return bullet_vision::GetShotgunRadius(); }

// Returns the active weapon diagnostic
const char* GetWeaponProbe() { return bullet_vision::GetWeaponProbe(); }

// Sets the bullet tunnel lifetime
void SetBulletDuration(float value) { bullet_vision::SetDuration(value); }

// Returns the bullet tunnel lifetime
float GetBulletDuration() { return bullet_vision::GetDuration(); }

// Stores the bullet tunnel enabled state
void SetBulletHolesEnabled(bool enabled) { bullet_vision::SetHolesEnabled(enabled); }

// Returns the bullet tunnel enabled state
bool GetBulletHolesEnabled() { return bullet_vision::GetHolesEnabled(); }

// Returns the active bullet tunnel count
int GetActiveBulletHoleCount() { return bullet_vision::GetActiveHoleCount(); }

// Returns bullet capture diagnostics
const char* GetBulletDiag() { return bullet_vision::GetDiagnostics(); }

// Returns the captured pellet count
int64_t GetBulletCount() { return bullet_vision::GetBulletCount(); }

// Returns the last captured pellet diagnostic
const char* GetLastBulletInfo() { return bullet_vision::GetLastBulletInfo(); }

// Returns the smoke hook call count
int64_t GetHitCount() { return smoke_vision::GetHitCount(); }

// Returns the smoke-blocked line count
int64_t GetBlockedCount() { return smoke_vision::GetBlockedCount(); }

// Checks whether the smoke auto-list was resolved
bool IsHookedActive() { return smoke_vision::AutoListReady(); }

// Returns the smoke hook diagnostic
const char* GetHookedStatus() { return smoke_vision::GetHookedStatus(); }

// Forwards a diagnostic smoke density query
int TestLos(float fromX, float fromY, float fromZ, float toX, float toY, float toZ, char* buffer, size_t bufferLength)
{
    return smoke_vision::TestLos(fromX, fromY, fromZ, toX, toY, toZ, buffer, bufferLength);
}

// Sets the smoke calculation mode
void SetSmokeMode(int mode) { smoke_vision::SetMode(mode); }

// Returns the smoke calculation mode
int GetSmokeMode() { return smoke_vision::GetMode(); }

// Sets the global density threshold
void SetDensityThreshold(float value) { smoke_vision::SetDensityThreshold(value); }

// Returns the global density threshold
float GetDensityThreshold() { return smoke_vision::GetDensityThreshold(); }

// Checks whether engine density calculation is ready
bool IsDensityFnResolved() { return smoke_vision::DensityFunctionReady(); }

// Returns validated memory read diagnostics
const char* GetSafeReadDiag() { return memory::Diagnostics(); }

// Sets or clears a bot-specific density threshold
void SetBotDensityThreshold(int slot, float value) { smoke_vision::SetBotDensityThreshold(slot, value); }

// Returns one bot-specific density threshold
float GetBotDensityThreshold(int slot) { return smoke_vision::GetBotDensityThreshold(slot); }

// Returns the supported bot slot count
int GetMaxBots() { return smoke_vision::GetMaxBots(); }

// Returns the last bot slot observed by the visibility hook
int GetLastBotSlot() { return smoke_vision::GetLastBotSlot(); }

// Checks whether the per-bot hook is active
bool IsVisiblePosHooked() { return smoke_vision::IsVisiblePosHooked(); }

// Returns the per-bot hook call count
int64_t GetIsVisiblePosCalls() { return smoke_vision::GetIsVisiblePosCalls(); }

// Returns the last observed controller handle
unsigned int GetLastCtrlHandle() { return smoke_vision::GetLastControllerHandle(); }

// Returns the last observed pawn pointer
uint64_t GetLastPawnPtr() { return smoke_vision::GetLastPawnPointer(); }

// Adds one smoke reveal slot
void AddRevealSlot(int slot) { smoke_vision::AddRevealSlot(slot); }

// Removes one smoke reveal slot
void RemoveRevealSlot(int slot) { smoke_vision::RemoveRevealSlot(slot); }

// Clears all smoke reveals
void ClearReveals() { smoke_vision::ClearReveals(); }

// Returns the configured smoke reveal mask
uint64_t GetRevealMask() { return smoke_vision::GetRevealMask(); }

// Returns one revealed player handle
unsigned int GetRevealHandle(int slot) { return smoke_vision::GetRevealHandle(slot); }

// Checks whether player visibility is hooked
bool IsVisiblePlayerHooked() { return smoke_vision::IsVisiblePlayerHooked(); }
} // namespace cs2bv::bot_vision
