#include "core/log.h"
// BotVision Metamod:Source plugin entry point

#include "plugin.h"
#include <ISmmPluginExt.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <eiface.h>
#include <icvar.h>
#include <convar.h>
#include <tier0/dbg.h>
#include <interfaces/interfaces.h>

#include "features/vision/BotVision.h"
#include "core/commands.h"
#include "core/gameconfig.h"
#include "utils/memory.h"
#include "utils/platform.h"

#define VERSION_STRING  "v" SEMVER " @ " GITHUB_SHA
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

PLUGIN_EXPOSE(cs2bv::BotVisionPlugin, cs2bv::g_plugin);

namespace cs2bv {

BotVisionPlugin g_plugin;

// Loads engine interfaces and installs the coordinated modules
bool BotVisionPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    if (!log::Init(ismm->GetBaseDir(), error, maxlen)) return false;

    if (!KHook::__exported__khook)
    {
        std::snprintf(error, maxlen, "Metamod with KHook support is required");
        BV_LOG_ERROR("%s", error);
        log::Close();
        return false;
    }

#ifndef _WIN32
    if (!cs2bv::memory::Initialize(error, maxlen))
    {
        BV_LOG_ERROR("%s", error);
        log::Close();
        return false;
    }
#endif

    cs2bv::commands::g_engine = static_cast<IVEngineServer2*>(ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));
    if (!cs2bv::commands::g_engine)
    {
        BV_LOG_WARN("IVEngineServer2 unavailable; commands print to server console only");
    }

    // Wires g_pCVar and registers every CON_COMMAND_F
    g_pCVar = static_cast<ICvar*>(ismm->GetEngineFactory()(CVAR_INTERFACE_VERSION, nullptr));
    if (!g_pCVar)
    {
        std::snprintf(error, maxlen, "Failed to get ICvar (%s)", CVAR_INTERFACE_VERSION);
        BV_LOG_ERROR("%s", error);
        log::Close();
        return false;
    }
    ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_CLIENT_CAN_EXECUTE);

    void* serverIface = ismm->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr);
    if (!serverIface)
    {
        std::snprintf(error, maxlen, "Failed to get IServerGameDLL");
        BV_LOG_ERROR("%s", error);
        log::Close();
        return false;
    }

    std::string gamedataPath = cs2bv::gameconfig::ComputePath();
    if (gamedataPath.empty())
    {
        std::snprintf(error, maxlen, "Failed to compute gamedata.json path");
        BV_LOG_ERROR("%s", error);
        log::Close();
        return false;
    }

    if (!cs2bv::bot_vision::Install(gamedataPath, serverIface, error, maxlen))
    {
        BV_LOG_ERROR("%s", error);
        log::Close();
        return false;
    }

    cs2bv::bot_vision::SetEngine(cs2bv::commands::g_engine);

    cs2bv::commands::Register();
    BV_LOG_INFO("Loaded %s", GetVersion());
    BV_LOG_DEBUG("Density threshold %.3f", cs2bv::bot_vision::GetDensityThreshold());
    return true;
}

// Removes commands, hooks, and acquired engine state
bool BotVisionPlugin::Unload(char* /*error*/, size_t /*maxlen*/)
{
    cs2bv::commands::Unregister();
    cs2bv::bot_vision::Remove();
    ConVar_Unregister();
    g_pCVar = nullptr;
    cs2bv::commands::g_engine = nullptr;
    cs2bv::bot_vision::SetEngine(nullptr);
    BV_LOG_INFO("Unloaded");
    log::Close();
    return true;
}

// Accepts a plugin pause request.
bool BotVisionPlugin::Pause(char*, size_t) { return true; }
// Accepts a plugin resume request.
bool BotVisionPlugin::Unpause(char*, size_t) { return true; }
// Returns plugin author metadata.
const char* BotVisionPlugin::GetAuthor() { return "XBribo(๑•.•๑)"; }
// Returns the plugin name.
const char* BotVisionPlugin::GetName() { return "BotVision"; }
// Returns the plugin description.
const char* BotVisionPlugin::GetDescription() { return "Volumetric smoke bots."; }
// Returns the plugin project URL.
const char* BotVisionPlugin::GetURL() { return "https://github.com/XBribo/CS2-Bot-Vision"; }
// Returns the plugin license.
const char* BotVisionPlugin::GetLicense() { return "AGPL3.0"; }
// Returns the version supplied by the build.
const char* BotVisionPlugin::GetVersion() { return VERSION_STRING; }
// Returns the compilation date and time.
const char* BotVisionPlugin::GetDate() { return BUILD_TIMESTAMP; }
// Returns the plugin log tag.
const char* BotVisionPlugin::GetLogTag() { return "BV"; }

} // namespace cs2bv
