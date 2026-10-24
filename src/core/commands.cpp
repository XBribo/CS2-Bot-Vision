#include "commands.h"
#include "config.h"
#include "features/vision/BotVision.h"

#include <tier0/dbg.h>
#include <convar.h>
#include <engine2/ivengineserver2.h>
#include <playerslot.h>

#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {
// Reads a bounded integer argument.
bool ReadInt(const CCommandContext& context, const char* argument, int minimum, int maximum, int& value)
{
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(argument, &end, 10);
    if (end == argument || *end != '\0' || errno == ERANGE || parsed < minimum || parsed > maximum)
    {
        cs2bv::commands::PrintToCaller(context, "expected an integer from %d to %d\n", minimum, maximum);
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

// Reads a finite float argument.
bool ReadFloat(const CCommandContext& context, const char* argument, float& value)
{
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(argument, &end);
    if (end == argument || *end != '\0' || errno == ERANGE || !std::isfinite(parsed))
    {
        cs2bv::commands::PrintToCaller(context, "expected a finite number within the float range\n");
        return false;
    }
    value = parsed;
    return true;
}

// Reads a player slot.
bool ReadSlot(const CCommandContext& context, const char* argument, int& slot)
{
    return ReadInt(context, argument, 0, cs2bv::bot_vision::GetMaxBots() - 1, slot);
}

// Reads a density value or a negative override reset.
bool ReadDensity(const CCommandContext& context, const char* argument, float& value, bool allowClear = false)
{
    if (!ReadFloat(context, argument, value)) return false;
    if ((allowClear && value < 0.0F) || cs2bv::config::IsValidFixedPointValue(value)) return true;

    cs2bv::commands::PrintToCaller(context, "density must be nonnegative and within the supported range\n");
    return false;
}
} // namespace

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

} // namespace cs2bv::commands

// Prints aggregate runtime diagnostics
CON_COMMAND_F(bv_status, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization,misc-unused-parameters)
              "Print BotVision plugin status.",
              FCVAR_NONE)
{
    cs2bv::commands::PrintToCaller(context, "[BotVision] smoke: mode=%d blocked=%lld/%lld heHoles=%d\n",
                                   cs2bv::bot_vision::GetSmokeMode(), cs2bv::bot_vision::GetBlockedCount(),
                                   cs2bv::bot_vision::GetHitCount(), cs2bv::bot_vision::GetActiveBlastCount());
    cs2bv::commands::PrintToCaller(context, "[BotVision] bulletHoles=%d\n",
                                   cs2bv::bot_vision::GetActiveBulletHoleCount());
    cs2bv::commands::PrintToCaller(context, "[BotVision] autoList=%s densityFn=%s he=%s\n", cs2bv::bot_vision::GetAutoListStatus(),
                                   cs2bv::bot_vision::IsDensityFnResolved() ? "resolved" : "missing", cs2bv::bot_vision::GetHeHookStatus());
    cs2bv::commands::PrintToCaller(context, "[BotVision] safeRead: %s\n", cs2bv::bot_vision::GetSafeReadDiag());
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
    float coordinates[6]{};
    for (int index = 0; index < 6; ++index)
    {
        if (!ReadFloat(context, args.Arg(index + 1), coordinates[index])) return;
    }
    char buf[1024];
    cs2bv::bot_vision::TestLos(coordinates[0], coordinates[1], coordinates[2], coordinates[3], coordinates[4], coordinates[5], buf,
                               sizeof(buf));
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
    int mode = 0;
    if (!ReadInt(context, args.Arg(1), 0, 1, mode)) return;
    cs2bv::bot_vision::SetSmokeMode(mode);
    cs2bv::commands::PrintToCaller(context, "smoke mode set to %d\n", mode);
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
    float threshold = 0.0F;
    if (!ReadDensity(context, args.Arg(1), threshold)) return;
    cs2bv::bot_vision::SetDensityThreshold(threshold);
    cs2bv::commands::PrintToCaller(context, "density threshold set to %.3f\n", cs2bv::bot_vision::GetDensityThreshold());
}

// Lists, reads, or changes per-bot density thresholds
CON_COMMAND_F(bv_bot_density, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_bot_density [<slot> <d>]  per-bot density threshold. "
              "Negative d clears back to global. No args lists all set slots.",
              FCVAR_NONE)
{
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
    int slot = 0;
    if (!ReadSlot(context, args.Arg(1), slot)) return;
    if (args.ArgC() < 3)
    {
        float v = cs2bv::bot_vision::GetBotDensityThreshold(slot);
        if (v < 0.0F)
            cs2bv::commands::PrintToCaller(context, "slot %d uses default (%.3f)\n", slot, cs2bv::bot_vision::GetDensityThreshold());
        else
            cs2bv::commands::PrintToCaller(context, "slot %d density = %.3f\n", slot, v);
        return;
    }
    float threshold = 0.0F;
    if (!ReadDensity(context, args.Arg(2), threshold, true)) return;
    cs2bv::bot_vision::SetBotDensityThreshold(slot, threshold);
    if (threshold < 0.0F) cs2bv::commands::PrintToCaller(context, "slot %d density cleared (uses default)\n", slot);
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

    int slot = 0;
    if (!ReadSlot(context, args.Arg(2), slot)) return;

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
    int enabled = 0;
    if (!ReadInt(context, args.Arg(1), 0, 1, enabled)) return;
    cs2bv::bot_vision::SetBulletHolesEnabled(enabled != 0);
    cs2bv::commands::PrintToCaller(context, "bullet holes set to %d\n", enabled);
}
