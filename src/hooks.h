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
    void OnHeDetonate(float x, float y, float z);
    void SetHeRadius(float v);
    float GetHeRadius();
    void SetHeDuration(float v);
    float GetHeDuration();
    int GetActiveBlastCount();

    // HE event-hook registration status
    void SetHeListenerStatus(bool managerResolved, bool listenerAdded);
    const char *GetHeListenerStatus();
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

} // namespace cs2bv::hooks
