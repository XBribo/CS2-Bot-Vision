// BotVision Metamod:Source plugin entry point

#include <ISmmPlugin.h>

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
#include "common/platform.h"
#include "common/raytrace_iface.h"

class BotVisionPlugin : public ISmmPlugin
{
  public:
    // Loads interfaces and installs all BotVision modules
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;

    // Removes all BotVision modules and console state
    bool Unload(char* error, size_t maxlen) override;

    // Accepts plugin pause requests
    bool Pause(char* /*error*/, size_t /*maxlen*/) override { return true; }

    // Accepts plugin unpause requests
    bool Unpause(char* /*error*/, size_t /*maxlen*/) override { return true; }

    // Retries optional interfaces after all plugins are loaded
    void AllPluginsLoaded() override;

    // Resolves the optional CRayTraceInterface and optionally reports failure
    void TryFetchRayTrace(bool reportFailure);

    ISmmAPI* m_ismm = nullptr;

    // Returns plugin author metadata
    const char* GetAuthor() override { return "XBribo(๑•.•๑)"; }

    // Returns plugin name metadata
    const char* GetName() override { return "BotVision"; }

    // Returns plugin description metadata
    const char* GetDescription() override { return "Volumetric smoke bots."; }

    // Returns plugin URL metadata
    const char* GetURL() override { return ""; }

    // Returns plugin license metadata
    const char* GetLicense() override { return "AGPL3.0"; }

    // Returns plugin version metadata
    const char* GetVersion() override { return "0.2.4"; }

    // Returns plugin build date metadata
    const char* GetDate() override { return __DATE__; }

    // Returns plugin log tag metadata
    const char* GetLogTag() override { return "BV"; }
};

BotVisionPlugin g_botVisionPlugin;
PLUGIN_EXPOSE(BotVisionPlugin, g_botVisionPlugin);

// Resolves gamedata.json beside the plugin directory
static std::string ComputeGamedataPath()
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

// Loads engine interfaces and installs the coordinated modules
bool BotVisionPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    cs2bv::commands::g_pEngine = static_cast<IVEngineServer2*>(ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));
    if (!cs2bv::commands::g_pEngine)
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

    if (!cs2bv::BotVision::Install(gamedataPath, serverIface, error, maxlen))
    {
        return false;
    }

    cs2bv::BotVision::SetEngine(cs2bv::commands::g_pEngine);

    // Fetch the Ray-Trace interface
    m_ismm = ismm;
    TryFetchRayTrace(false);

    cs2bv::commands::Register();
    char message[96];
    std::snprintf(message, sizeof(message), "[BotVision] loaded successfully (density threshold %.3f)\n",
                  cs2bv::BotVision::GetDensityThreshold());
    Msg("%s", message);
    return true;
}

// Resolves and publishes the optional external ray-trace interface
void BotVisionPlugin::TryFetchRayTrace(bool reportFailure)
{
    if (!m_ismm) return;
    int returnCode = 0;
    void* rayTrace = m_ismm->MetaFactory(RAYTRACE_INTERFACE_VERSION, &returnCode, nullptr);
    cs2bv::BotVision::SetRayTrace(rayTrace, returnCode);

    if (!rayTrace && reportFailure)
    {
        char message[160];
        std::snprintf(message, sizeof(message), "[BotVision] WARN: RayTrace %s unavailable (ret=%d)\n", RAYTRACE_INTERFACE_VERSION,
                      returnCode);
        Msg("%s", message);
    }
}

// Retries ray tracing after every Metamod plugin has loaded
void BotVisionPlugin::AllPluginsLoaded()
{
    if (!cs2bv::BotVision::RayTraceReady()) TryFetchRayTrace(true);
}

// Removes commands, hooks, and acquired engine state
bool BotVisionPlugin::Unload(char* /*error*/, size_t /*maxlen*/)
{
    cs2bv::commands::Unregister();
    cs2bv::BotVision::Remove();
    ConVar_Unregister();
    g_pCVar = nullptr;
    cs2bv::commands::g_pEngine = nullptr;
    cs2bv::BotVision::SetEngine(nullptr);
    Msg("%s", "[BotVision] plugin unloaded\n");
    return true;
}
