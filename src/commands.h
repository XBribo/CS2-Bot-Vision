#pragma once

class IVEngineServer2;
class CCommandContext;

namespace cs2bv::commands
{

    extern IVEngineServer2 *g_pEngine;

    void Register();
    void Unregister();
    void PrintToCaller(const CCommandContext &context, const char *fmt, ...);

} // namespace cs2bv::commands
