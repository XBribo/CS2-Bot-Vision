#include "core/log.h"
#include "core/config.h"
#include "core/cs2_sdk/src/core/gameconfig.h"

#include "SmokeVision.h"
#include "SmokeDensity.h"

#include "core/cs2_sdk/src/utils/hooks.h"
#include "memory.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/cs2_sdk/src/core/memory_module.h"

#include <nlohmann/json.hpp>
#include <tier0/dbg.h>
#include <entityhandle.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace cs2bv::smoke_vision {
namespace {
constexpr const char* kSmokeFunctionName = "CBotManager::IsVisibleThroughSmoke";
constexpr const char* kVisiblePosName = "CCSBot::IsVisiblePos";
constexpr const char* kVisiblePlayerName = "CCSBot::IsVisiblePlayer";
constexpr int kMaxBots = 64;
constexpr int kDefaultThreshold = INT_MIN;

cs2_sdk::hooks::NativeHook<bool, void*, const void*, const void*> g_smokeHook;
cs2_sdk::hooks::NativeHook<int64_t, int64_t, int64_t, char, void*> g_visiblePosHook;
cs2_sdk::hooks::NativeHook<bool, int64_t, void*, char, unsigned char*> g_visiblePlayerHook;

std::atomic<int64_t> g_hitCount{ 0 };
std::atomic<int64_t> g_blockedCount{ 0 };
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

bool BlocksSight(float density, float threshold) { return density > 0.0F && density >= threshold; }

uint64_t MixPointerValue(uintptr_t value)
{
    auto key = static_cast<uint64_t>(value);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return key;
}

int SlotFromController(uint32_t handle)
{
    if (handle == 0U || handle == 0xFFFFFFFFU) return -1;
    const int slot = CEntityHandle(handle).GetEntryIndex() - 1;
    return slot >= 0 && slot < kMaxBots ? slot : -1;
}

int BotSlotFromBot(int64_t bot)
{
    int64_t pawn = 0;
    const void* botAddress = reinterpret_cast<const void*>(bot); // NOLINT(performance-no-int-to-ptr)
    if (!memory::Read(botAddress, g_playerInBotOffset, pawn, memory::FailureDomain::Bot)) return -1;
    g_lastPawnPointer.store(static_cast<uint64_t>(pawn), std::memory_order_relaxed);
    if (!pawn) return -1;

    uint32_t handle = 0;
    const void* pawnAddress = reinterpret_cast<const void*>(pawn); // NOLINT(performance-no-int-to-ptr)
    if (!memory::Read(pawnAddress, g_controllerHandleOffset, handle, memory::FailureDomain::Bot)) return -1;
    g_lastControllerHandle.store(handle, std::memory_order_relaxed);
    const int slot = SlotFromController(handle);
    if (slot >= 0) g_lastBotSlot.store(slot, std::memory_order_relaxed);
    return slot;
}

bool IsRevealedPlayer(void* player, uint64_t revealMask)
{
    if (!revealMask || !player) return false;

    uint32_t handle = 0;
    std::memcpy(&handle, static_cast<const unsigned char*>(player) + g_controllerHandleOffset, sizeof(handle));
    const int slot = SlotFromController(handle);
    if (slot < 0 || (revealMask & (1ULL << slot)) == 0) return false;

    unsigned int expectedHandle = g_revealHandles[slot].load(std::memory_order_relaxed);
    if (expectedHandle == 0U)
    {
        g_revealHandles[slot].compare_exchange_strong(expectedHandle, handle, std::memory_order_relaxed);
        expectedHandle = g_revealHandles[slot].load(std::memory_order_relaxed);
    }
    return expectedHandle == handle;
}

size_t BotThresholdCacheIndex(int64_t bot) { return static_cast<size_t>(MixPointerValue(static_cast<uintptr_t>(bot))) & (kCacheSize - 1); }

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

KHook::Return<int64_t> HookedIsVisiblePos(int64_t self, int64_t, char, void*) noexcept
{
    g_isVisiblePosCalls.fetch_add(1, std::memory_order_relaxed);
    g_thresholdFrames.push_back(g_currentBotThresholdMilli);
    g_currentBotThresholdMilli = IsVolumeMode() && g_botThresholdOverrideCount.load(std::memory_order_relaxed) != 0
                                     ? CachedThresholdFromBot(self) : kDefaultThreshold;
    return { KHook::Action::Ignore };
}

KHook::Return<int64_t> IsVisiblePosPost(int64_t, int64_t, char, void*) noexcept
{
    g_currentBotThresholdMilli = g_thresholdFrames.back();
    g_thresholdFrames.pop_back();
    return { KHook::Action::Ignore };
}

KHook::Return<bool> HookedIsVisiblePlayer(int64_t self, void* player, char testFov, unsigned char* visibleParts) noexcept
{
    g_revealFrames.push_back(g_currentPlayerRevealed);
    const uint64_t revealMask = g_revealMask.load(std::memory_order_acquire);
    g_currentPlayerRevealed = IsRevealedPlayer(player, revealMask);
    return { KHook::Action::Ignore };
}

KHook::Return<bool> IsVisiblePlayerPost(int64_t, void*, char, unsigned char*) noexcept
{
    g_currentPlayerRevealed = g_revealFrames.back();
    g_revealFrames.pop_back();
    return { KHook::Action::Ignore };
}

KHook::Return<bool> HookedIsVisibleThroughSmoke(void* self, const void* from, const void* to) noexcept
{
    g_hitCount.fetch_add(1, std::memory_order_relaxed);
    if (g_currentPlayerRevealed) return { KHook::Action::Supersede, true };

    if (!IsVolumeMode()) return { KHook::Action::Ignore };
    const float density = smoke_density::Density(static_cast<const float*>(from), static_cast<const float*>(to), true);
    if (!std::isfinite(density)) return { KHook::Action::Ignore };
    int thresholdMilli = g_densityThresholdMilli.load(std::memory_order_relaxed);
    if (g_currentBotThresholdMilli != kDefaultThreshold) thresholdMilli = g_currentBotThresholdMilli;
    const float threshold = thresholdMilli * 0.001F;
    const bool blocked = BlocksSight(density, threshold);
    if (blocked) g_blockedCount.fetch_add(1, std::memory_order_relaxed);
    return { KHook::Action::Supersede, !blocked };
}

} // namespace

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

    void* visibilityTarget = nullptr;
    if (!smoke_density::Install(gamedata, serverModule, visibilityTarget, error, maxLength))
    {
        if (IsVolumeMode()) return false;
        BV_LOG_WARN("Stock smoke mode: %s", error && *error ? error : smoke_density::Status());
        visibilityTarget = cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, kSmokeFunctionName, error, maxLength);
        if (!visibilityTarget) return false;
    }
    if (!g_smokeHook.Install(visibilityTarget, &HookedIsVisibleThroughSmoke))
    {
        if (error && maxLength) std::snprintf(error, maxLength, "Hook failed: %s", kSmokeFunctionName);
        return false;
    }

    const auto installOptional = [&](bool fieldsReady, const char* name, auto& hook, auto pre, auto post) {
        char detail[256]{};
        const char* reason = "missing schema offset";
        if (fieldsReady)
        {
            if (void* target = cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, name, detail, sizeof(detail)))
            {
                if (hook.Install(target, pre, post)) return;
                reason = "hook registration failed";
            }
            else reason = detail;
        }
        hook.Remove();
        BV_LOG_WARN("%s disabled: %s", name, reason);
    };
    installOptional(g_controllerHandleOffset >= 0 && g_playerInBotOffset > 0, kVisiblePosName, g_visiblePosHook,
                    &HookedIsVisiblePos, &IsVisiblePosPost);
    installOptional(g_controllerHandleOffset >= 0, kVisiblePlayerName, g_visiblePlayerHook, &HookedIsVisiblePlayer, &IsVisiblePlayerPost);
    return true;
}

