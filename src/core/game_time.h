// Shared access to the current server time

#pragma once // NOLINT(portability-avoid-pragma-once)

namespace cs2bv::game_time {
// Stores the engine interface used for server time
void SetEngine(void* engine);

// Returns the current server time or zero when unavailable
float Now();
} // namespace cs2bv::game_time
