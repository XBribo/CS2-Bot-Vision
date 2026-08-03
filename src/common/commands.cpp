#include "commands.h"
#include "BotVision/BotVision.h"

#include <tier0/dbg.h>
#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace cs2bv::commands {

IVEngineServer2* g_pEngine = nullptr;

// Prints formatted text to the invoking console
void PrintToCaller(const CCommandContext& context, const char* fmt, ...)
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
CON_COMMAND_F(bv_status, "Print BotVision plugin status.", FCVAR_NONE)
{
    cs2bv::commands::PrintToCaller(context, "[BotVision] hits=%lld blocked=%lld hooked=%s heHoles=%d heEvent=%s\n",
                                   static_cast<long long>(cs2bv::BotVision::GetHitCount()),
                                   static_cast<long long>(cs2bv::BotVision::GetBlockedCount()), cs2bv::BotVision::GetHookedStatus(),
                                   cs2bv::BotVision::GetActiveBlastCount(), cs2bv::BotVision::GetHeListenerStatus());
    cs2bv::commands::PrintToCaller(context, "[BotVision] bullets=%lld holes=%d last: %s\n",
                                   static_cast<long long>(cs2bv::BotVision::GetBulletCount()), cs2bv::BotVision::GetActiveBulletHoleCount(),
                                   cs2bv::BotVision::GetLastBulletInfo());
    cs2bv::commands::PrintToCaller(context, "[BotVision] bulletDiag: %s\n", cs2bv::BotVision::GetBulletDiag());
    cs2bv::commands::PrintToCaller(context, "[BotVision] safeReadFailures: %s\n", cs2bv::BotVision::GetSafeReadDiag());
}

// Tests smoke density along an explicit line
CON_COMMAND_F(bv_test_los, "bv_test_los x1 y1 z1 x2 y2 z2 - query smoke density along segment.", FCVAR_NONE)
{
    if (args.ArgC() < 7)
    {
        cs2bv::commands::PrintToCaller(context, "usage: bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>\n");
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
CON_COMMAND_F(bv_smoke_mode, "bv_smoke_mode <0|1>  0=volume-smoke 1=ball-smoke.", FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context,
                                       "current mode=%d  densThr=%.3f  densityFn=%s\n"
                                       "  (0=volume-smoke 1=ball-smoke)\n",
                                       cs2bv::BotVision::GetSmokeMode(), cs2bv::BotVision::GetDensityThreshold(),
                                       cs2bv::BotVision::IsDensityFnResolved() ? "resolved" : "MISSING(mode0->ball-smoke)");
        return;
    }
    int m = std::atoi(args.Arg(1));
    if (m < 0 || m > 1) m = 0;
    cs2bv::BotVision::SetSmokeMode(m);
    cs2bv::commands::PrintToCaller(context, "smoke mode set to %d\n", m);
}

// Reads or changes the global density threshold
CON_COMMAND_F(bv_density_threshold, "bv_density_threshold <d>  mode-0 blocking threshold on density (default 0.19).", FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "current density threshold = %.3f\n", cs2bv::BotVision::GetDensityThreshold());
        return;
    }
    float v = (float)std::atof(args.Arg(1));
    if (v < 0.0f) v = 0.0f;
    cs2bv::BotVision::SetDensityThreshold(v);
    cs2bv::commands::PrintToCaller(context, "density threshold set to %.3f\n", cs2bv::BotVision::GetDensityThreshold());
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
            cs2bv::commands::PrintToCaller(context, "per-bot density unavailable (IsVisiblePos hook failed)\n");
            return;
        }
        cs2bv::commands::PrintToCaller(context, "per-bot density (global default=%.3f, lastBotSlot=%d):\n",
                                       cs2bv::BotVision::GetDensityThreshold(), cs2bv::BotVision::GetLastBotSlot());
        cs2bv::commands::PrintToCaller(context, "  probe: isVisiblePosCalls=%lld lastCtrlHandle=0x%X pawn=0x%llX\n",
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
        if (shown == 0) cs2bv::commands::PrintToCaller(context, "  (none set; all bots use default)\n");
        return;
    }
    /* query a single slot */
    int slot = std::atoi(args.Arg(1));
    if (args.ArgC() < 3)
    {
        float v = cs2bv::BotVision::GetBotDensityThreshold(slot);
        if (v < 0.0f)
            cs2bv::commands::PrintToCaller(context, "slot %d uses default (%.3f)\n", slot, cs2bv::BotVision::GetDensityThreshold());
        else
            cs2bv::commands::PrintToCaller(context, "slot %d density = %.3f\n", slot, v);
        return;
    }
    /* set (or clear if negative) */
    float v = (float)std::atof(args.Arg(2));
    cs2bv::BotVision::SetBotDensityThreshold(slot, v);
    if (v < 0.0f) cs2bv::commands::PrintToCaller(context, "slot %d density cleared (uses default)\n", slot);
    else
        cs2bv::commands::PrintToCaller(context, "slot %d density set to %.3f\n", slot, cs2bv::BotVision::GetBotDensityThreshold(slot));
}

