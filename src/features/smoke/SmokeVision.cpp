#include "core/log.h"
#include "core/config.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
// Smoke visibility hooks and per-bot threshold state

#include "SmokeVision.h"

#include "features/bullet/BulletVision.h"
#include "features/he/HeVision.h"
#include "game_time.h"
#include "core/cs2_sdk/src/utils/hooks.h"
#include "memory.h"
#include "core/cs2_sdk/src/utils/platform.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/cs2_sdk/src/core/memory_module.h"

#include <nlohmann/json.hpp>
#include <tier0/dbg.h>
#include <entityhandle.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <span>
#include <vector>

namespace cs2bv::smoke_vision {
namespace {
using GetSmokeDensityInLineFn = float(CS2_SDK_FASTCALL*)(const float* from, const float* to, float* outClosest);

constexpr const char* kSmokeFunctionName = "CBotManager::IsVisibleThroughSmoke";
constexpr const char* kAutoListName = "g_AutoList_SmokeProj_Head_Server";
constexpr const char* kDensityFunctionName = "GetSmokeDensityInLine";
constexpr const char* kVisiblePosName = "CCSBot::IsVisiblePos";
constexpr const char* kVisiblePlayerName = "CCSBot::IsVisiblePlayer";
constexpr int kMaxBots = 64;
constexpr int kDefaultThreshold = INT_MIN;

GetSmokeDensityInLineFn g_getSmokeDensityInLine = nullptr;
cs2_sdk::hooks::NativeHook<bool, void*, const void*, const void*> g_smokeHook;
cs2_sdk::hooks::NativeHook<int64_t, int64_t, int64_t, char, void*> g_visiblePosHook;
cs2_sdk::hooks::NativeHook<bool, int64_t, void*, char, unsigned char*> g_visiblePlayerHook;
void** g_autoListHead = nullptr;

std::atomic<int64_t> g_hitCount{ 0 };
std::atomic<int64_t> g_blockedCount{ 0 };
const char* g_autoListStatus = "not_attempted";
std::atomic<int> g_smokeMode{ 0 };
std::atomic<int> g_densityThresholdMilli{ 230 };

int g_controllerHandleOffset = -1;
int g_playerInBotOffset = -1;
std::atomic<int> g_botThresholdMilli[kMaxBots];
std::atomic<int> g_botThresholdOverrideCount{ 0 };
std::atomic<unsigned int> g_cacheGeneration{ 1 };
thread_local int g_currentBotThresholdMilli = kDefaultThreshold;
std::atomic<int> g_lastBotSlot{ -1 };
std::atomic<int64_t> g_isVisiblePosCalls{ 0 };
std::atomic<unsigned int> g_lastControllerHandle{ 0 };
std::atomic<uint64_t> g_lastPawnPointer{ 0 };
std::atomic<uint64_t> g_revealMask{ 0 };
std::atomic<unsigned int> g_revealHandles[kMaxBots];
thread_local bool g_currentPlayerRevealed = false;
thread_local std::vector<int> g_thresholdFrames;
thread_local std::vector<bool> g_revealFrames;

struct BotThresholdCacheEntry
{
    int64_t bot = 0;
    int thresholdMilli = kDefaultThreshold;
    unsigned int usesRemaining = 0;
    unsigned int generation = 0;
};

constexpr size_t kCacheSize = 256;
constexpr size_t kCacheProbeCount = 4;
constexpr unsigned int kCacheRefreshUses = 1024;
constexpr unsigned int kInvalidCacheRefreshUses = 32;
thread_local BotThresholdCacheEntry g_thresholdCache[kCacheSize];

// Checks positive density against the blocking threshold.
bool BlocksSight(float density, float threshold) { return density > 0.0F && density >= threshold; }

// Integrates the strongest hole mask once per slice.
float AdjustClientDensity(const float* from, const float* to, float density)
{
    if (!from || !to || !g_getSmokeDensityInLine || density <= 0.0F) return density;

    const float line[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    const float lineLengthSquared = (line[0] * line[0]) + (line[1] * line[1]) + (line[2] * line[2]);
    if (lineLengthSquared <= 0.001F) return density;

    const float now = game_time::Now();
    std::array<bullet_vision::Influence, bullet_vision::kMaxInfluences> bulletStorage;
    std::array<he_vision::Influence, he_vision::kMaxInfluences> heStorage;
    const size_t bulletCount = bullet_vision::GetHolesEnabled() ? bullet_vision::CollectInfluences(from, to, now, bulletStorage) : 0;
    const size_t heCount = he_vision::CollectInfluences(from, to, now, heStorage);
    const std::span bullets(bulletStorage.data(), bulletCount);
    const std::span blasts(heStorage.data(), heCount);

    std::array<float, 2 * (bullet_vision::kMaxInfluences + he_vision::kMaxInfluences)> boundaryStorage;
    size_t boundaryCount = 0;
    for (const auto& influence : bullets)
    {
        boundaryStorage[boundaryCount++] = influence.begin;
        boundaryStorage[boundaryCount++] = influence.finish;
    }
    for (const auto& influence : blasts)
    {
        boundaryStorage[boundaryCount++] = influence.begin;
        boundaryStorage[boundaryCount++] = influence.finish;
    }
    const std::span boundaries(boundaryStorage.data(), boundaryCount);
    std::ranges::sort(boundaries);

    constexpr int kDensitySlices = 5;
    float sampledDensity = 0.0F;
    float weightedDensity = 0.0F;
    // Preserve hole boundaries when sampling narrow tunnels.
    for (size_t interval = 1; interval < boundaries.size(); ++interval)
    {
        const float begin = boundaries[interval - 1];
        const float end = boundaries[interval];
        if (end <= begin) continue;

        for (int slice = 0; slice < kDensitySlices; ++slice)
        {
            const float sliceBeginAmount = begin + ((end - begin) * (static_cast<float>(slice) / kDensitySlices));
            const float sliceEndAmount = begin + ((end - begin) * (static_cast<float>(slice + 1) / kDensitySlices));
            const float midpointAmount = (sliceBeginAmount + sliceEndAmount) * 0.5F;
            const float midpoint[3] = { from[0] + (line[0] * midpointAmount), from[1] + (line[1] * midpointAmount),
                                        from[2] + (line[2] * midpointAmount) };

            float minimumScale = 1.0F;
            for (const auto& influence : bullets)
            {
                if (midpointAmount < influence.begin || midpointAmount > influence.finish) continue;
                minimumScale = std::min(minimumScale, bullet_vision::DensityScale(midpoint, influence));
            }
            for (const auto& influence : blasts)
            {
                if (midpointAmount < influence.begin || midpointAmount > influence.finish) continue;
                minimumScale = std::min(minimumScale, he_vision::DensityScale(midpoint, influence));
            }
            if (minimumScale >= 0.999F) continue;

            const float sliceBegin[3] = { from[0] + (line[0] * sliceBeginAmount), from[1] + (line[1] * sliceBeginAmount),
                                          from[2] + (line[2] * sliceBeginAmount) };
            const float sliceEnd[3] = { from[0] + (line[0] * sliceEndAmount), from[1] + (line[1] * sliceEndAmount),
                                        from[2] + (line[2] * sliceEndAmount) };
            const float sliceDensity = std::max(g_getSmokeDensityInLine(sliceBegin, sliceEnd, nullptr), 0.0F);
            sampledDensity += sliceDensity;
            weightedDensity += sliceDensity * minimumScale;
        }
    }
    if (sampledDensity <= 0.0001F) return density;

    const float averageScale = std::clamp(weightedDensity / sampledDensity, 0.0F, 1.0F);
    const float affectedDensity = std::min(sampledDensity, density);
    return std::max(density - (affectedDensity * (1.0F - averageScale)), 0.0F);
}

// Formats an install error for the plugin loader.
void ReportError(char* error, size_t maxLength, const char* format, ...) // NOLINT(modernize-avoid-variadic-functions)
{
    if (!error || maxLength == 0) return;
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(error, maxLength, format, arguments);
    va_end(arguments);
}

// Mixes a pointer for fixed-size cache indexing
uint64_t MixPointerValue(uintptr_t value)
{
    auto key = static_cast<uint64_t>(value);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return key;
}

// Resolves a RIP-relative pointer from a matched instruction
void* ResolveRipRelative(unsigned char* signatureStart, int relativeOffset, int instructionLength)
{
    if (!signatureStart || relativeOffset <= 0 || instructionLength < 4 || relativeOffset > instructionLength - 4) return nullptr;
#ifndef _WIN32
    errno = 0;
#endif
    void* target = cs2_sdk::memory::ResolveRelativeAddress(signatureStart, static_cast<size_t>(relativeOffset),
                                                           static_cast<size_t>(instructionLength));
    if (!target) memory::RecordFailure(memory::FailureDomain::Install, errno ? errno : EFAULT);
    return target;
}

// Reads an integer property from one gamedata entry
int GamedataInt(const nlohmann::json& gamedata, const char* name, const char* key, int defaultValue)
{
    auto entry = gamedata.find(name);
    if (entry == gamedata.end() || !entry->is_object()) return defaultValue;
    auto value = entry->find(key);
    if (value == entry->end() || !value->is_number_integer()) return defaultValue;
    return value->get<int>();
}

// Resolves the engine smoke projectile auto-list head
void ResolveAutoListHead(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    char error[256]{};
    void* site = cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, kAutoListName, error, sizeof(error));
    if (!site)
    {
        g_autoListStatus = "sig_failed";
        BV_LOG_WARN("AutoList unavailable: %s", error);
        return;
    }

    const int relativeOffset = GamedataInt(gamedata, kAutoListName, "offset", 3);
    const int instructionLength = GamedataInt(gamedata, kAutoListName, "rel_size", 7);
    void* target = ResolveRipRelative(static_cast<unsigned char*>(site), relativeOffset, instructionLength);
    if (!target || !memory::IsReadable(target, sizeof(void*)))
    {
        g_autoListStatus = "rel32_failed";
        BV_LOG_WARN("AutoList unavailable: target unreadable");
        return;
    }

    g_autoListHead = static_cast<void**>(target);
    g_autoListStatus = "resolved";
}

// Resolves a bot engine slot through its pawn controller handle
int BotSlotFromBot(int64_t bot)
{
    if (!bot || g_controllerHandleOffset < 0 || g_playerInBotOffset <= 0) return -1;

    int64_t pawn = 0;
    const void* botAddress = reinterpret_cast<const void*>(bot); // NOLINT(performance-no-int-to-ptr)
    if (!memory::Read(botAddress, g_playerInBotOffset, pawn, memory::FailureDomain::Bot)) return -1;
    g_lastPawnPointer.store(static_cast<uint64_t>(pawn), std::memory_order_relaxed);
    if (!pawn) return -1;

    uint32_t handle = 0;
    const void* pawnAddress = reinterpret_cast<const void*>(pawn); // NOLINT(performance-no-int-to-ptr)
    if (!memory::Read(pawnAddress, g_controllerHandleOffset, handle, memory::FailureDomain::Bot)) return -1;
    g_lastControllerHandle.store(handle, std::memory_order_relaxed);
    if (handle == 0U || handle == 0xFFFFFFFFU) return -1;

    const int controllerIndex = CEntityHandle(handle).GetEntryIndex();
    const int slot = controllerIndex - 1;
    if (slot < 0 || slot >= kMaxBots) return -1;
    g_lastBotSlot.store(slot, std::memory_order_relaxed);
    return slot;
}

// Checks and latches one player from the reveal slot mask
bool IsRevealedPlayer(void* player, uint64_t revealMask)
{
    if (revealMask == 0 || !player || g_controllerHandleOffset < 0) return false;

    uint32_t handle = 0;
    std::memcpy(&handle, static_cast<const unsigned char*>(player) + g_controllerHandleOffset, sizeof(handle));
    if (handle == 0U || handle == 0xFFFFFFFFU) return false;

    const int slot = CEntityHandle(handle).GetEntryIndex() - 1;
    if (slot < 0 || slot >= kMaxBots || (revealMask & (1ULL << slot)) == 0)
    {
        return false;
    }

    unsigned int expectedHandle = g_revealHandles[slot].load(std::memory_order_relaxed);
    if (expectedHandle == 0U)
    {
        g_revealHandles[slot].compare_exchange_strong(expectedHandle, handle, std::memory_order_relaxed);
        expectedHandle = g_revealHandles[slot].load(std::memory_order_relaxed);
    }
    return expectedHandle == handle;
}

// Returns the cache index for one bot pointer
size_t BotThresholdCacheIndex(int64_t bot) { return static_cast<size_t>(MixPointerValue(static_cast<uintptr_t>(bot))) & (kCacheSize - 1); }

// Returns a cached threshold and periodically revalidates the bot
int CachedThresholdFromBot(int64_t bot)
{
    if (!bot) return kDefaultThreshold;

    const unsigned int generation = g_cacheGeneration.load(std::memory_order_relaxed);
    const size_t startIndex = BotThresholdCacheIndex(bot);
    BotThresholdCacheEntry* replacement = &g_thresholdCache[startIndex];
    for (size_t probe = 0; probe < kCacheProbeCount; ++probe)
    {
        BotThresholdCacheEntry& entry = g_thresholdCache[(startIndex + probe) & (kCacheSize - 1)];
        if (entry.bot == bot && entry.generation == generation)
        {
            if (entry.usesRemaining > 0)
            {
                --entry.usesRemaining;
                return entry.thresholdMilli;
            }
            replacement = &entry;
            break;
        }
        if (entry.generation != generation || entry.bot == 0)
        {
            replacement = &entry;
            break;
        }
        if (entry.usesRemaining < replacement->usesRemaining) replacement = &entry;
    }

    const int slot = BotSlotFromBot(bot);
    const int threshold = slot >= 0 ? g_botThresholdMilli[slot].load(std::memory_order_relaxed) : kDefaultThreshold;
    replacement->bot = bot;
    replacement->thresholdMilli = threshold;
    replacement->usesRemaining = slot >= 0 ? kCacheRefreshUses : kInvalidCacheRefreshUses;
    replacement->generation = generation;
    return threshold;
}

// Sets the bot threshold for this visibility call.
KHook::Return<int64_t> HookedIsVisiblePos(int64_t self, int64_t, char, void*) noexcept
{
    g_isVisiblePosCalls.fetch_add(1, std::memory_order_relaxed);
    g_thresholdFrames.push_back(g_currentBotThresholdMilli);
    if (g_smokeMode.load(std::memory_order_relaxed) == 1 || g_botThresholdOverrideCount.load(std::memory_order_relaxed) == 0)
    {
        return { KHook::Action::Ignore };
    }

    g_currentBotThresholdMilli = CachedThresholdFromBot(self);
    return { KHook::Action::Ignore };
}

// Restores the enclosing visibility threshold.
KHook::Return<int64_t> IsVisiblePosPost(int64_t, int64_t, char, void*) noexcept
{
    g_currentBotThresholdMilli = g_thresholdFrames.back();
    g_thresholdFrames.pop_back();
    return { KHook::Action::Ignore };
}

// Sets target reveal state for the visibility scan
KHook::Return<bool> HookedIsVisiblePlayer(int64_t self, void* player, char testFov, unsigned char* visibleParts) noexcept
{
    g_revealFrames.push_back(g_currentPlayerRevealed);
    const uint64_t revealMask = g_revealMask.load(std::memory_order_acquire);
    if (revealMask != 0) g_currentPlayerRevealed = IsRevealedPlayer(player, revealMask);
    return { KHook::Action::Ignore };
}

// Restores the enclosing reveal state.
KHook::Return<bool> IsVisiblePlayerPost(int64_t, void*, char, unsigned char*) noexcept
{
    g_currentPlayerRevealed = g_revealFrames.back();
    g_revealFrames.pop_back();
    return { KHook::Action::Ignore };
}

// Determines smoke visibility from density and holes
KHook::Return<bool> HookedIsVisibleThroughSmoke(void* self, const void* from, const void* to) noexcept
{
    g_hitCount.fetch_add(1, std::memory_order_relaxed);
    if (g_currentPlayerRevealed) return { KHook::Action::Supersede, true };

    if (!IsVolumeMode() || !from || !to || !g_getSmokeDensityInLine)
    {
        return { KHook::Action::Ignore };
    }
    if (g_autoListHead && !*g_autoListHead) return { KHook::Action::Supersede, true };

    float fromValues[3]{};
    float toValues[3]{};
    std::memcpy(fromValues, from, sizeof(fromValues));
    std::memcpy(toValues, to, sizeof(toValues));

    const float density = g_getSmokeDensityInLine(fromValues, toValues, nullptr);
    int thresholdMilli = g_densityThresholdMilli.load(std::memory_order_relaxed);
    if (g_currentBotThresholdMilli != kDefaultThreshold) thresholdMilli = g_currentBotThresholdMilli;
    const float threshold = thresholdMilli * 0.001F;
    const float adjustedDensity = BlocksSight(density, threshold) ? AdjustClientDensity(fromValues, toValues, density) : density;
    const bool blocked = BlocksSight(adjustedDensity, threshold);
    if (blocked) g_blockedCount.fetch_add(1, std::memory_order_relaxed);
    return { KHook::Action::Supersede, !blocked };
}

} // namespace

// Installs required and optional smoke hooks
bool Install(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule, char* error, size_t maxLength)
{
    for (int slot = 0; slot < kMaxBots; ++slot)
    {
        g_botThresholdMilli[slot].store(kDefaultThreshold, std::memory_order_relaxed);
        g_revealHandles[slot].store(0, std::memory_order_relaxed);
    }
    g_botThresholdOverrideCount.store(0, std::memory_order_relaxed);
    g_cacheGeneration.fetch_add(1, std::memory_order_relaxed);
    g_revealMask.store(0, std::memory_order_relaxed);

    const cs2_sdk::schema::FieldBinding fields[] = {
        { &g_controllerHandleOffset, "CBasePlayerPawn", "m_hController" },
        { &g_playerInBotOffset, "CBot", "m_pPlayer" },
    };
    cs2_sdk::schema::ResolveFields(fields);

    char signatureError[256] = { 0 };
    void* target = cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, kSmokeFunctionName, signatureError, sizeof(signatureError));
    if (!target)
    {
        ReportError(error, maxLength, "%s", signatureError);
        return false;
    }

