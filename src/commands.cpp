#include "commands.h"
#include "hooks.h"

#include <tier0/dbg.h>
#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace cs2bv::commands
{

    IVEngineServer2 *g_pEngine = nullptr;

    void PrintToCaller(const CCommandContext &context, const char *fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        const CPlayerSlot slot = context.GetPlayerSlot();
        if (g_pEngine && slot.IsValid())
        {
            g_pEngine->ClientPrintf(slot, buf);
        }
        else
        {
            Msg("%s", buf);
        }
    }

    void Register() { /* CON_COMMAND_F self-registers via static init */ }
    void Unregister() { /* process-lifetime; no-op */ }

} // namespace cs2bv::commands

CON_COMMAND_F(bv_status,
              "Print BotVision plugin status.",
              FCVAR_NONE)
{
    cs2bv::commands::PrintToCaller(context,
                                     "[BotVision] hits=%lld blocked=%lld hooked=%s\n",
                                     static_cast<long long>(cs2bv::hooks::GetHitCount()),
                                     static_cast<long long>(cs2bv::hooks::GetBlockedCount()),
                                     cs2bv::hooks::GetHookedStatus());
}

CON_COMMAND_F(bv_test_los,
              "bv_test_los x1 y1 z1 x2 y2 z2 - query smoke density along segment.",
              FCVAR_NONE)
{
    if (args.ArgC() < 7)
    {
        cs2bv::commands::PrintToCaller(context,
                                         "usage: bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>\n");
        return;
    }
    float fx = (float)std::atof(args.Arg(1));
    float fy = (float)std::atof(args.Arg(2));
    float fz = (float)std::atof(args.Arg(3));
    float tx = (float)std::atof(args.Arg(4));
    float ty = (float)std::atof(args.Arg(5));
    float tz = (float)std::atof(args.Arg(6));
    char buf[1024];
    cs2bv::hooks::TestLos(fx, fy, fz, tx, ty, tz, buf, sizeof(buf));
    cs2bv::commands::PrintToCaller(context, "%s", buf);
}

CON_COMMAND_F(bv_smoke_mode,
              "bv_smoke_mode <0|1>  0=volume-smoke 1=ball-smoke.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context,
                                         "current mode=%d  densThr=%.3f  densityFn=%s\n"
                                         "  (0=volume-smoke 1=ball-smoke)\n",
                                         cs2bv::hooks::GetSmokeMode(),
                                         cs2bv::hooks::GetDensityThreshold(),
                                         cs2bv::hooks::IsDensityFnResolved() ? "resolved" : "MISSING(mode0->ball-smoke)");
        return;
    }
    int m = std::atoi(args.Arg(1));
    if (m < 0 || m > 1)
        m = 0;
    cs2bv::hooks::SetSmokeMode(m);
    cs2bv::commands::PrintToCaller(context, "smoke mode set to %d\n", m);
}

CON_COMMAND_F(bv_density_threshold,
              "bv_density_threshold <d>  mode-0 blocking threshold on density (default 0.2).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context,
                                         "current density threshold = %.3f\n",
                                         cs2bv::hooks::GetDensityThreshold());
        return;
    }
    float v = (float)std::atof(args.Arg(1));
    if (v < 0.0f)
        v = 0.0f;
    cs2bv::hooks::SetDensityThreshold(v);
    cs2bv::commands::PrintToCaller(context, "density threshold set to %.3f\n",
                                     cs2bv::hooks::GetDensityThreshold());
}
