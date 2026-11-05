// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "SmokeEffects.h"
#include <bit>
#include <cmath>
#include <limits>

namespace cs2bv::smoke_effects {
struct Geometry
{
    BulletSample bullet;
    float heFactor;
};

inline Geometry PrepareGeometry(Vec3 point, const HoleCache::PreparedView& effects)
{
    const auto bullet = effects.bullets.empty() ? HoleCache::IdentityBullet(point) : HoleCache::SampleBullet(point, effects);
    return { bullet, !effects.he.empty() ? HoleCache::ApplyHePrepared(bullet.position, 1.0F, effects) : 1.0F };
}

inline float EvaluateGeometry(const Geometry& geometry, float original, HoleCache::Sampler sample, void* volume)
{
    return HoleCache::BulletDensity(geometry.bullet, original, sample, volume) * geometry.heFactor;
}

// Cache entries store geometry by smoke handle, tick and exact coordinates. Density stays live.
class GeometryCache
{
  public:
    // A view borrows its snapshot for one synchronous traversal.
    class View
    {
      public:
        Geometry Sample(Vec3 point, uint32_t cell) const
        {
            // A single bullet has conservative capsule bounds.
            if (empty_ || (bounds_ && (point.x < bounds_->minimum.x || point.x > bounds_->maximum.x || point.y < bounds_->minimum.y ||
                                       point.y > bounds_->maximum.y || point.z < bounds_->minimum.z || point.z > bounds_->maximum.z)))
                return { { point, 0 }, 1 };
            return owner_.SampleAt(point, cell, *this);
        }

      private:
        friend class GeometryCache;
        View(GeometryCache& owner, HoleCache::PreparedView effects, SmokeKey smoke, uint64_t epoch)
            : owner_(owner), effects_(effects), epoch_(epoch), identity_((uint64_t(smoke.handle) << 32) | uint32_t(smoke.effectTick)),
              salt_(smoke.handle * 0x9e3779b1U), bounds_(effects.he.empty() && effects.bullets.size() == 1 ? &effects.bullets[0] : nullptr),
              empty_(effects.Empty())
        {
        }
        GeometryCache& owner_;
        HoleCache::PreparedView effects_;
        uint64_t epoch_, identity_;
        uint32_t salt_;
        const HoleCache::PreparedBullet* bounds_;
        bool empty_;
    };

    View Bind(HoleCache::PreparedView effects, SmokeKey smoke, float now, uint64_t generation)
    {
        if (now != time_ || generation != generation_)
        {
            time_ = now;
            generation_ = generation;
            if (++epoch_ == 0)
            {
                for (auto& entry : entries_)
                    entry.epoch = 0;
                ++epoch_;
            }
        }
        return View(*this, effects, smoke, epoch_);
    }
    View Bind(HoleCache::Snapshot&&, SmokeKey, float, uint64_t) = delete;
    View Bind(const HoleCache::Snapshot&&, SmokeKey, float, uint64_t) = delete;
    View Bind(HoleCache::SmokeView&&, SmokeKey, float, uint64_t) = delete;
    View Bind(const HoleCache::SmokeView&&, SmokeKey, float, uint64_t) = delete;
    Geometry Sample(Vec3 point, uint32_t cell, SmokeKey smoke, const HoleCache::Snapshot& effects, float now, uint64_t generation)
    {
        return Bind(effects, smoke, now, generation).Sample(point, cell);
    }
    void Reset()
    {
        time_ = std::numeric_limits<float>::quiet_NaN();
        hits_ = misses_ = 0;
    }
    uint64_t Hits() const { return hits_; }
    uint64_t Misses() const { return misses_; }

  private:
    Geometry SampleAt(Vec3 point, uint32_t cell, const View& view)
    {
        const auto coordinates = std::bit_cast<std::array<uint32_t, 3>>(point);
        auto& entry = entries_[(cell ^ view.salt_) & (entries_.size() - 1)];
        if (entry.epoch == view.epoch_ && entry.identity == view.identity_ && entry.coordinates == coordinates)
        {
            ++hits_;
            return entry.geometry;
        }
        ++misses_;
        const auto result = PrepareGeometry(point, view.effects_);
        entry = { view.epoch_, view.identity_, coordinates, result };
        return result;
    }
    struct Entry
    {
        uint64_t epoch = 0, identity = 0;
        std::array<uint32_t, 3> coordinates{};
        Geometry geometry{};
    };
    std::array<Entry, 32768> entries_{};
    uint64_t epoch_ = 0, generation_ = 0, hits_ = 0, misses_ = 0;
    float time_ = std::numeric_limits<float>::quiet_NaN();
};
} // namespace cs2bv::smoke_effects