    ResolveAutoListHead(gamedata, serverModule);
    if (!g_smokeHook.Install(target, &HookedIsVisibleThroughSmoke))
    {
        ReportError(error, maxLength, "Hook failed: %s", kSmokeFunctionName);
        return false;
    }

    char densityError[256] = { 0 };
    void* densityTarget = cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, kDensityFunctionName, densityError, sizeof(densityError));
    if (densityTarget)
    {
        g_getSmokeDensityInLine = reinterpret_cast<GetSmokeDensityInLineFn>(densityTarget);
    }
    else
    {
        BV_LOG_WARN("Using vanilla smoke: %s", densityError);
    }

    char visibleError[256] = { 0 };
    void* visibleTarget = g_controllerHandleOffset >= 0 && g_playerInBotOffset > 0
                              ? cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, kVisiblePosName, visibleError, sizeof(visibleError))
                              : nullptr;
    if (!visibleTarget || !g_visiblePosHook.Install(visibleTarget, &HookedIsVisiblePos, &IsVisiblePosPost))
    {
        g_visiblePosHook.Remove();
        const char* reason = visibleError;
        if (g_controllerHandleOffset < 0 || g_playerInBotOffset <= 0) reason = "schema offsets missing";
        else if (visibleTarget)
            reason = "IsVisiblePos hook failed";
        BV_LOG_WARN("Per-bot density disabled: %s", reason);
    }

    char visiblePlayerError[256] = { 0 };
    void* visiblePlayerTarget =
        g_controllerHandleOffset >= 0
            ? cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, kVisiblePlayerName, visiblePlayerError, sizeof(visiblePlayerError))
            : nullptr;
    if (!visiblePlayerTarget || !g_visiblePlayerHook.Install(visiblePlayerTarget, &HookedIsVisiblePlayer, &IsVisiblePlayerPost))
    {
        g_visiblePlayerHook.Remove();
        const char* reason = visiblePlayerError;
        if (g_controllerHandleOffset < 0) reason = "schema offset missing";
        else if (visiblePlayerTarget)
            reason = "IsVisiblePlayer hook failed";
        BV_LOG_WARN("Reveal disabled: %s", reason);
    }
    return true;
}

