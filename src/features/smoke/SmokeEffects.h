#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace cs2bv::smoke_effects {
struct Vec3
{
    float x = 0, y = 0, z = 0;
};
inline bool IsFinite(Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
struct SmokeKey
{
    std::uint32_t handle = 0;
    int effectTick = 0;
    bool operator==(const SmokeKey&) const = default;
};
struct BulletSample
{
    Vec3 position;
    float influence = 0;
};

// The server thread owns the cache.
class HoleCache
{
  public:
    static constexpr std::size_t HeCapacity = 5, BulletCapacity = 16;
    struct He
    {
        Vec3 center;
        float time = 0;
        std::vector<SmokeKey> affected;
        const void* owner = nullptr; // Projectile identity.
    };
    struct Bullet
    {
        Vec3 start, end;
        float time = 0, scale = 1;
        bool valid = false;
    };
    struct PreparedHe
    {
        const He* event;
        float age, impulse, recovery;
    };
    struct PreparedBullet
    {
        Vec3 start, end, direction;
        float age, scale;
        Vec3 delta;
        float length2, rejectDistance2;
        Vec3 minimum, maximum;
    };
    struct PreparedView
    {
        std::span<const PreparedHe> he;
        std::span<const PreparedBullet> bullets;
        bool Empty() const { return he.empty() && bullets.empty(); }
    };
    struct Snapshot
    {
        std::array<PreparedHe, HeCapacity> he{};
        std::array<PreparedBullet, BulletCapacity> bullets{};
        std::size_t heCount = 0, bulletCount = 0;
        bool Empty() const { return heCount == 0 && bulletCount == 0; }
        operator PreparedView() const { return { { he.data(), heCount }, { bullets.data(), bulletCount } }; }
    };
    // HE records are copied. Bullets borrow the query snapshot.
    struct SmokeView
    {
        std::array<PreparedHe, HeCapacity> he{};
        std::span<const PreparedBullet> bullets;
        std::size_t heCount = 0;
        bool Empty() const { return heCount == 0 && bullets.empty(); }
        operator PreparedView() const { return { { he.data(), heCount }, bullets }; }
    };
    // Capture validates finite coordinates/time and positive scale.
    void AddHe(Vec3 center, float time, std::vector<SmokeKey> affected, const void* owner = nullptr);
    void RemoveHe(const void* owner);
    void AddBullet(Vec3 start, Vec3 end, float time, float scale);
    Snapshot Prepare(float now) const;
    void Reset();
    std::uint64_t Generation() const { return generation_; }
    static BulletSample SampleBullet(Vec3 point, const PreparedView& snapshot);
    // Positive zero preserves empty-evaluator behavior.
    static BulletSample IdentityBullet(Vec3 point) { return { { point.x + 0.0F, point.y + 0.0F, point.z + 0.0F }, 0 }; }
    static bool MayAffectRay(Vec3 from, Vec3 to, const PreparedView& snapshot);
    static float ApplyBullet(float density, float influence);
    // Coordinates are finite. Grid bounds are origin +/- 320.
    static bool IntersectsVolume(Vec3 start, Vec3 end, Vec3 origin);
    // Preserve HE order. Server-side GPU depth is unavailable.
    static SmokeView ForSmokeView(const Snapshot& snapshot, SmokeKey key, float smokeStart, float now);
    static SmokeView ForSmokeView(Snapshot&&, SmokeKey, float, float) = delete;
    static SmokeView ForSmokeView(const Snapshot&&, SmokeKey, float, float) = delete;
    static float HeFactor(Vec3 point, const PreparedView& snapshot);
    // Server voxel density approximates client texture density.
    static float HeContribution(float density, float factor)
    {
        if (factor == 1 || density <= 0) return density;
        // Uniform-noise approximation of Smooth(saturate(5 * (2 * density + noise - 1))).
        const auto opacity = [](float d) {
            if (d < .1f) return 100 * d * d * d * (2 - 10 * d);
            if (d <= .5f) return 2 * d - .1f;
            if (d >= .6f) return 1.0f;
            const float tail = .6f - d;
            return 1 - 100 * tail * tail * tail * (2 - 10 * tail);
        };
        const float base = std::clamp(density, 0.0f, 1.0f);
        // Cancel cubic terms for stability near zero.
        const float ratio =
            base < .1f ? factor * factor * factor * (2 - 10 * base * factor) / (2 - 10 * base) : opacity(base * factor) / opacity(base);
        return density * ratio;
    }
    using Sampler = float (*)(void*, const Vec3*);
    static float BulletDensity(BulletSample bullet, float original, Sampler sample, void* volume)
    {
        if (bullet.influence <= 0) return original;
        const float density = sample(volume, &bullet.position);
        if (!std::isfinite(density) || density < 0) return std::numeric_limits<float>::quiet_NaN();
        return ApplyBullet(density, bullet.influence);
    }

  private:
    std::array<He, HeCapacity> he_{};
    std::array<Bullet, BulletCapacity> bullets_{};
    std::size_t heFirst_ = 0, heCount_ = 0, bulletCursor_ = 0;
    std::uint64_t generation_ = 0;
};
} // namespace cs2bv::smoke_effects
