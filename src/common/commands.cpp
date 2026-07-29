#include "commands.h"
#include "BotVision/BotVision.h"

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

    // Prints formatted text to the invoking client or server console
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

    // Retains the static command registration entry point
    void Register() { /* CON_COMMAND_F self-registers via static init */ }

    // Retains the process-lifetime command cleanup entry point
    void Unregister() { /* process-lifetime; no-op */ }

} // namespace cs2bv::commands

// Prints aggregate runtime diagnostics
CON_COMMAND_F(bv_status,
              "Print BotVision plugin status.",
              FCVAR_NONE)
{
    cs2bv::commands::PrintToCaller(context,
                                   "[BotVision] hits=%lld blocked=%lld hooked=%s heHoles=%d heEvent=%s\n",
                                    static_cast<long long>(cs2bv::BotVision::GetHitCount()),
                                    static_cast<long long>(cs2bv::BotVision::GetBlockedCount()),
                                    cs2bv::BotVision::GetHookedStatus(),
                                    cs2bv::BotVision::GetActiveBlastCount(),
                                    cs2bv::BotVision::GetHeListenerStatus());
    cs2bv::commands::PrintToCaller(context,
                                   "[BotVision] bullets=%lld holes=%d last: %s\n",
                                    static_cast<long long>(cs2bv::BotVision::GetBulletCount()),
                                    cs2bv::BotVision::GetActiveBulletHoleCount(),
                                    cs2bv::BotVision::GetLastBulletInfo());
    cs2bv::commands::PrintToCaller(context,
                                   "[BotVision] bulletDiag: %s\n",
                                    cs2bv::BotVision::GetBulletDiag());
    cs2bv::commands::PrintToCaller(context,
                                   "[BotVision] safeReadFailures: %s\n",
                                    cs2bv::BotVision::GetSafeReadDiag());
}

// Tests smoke density along an explicit line
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
    cs2bv::BotVision::TestLos(fx, fy, fz, tx, ty, tz, buf, sizeof(buf));
    cs2bv::commands::PrintToCaller(context, "%s", buf);
}

// Reads or changes the smoke calculation mode
CON_COMMAND_F(bv_smoke_mode,
              "bv_smoke_mode <0|1>  0=volume-smoke 1=ball-smoke.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context,
                                       "current mode=%d  densThr=%.3f  densityFn=%s\n"
                                       "  (0=volume-smoke 1=ball-smoke)\n",
                                        cs2bv::BotVision::GetSmokeMode(),
                                        cs2bv::BotVision::GetDensityThreshold(),
                                        cs2bv::BotVision::IsDensityFnResolved() ? "resolved" : "MISSING(mode0->ball-smoke)");
        return;
    }
    int m = std::atoi(args.Arg(1));
    if (m < 0 || m > 1)
        m = 0;
    cs2bv::BotVision::SetSmokeMode(m);
    cs2bv::commands::PrintToCaller(context, "smoke mode set to %d\n", m);
}

// Reads or changes the global density threshold
CON_COMMAND_F(bv_density_threshold,
              "bv_density_threshold <d>  mode-0 blocking threshold on density (default 0.2).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context,
                                       "current density threshold = %.3f\n",
                                        cs2bv::BotVision::GetDensityThreshold());
        return;
    }
    float v = (float)std::atof(args.Arg(1));
    if (v < 0.0f)
        v = 0.0f;
    cs2bv::BotVision::SetDensityThreshold(v);
    cs2bv::commands::PrintToCaller(context, "density threshold set to %.3f\n",
                                    cs2bv::BotVision::GetDensityThreshold());
}

// Lists, reads, or changes per-bot density thresholds
CON_COMMAND_F(bv_bot_density,
              "bv_bot_density [<slot> <d>]  per-bot density threshold. "
              "Negative d clears back to global. No args lists all set slots.",
              FCVAR_NONE)
{
    /* list every slot*/
    if (args.ArgC() < 2)
    {
        if (!cs2bv::BotVision::IsVisiblePosHooked())
        {
            cs2bv::commands::PrintToCaller(context,
                                           "per-bot density unavailable (IsVisiblePos hook failed)\n");
            return;
        }
        cs2bv::commands::PrintToCaller(context,
                                       "per-bot density (global default=%.3f, lastBotSlot=%d):\n",
                                        cs2bv::BotVision::GetDensityThreshold(),
                                        cs2bv::BotVision::GetLastBotSlot());
        cs2bv::commands::PrintToCaller(context,
                                       "  probe: isVisiblePosCalls=%lld lastCtrlHandle=0x%X pawn=0x%llX\n",
                                        static_cast<long long>(cs2bv::BotVision::GetIsVisiblePosCalls()),
                                        cs2bv::BotVision::GetLastCtrlHandle(),
                                        static_cast<unsigned long long>(cs2bv::BotVision::GetLastPawnPtr()));
        int n = cs2bv::BotVision::GetMaxBots();
        int shown = 0;
        for (int s = 0; s < n; ++s)
        {
            float v = cs2bv::BotVision::GetBotDensityThreshold(s);
            if (v >= 0.0f)
            {
                cs2bv::commands::PrintToCaller(context, "  slot %d = %.3f\n", s, v);
                ++shown;
            }
        }
        if (shown == 0)
            cs2bv::commands::PrintToCaller(context, "  (none set; all bots use default)\n");
        return;
    }
    /* query a single slot */
    int slot = std::atoi(args.Arg(1));
    if (args.ArgC() < 3)
    {
        float v = cs2bv::BotVision::GetBotDensityThreshold(slot);
        if (v < 0.0f)
            cs2bv::commands::PrintToCaller(context,
                                           "slot %d uses default (%.3f)\n",
                                            slot, cs2bv::BotVision::GetDensityThreshold());
        else
            cs2bv::commands::PrintToCaller(context, "slot %d density = %.3f\n", slot, v);
        return;
    }
    /* set (or clear if negative) */
    float v = (float)std::atof(args.Arg(2));
    cs2bv::BotVision::SetBotDensityThreshold(slot, v);
    if (v < 0.0f)
        cs2bv::commands::PrintToCaller(context, "slot %d density cleared (uses default)\n", slot);
    else
        cs2bv::commands::PrintToCaller(context, "slot %d density set to %.3f\n",
                                       slot, cs2bv::BotVision::GetBotDensityThreshold(slot));
}

