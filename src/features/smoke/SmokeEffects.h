// SPDX-License-Identifier: AGPL-3.0-only
// Adapted from unicbm/CS2-bot-improver-light@1d023039, native/BotVision.
#pragma once
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

// The cache belongs to the server thread.
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
    // HE records are owned; bullets borrow the query snapshot.
    struct SmokeView
    {
        std::array<PreparedHe, HeCapacity> he{};
        std::span<const PreparedBullet> bullets;
        std::size_t heCount = 0;
        bool Empty() const { return heCount == 0 && bullets.empty(); }
        operator PreparedView() const { return { { he.data(), heCount }, bullets }; }
    };
    void AddHe(Vec3 center, float time, std::vector<SmokeKey> affected, const void* owner = nullptr);
    void RemoveHe(const void* owner);
    void AddBullet(Vec3 start, Vec3 end, float time, float scale);
    Snapshot Prepare(float now) const;
    void Reset();
    std::uint64_t Generation() const { return generation_; }
    static BulletSample SampleBullet(Vec3 point, const PreparedView& snapshot);
    // Adding positive zero preserves the empty evaluator's behavior.
    static BulletSample IdentityBullet(Vec3 point) { return { { point.x + 0.0F, point.y + 0.0F, point.z + 0.0F }, 0 }; }
    static bool MayAffectRay(Vec3 from, Vec3 to, const PreparedView& snapshot);
    static float ApplyBullet(float density, float influence);
    // Finite coordinates; the grid spans origin +/- 320.
    static bool IntersectsVolume(Vec3 start, Vec3 end, Vec3 origin);
    // HE selection preserves event order. GPU depth is unavailable on the server.
    static Snapshot ForSmoke(const Snapshot& snapshot, SmokeKey key, float smokeStart, float now);
    static SmokeView ForSmokeView(const Snapshot& snapshot, SmokeKey key, float smokeStart, float now);
    static SmokeView ForSmokeView(Snapshot&&, SmokeKey, float, float) = delete;
    static SmokeView ForSmokeView(const Snapshot&&, SmokeKey, float, float) = delete;
    static float ApplyHePrepared(Vec3 point, float density, const PreparedView& snapshot);
    static float ApplyHe(Vec3 point, SmokeKey key, float smokeStart, float now, float density, const PreparedView& snapshot);
    using Sampler = float (*)(void*, const Vec3*);
    static float BulletDensity(BulletSample bullet, float original, Sampler sample, void* volume)
    {
        if (bullet.influence <= 0) return original;
        const float density = sample(volume, &bullet.position);
        if (!std::isfinite(density) || density < 0) return std::numeric_limits<float>::quiet_NaN();
        return ApplyBullet(density, bullet.influence);
    }
    static float EvaluateCellPrepared(Vec3 point, float original, const PreparedView& snapshot, Sampler sample, void* volume);
    static float EvaluateBulletCell(BulletSample bullet, float original, const PreparedView& snapshot, Sampler sample, void* volume);
    static float EvaluateCell(
        Vec3 point, float original, SmokeKey key, float smokeStart, float now, const PreparedView& snapshot, Sampler sample, void* volume);

  private:
    static float
    ApplyHeInternal(Vec3 point, SmokeKey key, float smokeStart, float now, float density, const PreparedView& snapshot, bool prepared);
    std::array<He, HeCapacity> he_{};
    std::array<Bullet, BulletCapacity> bullets_{};
    std::size_t heFirst_ = 0, heCount_ = 0, bulletCursor_ = 0;
    std::uint64_t generation_ = 0;
};
} // namespace cs2bv::smoke_effects