// Removes the smoke hooks and resolved runtime pointers
void Remove()
{
    g_visiblePlayerHook.Remove();
    g_visiblePosHook.Remove();
    g_smokeHook.Remove();
    g_getSmokeDensityInLine = nullptr;
    g_autoListHead = nullptr;
}

// Checks for volume-smoke mode
bool IsVolumeMode() { return g_smokeMode.load(std::memory_order_relaxed) == 0; }

// Checks for active smoke projectiles
bool HasSmokeProjectiles() { return g_autoListHead && *g_autoListHead != nullptr; }

// Calls the engine density function when available
float DensityInLine(const float* from, const float* to)
{
    return g_getSmokeDensityInLine ? g_getSmokeDensityInLine(from, to, nullptr) : 0.0F;
}

// Probes nearby native smoke density with occlusion checks
bool HasSmokeNearPoint(const float* point, float radius)
{
    if (!point || radius <= 0.0F) return false;
    if (!g_getSmokeDensityInLine) return HasSmokeProjectiles();

    static constexpr float kDirections[][3] = {
        { 1.0F, 0.0F, 0.0F },
        { -1.0F, 0.0F, 0.0F },
        { 0.0F, 1.0F, 0.0F },
        { 0.0F, -1.0F, 0.0F },
        { 0.0F, 0.0F, 1.0F },
        { 0.0F, 0.0F, -1.0F },
        { std::numbers::inv_sqrt3_v<float>, std::numbers::inv_sqrt3_v<float>, std::numbers::inv_sqrt3_v<float> },
        { std::numbers::inv_sqrt3_v<float>, std::numbers::inv_sqrt3_v<float>, -std::numbers::inv_sqrt3_v<float> },
        { std::numbers::inv_sqrt3_v<float>, -std::numbers::inv_sqrt3_v<float>, std::numbers::inv_sqrt3_v<float> },
        { std::numbers::inv_sqrt3_v<float>, -std::numbers::inv_sqrt3_v<float>, -std::numbers::inv_sqrt3_v<float> },
        { -std::numbers::inv_sqrt3_v<float>, std::numbers::inv_sqrt3_v<float>, std::numbers::inv_sqrt3_v<float> },
        { -std::numbers::inv_sqrt3_v<float>, std::numbers::inv_sqrt3_v<float>, -std::numbers::inv_sqrt3_v<float> },
        { -std::numbers::inv_sqrt3_v<float>, -std::numbers::inv_sqrt3_v<float>, std::numbers::inv_sqrt3_v<float> },
        { -std::numbers::inv_sqrt3_v<float>, -std::numbers::inv_sqrt3_v<float>, -std::numbers::inv_sqrt3_v<float> }
    };

    for (const auto& direction : kDirections)
    {
        float from[3] = { point[0], point[1], point[2] };
        float to[3] = { point[0] + (direction[0] * radius), point[1] + (direction[1] * radius), point[2] + (direction[2] * radius) };
        float closest[3]{};
        if (g_getSmokeDensityInLine(from, to, closest) > 0.0F && bullet_vision::IsLineUnobstructed(point, closest)) return true;
    }
    return false;
}

