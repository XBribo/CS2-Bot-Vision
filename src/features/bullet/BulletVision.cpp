#include "BulletVision.h"
#include "features/smoke/SmokeDensity.h"

namespace cs2bv::bullet_vision {
void SetHolesEnabled(bool enabled) { smoke_density::EnableBullets(enabled); }
bool GetHolesEnabled() { return smoke_density::BulletsEnabled(); }
} // namespace cs2bv::bullet_vision
