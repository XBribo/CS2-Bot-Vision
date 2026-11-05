// SPDX-License-Identifier: AGPL-3.0-only
// Adapted from unicbm/CS2-bot-improver-light@1d023039, native/BotVision.
#include "SmokeEffects.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace cs2bv::smoke_effects {
namespace {
float Sat(float x) { return std::clamp(x, 0.0f, 1.0f); }
float Smooth(float x)
{
    x = Sat(x);
    return x * x * (3 - 2 * x);
}
Vec3 Add(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 Sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 Mul(Vec3 a, float b) { return { a.x * b, a.y * b, a.z * b }; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
Vec3 Unit(Vec3 a)
{
    const float n = Length(a);
    return n > 0 ? Mul(a, 1 / n) : Vec3{};
}
} // namespace
void HoleCache::AddHe(Vec3 center, float time, std::vector<SmokeKey> affected, const void* owner)
{
    if (!IsFinite(center) || !std::isfinite(time) || affected.empty()) return;
    // Each projectile publishes one HE event.
    if (owner)
        for (std::size_t i = 0; i < heCount_; ++i)
            if (he_[(heFirst_ + i) % HeCapacity].owner == owner) return;
    he_[(heFirst_ + heCount_) % HeCapacity] = { center, time, std::move(affected), owner };
    if (heCount_ == HeCapacity) heFirst_ = (heFirst_ + 1) % HeCapacity;
    else ++heCount_;
    ++generation_;
}
void HoleCache::RemoveHe(const void* owner)
{
    if (!owner) return;
    for (std::size_t i = 0; i < heCount_; ++i)
    {
        auto& event = he_[(heFirst_ + i) % HeCapacity];
        if (event.owner != owner) continue;
        // The client replaces the removed entry with the newest event.
        auto& last = he_[(heFirst_ + heCount_ - 1) % HeCapacity];
        if (&event != &last) event = std::move(last);
        last = {};
        --heCount_;
        ++generation_;
        return;
    }
}
void HoleCache::AddBullet(Vec3 start, Vec3 end, float time, float scale)
{
    if (!IsFinite(start) || !IsFinite(end) || !std::isfinite(time) || !std::isfinite(scale) || scale <= 0 ||
        Dot(Sub(end, start), Sub(end, start)) <= 0)
        return;
    bullets_[bulletCursor_] = { start, end, time, scale, true };
    bulletCursor_ = (bulletCursor_ + 1) % BulletCapacity;
    ++generation_;
}
HoleCache::Snapshot HoleCache::Prepare(float now) const
{
    Snapshot result;
    if (!std::isfinite(now)) return result;
    for (std::size_t i = 0; i < heCount_; ++i)
    {
        const auto& h = he_[(heFirst_ + i) % HeCapacity];
        const float age = now - h.time;
        if (h.affected.empty() || age < 0 || age >= 5) continue;
        result.he[result.heCount++] = { &h, age, std::pow(1 - Smooth(age / 2), 128.0f), std::pow(Smooth((age - .5f) / 4.5f), 1.8f) };
    }
    for (std::size_t i = 0; i < BulletCapacity; ++i)
    {
        const auto& b = bullets_[(bulletCursor_ + i) % BulletCapacity];
        const float age = now - b.time;
        if (!b.valid || age <= 0 || age >= 1) continue;
        const float k = 10 * age;
        const Vec3 shift{ 0, 0, k };
        const Vec3 a = Add(b.start, shift), end = Add(Add(b.start, Mul(Sub(b.end, b.start), k)), shift);
        const Vec3 delta = Sub(end, a);
        // The broad phase uses 32/scale around the 20/scale support.
        const float rejectDistance = 32 / b.scale;
        // Bounds include the rounded endpoint a + delta.
        const Vec3 last = Add(a, delta);
        const auto lower = [rejectDistance](float x, float y) {
            return std::nextafter(std::min(x, y) - rejectDistance, -std::numeric_limits<float>::infinity());
        };
        const auto upper = [rejectDistance](float x, float y) {
            return std::nextafter(std::max(x, y) + rejectDistance, std::numeric_limits<float>::infinity());
        };
        result.bullets[result.bulletCount++] = { a,
                                                 end,
                                                 Unit(Sub(a, end)),
                                                 age,
                                                 b.scale,
                                                 delta,
                                                 Dot(delta, delta),
                                                 rejectDistance * rejectDistance,
                                                 { lower(a.x, last.x), lower(a.y, last.y), lower(a.z, last.z) },
                                                 { upper(a.x, last.x), upper(a.y, last.y), upper(a.z, last.z) } };
    }
    return result;
}
BulletSample HoleCache::SampleBullet(Vec3 point, const PreparedView& snapshot)
{
    if (snapshot.bullets.empty()) return IdentityBullet(point);
    float influence = 0;
    Vec3 direction{};
    for (std::size_t i = 0; i < snapshot.bullets.size(); ++i)
    {
        const auto& b = snapshot.bullets[i];
        if (snapshot.bullets.size() >= 4 && (point.x < b.minimum.x || point.x > b.maximum.x || point.y < b.minimum.y ||
                                             point.y > b.maximum.y || point.z < b.minimum.z || point.z > b.maximum.z))
            continue;
        const Vec3 delta = b.delta;
        const float length2 = b.length2;
        if (!(length2 > 0)) continue;
        const float t = Sat(Dot(Sub(point, b.start), delta) / length2);
        const Vec3 offset = Sub(point, Add(b.start, Mul(delta, t)));
        const float distance2 = Dot(offset, offset);
        if (distance2 > b.rejectDistance2) continue;
        const float q = Sat(.05f * std::sqrt(distance2) * b.scale);
        if (q >= 1) continue;
        const Vec3 endOffset = Sub(point, b.end);
        const float endDistance2 = Dot(endOffset, endOffset);
        const float e = endDistance2 >= 10000 ? 1.0f : std::min(std::sqrt(endDistance2) * .01f, 1.0f);
        const float u = Smooth(1 - Sat(std::min(q - e + 1, 1.0f) + b.age));
        influence = std::max(influence, u);
        direction = Add(direction, Mul(Sub(b.direction, direction), influence));
    }
    return { Add(point, Mul(Unit(direction), 20 * influence * influence * influence)), influence };
}
bool HoleCache::MayAffectRay(Vec3 from, Vec3 to, const PreparedView& snapshot)
{
    // HE warps the coordinates of subsequent effects.
    if (!snapshot.he.empty()) return true;
    // One cell of padding covers the DDA cell centers and boundary rounding.
    const Vec3 minimum{ std::min(from.x, to.x) - 20, std::min(from.y, to.y) - 20, std::min(from.z, to.z) - 20 };
    const Vec3 maximum{ std::max(from.x, to.x) + 20, std::max(from.y, to.y) + 20, std::max(from.z, to.z) + 20 };
    for (std::size_t i = 0; i < snapshot.bullets.size(); ++i)
    {
        const auto& b = snapshot.bullets[i];
        if (minimum.x <= b.maximum.x && maximum.x >= b.minimum.x && minimum.y <= b.maximum.y && maximum.y >= b.minimum.y &&
            minimum.z <= b.maximum.z && maximum.z >= b.minimum.z)
            return true;
    }
    return false;
}
float HoleCache::ApplyBullet(float density, float influence) { return Sat(density - (density + .05f) * influence); }
bool HoleCache::IntersectsVolume(Vec3 start, Vec3 end, Vec3 origin)
{
    const Vec3 a = Sub(start, origin), delta = Sub(end, start);
    const float p[]{ a.x, a.y, a.z }, d[]{ delta.x, delta.y, delta.z };
    float enter = 0, exit = 1;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (d[axis] == 0)
        {
            if (p[axis] < -320 || p[axis] > 320) return false;
            continue;
        }
        float near = (-320 - p[axis]) / d[axis], far = (320 - p[axis]) / d[axis];
        if (near > far) std::swap(near, far);
        enter = std::max(enter, near);
        exit = std::min(exit, far);
        if (enter > exit) return false;
    }
    return true;
}
HoleCache::Snapshot HoleCache::ForSmoke(const Snapshot& snapshot, SmokeKey key, float smokeStart, float now)
{
    const auto selected = ForSmokeView(snapshot, key, smokeStart, now);
    Snapshot result = snapshot;
    result.he = selected.he;
    result.heCount = selected.heCount;
    return result;
}
HoleCache::SmokeView HoleCache::ForSmokeView(const Snapshot& snapshot, SmokeKey key, float smokeStart, float now)
{
    SmokeView result;
    result.bullets = { snapshot.bullets.data(), snapshot.bulletCount };
    const float eligibleAge = now - smokeStart - .4f;
    for (std::size_t i = 0; i < snapshot.heCount; ++i)
    {
        const auto& h = snapshot.he[i];
        if (h.age < eligibleAge && std::find(h.event->affected.begin(), h.event->affected.end(), key) != h.event->affected.end())
            result.he[result.heCount++] = h;
    }
    return result;
}
float HoleCache::ApplyHe(Vec3 point, SmokeKey key, float smokeStart, float now, float density, const PreparedView& snapshot)
{
    return ApplyHeInternal(point, key, smokeStart, now, density, snapshot, false);
}
float HoleCache::ApplyHePrepared(Vec3 point, float density, const PreparedView& snapshot)
{
    return ApplyHeInternal(point, {}, 0, 0, density, snapshot, true);
}
float HoleCache::ApplyHeInternal(
    Vec3 point, SmokeKey key, float smokeStart, float now, float density, const PreparedView& snapshot, bool prepared)
{
    float factor = 1;
    for (std::size_t i = 0; i < snapshot.he.size(); ++i)
    {
        const auto& h = snapshot.he[i];
        if (!prepared && (!(h.age < now - smokeStart - .4f) ||
                          std::find(h.event->affected.begin(), h.event->affected.end(), key) == h.event->affected.end()))
            continue;
        const Vec3 offset = Sub(point, h.event->center);
        const float distance2 = Dot(offset, offset);
        if (distance2 >= 62500) continue;
        const float d = std::sqrt(distance2);
        if (!(d < 250)) continue;
        const float radial = Smooth((d + 250 * h.impulse - 200) / 40);
        factor = std::min(factor, radial + h.recovery);
        const float warp = (1 - Smooth((d - 100) / 150)) * (d >= 1250 * h.age ? 1.0f : 0.0f) * (1 - h.impulse);
        point = Add(point, Mul(Sub(h.event->center, point), warp));
    }
    return density * (.02f + .98f * factor);
}
void HoleCache::Reset()
{
    he_ = {};
    bullets_ = {};
    heFirst_ = heCount_ = bulletCursor_ = 0;
    ++generation_;
}
float HoleCache::EvaluateCell(
    Vec3 point, float original, SmokeKey key, float smokeStart, float now, const PreparedView& snapshot, Sampler sample, void* volume)
{
    const auto bullet = SampleBullet(point, snapshot);
    return ApplyHe(bullet.position, key, smokeStart, now, BulletDensity(bullet, original, sample, volume), snapshot);
}
float HoleCache::EvaluateCellPrepared(Vec3 point, float original, const PreparedView& snapshot, Sampler sample, void* volume)
{
    const auto bullet = snapshot.bullets.empty() ? IdentityBullet(point) : SampleBullet(point, snapshot);
    return EvaluateBulletCell(bullet, original, snapshot, sample, volume);
}
float HoleCache::EvaluateBulletCell(BulletSample bullet, float original, const PreparedView& snapshot, Sampler sample, void* volume)
{
    return ApplyHePrepared(bullet.position, BulletDensity(bullet, original, sample, volume), snapshot);
}
} // namespace cs2bv::smoke_effects
