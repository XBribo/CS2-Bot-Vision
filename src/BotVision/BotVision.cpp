// BotVision module coordinator and public runtime API

#include "BotVision.h"

#include "BulletVision/BulletVision.h"
#include "HeVision/HeVision.h"
#include "SmokeVision/SmokeVision.h"
#include "game_time.h"
#include "memory.h"
#include "platform.h"
#include "schema_resolver.h"
#include "sig_scan.h"

#include <nlohmann/json.hpp>
#include <tier0/dbg.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace cs2bv::bot_vision {
namespace {
constexpr int kDefaultSmokeMode = 0;
constexpr float kDefaultDensityThreshold = 0.23f;
constexpr float kDefaultHeRadius = 250.0f;
constexpr float kDefaultHeDuration = 5.0f;
constexpr bool kDefaultBulletHolesEnabled = true;
constexpr float kDefaultBulletRadius = 20.0f;
constexpr float kDefaultShotgunRadius = 80.0f;
constexpr float kDefaultBulletDuration = 1.0f;
constexpr float kDefaultBulletRange = 8192.0f;
constexpr double kMaximumFixedPointValue = static_cast<double>(std::numeric_limits<int>::max()) / 1000.0 - 1.0;

// Builds the packaged startup configuration
nlohmann::json BuildDefaultConfig()
{
    return { { "smoke", { { "mode", kDefaultSmokeMode }, { "density_threshold", kDefaultDensityThreshold } } },
             { "he_holes", { { "radius", kDefaultHeRadius }, { "duration", kDefaultHeDuration } } },
             { "bullet_holes",
               { { "enabled", kDefaultBulletHolesEnabled },
                 { "radius", kDefaultBulletRadius },
                 { "shotgun_radius", kDefaultShotgunRadius },
                 { "duration", kDefaultBulletDuration },
                 { "range", kDefaultBulletRange } } } };
}

// Reports one invalid startup setting
void WarnInvalidSetting(const char* setting)
{
    char message[192];
    std::snprintf(message, sizeof(message), "[BotVision] WARN: invalid config setting '%s'; using default\n", setting);
    Msg("%s", message);
}

// Applies one nonnegative fixed-point startup setting
void ApplyNonNegativeFloat(const nlohmann::json& section, const char* key, const char* setting, void (*setter)(float))
{
    if (!section.contains(key)) return;

    const auto& value = section[key];
    if (!value.is_number())
    {
        WarnInvalidSetting(setting);
        return;
    }

    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0.0 || number > kMaximumFixedPointValue)
    {
        WarnInvalidSetting(setting);
        return;
    }
    setter(static_cast<float>(number));
}

// Applies validated startup settings over the active defaults
void ApplyStartupConfig(const nlohmann::json& config)
{
    if (config.contains("smoke"))
    {
        const auto& smoke = config["smoke"];
        if (!smoke.is_object())
        {
            WarnInvalidSetting("smoke");
        }
        else
        {
            if (smoke.contains("mode"))
            {
                const auto& mode = smoke["mode"];
                if (mode.is_number_integer() && (mode == 0 || mode == 1))
                {
                    SetSmokeMode(mode.get<int>());
                }
                else
                {
                    WarnInvalidSetting("smoke.mode");
                }
            }
            ApplyNonNegativeFloat(smoke, "density_threshold", "smoke.density_threshold", SetDensityThreshold);
        }
    }

    if (config.contains("he_holes"))
    {
        const auto& heHoles = config["he_holes"];
        if (!heHoles.is_object())
        {
            WarnInvalidSetting("he_holes");
        }
        else
        {
            ApplyNonNegativeFloat(heHoles, "radius", "he_holes.radius", SetHeRadius);
            ApplyNonNegativeFloat(heHoles, "duration", "he_holes.duration", SetHeDuration);
        }
    }

    if (config.contains("bullet_holes"))
    {
        const auto& bulletHoles = config["bullet_holes"];
        if (!bulletHoles.is_object())
        {
            WarnInvalidSetting("bullet_holes");
        }
        else
        {
            if (bulletHoles.contains("enabled"))
            {
                if (bulletHoles["enabled"].is_boolean()) SetBulletHolesEnabled(bulletHoles["enabled"].get<bool>());
                else
                    WarnInvalidSetting("bullet_holes.enabled");
            }
            ApplyNonNegativeFloat(bulletHoles, "radius", "bullet_holes.radius", SetBulletRadius);
            ApplyNonNegativeFloat(bulletHoles, "shotgun_radius", "bullet_holes.shotgun_radius", SetBulletRadiusShotgun);
            ApplyNonNegativeFloat(bulletHoles, "duration", "bullet_holes.duration", SetBulletDuration);
            ApplyNonNegativeFloat(bulletHoles, "range", "bullet_holes.range", SetBulletRange);
        }
    }
}