void Remove()
{
    g_visiblePlayerHook.Remove();
    g_visiblePosHook.Remove();
    g_smokeHook.Remove();
    smoke_density::Remove();
}

bool IsVolumeMode() { return g_smokeMode.load(std::memory_order_relaxed) == 0; }

int64_t GetHitCount() { return g_hitCount.load(std::memory_order_relaxed); }

int64_t GetBlockedCount() { return g_blockedCount.load(std::memory_order_relaxed); }

const char* GetAutoListStatus() { return smoke_density::ListResolved() ? "resolved" : "unavailable"; }

void SetMode(int mode) { g_smokeMode.store(mode, std::memory_order_relaxed); }

int GetMode() { return g_smokeMode.load(std::memory_order_relaxed); }

void SetDensityThreshold(float value)
{
    if (!config::IsValidFixedPointValue(value)) return;
    g_densityThresholdMilli.store(static_cast<int>(value * 1000.0F), std::memory_order_relaxed);
}

float GetDensityThreshold() { return g_densityThresholdMilli.load(std::memory_order_relaxed) * 0.001F; }

bool DensityFunctionReady() { return smoke_density::Ready(); }

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

float GetBotDensityThreshold(int slot)
{
    if (slot < 0 || slot >= kMaxBots) return -1.0F;
    const int value = g_botThresholdMilli[slot].load(std::memory_order_relaxed);
    return value == kDefaultThreshold ? -1.0F : value * 0.001F;
}

