// Shared access to the current server time

#include "game_time.h"

#include <edict.h>
#include <eiface.h>

namespace cs2bv::game_time
{
    static IVEngineServer2 *g_engine = nullptr;

    // Stores the engine interface used for server time
    void SetEngine(void *engine)
    {
        g_engine = static_cast<IVEngineServer2 *>(engine);
    }

    // Reads the current server time
    float Now()
    {
        if (!g_engine)
            return 0.0f;
        CGlobalVars *globals = g_engine->GetServerGlobals();
        return globals ? globals->curtime : 0.0f;
    }
}
