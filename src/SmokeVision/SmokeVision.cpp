// Smoke visibility hooks and per-bot threshold state

#include "SmokeVision.h"

#include "BulletVision/BulletVision.h"
#include "HeVision/HeVision.h"
#include "hook.h"
#include "memory.h"
#include "platform.h"
#include "schema_resolver.h"
#include "sig_scan.h"

#include <nlohmann/json.hpp>
#include <tier0/dbg.h>

#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

namespace cs2bv::smoke_vision {
namespace {
using IsVisibleThroughSmokeFn = bool(CS2BV_FASTCALL*)(void* self, const void* from, const void* to);
using GetSmokeDensityInLineFn = float(CS2BV_FASTCALL*)(const float* from, const float* to, float* outClosest);
using IsVisiblePosFn = int64_t(CS2BV_FASTCALL*)(int64_t self, int64_t position, char testFov, void* entity);
using IsVisiblePlayerFn = bool(CS2BV_FASTCALL*)(int64_t self, void* player, char testFov, unsigned char* visibleParts);

constexpr const char* kSmokeFunctionName = "CBotManager::IsVisibleThroughSmoke";
constexpr const char* kAutoListName = "g_AutoList_SmokeProj_Head_Server";
constexpr const char* kDensityFunctionName = "GetSmokeDensityInLine";
constexpr const char* kVisiblePosName = "CCSBot::IsVisiblePos";
constexpr const char* kVisiblePlayerName = "CCSBot::IsVisiblePlayer";
constexpr int kMaxBots = 64;
constexpr int kDefaultThreshold = INT_MIN;

IsVisibleThroughSmokeFn g_originalIsVisibleThroughSmoke = nullptr;
GetSmokeDensityInLineFn g_getSmokeDensityInLine = nullptr;
IsVisiblePosFn g_originalIsVisiblePos = nullptr;
IsVisiblePlayerFn g_originalIsVisiblePlayer = nullptr;
Hook g_smokeHook;
Hook g_visiblePosHook;
Hook g_visiblePlayerHook;
void** g_autoListHead = nullptr;

std::atomic<int64_t> g_hitCount{ 0 };
std::atomic<int64_t> g_blockedCount{ 0 };
std::string g_hookedStatus = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)
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

// Calls the resolved native density function for effect modules
float SampleNativeDensity(const float* from, const float* to)
{
    return g_getSmokeDensityInLine ? g_getSmokeDensityInLine(from, to, nullptr) : 0.0F;
}

// Applies HE and bullet effects to native server density
float AdjustClientDensity(const float* from, const float* to, float density)
{
    float adjusted = density;
    if (bullet_vision::GetHolesEnabled()) adjusted = bullet_vision::AdjustDensity(from, to, adjusted, &SampleNativeDensity);
    return he_vision::AdjustDensity(from, to, adjusted, &SampleNativeDensity);
}

// Reports an install error to both the debug sink and plugin loader
void ReportError(char* error, size_t maxLength, const char* format, ...) // NOLINT(modernize-avoid-variadic-functions)
{
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);

    Msg("%s", "[BotVision] ");
    Msg("%s", buffer);
    Msg("%s", "\n");
    if (error && maxLength > 0) std::snprintf(error, maxLength, "%s", buffer);
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
    if (!signatureStart || relativeOffset <= 0 || instructionLength < relativeOffset + 4) return nullptr;

