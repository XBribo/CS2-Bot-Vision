// SPDX-License-Identifier: AGPL-3.0-only
// Adapted from unicbm/CS2-bot-improver-light@1d023039, native/BotVision.
#pragma once
#include "SmokeEffects.h"
#include <algorithm>
#include <cmath>

namespace cs2bv::smoke_effects {
// A source stores byte XYZ and simulation feedback.
struct SmokeSource
{
    std::uint32_t cell;
    float feedback;
};
static_assert(sizeof(SmokeSource) == 8);

inline bool SourceCenter(Vec3 origin, SmokeSource source, Vec3& center)
{
    const auto x = source.cell & 255, y = (source.cell >> 8) & 255, z = (source.cell >> 16) & 255;
    if (x >= 32 || y >= 32 || z >= 32) return false;
    center = { origin.x + (static_cast<float>(x) - 16) * 20 + 10, origin.y + (static_cast<float>(y) - 16) * 20 + 10,
               origin.z + (static_cast<float>(z) - 16) * 20 + 10 };
    return IsFinite(center);
}

// One reachable initial source admits the smoke.
template <typename Trace> bool HeAffectsSources(Vec3 blast, Vec3 origin, std::span<const Vec3> sources, Trace&& reachable)
{
    const float dx = std::max(std::abs(blast.x - origin.x) - 320.0f, 0.0f);
    const float dy = std::max(std::abs(blast.y - origin.y) - 320.0f, 0.0f);
    const float dz = std::max(std::abs(blast.z - origin.z) - 320.0f, 0.0f);
    if (dx * dx + dy * dy + dz * dz >= 256.0f * 256.0f) return false;
    for (const auto source : sources)
    {
        const float x = blast.x - source.x, y = blast.y - source.y, z = blast.z - source.z;
        if (x * x + y * y + z * z < 256.0f * 256.0f && reachable(blast, source)) return true;
    }
    return false;
}
} // namespace cs2bv::smoke_effects
