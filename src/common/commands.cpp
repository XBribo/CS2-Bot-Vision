#include "commands.h"
#include "BotVision/BotVision.h"

#include <tier0/dbg.h>
#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace cs2bv::commands {

IVEngineServer2* g_engine = nullptr;

// Prints formatted text to the invoking console
void PrintToCaller(const CCommandContext& context, const char* fmt, ...) // NOLINT(modernize-avoid-variadic-functions)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    const CPlayerSlot slot = context.GetPlayerSlot();
    if (g_engine && slot.IsValid())
    {
        g_engine->ClientPrintf(slot, buf);
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
CON_COMMAND_F(bv_status, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization,misc-unused-parameters)
              "Print BotVision plugin status.",
              FCVAR_NONE)
{
    cs2bv::commands::PrintToCaller(context, "[BotVision] hits=%lld blocked=%lld hooked=%s heHoles=%d heEvent=%s\n",
                                   cs2bv::bot_vision::GetHitCount(), cs2bv::bot_vision::GetBlockedCount(),
                                   cs2bv::bot_vision::GetHookedStatus(), cs2bv::bot_vision::GetActiveBlastCount(),
                                   cs2bv::bot_vision::GetHeListenerStatus());
    cs2bv::commands::PrintToCaller(context, "[BotVision] bullets=%lld holes=%d last: %s\n", cs2bv::bot_vision::GetBulletCount(),
                                   cs2bv::bot_vision::GetActiveBulletHoleCount(), cs2bv::bot_vision::GetLastBulletInfo());
    cs2bv::commands::PrintToCaller(context, "[BotVision] bulletDiag: %s\n", cs2bv::bot_vision::GetBulletDiag());
    cs2bv::commands::PrintToCaller(context, "[BotVision] safeReadFailures: %s\n", cs2bv::bot_vision::GetSafeReadDiag());
}

// Tests smoke density along an explicit line
CON_COMMAND_F(bv_test_los, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_test_los x1 y1 z1 x2 y2 z2 - query smoke density along segment.",
              FCVAR_NONE)
{
    if (args.ArgC() < 7)
    {
        cs2bv::commands::PrintToCaller(context, "usage: bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>\n");
        return;
    }
    const auto fx = static_cast<float>(std::atof(args.Arg(1))); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    const auto fy = static_cast<float>(std::atof(args.Arg(2))); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    const auto fz = static_cast<float>(std::atof(args.Arg(3))); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    const auto tx = static_cast<float>(std::atof(args.Arg(4))); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    const auto ty = static_cast<float>(std::atof(args.Arg(5))); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    const auto tz = static_cast<float>(std::atof(args.Arg(6))); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    char buf[1024];
    cs2bv::bot_vision::TestLos(fx, fy, fz, tx, ty, tz, buf, sizeof(buf));
    cs2bv::commands::PrintToCaller(context, "%s", buf);
}

// Reads or changes the smoke calculation mode
CON_COMMAND_F(bv_smoke_mode, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_smoke_mode <0|1>  0=volume-smoke 1=ball-smoke.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context,
                                       "current mode=%d  densThr=%.3f  densityFn=%s\n"
                                       "  (0=volume-smoke 1=ball-smoke)\n",
                                       cs2bv::bot_vision::GetSmokeMode(), cs2bv::bot_vision::GetDensityThreshold(),
                                       cs2bv::bot_vision::IsDensityFnResolved() ? "resolved" : "MISSING(mode0->ball-smoke)");
        return;
    }
    int m = std::atoi(args.Arg(1)); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    if (m < 0 || m > 1) m = 0;
    cs2bv::bot_vision::SetSmokeMode(m);
    cs2bv::commands::PrintToCaller(context, "smoke mode set to %d\n", m);
}

// Reads or changes the global density threshold
CON_COMMAND_F(bv_density_threshold, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_density_threshold <d>  mode-0 blocking threshold from config.json.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "current density threshold = %.3f\n", cs2bv::bot_vision::GetDensityThreshold());
        return;
    }
    auto v = static_cast<float>(std::atof(args.Arg(1))); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    v = std::max(v, 0.0F);
    cs2bv::bot_vision::SetDensityThreshold(v);
    cs2bv::commands::PrintToCaller(context, "density threshold set to %.3f\n", cs2bv::bot_vision::GetDensityThreshold());
}