// Returns the smoke hook call count
int64_t GetHitCount() { return g_hitCount.load(std::memory_order_relaxed); }

// Returns the blocked line count
int64_t GetBlockedCount() { return g_blockedCount.load(std::memory_order_relaxed); }

// Returns the smoke auto-list status.
const char* GetAutoListStatus() { return g_autoListStatus; }

// Stores the smoke calculation mode
void SetMode(int mode) { g_smokeMode.store(mode, std::memory_order_relaxed); }

// Returns the smoke calculation mode
int GetMode() { return g_smokeMode.load(std::memory_order_relaxed); }

// Stores the global density threshold
void SetDensityThreshold(float value)
{
    if (!config::IsValidFixedPointValue(value)) return;
    g_densityThresholdMilli.store(static_cast<int>(value * 1000.0F), std::memory_order_relaxed);
}

// Returns the global density threshold
float GetDensityThreshold() { return g_densityThresholdMilli.load(std::memory_order_relaxed) * 0.001F; }

// Checks whether the density function was resolved
bool DensityFunctionReady() { return g_getSmokeDensityInLine != nullptr; }

// Stores or clears a slot-specific density threshold
void SetBotDensityThreshold(int slot, float value)
{
    if (slot < 0 || slot >= kMaxBots) return;
    if (!std::isfinite(value) || (value >= 0.0F && !config::IsValidFixedPointValue(value))) return;

    const int next = value < 0.0F ? kDefaultThreshold : static_cast<int>(value * 1000.0F);
    const int previous = g_botThresholdMilli[slot].exchange(next, std::memory_order_relaxed);
    if (previous == next) return;

    if (previous == kDefaultThreshold && next != kDefaultThreshold)
    {
        g_botThresholdOverrideCount.fetch_add(1, std::memory_order_relaxed);
    }
    else if (previous != kDefaultThreshold && next == kDefaultThreshold)
    {
        g_botThresholdOverrideCount.fetch_sub(1, std::memory_order_relaxed);
    }
    g_cacheGeneration.fetch_add(1, std::memory_order_relaxed);
}

