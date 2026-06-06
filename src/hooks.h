#pragma once

#include <string>

namespace cs2bv::hooks
{

    bool Install(const std::string &gamedataPath,
                 void *serverInterface,
                 char *error = nullptr, size_t maxlen = 0);
    void Remove();
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