// Lists, reads, or changes per-bot density thresholds
CON_COMMAND_F(bv_bot_density, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_bot_density [<slot> <d>]  per-bot density threshold. "
              "Negative d clears back to global. No args lists all set slots.",
              FCVAR_NONE)
{
    /* list every slot*/
    if (args.ArgC() < 2)
    {
        if (!cs2bv::bot_vision::IsVisiblePosHooked())
        {
            cs2bv::commands::PrintToCaller(context, "per-bot density unavailable (IsVisiblePos hook failed)\n");
            return;
        }
        cs2bv::commands::PrintToCaller(context, "per-bot density (global default=%.3f, lastBotSlot=%d):\n",
                                       cs2bv::bot_vision::GetDensityThreshold(), cs2bv::bot_vision::GetLastBotSlot());
        cs2bv::commands::PrintToCaller(context, "  probe: isVisiblePosCalls=%lld lastCtrlHandle=0x%X pawn=0x%llX\n",
                                       cs2bv::bot_vision::GetIsVisiblePosCalls(), cs2bv::bot_vision::GetLastCtrlHandle(),
                                       cs2bv::bot_vision::GetLastPawnPtr());
        int n = cs2bv::bot_vision::GetMaxBots();
        int shown = 0;
        for (int s = 0; s < n; ++s)
        {
            float v = cs2bv::bot_vision::GetBotDensityThreshold(s);
            if (v >= 0.0F)
            {
                cs2bv::commands::PrintToCaller(context, "  slot %d = %.3f\n", s, v);
                ++shown;
            }
        }
        if (shown == 0) cs2bv::commands::PrintToCaller(context, "  (none set; all bots use default)\n");
        return;
    }
    /* query a single slot */
    const int slot = std::atoi(args.Arg(1)); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    if (args.ArgC() < 3)
    {
        float v = cs2bv::bot_vision::GetBotDensityThreshold(slot);
        if (v < 0.0F)
            cs2bv::commands::PrintToCaller(context, "slot %d uses default (%.3f)\n", slot, cs2bv::bot_vision::GetDensityThreshold());
        else
            cs2bv::commands::PrintToCaller(context, "slot %d density = %.3f\n", slot, v);
        return;
    }
    /* set (or clear if negative) */
    auto v = static_cast<float>(std::atof(args.Arg(2))); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    cs2bv::bot_vision::SetBotDensityThreshold(slot, v);
    if (v < 0.0F) cs2bv::commands::PrintToCaller(context, "slot %d density cleared (uses default)\n", slot);
    else
        cs2bv::commands::PrintToCaller(context, "slot %d density set to %.3f\n", slot, cs2bv::bot_vision::GetBotDensityThreshold(slot));
}

// Manages players whose smoke visibility checks are bypassed
CON_COMMAND_F(bv_reveal, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_reveal <add|remove|list|clear> [slot].",
              FCVAR_NONE)
{
    const char* action = args.ArgC() >= 2 ? args.Arg(1) : "list";
    if (std::strcmp(action, "list") == 0)
    {
        const uint64_t mask = cs2bv::bot_vision::GetRevealMask();
        cs2bv::commands::PrintToCaller(context, "smoke reveals (hook=%s):\n",
                                       cs2bv::bot_vision::IsVisiblePlayerHooked() ? "active" : "unavailable");
        int shown = 0;
        for (int slot = 0; slot < cs2bv::bot_vision::GetMaxBots(); ++slot)
        {
            if ((mask & (1ULL << slot)) == 0) continue;
            cs2bv::commands::PrintToCaller(context, "  slot %d handle=0x%X\n", slot, cs2bv::bot_vision::GetRevealHandle(slot));
            ++shown;
        }
        if (shown == 0) cs2bv::commands::PrintToCaller(context, "  (none)\n");
        return;
    }

    if (std::strcmp(action, "clear") == 0)
    {
        cs2bv::bot_vision::ClearReveals();
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

    const int slot = std::atoi(args.Arg(2)); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    if (slot < 0 || slot >= cs2bv::bot_vision::GetMaxBots())
    {
        cs2bv::commands::PrintToCaller(context, "slot must be 0-%d\n", cs2bv::bot_vision::GetMaxBots() - 1);
        return;
    }

    if (add)
    {
        cs2bv::bot_vision::AddRevealSlot(slot);
        cs2bv::commands::PrintToCaller(context, "smoke reveal added: slot %d (%s)\n", slot,
                                       cs2bv::bot_vision::IsVisiblePlayerHooked() ? "active" : "IsVisiblePlayer hook unavailable");
    }
    else
    {
        cs2bv::bot_vision::RemoveRevealSlot(slot);
        cs2bv::commands::PrintToCaller(context, "smoke reveal removed: slot %d\n", slot);
    }
}

// Enables or disables bullet smoke tunnels
CON_COMMAND_F(bv_bullet_holes, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_bullet_holes <0|1>  enable bullet-through-smoke holes from config.json.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        cs2bv::commands::PrintToCaller(context, "bullet holes = %d\n", cs2bv::bot_vision::GetBulletHolesEnabled() ? 1 : 0);
        return;
    }
    const bool e = std::atoi(args.Arg(1)) != 0; // NOLINT(bugprone-unchecked-string-to-number-conversion)
    cs2bv::bot_vision::SetBulletHolesEnabled(e);
    cs2bv::commands::PrintToCaller(context, "bullet holes set to %d\n", e ? 1 : 0);
}