// Returns a slot-specific density threshold
float GetBotDensityThreshold(int slot)
{
    if (slot < 0 || slot >= kMaxBots) return -1.0F;
    const int value = g_botThresholdMilli[slot].load(std::memory_order_relaxed);
    return value == kDefaultThreshold ? -1.0F : value * 0.001F;
}

// Returns the maximum bot slot count
int GetMaxBots() { return kMaxBots; }

// Returns the last resolved bot slot
int GetLastBotSlot() { return g_lastBotSlot.load(std::memory_order_relaxed); }

// Checks whether the per-bot hook is installed
bool IsVisiblePosHooked() { return g_visiblePosHook.Active(); }

// Returns the per-bot hook call count
int64_t GetIsVisiblePosCalls() { return g_isVisiblePosCalls.load(std::memory_order_relaxed); }

// Returns the last controller handle
unsigned int GetLastControllerHandle() { return g_lastControllerHandle.load(std::memory_order_relaxed); }

// Returns the last pawn pointer
uint64_t GetLastPawnPointer() { return g_lastPawnPointer.load(std::memory_order_relaxed); }

// Adds a reveal slot and resets its entity generation
void AddRevealSlot(int slot)
{
    if (slot < 0 || slot >= kMaxBots) return;
    g_revealHandles[slot].store(0, std::memory_order_relaxed);
    g_revealMask.fetch_or(1ULL << slot, std::memory_order_release);
}

