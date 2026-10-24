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

namespace bot_vision = cs2bv::bot_vision;
using cs2bv::commands::PrintToCaller;

namespace {
// Reads a bounded integer argument.
bool ReadInt(const CCommandContext& context, const char* argument, int minimum, int maximum, int& value)
{
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(argument, &end, 10);
    if (end == argument || *end != '\0' || errno == ERANGE || parsed < minimum || parsed > maximum)
    {
        PrintToCaller(context, "Integer required (%d-%d).\n", minimum, maximum);
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
        PrintToCaller(context, "Finite float required.\n");
        return false;
    }
    value = parsed;
    return true;
}

// Reads a player slot.
bool ReadSlot(const CCommandContext& context, const char* argument, int& slot)
{
    return ReadInt(context, argument, 0, bot_vision::GetMaxBots() - 1, slot);
}

// Reads a density value or a negative override reset.
bool ReadDensity(const CCommandContext& context, const char* argument, float& value, bool allowClear = false)
{
    if (!ReadFloat(context, argument, value)) return false;
    if ((allowClear && value < 0.0F) || cs2bv::config::IsValidFixedPointValue(value)) return true;

    PrintToCaller(context, "Density out of range.\n");
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
    PrintToCaller(context,
                  "[BotVision] mode=%d blocked=%lld/%lld heHoles=%d bulletHoles=%d\n"
                  "[BotVision] autoList=%s density=%s he=%s\n"
                  "[BotVision] reads: %s\n",
                  bot_vision::GetSmokeMode(), bot_vision::GetBlockedCount(), bot_vision::GetHitCount(), bot_vision::GetActiveBlastCount(),
                  bot_vision::GetActiveBulletHoleCount(), bot_vision::GetAutoListStatus(),
                  bot_vision::IsDensityFnResolved() ? "resolved" : "missing", bot_vision::GetHeHookStatus(), bot_vision::GetSafeReadDiag());
}

// Tests smoke density along an explicit line
CON_COMMAND_F(bv_test_los, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_test_los x1 y1 z1 x2 y2 z2 - query smoke density along segment.",
              FCVAR_NONE)
{
    if (args.ArgC() < 7)
    {
        PrintToCaller(context, "usage: bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>\n");
        return;
    }
    float coordinates[6]{};
    for (int index = 0; index < 6; ++index)
    {
        if (!ReadFloat(context, args.Arg(index + 1), coordinates[index])) return;
    }
    char buf[1024];
    bot_vision::TestLos(coordinates[0], coordinates[1], coordinates[2], coordinates[3], coordinates[4], coordinates[5], buf, sizeof(buf));
    PrintToCaller(context, "%s", buf);
}

// Reads or changes the smoke calculation mode
CON_COMMAND_F(bv_smoke_mode, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_smoke_mode <0|1>  0=volume-smoke 1=ball-smoke.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        PrintToCaller(context, "smoke_mode=%d (0=volume, 1=vanilla) density_threshold=%.3f density=%s\n", bot_vision::GetSmokeMode(),
                      bot_vision::GetDensityThreshold(), bot_vision::IsDensityFnResolved() ? "resolved" : "missing (vanilla fallback)");
        return;
    }
    int mode = 0;
    if (!ReadInt(context, args.Arg(1), 0, 1, mode)) return;
    bot_vision::SetSmokeMode(mode);
    PrintToCaller(context, "smoke_mode=%d\n", mode);
}

// Reads or changes the global density threshold
CON_COMMAND_F(bv_density_threshold, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_density_threshold <d>  mode-0 blocking threshold from config.json.",
              FCVAR_NONE)
{
    if (args.ArgC() >= 2)
    {
        float threshold = 0.0F;
        if (!ReadDensity(context, args.Arg(1), threshold)) return;
        bot_vision::SetDensityThreshold(threshold);
    }
    PrintToCaller(context, "density_threshold=%.3f\n", bot_vision::GetDensityThreshold());
}

