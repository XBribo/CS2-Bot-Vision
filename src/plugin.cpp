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

#include "BotVision/BotVision.h"
#include "common/commands.h"
#include "common/memory.h"
#include "common/platform.h"

#define VERSION_STRING  "v" SEMVER " @ " GITHUB_SHA
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

PLUGIN_EXPOSE(cs2bv::BotVisionPlugin, cs2bv::g_plugin);

namespace cs2bv {

BotVisionPlugin g_plugin;

namespace {
// Resolves gamedata.json beside the plugin directory
std::string ComputeGamedataPath()
{
    std::filesystem::path path(cs2bv::platform::SelfModulePath());
    if (path.empty()) return "";

    for (int i = 0; i < 3; ++i)
    {
        if (!path.has_parent_path()) return "";
        path = path.parent_path();
    }
    return (path / "gamedata.json").string();
}
} // namespace

// Loads engine interfaces and installs the coordinated modules
bool BotVisionPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    if (!KHook::__exported__khook)
    {
        std::snprintf(error, maxlen, "Metamod with KHook support is required");
        return false;
    }

#ifndef _WIN32
    if (!cs2bv::memory::Initialize(error, maxlen)) return false;
#endif

    cs2bv::commands::g_engine = static_cast<IVEngineServer2*>(ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));
    if (!cs2bv::commands::g_engine)
    {
        Msg("%s", "[BotVision] WARN: IVEngineServer2 unavailable; commands print to server console only\n");
    }

    // Wires g_pCVar and registers every CON_COMMAND_F
    g_pCVar = static_cast<ICvar*>(ismm->GetEngineFactory()(CVAR_INTERFACE_VERSION, nullptr));
    if (!g_pCVar)
    {
        std::snprintf(error, maxlen, "Failed to get ICvar (%s)", CVAR_INTERFACE_VERSION);
        return false;
    }
    ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_CLIENT_CAN_EXECUTE);

    void* serverIface = ismm->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr);
    if (!serverIface)
    {
        std::snprintf(error, maxlen, "Failed to get IServerGameDLL");
        return false;
    }

    std::string gamedataPath = ComputeGamedataPath();
    if (gamedataPath.empty())
    {
        std::snprintf(error, maxlen, "Failed to compute gamedata.json path");
        return false;
    }

    if (!cs2bv::bot_vision::Install(gamedataPath, serverIface, error, maxlen))
    {
        return false;
    }

    cs2bv::bot_vision::SetEngine(cs2bv::commands::g_engine);

    cs2bv::commands::Register();
    char message[96];
    std::snprintf(message, sizeof(message), "[BotVision] loaded successfully (density threshold %.3f)\n",
                  cs2bv::bot_vision::GetDensityThreshold());
    Msg("%s", message);
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
    Msg("%s", "[BotVision] plugin unloaded\n");
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