// Removes one reveal slot
void RemoveRevealSlot(int slot)
{
    if (slot < 0 || slot >= kMaxBots) return;
    g_revealMask.fetch_and(~(1ULL << slot), std::memory_order_release);
    g_revealHandles[slot].store(0, std::memory_order_relaxed);
}

// Clears all reveal slots
void ClearReveals()
{
    g_revealMask.store(0, std::memory_order_release);
    for (auto& revealHandle : g_revealHandles)
    {
        revealHandle.store(0, std::memory_order_relaxed);
    }
}

// Returns the configured reveal mask
uint64_t GetRevealMask() { return g_revealMask.load(std::memory_order_acquire); }

// Returns a revealed player's latched controller handle
unsigned int GetRevealHandle(int slot)
{
    if (slot < 0 || slot >= kMaxBots) return 0;
    return g_revealHandles[slot].load(std::memory_order_relaxed);
}

// Checks whether player visibility is hooked
bool IsVisiblePlayerHooked() { return g_visiblePlayerHook.Active(); }

// Formats a diagnostic density query
int TestLos(float fromX, float fromY, float fromZ, float toX, float toY, float toZ, char* buffer, size_t bufferLength)
{
    if (!buffer || bufferLength < 128) return 0;

    float from[3] = { fromX, fromY, fromZ };
    float to[3] = { toX, toY, toZ };
    int written = std::snprintf(buffer, bufferLength, "from=(%.1f,%.1f,%.1f) to=(%.1f,%.1f,%.1f)\n", fromX, fromY, fromZ, toX, toY, toZ);

    if (!g_getSmokeDensityInLine)
    {
        written += std::snprintf(buffer + written, bufferLength - written, "GetSmokeDensityInLine unresolved -> mode 0 is unavailable\n");
        return written;
    }

    const float density = g_getSmokeDensityInLine(from, to, nullptr);
    const float adjustedDensity = AdjustClientDensity(from, to, density);
    const float threshold = GetDensityThreshold();
    const bool engineBlocked = BlocksSight(density, threshold);
    const bool blocked = BlocksSight(adjustedDensity, threshold);
    written += std::snprintf(buffer + written, bufferLength - written,
                             "density=%.4f adjusted=%.4f threshold=%.4f engineBlock=%d blocked=%d activeHe=%d activeBullets=%d\n", density,
                             adjustedDensity, threshold, engineBlocked ? 1 : 0, blocked ? 1 : 0, he_vision::GetActiveCount(),
                             bullet_vision::GetActiveHoleCount());
    return written;
}
} // namespace cs2bv::smoke_vision
