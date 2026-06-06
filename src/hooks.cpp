#include "hooks.h"
#include "sig_scan.h"

#include <Windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using IsVisibleThroughSmoke_t = bool(__fastcall *)(void *self, const void *from, const void *to);

using GetSmokeDensityInLine_t = float(__fastcall *)(const float *from, const float *to, float *outClosest);

static IsVisibleThroughSmoke_t g_origIsVisibleThroughSmoke = nullptr;
static GetSmokeDensityInLine_t g_fnGetSmokeDensityInLine = nullptr;
static std::atomic<long long> g_hitCount{0};
static std::atomic<long long> g_blockedCount{0};
static void **g_pAutoListHead = nullptr;
static std::string g_hookedStatus = "not_attempted"; // bv_status
static std::atomic<int> g_smokeMode{0};
static std::atomic<int> g_densityThrMilli{200}; // bv_density_threshold * 1000 (default 0.2 → 200)

static const char *kFuncName = "CBotManager::IsVisibleThroughSmoke";
static const char *kHeadName = "g_AutoList_SmokeProj_Head_Server";
static const char *kDensityFnName = "GetSmokeDensityInLine"; // sub_18093FC00

static bool __fastcall HookedIsVisibleThroughSmoke(void *self, const void *from, const void *to)
{
    g_hitCount.fetch_add(1, std::memory_order_relaxed);

    int mode = g_smokeMode.load(std::memory_order_relaxed);
    if (mode == 1)
        return g_origIsVisibleThroughSmoke(self, from, to);
    if (!from || !to)
        return g_origIsVisibleThroughSmoke(self, from, to);

    if (!g_fnGetSmokeDensityInLine)
        return g_origIsVisibleThroughSmoke(self, from, to);
    const float *fa = static_cast<const float *>(from);
    const float *fb = static_cast<const float *>(to);
    float dens = g_fnGetSmokeDensityInLine(fa, fb, nullptr);
    float thr = g_densityThrMilli.load(std::memory_order_relaxed) * 0.001f;
    if (dens >= thr)
    {
        g_blockedCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

namespace cs2bv::hooks
{

    static void ReportError(char *error, size_t maxlen, const char *fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        OutputDebugStringA("[BotVision] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
        if (error && maxlen > 0)
            std::snprintf(error, maxlen, "%s", buf);
    }

    static void *ResolveRipRelative(unsigned char *sigStart, int relOffset, int instLen)
    {
        if (!sigStart || relOffset <= 0 || instLen < relOffset + 4)
            return nullptr;
        int32_t disp = *reinterpret_cast<int32_t *>(sigStart + relOffset);
        return sigStart + instLen + disp;
    }

    // Read an int field from a gamedata
    static int GamedataInt(const nlohmann::json &gamedata, const char *name,
                           const char *key, int defVal)
    {
        auto it = gamedata.find(name);
        if (it == gamedata.end() || !it->is_object())
            return defVal;
        auto vit = it->find(key);
        if (vit == it->end() || !vit->is_number_integer())
            return defVal;
        return vit->get<int>();
    }

    // Resolve g_AutoList_SmokeProj_Head_Server
    static void TryResolveAutoListHead(const nlohmann::json &gamedata,
                                       const cs2bv::sig::ModuleInfo &serverMod)
    {
        std::string sigStr = cs2bv::sig::FindPlatformSig(gamedata, kHeadName);
        if (sigStr.empty())
        {
            g_hookedStatus = "sig_empty";
            OutputDebugStringA("[BotVision] AutoList entry/sig missing; hook disabled\n");
            return;
        }
        int relOff = GamedataInt(gamedata, kHeadName, "offset", 3);
        int instLen = GamedataInt(gamedata, kHeadName, "rel_size", 7);

        std::vector<uint8_t> pat;
        std::vector<bool> wild;
        if (!cs2bv::sig::ParseSigString(sigStr, pat, wild))
        {
            g_hookedStatus = "sig_parse_failed";
            OutputDebugStringA("[BotVision] AutoList sig parse failed\n");
            return;
        }
        void *site = cs2bv::sig::FindPatternIn(serverMod, pat, wild);
        if (!site)
        {
            g_hookedStatus = "sig_not_found";
            OutputDebugStringA("[BotVision] AutoList sig not found\n");
            return;
        }
        void *target = ResolveRipRelative(static_cast<unsigned char *>(site), relOff, instLen);
        if (!target)
        {
            g_hookedStatus = "rel32_failed";
            OutputDebugStringA("[BotVision] AutoList rel32 resolve failed\n");
            return;
        }
        g_pAutoListHead = static_cast<void **>(target);
        char dbg[160];
        std::snprintf(dbg, sizeof(dbg), "[BotVision] AutoList head @ %p (hook active)\n", target);
        OutputDebugStringA(dbg);

        char status[96];
        std::snprintf(status, sizeof(status), "ON@%p", target);
        g_hookedStatus = status;
    }

    bool Install(const std::string &gamedataPath, void *serverInterface, char *error, size_t maxlen)
    {
        nlohmann::json gamedata;
        if (!cs2bv::sig::LoadGamedata(gamedataPath.c_str(), gamedata))
        {
            ReportError(error, maxlen, "failed to read/parse gamedata.json at %s", gamedataPath.c_str());
            return false;
        }

        cs2bv::sig::ModuleInfo server = cs2bv::sig::ModuleFromInterfacePtr(serverInterface);
        if (!server)
        {
            ReportError(error, maxlen, "could not resolve CS2 server module from interface ptr=%p", serverInterface);
            return false;
        }

        char sigErr[256] = {0};
        void *target = cs2bv::sig::ResolveSig(gamedata, server, kFuncName, sigErr, sizeof(sigErr));
        if (!target)
        {
            ReportError(error, maxlen, "%s", sigErr);
            return false;
        }

        char buf[160];
        std::snprintf(buf, sizeof(buf), "[BotVision] %s @ %p (RVA 0x%llX)\n", kFuncName, target,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target) -
                                                      reinterpret_cast<uintptr_t>(server.Base)));
        OutputDebugStringA(buf);

        TryResolveAutoListHead(gamedata, server);

        if (MH_Initialize() != MH_OK)
        {
            ReportError(error, maxlen, "MH_Initialize failed");
            return false;
        }
        if (MH_CreateHook(target, reinterpret_cast<void *>(&HookedIsVisibleThroughSmoke),
                          reinterpret_cast<void **>(&g_origIsVisibleThroughSmoke)) != MH_OK)
        {
            ReportError(error, maxlen, "MH_CreateHook failed");
            return false;
        }
        if (MH_EnableHook(target) != MH_OK)
        {
            ReportError(error, maxlen, "MH_EnableHook failed");
            return false;
        }

        // Resolve GetSmokeDensityInLine
        {
            char dfErr[256] = {0};
            void *dfTarget = cs2bv::sig::ResolveSig(gamedata, server, kDensityFnName, dfErr, sizeof(dfErr));
            if (dfTarget)
            {
                g_fnGetSmokeDensityInLine = reinterpret_cast<GetSmokeDensityInLine_t>(dfTarget);
                char db[160];
                std::snprintf(db, sizeof(db),
                              "[BotVision] GetSmokeDensityInLine @ %p (mode 0 active)\n", dfTarget);
                OutputDebugStringA(db);
            }
            else
            {
                char db[320];
                std::snprintf(db, sizeof(db),
                              "[BotVision] %s; mode 0 falls back to ball-smoke\n", dfErr);
                OutputDebugStringA(db);
            }
        }

        OutputDebugStringA("[BotVision] detour installed\n");
        return true;
    }

    void Remove()
    {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        char buf[160];
        std::snprintf(buf, sizeof(buf), "[BotVision] removed: hits=%lld blocked=%lld\n",
                      static_cast<long long>(g_hitCount.load()),
                      static_cast<long long>(g_blockedCount.load()));
        OutputDebugStringA(buf);
    }

    long long GetHitCount() { return g_hitCount.load(std::memory_order_relaxed); }
    long long GetBlockedCount() { return g_blockedCount.load(std::memory_order_relaxed); }
    bool IsHookedActive() { return g_pAutoListHead != nullptr; }
    const char *GetHookedStatus() { return g_hookedStatus.c_str(); }
    void SetSmokeMode(int mode) { g_smokeMode.store(mode, std::memory_order_relaxed); }
    int GetSmokeMode() { return g_smokeMode.load(std::memory_order_relaxed); }
    void SetDensityThreshold(float v) { g_densityThrMilli.store((int)(v * 1000), std::memory_order_relaxed); }
    float GetDensityThreshold() { return g_densityThrMilli.load(std::memory_order_relaxed) * 0.001f; }
    bool IsDensityFnResolved() { return g_fnGetSmokeDensityInLine != nullptr; }

    int TestLos(float fx, float fy, float fz, float tx, float ty, float tz,
                char *buf, size_t buflen)
    {
        if (!buf || buflen < 128)
            return 0;
        float from[3] = {fx, fy, fz};
        float to[3] = {tx, ty, tz};
        int written = std::snprintf(buf, buflen,
                                    "from=(%.1f,%.1f,%.1f) to=(%.1f,%.1f,%.1f)\n",
                                    fx, fy, fz, tx, ty, tz);

        if (!g_fnGetSmokeDensityInLine)
        {
            written += std::snprintf(buf + written, buflen - written,
                                     "GetSmokeDensityInLine unresolved -> mode 0 is ball-smoke\n");
            return written;
        }

        float dens = g_fnGetSmokeDensityInLine(from, to, nullptr);
        float thr = g_densityThrMilli.load(std::memory_order_relaxed) * 0.001f;
        written += std::snprintf(buf + written, buflen - written,
                                 "density=%.4f  threshold=%.4f  blocked=%d\n",
                                 dens, thr, dens >= thr ? 1 : 0);
        return written;
    }

} // namespace cs2bv::hooks
