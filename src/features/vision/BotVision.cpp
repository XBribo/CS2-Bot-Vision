#include "core/config.h"
#include "core/log.h"
#include "core/cs2_sdk/src/core/gameconfig.h"

#include "BotVision.h"

#include "features/bullet/BulletVision.h"
#include "features/he/HeVision.h"
#include "features/smoke/SmokeVision.h"
#include "game_time.h"
#include "memory.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "plugin.h"
#include "core/cs2_sdk/src/core/memory_module.h"

#include <nlohmann/json.hpp>
#include <tier0/dbg.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace cs2bv::bot_vision {
bool Install(const std::string& gamedataPath, char* error, size_t maxLength)
{
    nlohmann::json gamedata;
    if (!cs2_sdk::gameconfig::LoadGamedata(gamedataPath.c_str(), gamedata))
    {
        const char* format = "failed to read/parse gamedata.json at %s";
        if (error && maxLength > 0)
        {
            std::snprintf(error, maxLength, format, gamedataPath.c_str());
        }
        return false;
    }

    config::LoadStartupConfig(gamedataPath);

    cs2_sdk::modules::Initialize();
    if (!cs2_sdk::modules::server || !cs2_sdk::modules::server->Code())
    {
        if (error && maxLength > 0)
        {
            std::snprintf(error, maxLength, "could not open CS2 server module at '%s'",
                          cs2_sdk::modules::server ? cs2_sdk::modules::server->Path() : "");
        }
        return false;
    }

    const cs2_sdk::modules::ModuleInfo& serverModule = cs2_sdk::modules::server->Code();

    if (!cs2_sdk::schema::Init(g_schemaSystem))
    {
        BV_LOG_WARN("SchemaSystem unavailable; optional features disabled");
    }

    if (!smoke_vision::Install(gamedata, serverModule, error, maxLength)) return false;

    he_vision::Install(gamedata, serverModule);
    bullet_vision::Install(gamedata, serverModule);
    return true;
}

void Remove()
{
    bullet_vision::Remove();
    he_vision::Remove();
    smoke_vision::Remove();

    BV_LOG_DEBUG("Removed: hits=%lld blocked=%lld", GetHitCount(), GetBlockedCount());
}

void SetEngine(void* engine) { game_time::SetEngine(engine); }

void SetHeRadius(float value) { he_vision::SetRadius(value); }

void SetHeDuration(float value) { he_vision::SetDuration(value); }

int GetActiveBlastCount() { return he_vision::GetActiveCount(); }

const char* GetHeHookStatus() { return he_vision::GetHookStatus(); }

void SetBulletRadius(float value) { bullet_vision::SetRadius(value); }

void SetBulletRadiusShotgun(float value) { bullet_vision::SetShotgunRadius(value); }

void SetBulletDuration(float value) { bullet_vision::SetDuration(value); }

void SetBulletHolesEnabled(bool enabled) { bullet_vision::SetHolesEnabled(enabled); }

bool GetBulletHolesEnabled() { return bullet_vision::GetHolesEnabled(); }

int GetActiveBulletHoleCount() { return bullet_vision::GetActiveHoleCount(); }

int64_t GetHitCount() { return smoke_vision::GetHitCount(); }

int64_t GetBlockedCount() { return smoke_vision::GetBlockedCount(); }

const char* GetAutoListStatus() { return smoke_vision::GetAutoListStatus(); }

int TestLos(float fromX, float fromY, float fromZ, float toX, float toY, float toZ, char* buffer, size_t bufferLength)
{
    return smoke_vision::TestLos(fromX, fromY, fromZ, toX, toY, toZ, buffer, bufferLength);
}

void SetSmokeMode(int mode) { smoke_vision::SetMode(mode); }

int GetSmokeMode() { return smoke_vision::GetMode(); }

void SetDensityThreshold(float value) { smoke_vision::SetDensityThreshold(value); }

float GetDensityThreshold() { return smoke_vision::GetDensityThreshold(); }

bool IsDensityFnResolved() { return smoke_vision::DensityFunctionReady(); }

const char* GetSafeReadDiag() { return memory::Diagnostics(); }

void SetBotDensityThreshold(int slot, float value) { smoke_vision::SetBotDensityThreshold(slot, value); }

float GetBotDensityThreshold(int slot) { return smoke_vision::GetBotDensityThreshold(slot); }

int GetMaxBots() { return smoke_vision::GetMaxBots(); }

int GetLastBotSlot() { return smoke_vision::GetLastBotSlot(); }

bool IsVisiblePosHooked() { return smoke_vision::IsVisiblePosHooked(); }

int64_t GetIsVisiblePosCalls() { return smoke_vision::GetIsVisiblePosCalls(); }

unsigned int GetLastCtrlHandle() { return smoke_vision::GetLastControllerHandle(); }

uint64_t GetLastPawnPtr() { return smoke_vision::GetLastPawnPointer(); }

void AddRevealSlot(int slot) { smoke_vision::AddRevealSlot(slot); }

void RemoveRevealSlot(int slot) { smoke_vision::RemoveRevealSlot(slot); }

void ClearReveals() { smoke_vision::ClearReveals(); }

uint64_t GetRevealMask() { return smoke_vision::GetRevealMask(); }

unsigned int GetRevealHandle(int slot) { return smoke_vision::GetRevealHandle(slot); }

bool IsVisiblePlayerHooked() { return smoke_vision::IsVisiblePlayerHooked(); }
} // namespace cs2bv::bot_vision