// Lists, reads, or changes per-bot density thresholds
CON_COMMAND_F(bv_bot_density, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_bot_density [<slot> <d>]  per-bot density threshold. "
              "Negative d clears back to global. No args lists all set slots.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        if (!bot_vision::IsVisiblePosHooked())
        {
            PrintToCaller(context, "Bot density unavailable: IsVisiblePos hook missing.\n");
            return;
        }
        PrintToCaller(context,
                      "bot_density: global=%.3f lastSlot=%d\n"
                      "  calls=%lld controller=0x%X pawn=0x%llX\n",
                      bot_vision::GetDensityThreshold(), bot_vision::GetLastBotSlot(), bot_vision::GetIsVisiblePosCalls(),
                      bot_vision::GetLastCtrlHandle(), bot_vision::GetLastPawnPtr());
        const int maxBots = bot_vision::GetMaxBots();
        int shown = 0;
        for (int slot = 0; slot < maxBots; ++slot)
        {
            const float threshold = bot_vision::GetBotDensityThreshold(slot);
            if (threshold >= 0.0F)
            {
                PrintToCaller(context, "  slot %d = %.3f\n", slot, threshold);
                ++shown;
            }
        }
        if (shown == 0) PrintToCaller(context, "  no overrides\n");
        return;
    }
    int slot = 0;
    if (!ReadSlot(context, args.Arg(1), slot)) return;
    if (args.ArgC() >= 3)
    {
        float threshold = 0.0F;
        if (!ReadDensity(context, args.Arg(2), threshold, true)) return;
        bot_vision::SetBotDensityThreshold(slot, threshold);
    }
    const float threshold = bot_vision::GetBotDensityThreshold(slot);
    if (threshold < 0.0F) PrintToCaller(context, "slot %d density=default (%.3f)\n", slot, bot_vision::GetDensityThreshold());
    else
        PrintToCaller(context, "slot %d density=%.3f\n", slot, threshold);
}

// Manages players whose smoke visibility checks are bypassed
CON_COMMAND_F(bv_reveal, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_reveal <add|remove|list|clear> [slot].",
              FCVAR_NONE)
{
    const char* action = args.ArgC() >= 2 ? args.Arg(1) : "list";
    if (std::strcmp(action, "list") == 0)
    {
        const uint64_t mask = bot_vision::GetRevealMask();
        PrintToCaller(context, "reveal: hook=%s\n", bot_vision::IsVisiblePlayerHooked() ? "active" : "unavailable");
        int shown = 0;
        for (int slot = 0; slot < bot_vision::GetMaxBots(); ++slot)
        {
            if ((mask & (1ULL << slot)) == 0) continue;
            PrintToCaller(context, "  slot %d handle=0x%X\n", slot, bot_vision::GetRevealHandle(slot));
            ++shown;
        }
        if (shown == 0) PrintToCaller(context, "  none\n");
        return;
    }

    if (std::strcmp(action, "clear") == 0)
    {
        bot_vision::ClearReveals();
        PrintToCaller(context, "reveal: cleared\n");
        return;
    }

    const bool add = std::strcmp(action, "add") == 0;
    const bool remove = std::strcmp(action, "remove") == 0;
    if ((!add && !remove) || args.ArgC() < 3)
    {
        PrintToCaller(context, "usage: bv_reveal <add|remove> <slot>, "
                               "bv_reveal <list|clear>\n");
        return;
    }

    int slot = 0;
    if (!ReadSlot(context, args.Arg(2), slot)) return;

    if (add)
    {
        bot_vision::AddRevealSlot(slot);
        PrintToCaller(context, "reveal: slot=%d added (hook=%s)\n", slot, bot_vision::IsVisiblePlayerHooked() ? "active" : "unavailable");
    }
    else
    {
        bot_vision::RemoveRevealSlot(slot);
        PrintToCaller(context, "reveal: slot=%d removed\n", slot);
    }
}

// Enables or disables bullet smoke tunnels
CON_COMMAND_F(bv_bullet_holes, // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)
              "bv_bullet_holes <0|1>  enable bullet-through-smoke holes from config.json.",
              FCVAR_NONE)
{
    if (args.ArgC() >= 2)
    {
        int enabled = 0;
        if (!ReadInt(context, args.Arg(1), 0, 1, enabled)) return;
        bot_vision::SetBulletHolesEnabled(enabled != 0);
    }
    PrintToCaller(context, "bullet_holes=%d\n", bot_vision::GetBulletHolesEnabled() ? 1 : 0);
}
