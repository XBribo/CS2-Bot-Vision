#include "hooks.h"
#include "sig_scan.h"
#include "raytrace_iface.h"
#include "cdetour.h"

#include <Windows.h>
#include <funchook.h>

#include <atomic>
#include <climits>
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
#include <edict.h>  // CGlobalVars

/* HE detonate hook */
struct HeBlast
{
    float x, y, z; // detonation center
    float startTime;
};

/* Per-pellet bullet hole*/
struct BulletHole
{
    float start[3];
    float end[3];
    float startTime;
    float radius; // per-hole radius
};

static const size_t kMaxBulletHoles = 64; // ring cap; LOS query bounded by this

static IVEngineServer2 *g_pEngine = nullptr;
static cs2bv::rt::CRayTraceInterface *g_pRayTrace = nullptr; // bullet wall-clip
static int g_rtRet = -999;
static std::mutex g_blastMutex;
static std::vector<HeBlast> g_blasts;
static std::mutex g_bulletHoleMutex;
static std::vector<BulletHole> g_bulletHoles;
static std::atomic<int> g_heRadiusMilli{75000};            // bv_he_radius * 1000 (default 75)
static std::atomic<int> g_heDurationMilli{3200};           // bv_he_duration * 1000 (default 3.2s)
static std::atomic<int> g_bulletRadiusMilli{12000};        // bv_bullet_radius * 1000 (default 12)
static std::atomic<int> g_bulletRadiusShotgunMilli{28000}; // bv_bullet_radius_shotgun * 1000 (default 28)
static std::atomic<int> g_bulletDurationMilli{150};        // bv_bullet_duration * 1000 (default 0.15s)
static std::atomic<int> g_bulletRangeMilli{8192000};       // bv_bullet_range * 1000 (default 8192)
static std::atomic<int> g_bulletHolesEnabled{1};           // bv_bullet_holes (default on)
static std::string g_heListenerStatus = "not_attempted";   // hegrenade_detonate registration result

using IsVisibleThroughSmoke_t = bool(__fastcall *)(void *self, const void *from, const void *to);

using GetSmokeDensityInLine_t = float(__fastcall *)(const float *from, const float *to, float *outClosest);

// CHEGrenadeProjectile::Detonate
using HeDetonate_t = __int64(__fastcall *)(void *self);
static HeDetonate_t g_origHeDetonate = nullptr;
static const char *kHeDetonateName = "CHEGrenadeProjectile::Detonate";

// CBaseEntity -> CBodyComponent* -> CGameSceneNode* (offsets loaded from gamedata)
static int kBodyComponentOffset = 0x30;
static int kSceneNodeInBodyComponentOffset = 0x8;
static int kAbsOriginOffset = 200;

// Per-pellet bullet trace
using PelletTrace_t = __int64(__fastcall *)(
    __int64 a1, void *a2, __int64 a3, float a4, float a5, int a6, unsigned char a7,
    int a8, int a9, float a10, __int64 a11, int *a12, float a13, float a14,
    __int64 a15, __int64 a16, int a17, int a18, void *a19, __int64 a20);
static PelletTrace_t g_origPelletTrace = nullptr;
static const char *kPelletTraceName = "BulletPelletTrace";

// CCSPlayer_WeaponServices::GetSlot - resolve active weapon for shotgun check
using GetSlot_t = void *(__fastcall *)(void *ws, int slot, unsigned int pos);
static GetSlot_t g_pGetSlot = nullptr;
static const char *kGetSlotName = "CCSPlayer_WeaponServices::GetSlot";

// CCSBot::IsVisible(pos) - wraps the smoke check; a1 = querying bot pawn
using IsVisiblePos_t = __int64(__fastcall *)(__int64 self, __int64 pos, char testFOV, void *ent);
static IsVisiblePos_t g_origIsVisiblePos = nullptr;
static const char *kIsVisiblePosName = "CCSBot::IsVisiblePos";

// pawn -> m_hController; controller entindex-1 == engine slot (status id)
static int kOffControllerHandleInPawn = 0xBB0;
// CBot base object holds its player pawn pointer
static int kOffPlayerInBot = 0x18;
static const int kMaxBots = 64;

// pawn -> WeaponServices -> m_hActiveWeapon; weapon -> def index
static int kOffWeaponServicesInPawn = 0xA30;
static int kOffActiveWeaponInWs = 0x60;
static int kOffItemDefIndexInWeapon = 0xA00;
static int kOffEntityIdentity = 0x10;
static int kOffEHandleInIdentity = 0x10;

// items_game def indices for shotguns
static bool IsShotgunDef(int def)
{
    return def == 25 /*xm1014*/ || def == 27 /*mag7*/ ||
           def == 29 /*sawedoff*/ || def == 35 /*nova*/;
}

// Probe state: last active-weapon def seen by the pellet hook
static std::atomic<int> g_lastWeaponDef{-1};
static std::atomic<int> g_lastWeaponShotgun{0};

