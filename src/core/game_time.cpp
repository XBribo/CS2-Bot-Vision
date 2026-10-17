#include "game_time.h"

#include <engine2/ivengineserver2.h>
#include <engine2/gameshareddefs.h>

namespace cs2bv::game_time {
namespace {
IVEngineServer2* g_engine = nullptr;
} // namespace

void SetEngine(void* engine) { g_engine = static_cast<IVEngineServer2*>(engine); }

float Now()
{
    if (!g_engine) return 0.0F;
    CGlobalVars* globals = g_engine->GetServerGlobals();
    return globals ? globals->curtime : 0.0F;
}
} // namespace cs2bv::game_time
