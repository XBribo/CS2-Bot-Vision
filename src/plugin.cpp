#include "core/log.h"
#include "plugin.h"
#include <ISmmPluginExt.h>

#include <cstdio>
#include <string>

#include <engine2/ivengineserver2.h>
#include <icvar.h>
#include <convar.h>
#include <tier0/dbg.h>
#include <interfaces/interfaces.h>
#include <schemasystem/schemasystem.h>

#include "features/vision/BotVision.h"
#include "core/commands.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "utils/memory.h"

#define VERSION_STRING  "v" SEMVER " @ " GITHUB_SHA
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

PLUGIN_EXPOSE(cs2bv::BotVisionPlugin, cs2bv::g_plugin);

ISchemaSystem* g_schemaSystem = nullptr;

namespace cs2bv {
namespace {
// Releases partial or active runtime state.
void ClearRuntime()
{
    bot_vision::Remove();
    ConVar_Unregister();
    g_pCVar = nullptr;
    commands::g_engine = nullptr;
    bot_vision::SetEngine(nullptr);
    cs2_sdk::schema::Reset();
    g_schemaSystem = nullptr;
}
} // namespace

BotVisionPlugin g_plugin;

bool BotVisionPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    if (!log::Init(ismm->GetBaseDir(), "BotVision", error, maxlen)) return false;

    // Reports one failure and rolls back initialization.
    const auto fail = [error, maxlen](const char* reason) {
        if (error && maxlen > 0) std::snprintf(error, maxlen, "%s", reason);
        BV_LOG_ERROR("Load failed: %s", reason);
        ClearRuntime();
        log::Close();
        return false;
    };

    if (!KHook::__exported__khook) return fail("KHook unavailable");

    g_schemaSystem = static_cast<ISchemaSystem*>(ismm->VInterfaceMatch(ismm->GetEngineFactory(), SCHEMASYSTEM_INTERFACE_VERSION, 0));

    char detail[256]{};
#ifndef _WIN32
    if (!memory::Initialize(detail, sizeof(detail))) return fail(detail);
#endif

    commands::g_engine = static_cast<IVEngineServer2*>(ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));
    if (!commands::g_engine) return fail("IVEngineServer2 unavailable");
    bot_vision::SetEngine(commands::g_engine);

    g_pCVar = static_cast<ICvar*>(ismm->GetEngineFactory()(CVAR_INTERFACE_VERSION, nullptr));
    if (!g_pCVar)
    {
        std::snprintf(detail, sizeof(detail), "ICvar unavailable: %s", CVAR_INTERFACE_VERSION);
        return fail(detail);
    }

    const std::string gamedataPath = cs2_sdk::gameconfig::ComputePath();
    if (gamedataPath.empty()) return fail("Gamedata path unavailable");
    if (!bot_vision::Install(gamedataPath, detail, sizeof(detail))) return fail(detail);

    // Register commands only after initialization succeeds.
    ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_CLIENT_CAN_EXECUTE);
    BV_LOG_INFO("Loaded %s", GetVersion());
    return true;
}

bool BotVisionPlugin::Unload(char* /*error*/, size_t /*maxlen*/)
{
    ClearRuntime();
    BV_LOG_INFO("Unloaded");
    log::Close();
    return true;
}

const char* BotVisionPlugin::GetAuthor() { return "XBribo(๑•.•๑)"; }
const char* BotVisionPlugin::GetName() { return "BotVision"; }
const char* BotVisionPlugin::GetDescription() { return "Volumetric smoke bots."; }
const char* BotVisionPlugin::GetURL() { return "https://github.com/XBribo/CS2-Bot-Vision"; }
const char* BotVisionPlugin::GetLicense() { return "AGPL3.0"; }
const char* BotVisionPlugin::GetVersion() { return VERSION_STRING; }
const char* BotVisionPlugin::GetDate() { return BUILD_TIMESTAMP; }
const char* BotVisionPlugin::GetLogTag() { return "BV"; }

} // namespace cs2bv