// Verification state
static std::atomic<long long> g_bulletCount{0};
static std::mutex g_bulletMutex;
static float g_lastBulletSrc[3] = {0, 0, 0};
static float g_lastBulletAng[3] = {0, 0, 0};
static float g_lastBulletFwd[3] = {0, 0, 0};

static std::atomic<long long> g_traceAttempts{0}; // times we entered the trace branch
static std::atomic<long long> g_traceHits{0};     // times TraceEndShape returned true

static IsVisibleThroughSmoke_t g_origIsVisibleThroughSmoke = nullptr;
static GetSmokeDensityInLine_t g_fnGetSmokeDensityInLine = nullptr;

// funchook detours (orig trampolines written into the g_orig* pointers above)
static cs2bv::hooks::CDetour<IsVisibleThroughSmoke_t> g_dIsVisibleThroughSmoke{"CBotManager::IsVisibleThroughSmoke"};
static cs2bv::hooks::CDetour<HeDetonate_t> g_dHeDetonate{kHeDetonateName};
static cs2bv::hooks::CDetour<PelletTrace_t> g_dPelletTrace{kPelletTraceName};
static cs2bv::hooks::CDetour<IsVisiblePos_t> g_dIsVisiblePos{kIsVisiblePosName};

static std::atomic<long long> g_hitCount{0};
static std::atomic<long long> g_blockedCount{0};
static void **g_pAutoListHead = nullptr;
static std::string g_hookedStatus = "not_attempted"; // bv_status
static std::atomic<int> g_smokeMode{0};
static std::atomic<int> g_densityThrMilli{200}; // bv_density_threshold * 1000 (default 0.2 → 200)

// Per-bot density threshold (indexed by engine slot 0..63). INT_MIN = use global default.
// Stamped by the IsVisiblePos hook, read by the smoke hook on the same thread.
static const int kSlotDefault = INT_MIN;
static std::atomic<int> g_botThrMilli[kMaxBots];
static thread_local int g_curBotSlot = -1;               // slot of the bot currently querying (-1 = none/non-bot)
static std::atomic<int> g_lastBotSlot{-1};               // last slot seen by IsVisiblePos
static std::atomic<long long> g_isVisiblePosCalls{0};    // times IsVisiblePos hook fired
static std::atomic<unsigned int> g_lastCtrlHandle{0};    // last raw controller handle read
static std::atomic<unsigned long long> g_lastPawnPtr{0}; // last pawn ptr

static std::atomic<long long> g_safeReadSceneFailures{0};
static std::atomic<long long> g_safeReadBotFailures{0};
static std::atomic<long long> g_safeReadWeaponFailures{0};
static std::atomic<long long> g_safeReadBulletFailures{0};
static std::atomic<long long> g_safeReadSmokeFailures{0};
static std::atomic<long long> g_safeReadInstallFailures{0};

static const char *kFuncName = "CBotManager::IsVisibleThroughSmoke";
static const char *kHeadName = "g_AutoList_SmokeProj_Head_Server";
static const char *kDensityFnName = "GetSmokeDensityInLine";

// Checks that every page in a memory range is committed and readable
static bool IsReadableMemory(const void *address, size_t size)
{
    if (!address || size == 0)
        return false;

    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    if (size > UINTPTR_MAX - cursor)
        return false;
    const uintptr_t end = cursor + size;

    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION memoryInfo{};
        if (VirtualQuery(reinterpret_cast<const void *>(cursor), &memoryInfo,
                         sizeof(memoryInfo)) == 0)
            return false;
        if (memoryInfo.State != MEM_COMMIT ||
            (memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;

        const DWORD protection = memoryInfo.Protect & 0xFF;
        const bool readable = protection == PAGE_READONLY ||
                              protection == PAGE_READWRITE ||
                              protection == PAGE_WRITECOPY ||
                              protection == PAGE_EXECUTE_READ ||
                              protection == PAGE_EXECUTE_READWRITE ||
                              protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable)
            return false;

        const uintptr_t regionStart = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
        if (memoryInfo.RegionSize > UINTPTR_MAX - regionStart)
            return false;
        const uintptr_t regionEnd = regionStart + memoryInfo.RegionSize;
        if (regionEnd <= cursor)
            return false;
        cursor = regionEnd;
    }
    return true;
}

