#pragma once

#include <string>

namespace cs2bv::hooks
{

    bool Install(const std::string &gamedataPath,
                 void *serverInterface,
                 char *error = nullptr, size_t maxlen = 0);
    void Remove();

    // HE smoke-hole support
    void SetEngine(void *engine);
    void SetRayTrace(void *rayTraceInterface, int ret); // CRayTraceInterface*
    bool RayTraceReady();
    void OnHeDetonate(float x, float y, float z);
    void SetHeRadius(float v);
    float GetHeRadius();
    void SetHeDuration(float v);
    float GetHeDuration();
    int GetActiveBlastCount();

    // HE event-hook registration status
    void SetHeListenerStatus(bool managerResolved, bool listenerAdded);
    const char *GetHeListenerStatus();

    // Bullet smoke-hole support
    void OnBulletHole(const float start[3], const float end[3], float radius);
    void SetBulletRadius(float v);
    float GetBulletRadius();
    void SetBulletRadiusShotgun(float v);
    float GetBulletRadiusShotgun();
    const char *GetWeaponProbe();
    void SetBulletDuration(float v);
    float GetBulletDuration();
    void SetBulletRange(float v);
    float GetBulletRange();
    void SetBulletHolesEnabled(bool e);
    bool GetBulletHolesEnabled();
    int GetActiveBulletHoleCount();
    const char *GetBulletDiag();

    // Bullet
    long long GetBulletCount();
    const char *GetLastBulletInfo();
    long long GetHitCount();
    long long GetBlockedCount();
    bool IsHookedActive();
    const char *GetHookedStatus();
    int TestLos(float fx, float fy, float fz, float tx, float ty, float tz,
                char *buf, size_t buflen);
    void SetSmokeMode(int mode);
    int GetSmokeMode();
    void SetDensityThreshold(float v);
    float GetDensityThreshold();
    bool IsDensityFnResolved();
    // Formats safe-read failure counters for runtime diagnostics
    const char *GetSafeReadDiag();

    // Per-bot density threshold
    void SetBotDensityThreshold(int slot, float v);
    float GetBotDensityThreshold(int slot); // -1 if unset
    int GetMaxBots();
    int GetLastBotSlot();                // last slot resolved by IsVisiblePos hook (-1 = none)
    bool IsVisiblePosHooked();           // false = per-bot density unavailable
    long long GetIsVisiblePosCalls();    // probe: times the hook fired
    unsigned int GetLastCtrlHandle();    // probe: last raw controller handle read
    unsigned long long GetLastPawnPtr(); // probe: last pawn ptr

} // namespace cs2bv::hooks