// Reads or changes the HE hole radius
CON_COMMAND_F(bv_he_radius,
              "bv_he_radius <r>  HE smoke-hole radius in units (default 200).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "current HE hole radius = %.1f\n",
                                        cs2bv::BotVision::GetHeRadius());
        return;
    }
    float v = (float)std::atof(args.Arg(1));
    if (v < 0.0f)
        v = 0.0f;
    cs2bv::BotVision::SetHeRadius(v);
    cs2bv::commands::PrintToCaller(context, "HE hole radius set to %.1f\n",
                                    cs2bv::BotVision::GetHeRadius());
}

// Reads or changes the HE hole lifetime
CON_COMMAND_F(bv_he_duration,
              "bv_he_duration <s>  HE smoke-hole lifetime in seconds (default 3.5).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "current HE hole duration = %.2f\n",
                                        cs2bv::BotVision::GetHeDuration());
        return;
    }
    float v = (float)std::atof(args.Arg(1));
    if (v < 0.0f)
        v = 0.0f;
    cs2bv::BotVision::SetHeDuration(v);
    cs2bv::commands::PrintToCaller(context, "HE hole duration set to %.2f\n",
                                    cs2bv::BotVision::GetHeDuration());
}

// Reads or changes the normal bullet tunnel radius
CON_COMMAND_F(bv_bullet_radius,
              "bv_bullet_radius <r>  bullet capsule-hole radius in units (default 12).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "current bullet hole radius = %.1f\n",
                                        cs2bv::BotVision::GetBulletRadius());
        return;
    }
    float v = (float)std::atof(args.Arg(1));
    if (v < 0.0f)
        v = 0.0f;
    cs2bv::BotVision::SetBulletRadius(v);
    cs2bv::commands::PrintToCaller(context, "bullet hole radius set to %.1f\n",
                                    cs2bv::BotVision::GetBulletRadius());
}

// Reads or changes the shotgun bullet tunnel radius
CON_COMMAND_F(bv_bullet_radius_shotgun,
              "bv_bullet_radius_shotgun <r>  shotgun bullet-hole radius in units (default 28).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "current shotgun bullet hole radius = %.1f\n",
                                        cs2bv::BotVision::GetBulletRadiusShotgun());
        return;
    }
    float v = (float)std::atof(args.Arg(1));
    if (v < 0.0f)
        v = 0.0f;
    cs2bv::BotVision::SetBulletRadiusShotgun(v);
    cs2bv::commands::PrintToCaller(context, "shotgun bullet hole radius set to %.1f\n",
                                    cs2bv::BotVision::GetBulletRadiusShotgun());
}

// Reads or changes the bullet tunnel lifetime
CON_COMMAND_F(bv_bullet_duration,
              "bv_bullet_duration <s>  bullet hole lifetime in seconds (default 0.2).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "current bullet hole duration = %.2f\n",
                                        cs2bv::BotVision::GetBulletDuration());
        return;
    }
    float v = (float)std::atof(args.Arg(1));
    if (v < 0.0f)
        v = 0.0f;
    cs2bv::BotVision::SetBulletDuration(v);
    cs2bv::commands::PrintToCaller(context, "bullet hole duration set to %.2f\n",
                                    cs2bv::BotVision::GetBulletDuration());
}

// Enables or disables bullet smoke tunnels
CON_COMMAND_F(bv_bullet_holes,
              "bv_bullet_holes <0|1>  enable bullet-through-smoke holes (default 1).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "bullet holes = %d\n",
                                        cs2bv::BotVision::GetBulletHolesEnabled() ? 1 : 0);
        return;
    }
    bool e = std::atoi(args.Arg(1)) != 0;
    cs2bv::BotVision::SetBulletHolesEnabled(e);
    cs2bv::commands::PrintToCaller(context, "bullet holes set to %d\n", e ? 1 : 0);
}
