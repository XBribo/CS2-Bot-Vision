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
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <eiface.h> // IVEngineServer2::GetServerGlobals
#include <edict.h>  // CGlobalVars (curtime)

/* HE detonate hook */
struct HeBlast
{
    float x, y, z;   // detonation center
    float startTime; // curtime when recorded
};

static IVEngineServer2 *g_pEngine = nullptr;
static std::mutex g_blastMutex;
static std::vector<HeBlast> g_blasts;
static std::atomic<int> g_heRadiusMilli{200000};         // bv_he_radius * 1000 (default 200)
static std::atomic<int> g_heDurationMilli{3500};         // bv_he_duration * 1000 (default 3.5s)
static std::string g_heListenerStatus = "not_attempted"; // hegrenade_detonate registration result

using IsVisibleThroughSmoke_t = bool(__fastcall *)(void *self, const void *from, const void *to);

using GetSmokeDensityInLine_t = float(__fastcall *)(const float *from, const float *to, float *outClosest);

// CHEGrenadeProjectile::Detonate
using HeDetonate_t = __int64(__fastcall *)(void *self);
static HeDetonate_t g_origHeDetonate = nullptr;
static const char *kHeDetonateName = "CHEGrenadeProjectile::Detonate";

// CBaseEntity origin layout
static const int kSceneNodeOffset = 624;
static const int kAbsOriginOffset = 200;

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
static const char *kDensityFnName = "GetSmokeDensityInLine";

// Current curtime
static float NowTime()
{
    if (!g_pEngine)
        return 0.0f;
    CGlobalVars *gv = g_pEngine->GetServerGlobals();
    return gv ? gv->curtime : 0.0f;
}

// Shortest squared distance from point p to segment a->b
static float DistSqPointSeg(const float p[3], const float a[3], const float b[3])
{
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = len2 > 0.0f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0.0f;
    if (t < 0.0f)
        t = 0.0f;
    else if (t > 1.0f)
        t = 1.0f;
    float c[3] = {a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t};
    float d[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

/* True if the LOS segment passes through any active HE hole.
   Hole radius shrinks linearly with age: r(age) = baseR * (1 - age/dur) */
static bool SegmentClearedByHeHole(const float *from, const float *to)
{
    float dur = g_heDurationMilli.load(std::memory_order_relaxed) * 0.001f;
    float baseR = g_heRadiusMilli.load(std::memory_order_relaxed) * 0.001f;
    if (dur <= 0.0f || baseR <= 0.0f)
        return false;
    float now = NowTime();

    std::lock_guard<std::mutex> lk(g_blastMutex);
    bool cleared = false;
    size_t w = 0;
    for (size_t i = 0; i < g_blasts.size(); ++i)
    {
        float age = now - g_blasts[i].startTime;
        if (age < 0.0f || age >= dur)
            continue;
        g_blasts[w++] = g_blasts[i];
        float r = baseR * (1.0f - age / dur);
        float center[3] = {g_blasts[i].x, g_blasts[i].y, g_blasts[i].z};
        if (DistSqPointSeg(center, from, to) <= r * r)
            cleared = true;
    }
    g_blasts.resize(w);
    return cleared;
}

// CHEGrenadeProjectile::Detonate
static __int64 __fastcall HookedHeDetonate(void *self)
{
    if (self)
    {
        auto entity = reinterpret_cast<uintptr_t>(self);
        uintptr_t node = *reinterpret_cast<uintptr_t *>(entity + kSceneNodeOffset);
        if (node)
        {
            const float *origin = reinterpret_cast<const float *>(node + kAbsOriginOffset);
            cs2bv::hooks::OnHeDetonate(origin[0], origin[1], origin[2]);
        }
    }
    return g_origHeDetonate(self);
}

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
        if (SegmentClearedByHeHole(fa, fb))
            return true;
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

        // Resolve + hook CHEGrenadeProjectile::Detonate
        {
            char heErr[256] = {0};
            void *heTarget = cs2bv::sig::ResolveSig(gamedata, server, kHeDetonateName, heErr, sizeof(heErr));
            if (heTarget &&
                MH_CreateHook(heTarget, reinterpret_cast<void *>(&HookedHeDetonate),
                              reinterpret_cast<void **>(&g_origHeDetonate)) == MH_OK &&
                MH_EnableHook(heTarget) == MH_OK)
            {
                char hb[160];
                std::snprintf(hb, sizeof(hb),
                              "[BotVision] %s @ %p (HE holes active)\n", kHeDetonateName, heTarget);
                OutputDebugStringA(hb);
                g_heListenerStatus = "hook=ok";
            }
            else
            {
                char hb[320];
                std::snprintf(hb, sizeof(hb),
                              "[BotVision] HE detonate hook failed (%s); HE holes disabled\n",
                              heTarget ? "MinHook error" : heErr);
                OutputDebugStringA(hb);
                g_heListenerStatus = heTarget ? "hook=FAIL" : "sig=FAIL";
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

    void SetEngine(void *engine) { g_pEngine = static_cast<IVEngineServer2 *>(engine); }

    // Record an HE detonation as a new active hole
    void OnHeDetonate(float x, float y, float z)
    {
        float t = NowTime();
        {
            std::lock_guard<std::mutex> lk(g_blastMutex);
            g_blasts.push_back({x, y, z, t});
        }
        char dbg[160];
        std::snprintf(dbg, sizeof(dbg),
                      "[BotVision] HE detonate @ (%.1f,%.1f,%.1f) t=%.2f total=%d\n",
                      x, y, z, t, GetActiveBlastCount());
        OutputDebugStringA(dbg);
    }

    void SetHeRadius(float v) { g_heRadiusMilli.store((int)(v * 1000), std::memory_order_relaxed); }
    float GetHeRadius() { return g_heRadiusMilli.load(std::memory_order_relaxed) * 0.001f; }
    void SetHeDuration(float v) { g_heDurationMilli.store((int)(v * 1000), std::memory_order_relaxed); }
    float GetHeDuration() { return g_heDurationMilli.load(std::memory_order_relaxed) * 0.001f; }

    int GetActiveBlastCount()
    {
        std::lock_guard<std::mutex> lk(g_blastMutex);
        return (int)g_blasts.size();
    }

    void SetHeListenerStatus(bool managerResolved, bool listenerAdded)
    {
        g_heListenerStatus = managerResolved
                                 ? (listenerAdded ? "mgr=ok,add=ok" : "mgr=ok,add=FAIL")
                                 : "mgr=NULL";
    }
    const char *GetHeListenerStatus() { return g_heListenerStatus.c_str(); }

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
        bool engineBlock = dens >= thr;
        bool heCleared = SegmentClearedByHeHole(from, to);
        bool finalBlock = engineBlock && !heCleared;
        written += std::snprintf(buf + written, buflen - written,
                                 "density=%.4f  threshold=%.4f  engineBlock=%d  heCleared=%d  blocked=%d  activeHoles=%d\n",
                                 dens, thr, engineBlock ? 1 : 0, heCleared ? 1 : 0,
                                 finalBlock ? 1 : 0, GetActiveBlastCount());
        return written;
    }

} // namespace cs2bv::hooks
