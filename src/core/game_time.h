#pragma once // NOLINT(portability-avoid-pragma-once)

namespace cs2bv::game_time {
bool Initialize();

void Reset();

struct Sample
{
    const void* server;
    float time;
};
// Server world time. Unavailable time is NaN.
Sample Read();
} // namespace cs2bv::game_time
