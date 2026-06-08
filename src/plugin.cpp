// BotVision Metamod:Source plugin entry point

#include <ISmmPlugin.h>

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include <eiface.h>
#include <icvar.h>
#include <convar.h>
#include <interfaces/interfaces.h>

#include "hooks.h"
#include "commands.h"
#include "raytrace_iface.h"

class BotVisionPlugin : public ISmmPlugin
{
public:
    bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
    bool Unload(char *error, size_t maxlen) override;

    bool Pause(char * /*error*/, size_t /*maxlen*/) override { return true; }
    bool Unpause(char * /*error*/, size_t /*maxlen*/) override { return true; }
    void AllPluginsLoaded() override;

    void TryFetchRayTrace(); // resolve CRayTraceInterface
    ISmmAPI *m_ismm = nullptr;

    const char *GetAuthor() override { return "CS2-Bot-Vision"; }
    const char *GetName() override { return "BotVision"; }
    const char *GetDescription() override { return "Volumetric smoke bots."; }
    const char *GetURL() override { return ""; }
    const char *GetLicense() override { return "GPLv3"; }
    const char *GetVersion() override { return "0.1.2"; }
    const char *GetDate() override { return __DATE__; }
    const char *GetLogTag() override { return "BOTVISION"; }
};

BotVisionPlugin g_botVisionPlugin;
PLUGIN_EXPOSE(BotVisionPlugin, g_botVisionPlugin);

static HMODULE GetSelfModule()
{
    HMODULE mod = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetSelfModule),
        &mod);
    return mod;
}

static std::string ComputeGamedataPath()
{
    char path[MAX_PATH] = {0};
    if (GetModuleFileNameA(GetSelfModule(), path, MAX_PATH) == 0)
        return "";

    for (int i = 0; i < 3; ++i)
    {
        char *slash = std::strrchr(path, '\\');
        if (!slash)
            return "";
        *slash = '\0';
    }
    std::string result(path);
    result += "\\gamedata.json";
    return result;
}

bool BotVisionPlugin::Load(PluginId id, ISmmAPI *ismm,
                           char *error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    cs2bv::commands::g_pEngine = static_cast<IVEngineServer2 *>(
        ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));
    if (!cs2bv::commands::g_pEngine)
    {
        OutputDebugStringA("[BotVision] WARN: IVEngineServer2 unavailable; commands print to server console only\n");
    }

    // Wires g_pCVar and registers every CON_COMMAND_F
    g_pCVar = static_cast<ICvar *>(
        ismm->GetEngineFactory()(CVAR_INTERFACE_VERSION, nullptr));
    if (!g_pCVar)
    {
        std::snprintf(error, maxlen,
                      "Failed to get ICvar (%s)",
                      CVAR_INTERFACE_VERSION);
        return false;
    }
    ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_CLIENT_CAN_EXECUTE);

    void *serverIface =
        ismm->GetServerFactory()(INTERFACEVERSION_SERVERGAMEDLL, nullptr);
    if (!serverIface)
    {
        std::snprintf(error, maxlen,
                      "Failed to get IServerGameDLL");
        return false;
    }

    std::string gamedataPath = ComputeGamedataPath();
    if (gamedataPath.empty())
    {
        std::snprintf(error, maxlen, "Failed to compute gamedata.json path");
        return false;
    }

    char dbg[MAX_PATH + 64];
    std::snprintf(dbg, sizeof(dbg),
                  "[BotVision] Load: gamedata=%s\n", gamedataPath.c_str());
    OutputDebugStringA(dbg);

    if (!cs2bv::hooks::Install(gamedataPath, serverIface, error, maxlen))
    {
        return false;
    }

    cs2bv::hooks::SetEngine(cs2bv::commands::g_pEngine);

    // Fetch the Ray-Trace interface
    m_ismm = ismm;
    TryFetchRayTrace();

    cs2bv::commands::Register();
    OutputDebugStringA("[BotVision] plugin loaded successfully\n");
    return true;
}

void BotVisionPlugin::TryFetchRayTrace()
{
    if (!m_ismm)
        return;
    int rtRet = 0;
    void *rtIface = m_ismm->MetaFactory(RAYTRACE_INTERFACE_VERSION, &rtRet, nullptr);
    cs2bv::hooks::SetRayTrace(rtIface, rtRet);
    char rb[160];
    std::snprintf(rb, sizeof(rb), rtIface ? "[BotVision] RayTrace %s @ %p (bullet wall-clip active)\n" : "[BotVision] WARN: RayTrace %s unavailable (ret=%d); bullet holes disabled\n",
                  RAYTRACE_INTERFACE_VERSION, rtIface ? (void *)rtIface : (void *)(intptr_t)rtRet);
    OutputDebugStringA(rb);
}

void BotVisionPlugin::AllPluginsLoaded()
{
    // Retry once everyone is up
    if (!cs2bv::hooks::RayTraceReady())
        TryFetchRayTrace();
}

bool BotVisionPlugin::Unload(char * /*error*/, size_t /*maxlen*/)
{
    cs2bv::commands::Unregister();
    cs2bv::hooks::Remove();
    ConVar_Unregister();
    g_pCVar = nullptr;
    cs2bv::commands::g_pEngine = nullptr;
    OutputDebugStringA("[BotVision] plugin unloaded\n");
    return true;
}