// Loads config.json beside gamedata.json and creates it when missing
void LoadStartupConfig(const std::string& gamedataPath)
{
    const nlohmann::json defaults = BuildDefaultConfig();
    ApplyStartupConfig(defaults);

    std::filesystem::path configPath(gamedataPath);
    configPath.replace_filename("config.json");
    std::ifstream configFile(configPath, std::ios::binary);
    if (!configFile.is_open())
    {
        std::ofstream defaultFile(configPath, std::ios::trunc);
        if (defaultFile.is_open())
        {
            defaultFile << defaults.dump(4) << '\n';
        }
        else
        {
            Msg("%s", "[BotVision] WARN: config.json missing and could not be created; using defaults\n");
        }
        return;
    }

    const std::string configText((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
    const nlohmann::json config = nlohmann::json::parse(configText, nullptr, false);
    if (config.is_discarded() || !config.is_object())
    {
        Msg("%s", "[BotVision] WARN: config.json parse error; using defaults\n");
        return;
    }
    ApplyStartupConfig(config);
}
} // namespace

// Loads gamedata and installs the required and optional modules
bool Install(const std::string& gamedataPath, void* serverInterface, char* error, size_t maxLength)
{
    nlohmann::json gamedata;
    if (!sig::LoadGamedata(gamedataPath.c_str(), gamedata))
    {
        const char* format = "failed to read/parse gamedata.json at %s";
        if (error && maxLength > 0)
        {
            std::snprintf(error, maxLength, format, gamedataPath.c_str());
        }
        char message[512];
        std::snprintf(message, sizeof(message), "[BotVision] failed to read/parse gamedata.json at %s\n", gamedataPath.c_str());
        Msg("%s", message);
        return false;
    }

    LoadStartupConfig(gamedataPath);

    sig::ModuleInfo serverModule = sig::ModuleFromInterfacePtr(serverInterface);
    if (!serverModule)
    {
#if defined(_WIN32)
        serverModule = sig::ModuleFromName("server.dll");
#else
        serverModule = sig::ModuleFromName("libserver.so");
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
        Msg("%s", message);
        return false;
    }

    if (!schema::Init())
    {
        Msg("%s", "[BotVision] WARN: SchemaSystem unavailable; optional features disabled\n");
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
    Msg("%s", message);
}

// Stores the engine interface for shared server time
void SetEngine(void* engine) { game_time::SetEngine(engine); }

// Forwards the external ray-trace interface to bullet capture
void SetRayTrace(void* rayTraceInterface, int returnCode) { bullet_vision::SetRayTrace(rayTraceInterface, returnCode); }

// Checks whether external ray tracing is ready
bool RayTraceReady() { return bullet_vision::RayTraceReady(); }

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

// Sets the bullet trace range
void SetBulletRange(float value) { bullet_vision::SetRange(value); }

// Returns the bullet trace range
float GetBulletRange() { return bullet_vision::GetRange(); }

// Stores the bullet tunnel enabled state
void SetBulletHolesEnabled(bool enabled) { bullet_vision::SetHolesEnabled(enabled); }

// Returns the bullet tunnel enabled state
bool GetBulletHolesEnabled() { return bullet_vision::GetHolesEnabled(); }

// Returns the active bullet tunnel count
int GetActiveBulletHoleCount() { return bullet_vision::GetActiveHoleCount(); }

// Returns bullet capture diagnostics
const char* GetBulletDiag() { return bullet_vision::GetDiagnostics(); }

// Returns the captured pellet count
long long GetBulletCount() { return bullet_vision::GetBulletCount(); }

// Returns the last captured pellet diagnostic
const char* GetLastBulletInfo() { return bullet_vision::GetLastBulletInfo(); }

// Returns the smoke hook call count
long long GetHitCount() { return smoke_vision::GetHitCount(); }

// Returns the smoke-blocked line count
long long GetBlockedCount() { return smoke_vision::GetBlockedCount(); }

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
long long GetIsVisiblePosCalls() { return smoke_vision::GetIsVisiblePosCalls(); }

// Returns the last observed controller handle
unsigned int GetLastCtrlHandle() { return smoke_vision::GetLastControllerHandle(); }

// Returns the last observed pawn pointer
unsigned long long GetLastPawnPtr() { return smoke_vision::GetLastPawnPointer(); }

// Adds one smoke reveal slot
void AddRevealSlot(int slot) { smoke_vision::AddRevealSlot(slot); }

// Removes one smoke reveal slot
void RemoveRevealSlot(int slot) { smoke_vision::RemoveRevealSlot(slot); }

// Clears all smoke reveals
void ClearReveals() { smoke_vision::ClearReveals(); }

// Returns the configured smoke reveal mask
unsigned long long GetRevealMask() { return smoke_vision::GetRevealMask(); }

// Returns one revealed player handle
unsigned int GetRevealHandle(int slot) { return smoke_vision::GetRevealHandle(slot); }

// Checks whether player visibility is hooked
bool IsVisiblePlayerHooked() { return smoke_vision::IsVisiblePlayerHooked(); }
} // namespace cs2bv::bot_vision