    int32_t displacement = 0;
    if (!memory::Read(signatureStart, static_cast<size_t>(relativeOffset), displacement, memory::FailureDomain::Install)) return nullptr;
    return signatureStart + instructionLength + displacement;
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

// Resolves a signature already replaced by a rel32 detour
void* ResolveWithDetourFallback(
    const nlohmann::json& gamedata, const sig::ModuleInfo& module, const char* name, bool& usedFallback, char* error, size_t errorLength)
{
    usedFallback = false;

    char primaryError[256] = { 0 };
    void* target = sig::ResolveSig(gamedata, module, name, primaryError, sizeof(primaryError));
    if (target) return target;

    const std::string signature = sig::FindPlatformSig(gamedata, name);
    std::vector<uint8_t> pattern;
    std::vector<bool> wildcards;
    constexpr size_t kRel32JumpSize = 5;
    if (signature.empty() || !sig::ParseSigString(signature, pattern, wildcards) || pattern.size() <= kRel32JumpSize)
    {
        if (error && errorLength > 0) std::snprintf(error, errorLength, "%s", primaryError);
        return nullptr;
    }

    void* resolved = nullptr;
    size_t matchCount = 0;
    const size_t tailSize = pattern.size() - kRel32JumpSize;
    for (const sig::ModuleSegment& segment : module.segments)
    {
        if (!segment.base || segment.size < pattern.size()) continue;

        for (size_t offset = kRel32JumpSize; offset + tailSize <= segment.size; ++offset)
        {
            bool matches = true;
            for (size_t index = 0; index < tailSize; ++index)
            {
                const size_t patternIndex = kRel32JumpSize + index;
                if (!wildcards[patternIndex] && segment.base[offset + index] != pattern[patternIndex])
                {
                    matches = false;
                    break;
                }
            }
            if (!matches) continue;

            unsigned char* candidate = segment.base + offset - kRel32JumpSize;
            if (candidate[0] != 0xE9) continue;

            int32_t displacement = 0;
            std::memcpy(&displacement, candidate + 1, sizeof(displacement));
            void* detourTarget = candidate + kRel32JumpSize + displacement;
            if (!memory::IsReadable(detourTarget, 1)) continue;

            resolved = candidate;
            ++matchCount;
        }
    }

    if (matchCount == 1)
    {
        usedFallback = true;
        return resolved;
    }

    if (error && errorLength > 0)
    {
        std::snprintf(error, errorLength, "%s; rel32 detour tail matches=%zu", primaryError, matchCount);
    }
    return nullptr;
}

// Resolves the engine smoke projectile auto-list head
void ResolveAutoListHead(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    const std::string signature = sig::FindPlatformSig(gamedata, kAutoListName);
    if (signature.empty())
    {
        g_hookedStatus = "sig_empty";
        Msg("%s", "[BotVision] AutoList entry/sig missing; hook disabled\n");
        return;
    }

    const int relativeOffset = GamedataInt(gamedata, kAutoListName, "offset", 3);
    const int instructionLength = GamedataInt(gamedata, kAutoListName, "rel_size", 7);
    std::vector<uint8_t> pattern;
    std::vector<bool> wildcards;
    if (!sig::ParseSigString(signature, pattern, wildcards))
    {
        g_hookedStatus = "sig_parse_failed";
        Msg("%s", "[BotVision] AutoList sig parse failed\n");
        return;
    }

    void* site = sig::FindPatternIn(serverModule, pattern, wildcards);
    if (!site)
    {
        g_hookedStatus = "sig_not_found";
        Msg("%s", "[BotVision] AutoList sig not found\n");
        return;
    }

    void* target = ResolveRipRelative(static_cast<unsigned char*>(site), relativeOffset, instructionLength);
    if (!target)
    {
        g_hookedStatus = "rel32_failed";
        Msg("%s", "[BotVision] AutoList rel32 resolve failed\n");
        return;
    }

    g_autoListHead = static_cast<void**>(target);
    char status[96];
    std::snprintf(status, sizeof(status), "ON@%p", target);
    g_hookedStatus = status;
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

    const int controllerIndex = static_cast<int>(handle & 0x7FFFU);
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

    const int slot = static_cast<int>(handle & 0x7FFFU) - 1;
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

// Stamps a bot-specific threshold around the original visibility call
int64_t CS2BV_FASTCALL HookedIsVisiblePos(int64_t self, int64_t position, char testFov, void* entity)
{
    g_isVisiblePosCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_smokeMode.load(std::memory_order_relaxed) == 1 || g_botThresholdOverrideCount.load(std::memory_order_relaxed) == 0)
    {
        return g_originalIsVisiblePos(self, position, testFov, entity);
    }

    const int threshold = CachedThresholdFromBot(self);
    if (threshold == kDefaultThreshold)
    {
        if (g_currentBotThresholdMilli == kDefaultThreshold)
        {
            return g_originalIsVisiblePos(self, position, testFov, entity);
        }

        const int previous = g_currentBotThresholdMilli;
        g_currentBotThresholdMilli = kDefaultThreshold;
        const int64_t result = g_originalIsVisiblePos(self, position, testFov, entity);
        g_currentBotThresholdMilli = previous;
        return result;
    }

    const int previous = g_currentBotThresholdMilli;
    g_currentBotThresholdMilli = threshold;
    const int64_t result = g_originalIsVisiblePos(self, position, testFov, entity);
    g_currentBotThresholdMilli = previous;
    return result;
}

// Stamps target reveal state around one complete player visibility scan
bool CS2BV_FASTCALL HookedIsVisiblePlayer(int64_t self, void* player, char testFov, unsigned char* visibleParts)
{
    const uint64_t revealMask = g_revealMask.load(std::memory_order_acquire);
    if (revealMask == 0)
    {
        return g_originalIsVisiblePlayer(self, player, testFov, visibleParts);
    }

    const bool previousReveal = g_currentPlayerRevealed;
    g_currentPlayerRevealed = IsRevealedPlayer(player, revealMask);
    const bool result = g_originalIsVisiblePlayer(self, player, testFov, visibleParts);
    g_currentPlayerRevealed = previousReveal;
    return result;
}

// Replaces binary smoke visibility with density and hole checks
bool CS2BV_FASTCALL HookedIsVisibleThroughSmoke(void* self, const void* from, const void* to)
{
    g_hitCount.fetch_add(1, std::memory_order_relaxed);
    if (g_currentPlayerRevealed) return true;

    if (!IsVolumeMode() || !from || !to || !g_getSmokeDensityInLine)
    {
        return g_originalIsVisibleThroughSmoke(self, from, to);
    }

    float fromValues[3]{};
    float toValues[3]{};
    if (!memory::Read(from, 0, fromValues, memory::FailureDomain::Smoke) || !memory::Read(to, 0, toValues, memory::FailureDomain::Smoke))
    {
        return g_originalIsVisibleThroughSmoke(self, from, to);
    }

    const float density = g_getSmokeDensityInLine(fromValues, toValues, nullptr);
    int thresholdMilli = g_densityThresholdMilli.load(std::memory_order_relaxed);
    if (g_currentBotThresholdMilli != kDefaultThreshold) thresholdMilli = g_currentBotThresholdMilli;
    const float threshold = thresholdMilli * 0.001F;
    if (density >= threshold)
    {
        if (AdjustClientDensity(fromValues, toValues, density) < threshold) return true;

        g_blockedCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

} // namespace

// Installs required and optional smoke hooks
bool Install(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule, char* error, size_t maxLength)
{
    for (int slot = 0; slot < kMaxBots; ++slot)
    {
        g_botThresholdMilli[slot].store(kDefaultThreshold, std::memory_order_relaxed);
        g_revealHandles[slot].store(0, std::memory_order_relaxed);
    }
    g_botThresholdOverrideCount.store(0, std::memory_order_relaxed);
    g_cacheGeneration.fetch_add(1, std::memory_order_relaxed);
    g_revealMask.store(0, std::memory_order_relaxed);

    g_controllerHandleOffset = schema::GetFieldOffset("CBasePlayerPawn", "m_hController");
    g_playerInBotOffset = sig::ResolveOffset(gamedata, "Bot::m_pPlayer", -1);

    char signatureError[256] = { 0 };
    void* target = sig::ResolveSig(gamedata, serverModule, kSmokeFunctionName, signatureError, sizeof(signatureError));
    if (!target)
    {
        ReportError(error, maxLength, "%s", signatureError);
        return false;
    }

    ResolveAutoListHead(gamedata, serverModule);
    if (!g_smokeHook.Create(target, reinterpret_cast<void*>(&HookedIsVisibleThroughSmoke),
                            reinterpret_cast<void**>(&g_originalIsVisibleThroughSmoke)))
    {
        ReportError(error, maxLength, "funchook_prepare failed for %s", kSmokeFunctionName);
        return false;
    }
    if (!g_smokeHook.Enable())
    {
        g_smokeHook.Remove();
        g_originalIsVisibleThroughSmoke = nullptr;
        ReportError(error, maxLength, "funchook_install failed for %s", kSmokeFunctionName);
        return false;
    }

    char densityError[256] = { 0 };
    void* densityTarget = sig::ResolveSig(gamedata, serverModule, kDensityFunctionName, densityError, sizeof(densityError));
    if (densityTarget)
    {
        g_getSmokeDensityInLine = reinterpret_cast<GetSmokeDensityInLineFn>(densityTarget);
    }
    else
    {
        char warning[320];
        std::snprintf(warning, sizeof(warning), "[BotVision] %s; mode 0 falls back to vanilla-smoke\n", densityError);
        Msg("%s", warning);
    }

    char visibleError[256] = { 0 };
    bool chainedDetour = false;
    void* visibleTarget =
        g_controllerHandleOffset >= 0 && g_playerInBotOffset > 0
            ? ResolveWithDetourFallback(gamedata, serverModule, kVisiblePosName, chainedDetour, visibleError, sizeof(visibleError))
            : nullptr;
    if (visibleTarget &&
        g_visiblePosHook.Create(visibleTarget, reinterpret_cast<void*>(&HookedIsVisiblePos),
                                reinterpret_cast<void**>(&g_originalIsVisiblePos)) &&
        g_visiblePosHook.Enable())
    {
        (void)chainedDetour;
    }
    else
    {
        g_visiblePosHook.Remove();
        g_originalIsVisiblePos = nullptr;
        char warning[320];
        const char* reason = visibleError;
        if (g_controllerHandleOffset < 0 || g_playerInBotOffset <= 0) reason = "required offset unavailable";
        else if (visibleTarget)
            reason = "funchook error";
        std::snprintf(warning, sizeof(warning), "[BotVision] IsVisiblePos hook failed (%s); per-bot density disabled\n", reason);
        Msg("%s", warning);
    }

    char visiblePlayerError[256] = { 0 };
    bool chainedPlayerDetour = false;
    void* visiblePlayerTarget = g_controllerHandleOffset >= 0
                                    ? ResolveWithDetourFallback(gamedata, serverModule, kVisiblePlayerName, chainedPlayerDetour,
                                                                visiblePlayerError, sizeof(visiblePlayerError))
                                    : nullptr;
    if (visiblePlayerTarget &&
        g_visiblePlayerHook.Create(visiblePlayerTarget, reinterpret_cast<void*>(&HookedIsVisiblePlayer),
                                   reinterpret_cast<void**>(&g_originalIsVisiblePlayer)) &&
        g_visiblePlayerHook.Enable())
    {
        (void)chainedPlayerDetour;
    }
    else
    {
        g_visiblePlayerHook.Remove();
        g_originalIsVisiblePlayer = nullptr;
        char warning[320];
        const char* reason = visiblePlayerError;
        if (g_controllerHandleOffset < 0) reason = "required offset unavailable";
        else if (visiblePlayerTarget)
            reason = "funchook error";
        std::snprintf(warning, sizeof(warning), "[BotVision] IsVisiblePlayer hook failed (%s); target reveal disabled\n", reason);
        Msg("%s", warning);
    }
    return true;
}

// Removes the smoke hooks and resolved runtime pointers
void Remove()
{
    g_visiblePlayerHook.Remove();
    g_originalIsVisiblePlayer = nullptr;
    g_visiblePosHook.Remove();
    g_originalIsVisiblePos = nullptr;
    g_smokeHook.Remove();
    g_originalIsVisibleThroughSmoke = nullptr;
    g_getSmokeDensityInLine = nullptr;
    g_autoListHead = nullptr;
}

// Checks for volume-smoke mode
bool IsVolumeMode() { return g_smokeMode.load(std::memory_order_relaxed) == 0; }

// Checks whether the auto-list pointer is available
bool AutoListReady() { return g_autoListHead != nullptr; }

// Safely checks whether the smoke auto-list is nonempty
bool HasSmokeProjectiles()
{
    void* head = nullptr;
    return g_autoListHead && memory::Read(static_cast<const void*>(g_autoListHead), 0, head, memory::FailureDomain::Smoke) &&
           head != nullptr;
}

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

// Returns the smoke hook diagnostic state
const char* GetHookedStatus() { return g_hookedStatus.c_str(); }

// Stores the smoke calculation mode
void SetMode(int mode) { g_smokeMode.store(mode, std::memory_order_relaxed); }

// Returns the smoke calculation mode
int GetMode() { return g_smokeMode.load(std::memory_order_relaxed); }

// Stores the global density threshold
void SetDensityThreshold(float value) { g_densityThresholdMilli.store(static_cast<int>(value * 1000.0F), std::memory_order_relaxed); }

// Returns the global density threshold
float GetDensityThreshold() { return g_densityThresholdMilli.load(std::memory_order_relaxed) * 0.001F; }

// Checks whether the density function was resolved
bool DensityFunctionReady() { return g_getSmokeDensityInLine != nullptr; }

// Stores or clears a slot-specific density threshold
void SetBotDensityThreshold(int slot, float value)
{
    if (slot < 0 || slot >= kMaxBots) return;

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
bool IsVisiblePosHooked() { return g_originalIsVisiblePos != nullptr; }

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
bool IsVisiblePlayerHooked() { return g_originalIsVisiblePlayer != nullptr; }

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
    const bool engineBlocked = density >= threshold;
    const bool blocked = adjustedDensity >= threshold;
    written += std::snprintf(buffer + written, bufferLength - written,
                             "density=%.4f adjusted=%.4f threshold=%.4f engineBlock=%d blocked=%d activeHe=%d activeBullets=%d\n", density,
                             adjustedDensity, threshold, engineBlocked ? 1 : 0, blocked ? 1 : 0, he_vision::GetActiveCount(),
                             bullet_vision::GetActiveHoleCount());
    return written;
}
} // namespace cs2bv::smoke_vision
