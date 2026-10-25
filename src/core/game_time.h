#pragma once // NOLINT(portability-avoid-pragma-once)

namespace cs2bv::game_time {
// Resolves the network server service.
bool Initialize();

// Releases the service interface after hooks stop.
void Reset();

// Returns the server time or zero before a server exists.
float Now();
} // namespace cs2bv::game_time
