#include "game_time.h"
#include "plugin.h"

#include <engine2/gameshareddefs.h>
#include <engine2/inetworkserverservice.h>
#include <engine2/networkgameserverbase.h>
#include <interfaces/interfaces.h>

namespace cs2bv::game_time {
namespace {
INetworkServerService* g_serverService = nullptr;
} // namespace

// Resolves the map-independent server service.
bool Initialize()
{
    g_serverService = static_cast<INetworkServerService*>(g_SMAPI->GetEngineFactory()(NETWORKSERVERSERVICE_INTERFACE_VERSION, nullptr));
    return g_serverService != nullptr;
}

// Releases the service after effect hooks stop.
void Reset() { g_serverService = nullptr; }

// Reacquires the current server across map changes.
float Now()
{
    auto* server = g_serverService ? g_serverService->GetIGameServer() : nullptr;
    CGlobalVars* globals = server ? server->GetGlobals() : nullptr;
    return globals ? globals->curtime : 0.0F;
}
} // namespace cs2bv::game_time