// Reads a value only after validating the source range and records failures
template <typename T>
static bool SafeRead(const void *base, size_t offset, T &out,
                     std::atomic<long long> &failureCounter)
{
    if (!base)
    {
        failureCounter.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uintptr_t baseAddress = reinterpret_cast<uintptr_t>(base);
    if (offset > UINTPTR_MAX - baseAddress)
    {
        failureCounter.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const void *address = reinterpret_cast<const void *>(baseAddress + offset);
    if (!IsReadableMemory(address, sizeof(T)))
    {
        failureCounter.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::memcpy(&out, address, sizeof(T));
    return true;
}

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

/* True if the LOS segment passes through any active HE hole
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

// Shortest squared distance between segment p1->q1 and segment p2->q2
static float DistSqSegSeg(const float p1[3], const float q1[3],
                          const float p2[3], const float q2[3])
{
    float d1[3] = {q1[0] - p1[0], q1[1] - p1[1], q1[2] - p1[2]};
    float d2[3] = {q2[0] - p2[0], q2[1] - p2[1], q2[2] - p2[2]};
    float r[3] = {p1[0] - p2[0], p1[1] - p2[1], p1[2] - p2[2]};
    float a = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2];
    float e = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2];
    float f = d2[0] * r[0] + d2[1] * r[1] + d2[2] * r[2];
    float s, t;
    const float kEps = 1e-8f;
    if (a <= kEps && e <= kEps)
    {
        s = t = 0.0f;
    }
    else if (a <= kEps)
    {
        s = 0.0f;
        t = f / e;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    else
    {
        float c = d1[0] * r[0] + d1[1] * r[1] + d1[2] * r[2];
        if (e <= kEps)
        {
            t = 0.0f;
            s = -c / a;
            s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
        }
        else
        {
            float b = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2];
            float denom = a * e - b * b;
            s = denom > kEps ? (b * f - c * e) / denom : 0.0f;
            s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
            t = (b * s + f) / e;
            if (t < 0.0f)
            {
                t = 0.0f;
                s = -c / a;
                s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
            }
            else if (t > 1.0f)
            {
                t = 1.0f;
                s = (b - c) / a;
                s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
            }
        }
    }
    float c1[3] = {p1[0] + d1[0] * s, p1[1] + d1[1] * s, p1[2] + d1[2] * s};
    float c2[3] = {p2[0] + d2[0] * t, p2[1] + d2[1] * t, p2[2] + d2[2] * t};
    float dv[3] = {c1[0] - c2[0], c1[1] - c2[1], c1[2] - c2[2]};
    return dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2];
}

// True if the bot eye lies inside any active bullet tunnel
static bool SegmentClearedByBulletHole(const float *from, const float * /*to*/)
{
    float dur = g_bulletDurationMilli.load(std::memory_order_relaxed) * 0.001f;
    if (dur <= 0.0f)
        return false;
    float now = NowTime();

    std::lock_guard<std::mutex> lk(g_bulletHoleMutex);
    bool cleared = false;
    size_t w = 0;
    for (size_t i = 0; i < g_bulletHoles.size(); ++i)
    {
        float age = now - g_bulletHoles[i].startTime;
        if (age < 0.0f || age >= dur)
            continue;
        g_bulletHoles[w++] = g_bulletHoles[i];
        float r = g_bulletHoles[i].radius; // per-hole radius (shotgun = wider)
        if (r > 0.0f &&
            DistSqPointSeg(from, g_bulletHoles[i].start, g_bulletHoles[i].end) <= r * r)
            cleared = true;
    }
    g_bulletHoles.resize(w);
    return cleared;
}

// Resolve shooter pawn's active-weapon def index via GetSlot
static int ActiveWeaponDef(void *pawn)
{
    if (!pawn || !g_pGetSlot)
        return -1;
    void *ws = nullptr;
    if (!SafeRead(pawn, kOffWeaponServicesInPawn, ws, g_safeReadWeaponFailures) || !ws)
        return -1;
    uint32_t activeH = 0;
    if (!SafeRead(ws, kOffActiveWeaponInWs, activeH, g_safeReadWeaponFailures))
        return -1;
    if (activeH == 0u || activeH == 0xFFFFFFFFu)
        return -1;
    int activeIdx = static_cast<int>(activeH & 0x7FFFu);
    for (int slot = 0; slot <= 4; ++slot)
    {
        unsigned int maxPos = (slot == 3) ? 8u : 1u; // grenades slot holds many
        for (unsigned int pos = 0; pos < maxPos; ++pos)
        {
            unsigned int posArg = (slot == 3) ? pos : 0xFFFFFFFFu;
            void *w = g_pGetSlot(ws, slot, posArg);
            if (!w)
                continue;
            void *identity = nullptr;
            if (!SafeRead(w, kOffEntityIdentity, identity, g_safeReadWeaponFailures) || !identity)
                continue;
            uint32_t h = 0;
            if (!SafeRead(identity, kOffEHandleInIdentity, h, g_safeReadWeaponFailures))
                continue;
            if ((static_cast<int>(h & 0x7FFFu)) != activeIdx)
                continue;
            uint16_t itemDefinitionIndex = 0;
            if (!SafeRead(w, kOffItemDefIndexInWeapon, itemDefinitionIndex,
                          g_safeReadWeaponFailures))
                return -1;
            return itemDefinitionIndex;
        }
    }
    return -1;
}

// Per-pellet trace
static __int64 __fastcall HookedPelletTrace(
    __int64 a1, void *a2, __int64 a3, float a4, float a5, int a6, unsigned char a7,
    int a8, int a9, float a10, __int64 a11, int *a12, float a13, float a14,
    __int64 a15, __int64 a16, int a17, int a18, void *a19, __int64 a20)
{
    g_bulletCount.fetch_add(1, std::memory_order_relaxed);
    if (a2 && a3)
    {
        float srcValues[3]{};
        float angValues[3]{};
        if (!SafeRead(a2, 0, srcValues, g_safeReadBulletFailures) ||
            !SafeRead(reinterpret_cast<const void *>(a3), 0, angValues,
                      g_safeReadBulletFailures))
            return g_origPelletTrace(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                                     a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);

        const float *src = srcValues;
        const float *ang = angValues; // QAngle{pitch,yaw,roll}
        // AngleVectors(ang) → fwd/right/up
        float pitch = ang[0] * 0.01745329252f, yaw = ang[1] * 0.01745329252f, roll = ang[2] * 0.01745329252f;
        float sp = std::sin(pitch), cp = std::cos(pitch);
        float sy = std::sin(yaw), cy = std::cos(yaw);
        float sr = std::sin(roll), cr = std::cos(roll);
        float fwd[3] = {cp * cy, cp * sy, -sp};
        float right[3] = {-1.f * sr * sp * cy + -1.f * cr * -sy,
                          -1.f * sr * sp * sy + -1.f * cr * cy,
                          -1.f * sr * cp};
        float up[3] = {cr * sp * cy + -sr * -sy, cr * sp * sy + -sr * cy, cr * cp};
        // pellet direction: dir = fwd - right*a13 + up*a14
        float dir[3] = {fwd[0] - right[0] * a13 + up[0] * a14,
                        fwd[1] - right[1] * a13 + up[1] * a14,
                        fwd[2] - right[2] * a13 + up[2] * a14};

        {
            std::lock_guard<std::mutex> lk(g_bulletMutex);
            for (int i = 0; i < 3; ++i)
            {
                g_lastBulletSrc[i] = src[i];
                g_lastBulletAng[i] = ang[i];
                g_lastBulletFwd[i] = dir[i];
            }
        }

        // Register a capsule hole
        void *smokeHead = nullptr;
        bool smokePresent = g_pAutoListHead &&
                            SafeRead(g_pAutoListHead, 0, smokeHead, g_safeReadBulletFailures) &&
                            smokeHead;
        if (g_bulletHolesEnabled.load(std::memory_order_relaxed) && g_pRayTrace && smokePresent)
        {
            g_traceAttempts.fetch_add(1, std::memory_order_relaxed);
            float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (len > 1e-4f)
            {
                // shooter pawn; also used to pick per-weapon radius
                void *shooter = nullptr;
                if (a1 && !SafeRead(reinterpret_cast<const void *>(a1), 56, shooter,
                                    g_safeReadBulletFailures))
                    shooter = nullptr;
                // resolve active weapon -> shotgun gets a wider hole
                int def = ActiveWeaponDef(shooter);
                bool isShotgun = IsShotgunDef(def);
                g_lastWeaponDef.store(def, std::memory_order_relaxed);
                g_lastWeaponShotgun.store(isShotgun ? 1 : 0, std::memory_order_relaxed);
                float radius = (isShotgun ? g_bulletRadiusShotgunMilli : g_bulletRadiusMilli)
                                   .load(std::memory_order_relaxed) *
                               0.001f;

                float range = g_bulletRangeMilli.load(std::memory_order_relaxed) * 0.001f;
                float inv = range / len;
                cs2bv::rt::Vector start{src[0], src[1], src[2]};
                cs2bv::rt::Vector end{src[0] + dir[0] * inv, src[1] + dir[1] * inv, src[2] + dir[2] * inv};
                cs2bv::rt::TraceOptions opts; // default InteractsWith = MASK_SHOT_PHYSICS
                cs2bv::rt::TraceResult res;
                if (g_pRayTrace->TraceEndShape(&start, &end, shooter, &opts, &res))
                {
                    g_traceHits.fetch_add(1, std::memory_order_relaxed);
                    float endp[3] = {res.EndPos.x, res.EndPos.y, res.EndPos.z};
                    cs2bv::hooks::OnBulletHole(src, endp, radius);
                }
                else
                {
                    float endp[3] = {end.x, end.y, end.z}; // no hit → full range
                    cs2bv::hooks::OnBulletHole(src, endp, radius);
                }
            }
        }
    }
    return g_origPelletTrace(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                             a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
}

// CHEGrenadeProjectile::Detonate
static __int64 __fastcall HookedHeDetonate(void *self)
{
    if (self)
    {
        auto entity = reinterpret_cast<uintptr_t>(self);
        uintptr_t bodyComponent = 0;
        if (!SafeRead(reinterpret_cast<const void *>(entity), kBodyComponentOffset,
                      bodyComponent, g_safeReadSceneFailures) ||
            !bodyComponent)
            return g_origHeDetonate(self);

        uintptr_t node = 0;
        if (!SafeRead(reinterpret_cast<const void *>(bodyComponent),
                      kSceneNodeInBodyComponentOffset, node, g_safeReadSceneFailures))
            return g_origHeDetonate(self);
        if (node)
        {
            float origin[3]{};
            if (!SafeRead(reinterpret_cast<const void *>(node), kAbsOriginOffset,
                          origin, g_safeReadSceneFailures))
                return g_origHeDetonate(self);
            cs2bv::hooks::OnHeDetonate(origin[0], origin[1], origin[2]);
        }
    }
    return g_origHeDetonate(self);
}

// Resolve the querying bot's engine slot from the CCSBot AI object
static int BotSlotFromBot(__int64 bot)
{
    if (!bot)
        return -1;
    __int64 pawn = 0;
    if (!SafeRead(reinterpret_cast<const void *>(bot), kOffPlayerInBot, pawn,
                  g_safeReadBotFailures))
        return -1;
    g_lastPawnPtr.store(static_cast<unsigned long long>(pawn), std::memory_order_relaxed);
    if (!pawn)
        return -1;
    uint32_t h = 0;
    if (!SafeRead(reinterpret_cast<const void *>(pawn), kOffControllerHandleInPawn, h,
                  g_safeReadBotFailures))
        return -1;
    g_lastCtrlHandle.store(h, std::memory_order_relaxed); // probe: raw handle
    if (h == 0u || h == 0xFFFFFFFFu)
        return -1;
    int ctrlIdx = static_cast<int>(h & 0x7FFFu);
    int slot = ctrlIdx - 1; // controller entindex is slot+1
    if (slot < 0 || slot >= kMaxBots)
        return -1;
    return slot;
}

// CCSBot::IsVisible(pos)
static __int64 __fastcall HookedIsVisiblePos(__int64 self, __int64 pos, char testFOV, void *ent)
{
    g_isVisiblePosCalls.fetch_add(1, std::memory_order_relaxed); // probe: hook fired
    int prev = g_curBotSlot;
    int slot = BotSlotFromBot(self);
    g_curBotSlot = slot;
    if (slot >= 0)
        g_lastBotSlot.store(slot, std::memory_order_relaxed);
    __int64 ret = g_origIsVisiblePos(self, pos, testFOV, ent);
    g_curBotSlot = prev;
    return ret;
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
    float fromValues[3]{};
    float toValues[3]{};
    if (!SafeRead(from, 0, fromValues, g_safeReadSmokeFailures) ||
        !SafeRead(to, 0, toValues, g_safeReadSmokeFailures))
        return g_origIsVisibleThroughSmoke(self, from, to);
    const float *fa = fromValues;
    const float *fb = toValues;
    float dens = g_fnGetSmokeDensityInLine(fa, fb, nullptr);
    // Per-bot threshold
    int thrMilli = g_densityThrMilli.load(std::memory_order_relaxed);
    int slot = g_curBotSlot;
    if (slot >= 0 && slot < kMaxBots)
    {
        int botThr = g_botThrMilli[slot].load(std::memory_order_relaxed);
        if (botThr != kSlotDefault)
            thrMilli = botThr;
    }
    float thr = thrMilli * 0.001f;
    if (dens >= thr)
    {
        if (SegmentClearedByHeHole(fa, fb))
            return true;
        if (SegmentClearedByBulletHole(fa, fb))
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
        int32_t disp = 0;
        if (!SafeRead(sigStart, static_cast<size_t>(relOffset), disp,
                      g_safeReadInstallFailures))
            return nullptr;
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

    // Resolve a function whose first rel32 jump bytes were replaced by another detour
    static void *ResolveSigWithRel32DetourFallback(const nlohmann::json &gamedata,
                                                   const cs2bv::sig::ModuleInfo &module,
                                                   const char *name, bool &usedFallback,
                                                   char *errorOut, size_t errorOutLen)
    {
        usedFallback = false;

        char primaryError[256] = {0};
        void *target = cs2bv::sig::ResolveSig(
            gamedata, module, name, primaryError, sizeof(primaryError));
        if (target)
            return target;

        std::string sigStr = cs2bv::sig::FindPlatformSig(gamedata, name);
        std::vector<uint8_t> pattern;
        std::vector<bool> wild;
        constexpr size_t kRel32JumpSize = 5;
        if (sigStr.empty() || !cs2bv::sig::ParseSigString(sigStr, pattern, wild) ||
            pattern.size() <= kRel32JumpSize)
        {
            if (errorOut && errorOutLen > 0)
                std::snprintf(errorOut, errorOutLen, "%s", primaryError);
            return nullptr;
        }

        void *resolved = nullptr;
        size_t matchCount = 0;
        const size_t tailSize = pattern.size() - kRel32JumpSize;
        for (const cs2bv::sig::ModuleSegment &segment : module.Segments)
        {
            if (!segment.Base || segment.Size < pattern.size())
                continue;

            for (size_t i = kRel32JumpSize; i + tailSize <= segment.Size; ++i)
            {
                bool matches = true;
                for (size_t j = 0; j < tailSize; ++j)
                {
                    const size_t patternIndex = kRel32JumpSize + j;
                    if (!wild[patternIndex] &&
                        segment.Base[i + j] != pattern[patternIndex])
                    {
                        matches = false;
                        break;
                    }
                }
                if (!matches)
                    continue;

                unsigned char *candidate = segment.Base + i - kRel32JumpSize;
                if (candidate[0] != 0xE9)
                    continue;

                int32_t displacement = 0;
                std::memcpy(&displacement, candidate + 1, sizeof(displacement));
                void *detourTarget = candidate + kRel32JumpSize + displacement;
                if (!IsReadableMemory(detourTarget, 1))
                    continue;

                resolved = candidate;
                ++matchCount;
            }
        }

        if (matchCount == 1)
        {
            usedFallback = true;
            return resolved;
        }

        if (errorOut && errorOutLen > 0)
        {
            std::snprintf(errorOut, errorOutLen,
                          "%s; rel32 detour tail matches=%zu", primaryError, matchCount);
        }
        return nullptr;
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
        // Per-bot thresholds start unset
        for (int i = 0; i < kMaxBots; ++i)
            g_botThrMilli[i].store(kSlotDefault, std::memory_order_relaxed);

        nlohmann::json gamedata;
        if (!cs2bv::sig::LoadGamedata(gamedataPath.c_str(), gamedata))
        {
            ReportError(error, maxlen, "failed to read/parse gamedata.json at %s", gamedataPath.c_str());
            return false;
        }

        // Load class-member offsets (fall back to built-in defaults if absent)
        using cs2bv::sig::ResolveOffset;
        kBodyComponentOffset = ResolveOffset(gamedata, "CBaseEntity::m_CBodyComponent", kBodyComponentOffset);
        kSceneNodeInBodyComponentOffset = ResolveOffset(
            gamedata, "CBodyComponent::m_pSceneNode", kSceneNodeInBodyComponentOffset);
        kAbsOriginOffset = ResolveOffset(gamedata, "CGameSceneNode::m_vecAbsOrigin", kAbsOriginOffset);
        kOffControllerHandleInPawn = ResolveOffset(gamedata, "CBasePlayerPawn::m_hController", kOffControllerHandleInPawn);
        kOffPlayerInBot = ResolveOffset(gamedata, "Bot::m_pPlayer", kOffPlayerInBot);
        kOffWeaponServicesInPawn = ResolveOffset(gamedata, "CBasePlayerPawn::m_pWeaponServices", kOffWeaponServicesInPawn);
        kOffActiveWeaponInWs = ResolveOffset(gamedata, "CPlayer_WeaponServices::m_hActiveWeapon", kOffActiveWeaponInWs);
        kOffItemDefIndexInWeapon = ResolveOffset(
            gamedata, "CBasePlayerWeapon::m_iItemDefinitionIndex", kOffItemDefIndexInWeapon);
        kOffEntityIdentity = ResolveOffset(gamedata, "CEntityInstance::m_pEntity", kOffEntityIdentity);
        kOffEHandleInIdentity = kOffEntityIdentity; // same 0x10 slot

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

        if (!g_dIsVisibleThroughSmoke.Create(target, reinterpret_cast<void *>(&HookedIsVisibleThroughSmoke),
                                             reinterpret_cast<void **>(&g_origIsVisibleThroughSmoke)))
        {
            ReportError(error, maxlen, "funchook_prepare failed for %s", kFuncName);
            return false;
        }
        if (!g_dIsVisibleThroughSmoke.Enable())
        {
            ReportError(error, maxlen, "funchook_install failed for %s", kFuncName);
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
                g_dHeDetonate.Create(heTarget, reinterpret_cast<void *>(&HookedHeDetonate),
                                     reinterpret_cast<void **>(&g_origHeDetonate)) &&
                g_dHeDetonate.Enable())
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
                              heTarget ? "funchook error" : heErr);
                OutputDebugStringA(hb);
                g_heListenerStatus = heTarget ? "hook=FAIL" : "sig=FAIL";
            }
        }

        // Resolve + hook per-pellet bullet trace (records bullet paths; shotgun = N pellets)
        {
            char fbErr[256] = {0};
            void *fbTarget = cs2bv::sig::ResolveSig(gamedata, server, kPelletTraceName, fbErr, sizeof(fbErr));
            if (fbTarget &&
                g_dPelletTrace.Create(fbTarget, reinterpret_cast<void *>(&HookedPelletTrace),
                                      reinterpret_cast<void **>(&g_origPelletTrace)) &&
                g_dPelletTrace.Enable())
            {
                char fb[160];
                std::snprintf(fb, sizeof(fb),
                              "[BotVision] %s @ %p (bullet capture active)\n", kPelletTraceName, fbTarget);
                OutputDebugStringA(fb);
            }
            else
            {
                char fb[320];
                std::snprintf(fb, sizeof(fb),
                              "[BotVision] pellet-trace hook failed (%s); bullet holes disabled\n",
                              fbTarget ? "funchook error" : fbErr);
                OutputDebugStringA(fb);
            }
        }

        // Resolve GetSlot
        {
            char gsErr[256] = {0};
            void *gsTarget = cs2bv::sig::ResolveSig(gamedata, server, kGetSlotName, gsErr, sizeof(gsErr));
            if (gsTarget)
            {
                g_pGetSlot = reinterpret_cast<GetSlot_t>(gsTarget);
                char gs[160];
                std::snprintf(gs, sizeof(gs),
                              "[BotVision] %s @ %p (shotgun radius active)\n", kGetSlotName, gsTarget);
                OutputDebugStringA(gs);
            }
            else
            {
                char gs[320];
                std::snprintf(gs, sizeof(gs),
                              "[BotVision] %s; shotgun radius disabled (all bullets use normal radius)\n", gsErr);
                OutputDebugStringA(gs);
            }
        }

        // Resolve + hook CCSBot::IsVisible(pos)
        {
            char ivErr[256] = {0};
            bool chainedDetour = false;
            void *ivTarget = ResolveSigWithRel32DetourFallback(
                gamedata, server, kIsVisiblePosName, chainedDetour, ivErr, sizeof(ivErr));
            if (ivTarget &&
                g_dIsVisiblePos.Create(ivTarget, reinterpret_cast<void *>(&HookedIsVisiblePos),
                                       reinterpret_cast<void **>(&g_origIsVisiblePos)) &&
                g_dIsVisiblePos.Enable())
            {
                char iv[160];
                std::snprintf(iv, sizeof(iv),
                              "[BotVision] %s @ %p (per-bot density active%s)\n",
                              kIsVisiblePosName, ivTarget,
                              chainedDetour ? ", chained existing detour" : "");
                OutputDebugStringA(iv);
            }
            else
            {
                char iv[320];
                std::snprintf(iv, sizeof(iv),
                              "[BotVision] IsVisiblePos hook failed (%s); per-bot density disabled\n",
                              ivTarget ? "funchook error" : ivErr);
                OutputDebugStringA(iv);
            }
        }

        OutputDebugStringA("[BotVision] detour installed\n");
        return true;
    }

    void Remove()
    {
        g_dIsVisibleThroughSmoke.Free();
        g_dHeDetonate.Free();
        g_dPelletTrace.Free();
        g_dIsVisiblePos.Free();
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

    // Formats safe-read failure counters by runtime subsystem
    const char *GetSafeReadDiag()
    {
        static char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "scene=%lld bot=%lld weapon=%lld bullet=%lld smoke=%lld install=%lld",
                      static_cast<long long>(g_safeReadSceneFailures.load(std::memory_order_relaxed)),
                      static_cast<long long>(g_safeReadBotFailures.load(std::memory_order_relaxed)),
                      static_cast<long long>(g_safeReadWeaponFailures.load(std::memory_order_relaxed)),
                      static_cast<long long>(g_safeReadBulletFailures.load(std::memory_order_relaxed)),
                      static_cast<long long>(g_safeReadSmokeFailures.load(std::memory_order_relaxed)),
                      static_cast<long long>(g_safeReadInstallFailures.load(std::memory_order_relaxed)));
        return buf;
    }

    // Per-bot density threshold
    void SetBotDensityThreshold(int slot, float v)
    {
        if (slot < 0 || slot >= kMaxBots)
            return;
        if (v < 0.0f)
            g_botThrMilli[slot].store(kSlotDefault, std::memory_order_relaxed);
        else
            g_botThrMilli[slot].store((int)(v * 1000), std::memory_order_relaxed);
    }
    // Returns slot's threshold, or -1 if unset
    float GetBotDensityThreshold(int slot)
    {
        if (slot < 0 || slot >= kMaxBots)
            return -1.0f;
        int m = g_botThrMilli[slot].load(std::memory_order_relaxed);
        return m == kSlotDefault ? -1.0f : m * 0.001f;
    }
    int GetMaxBots() { return kMaxBots; }
    int GetLastBotSlot() { return g_lastBotSlot.load(std::memory_order_relaxed); }
    bool IsVisiblePosHooked() { return g_origIsVisiblePos != nullptr; }
    long long GetIsVisiblePosCalls() { return g_isVisiblePosCalls.load(std::memory_order_relaxed); }
    unsigned int GetLastCtrlHandle() { return g_lastCtrlHandle.load(std::memory_order_relaxed); }
    unsigned long long GetLastPawnPtr() { return g_lastPawnPtr.load(std::memory_order_relaxed); }

    void SetEngine(void *engine) { g_pEngine = static_cast<IVEngineServer2 *>(engine); }

    void SetRayTrace(void *rt, int ret)
    {
        g_pRayTrace = static_cast<cs2bv::rt::CRayTraceInterface *>(rt);
        g_rtRet = ret;
    }
    bool RayTraceReady() { return g_pRayTrace != nullptr; }

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

    // Record a bullet capsule hole
    void OnBulletHole(const float start[3], const float end[3], float radius)
    {
        float t = NowTime();
        std::lock_guard<std::mutex> lk(g_bulletHoleMutex);
        if (g_bulletHoles.size() < kMaxBulletHoles)
        {
            BulletHole h;
            for (int i = 0; i < 3; ++i)
            {
                h.start[i] = start[i];
                h.end[i] = end[i];
            }
            h.startTime = t;
            h.radius = radius;
            g_bulletHoles.push_back(h);
        }
        else
        {
            // find oldest and replace
            size_t oldest = 0;
            for (size_t i = 1; i < g_bulletHoles.size(); ++i)
                if (g_bulletHoles[i].startTime < g_bulletHoles[oldest].startTime)
                    oldest = i;
            for (int i = 0; i < 3; ++i)
            {
                g_bulletHoles[oldest].start[i] = start[i];
                g_bulletHoles[oldest].end[i] = end[i];
            }
            g_bulletHoles[oldest].startTime = t;
            g_bulletHoles[oldest].radius = radius;
        }
    }

    void SetBulletRadius(float v) { g_bulletRadiusMilli.store((int)(v * 1000), std::memory_order_relaxed); }
    float GetBulletRadius() { return g_bulletRadiusMilli.load(std::memory_order_relaxed) * 0.001f; }
    void SetBulletRadiusShotgun(float v) { g_bulletRadiusShotgunMilli.store((int)(v * 1000), std::memory_order_relaxed); }
    float GetBulletRadiusShotgun() { return g_bulletRadiusShotgunMilli.load(std::memory_order_relaxed) * 0.001f; }
    void SetBulletDuration(float v) { g_bulletDurationMilli.store((int)(v * 1000), std::memory_order_relaxed); }
    float GetBulletDuration() { return g_bulletDurationMilli.load(std::memory_order_relaxed) * 0.001f; }
    void SetBulletRange(float v) { g_bulletRangeMilli.store((int)(v * 1000), std::memory_order_relaxed); }
    float GetBulletRange() { return g_bulletRangeMilli.load(std::memory_order_relaxed) * 0.001f; }
    void SetBulletHolesEnabled(bool e) { g_bulletHolesEnabled.store(e ? 1 : 0, std::memory_order_relaxed); }
    bool GetBulletHolesEnabled() { return g_bulletHolesEnabled.load(std::memory_order_relaxed) != 0; }

    int GetActiveBulletHoleCount()
    {
        std::lock_guard<std::mutex> lk(g_bulletHoleMutex);
        return (int)g_bulletHoles.size();
    }

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

    long long GetBulletCount() { return g_bulletCount.load(std::memory_order_relaxed); }

    const char *GetLastBulletInfo()
    {
        static char buf[160];
        std::lock_guard<std::mutex> lk(g_bulletMutex);
        std::snprintf(buf, sizeof(buf),
                      "src=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f) fwd=(%.2f,%.2f,%.2f)",
                      g_lastBulletSrc[0], g_lastBulletSrc[1], g_lastBulletSrc[2],
                      g_lastBulletAng[0], g_lastBulletAng[1], g_lastBulletAng[2],
                      g_lastBulletFwd[0], g_lastBulletFwd[1], g_lastBulletFwd[2]);
        return buf;
    }

    // Diagnostics
    const char *GetBulletDiag()
    {
        static char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "rt=%s ret=%d enabled=%d autolist=%s attempts=%lld traceHits=%lld",
                      g_pRayTrace ? "OK" : "NULL",
                      g_rtRet,
                      g_bulletHolesEnabled.load(std::memory_order_relaxed),
                      g_pAutoListHead ? "set" : "NULL",
                      static_cast<long long>(g_traceAttempts.load(std::memory_order_relaxed)),
                      static_cast<long long>(g_traceHits.load(std::memory_order_relaxed)));
        return buf;
    }

    // Probe: last active-weapon def index seen by the pellet hook
    const char *GetWeaponProbe()
    {
        static char buf[128];
        int def = g_lastWeaponDef.load(std::memory_order_relaxed);
        std::snprintf(buf, sizeof(buf),
                      "lastDef=%d shotgun=%d shotgunRadius=%.1f normalRadius=%.1f getSlot=%s",
                      def,
                      g_lastWeaponShotgun.load(std::memory_order_relaxed),
                      GetBulletRadiusShotgun(), GetBulletRadius(),
                      g_pGetSlot ? "OK" : "NULL");
        return buf;
    }

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
        bool bulletCleared = SegmentClearedByBulletHole(from, to);
        bool finalBlock = engineBlock && !heCleared && !bulletCleared;
        written += std::snprintf(buf + written, buflen - written,
                                 "density=%.4f  threshold=%.4f  engineBlock=%d  heCleared=%d  bulletCleared=%d  blocked=%d  activeHoles=%d\n",
                                 dens, thr, engineBlock ? 1 : 0, heCleared ? 1 : 0, bulletCleared ? 1 : 0,
                                 finalBlock ? 1 : 0, GetActiveBlastCount());
        return written;
    }

} // namespace cs2bv::hooks
