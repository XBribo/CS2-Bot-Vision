#include "game_time.h"
#include "plugin.h"

#include <engine2/inetworkserverservice.h>
#include <engine2/networkgameserverbase.h>
#include <interfaces/interfaces.h>
#include <limits>

namespace cs2bv::game_time {
namespace {
INetworkServerService* g_serverService = nullptr;
} // namespace

bool Initialize()
{
    g_serverService = static_cast<INetworkServerService*>(g_SMAPI->GetEngineFactory()(NETWORKSERVERSERVICE_INTERFACE_VERSION, nullptr));
    return g_serverService != nullptr;
}

void Reset() { g_serverService = nullptr; }

Sample Read()
{
    auto* server = g_serverService ? g_serverService->GetIGameServer() : nullptr;
    return { server, server ? server->GetTime() : std::numeric_limits<float>::quiet_NaN() };
}
} // namespace cs2bv::game_time