int GetMaxBots() { return kMaxBots; }

int GetLastBotSlot() { return g_lastBotSlot.load(std::memory_order_relaxed); }

bool IsVisiblePosHooked() { return g_visiblePosHook.Active(); }

int64_t GetIsVisiblePosCalls() { return g_isVisiblePosCalls.load(std::memory_order_relaxed); }

unsigned int GetLastControllerHandle() { return g_lastControllerHandle.load(std::memory_order_relaxed); }

uint64_t GetLastPawnPointer() { return g_lastPawnPointer.load(std::memory_order_relaxed); }

void AddRevealSlot(int slot)
{
    if (slot < 0 || slot >= kMaxBots) return;
    g_revealHandles[slot].store(0, std::memory_order_relaxed);
    g_revealMask.fetch_or(1ULL << slot, std::memory_order_release);
}

void RemoveRevealSlot(int slot)
{
    if (slot < 0 || slot >= kMaxBots) return;
    g_revealMask.fetch_and(~(1ULL << slot), std::memory_order_release);
    g_revealHandles[slot].store(0, std::memory_order_relaxed);
}

void ClearReveals()
{
    g_revealMask.store(0, std::memory_order_release);
    for (auto& revealHandle : g_revealHandles)
    {
        revealHandle.store(0, std::memory_order_relaxed);
    }
}

uint64_t GetRevealMask() { return g_revealMask.load(std::memory_order_acquire); }

unsigned int GetRevealHandle(int slot)
{
    if (slot < 0 || slot >= kMaxBots) return 0;
    return g_revealHandles[slot].load(std::memory_order_relaxed);
}

bool IsVisiblePlayerHooked() { return g_visiblePlayerHook.Active(); }

int TestLos(float fromX, float fromY, float fromZ, float toX, float toY, float toZ, char* buffer, size_t bufferLength)
{
    if (!buffer || !bufferLength) return 0;
    size_t written = 0;
    auto append = [&](const char* format, auto... args) {
        const size_t remaining = bufferLength - written;
        const int needed = std::snprintf(buffer + written, remaining, format, args...);
        if (needed < 0)
        {
            buffer[written] = '\0';
            return false;
        }
        written += std::min(static_cast<size_t>(needed), remaining - 1);
        return static_cast<size_t>(needed) < remaining;
    };

    if (!append("from=(%.1f,%.1f,%.1f) to=(%.1f,%.1f,%.1f)\n", fromX, fromY, fromZ, toX, toY, toZ))
        return static_cast<int>(written);

    if (smoke_density::Ready())
    {
        const float from[]{ fromX, fromY, fromZ }, to[]{ toX, toY, toZ };
        const float raw = smoke_density::Density(from, to, false), adjusted = smoke_density::Density(from, to, true);
        if (std::isfinite(raw) && std::isfinite(adjusted))
        {
            const float threshold = GetDensityThreshold();
            const auto [he, bullets] = smoke_density::ActiveEffectCounts();
            append("density=%.4f adjusted=%.4f threshold=%.4f engineBlock=%d blocked=%d activeHe=%d activeBullets=%d\n",
                   raw, adjusted, threshold, BlocksSight(raw, threshold), BlocksSight(adjusted, threshold), he, bullets);
            return static_cast<int>(written);
        }
    }
    append("query unavailable (model=%s)\n", smoke_density::Status());
    return static_cast<int>(written);
}
} // namespace cs2bv::smoke_vision
