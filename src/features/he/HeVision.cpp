#include "HeVision.h"
#include "features/smoke/SmokeDensity.h"

namespace cs2bv::he_vision {
const char* GetHookStatus() { return smoke_density::Status(); }
} // namespace cs2bv::he_vision