// Manages players whose smoke visibility checks are bypassed
CON_COMMAND_F(bv_reveal, "bv_reveal <add|remove|list|clear> [slot].", FCVAR_NONE)
{
    const char* action = args.ArgC() >= 2 ? args.Arg(1) : "list";
    if (std::strcmp(action, "list") == 0)
    {
        const unsigned long long mask = cs2bv::BotVision::GetRevealMask();
        cs2bv::commands::PrintToCaller(context, "smoke reveals (hook=%s):\n",
                                       cs2bv::BotVision::IsVisiblePlayerHooked() ? "active" : "unavailable");
        int shown = 0;
        for (int slot = 0; slot < cs2bv::BotVision::GetMaxBots(); ++slot)
        {
            if ((mask & (1ULL << slot)) == 0) continue;
            cs2bv::commands::PrintToCaller(context, "  slot %d handle=0x%X\n", slot, cs2bv::BotVision::GetRevealHandle(slot));
            ++shown;
        }
        if (shown == 0) cs2bv::commands::PrintToCaller(context, "  (none)\n");
        return;
    }

    if (std::strcmp(action, "clear") == 0)
    {
        cs2bv::BotVision::ClearReveals();
        cs2bv::commands::PrintToCaller(context, "all smoke reveals cleared\n");
        return;
    }

    const bool add = std::strcmp(action, "add") == 0;
    const bool remove = std::strcmp(action, "remove") == 0;
    if ((!add && !remove) || args.ArgC() < 3)
    {
        cs2bv::commands::PrintToCaller(context, "usage: bv_reveal <add|remove> <slot>, "
                                                "bv_reveal <list|clear>\n");
        return;
    }

    const int slot = std::atoi(args.Arg(2));
    if (slot < 0 || slot >= cs2bv::BotVision::GetMaxBots())
    {
        cs2bv::commands::PrintToCaller(context, "slot must be 0-%d\n", cs2bv::BotVision::GetMaxBots() - 1);
        return;
    }

    if (add)
    {
        cs2bv::BotVision::AddRevealSlot(slot);
        cs2bv::commands::PrintToCaller(context, "smoke reveal added: slot %d (%s)\n", slot,
                                       cs2bv::BotVision::IsVisiblePlayerHooked() ? "active" : "IsVisiblePlayer hook unavailable");
    }
    else
    {
        cs2bv::BotVision::RemoveRevealSlot(slot);
        cs2bv::commands::PrintToCaller(context, "smoke reveal removed: slot %d\n", slot);
    }
}

// Enables or disables bullet smoke tunnels
CON_COMMAND_F(bv_bullet_holes, "bv_bullet_holes <0|1>  enable bullet-through-smoke holes (default 1).", FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "bullet holes = %d\n", cs2bv::BotVision::GetBulletHolesEnabled() ? 1 : 0);
        return;
    }
    bool e = std::atoi(args.Arg(1)) != 0;
    cs2bv::BotVision::SetBulletHolesEnabled(e);
    cs2bv::commands::PrintToCaller(context, "bullet holes set to %d\n", e ? 1 : 0);
}
