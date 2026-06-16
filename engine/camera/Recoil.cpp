// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Recoil.cpp — see Recoil.h.
//
// No <cmath> here on purpose: the whole point of the model is that it needs no
// trigonometry and no libm. Everything is +,-,*,/ on f32 plus an integer
// splitmix64 hash, so the spray is bit-identical for a given (pattern, shot
// sequence) and replay-safe on the lane's strict-FP build.

#include "camera/Recoil.h"

namespace psynder::camera {

namespace {

// ─── Local splitmix64 hash (lockstep-safe pseudo-randomness) ─────────────
// Reimplemented locally — deliberately NOT a cross-lane dependency on
// gameplay::Rng / gameplay::spread_seed (Spread.{h,cpp}), which share the same
// splitmix64 avalanche constants (Steele / Lea / Flood). All operations are
// exact on u64, so the hash is identical on every platform. We hash the
// (seed, shot_index) pair into a single u64 and derive a deterministic
// horizontal kick from it.
u64 splitmix64(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Hash the (seed, shot_index) pair. Fold the seed in first, then avalanche the
// shot_index (offset by a second odd constant so shot_index==0 isn't a
// degenerate input), then mix the two together. Pure integer ops.
u64 recoil_hash(u32 seed, u32 shot_index) noexcept {
    u64 h = splitmix64(static_cast<u64>(seed));
    h ^= splitmix64(static_cast<u64>(shot_index) + 0x632BE59BD9B4E019ull);
    return splitmix64(h);
}

// Map a hash to a deterministic signed unit value in [-1, 1] with NO trig and
// NO float RNG. We take the top 25 bits of the hash: bit 24 becomes the SIGN
// and the low 24 bits become the magnitude. 24-bit integers are exactly
// representable in f32 and 1/2^24 is exact, so magnitude = bits24 / 2^24 lands
// in [0, 1) identically on every platform; the sign flip keeps it exact. The
// result therefore lies in (-1, 1] / [-1, 1) and weaves both directions as the
// shot_index walks the pattern — a real left/right spray, not a one-sided drift.
f32 signed_unit(u64 hash) noexcept {
    const u32 bits24 = static_cast<u32>(hash >> 40);          // top 24 bits  -> [0, 2^24)
    const f32 mag    = static_cast<f32>(bits24) * (1.0f / 16777216.0f);  // 2^24
    const bool neg   = ((hash >> 39) & 1ull) != 0ull;         // next bit -> sign
    return neg ? -mag : mag;
}

// Move `value` toward zero by `step` (step >= 0), clamping AT zero so it never
// crosses past the origin. Branch on sign so a positive offset decays down to
// 0 and a negative offset rises up to 0 — never overshooting.
f32 decay_toward_zero(f32 value, f32 step) noexcept {
    if (value > 0.0f) {
        value -= step;
        if (value < 0.0f) value = 0.0f;
    } else if (value < 0.0f) {
        value += step;
        if (value > 0.0f) value = 0.0f;
    }
    return value;
}

}  // namespace

void recoil_fire(RecoilState& s, const RecoilPattern& p) noexcept {
    // The horizontal sample uses the CURRENT shot_index (0-based) so the very
    // first shot of a fresh spray draws pattern slot 0; THEN we advance.
    const f32 weave = p.yaw_amplitude_deg * signed_unit(recoil_hash(p.seed, s.shot_index));
    s.yaw_offset_deg += weave;

    // Vertical climb, saturating at the cap. The climb only ever increases
    // here (recovery is the only thing that brings pitch back down), and it
    // never exceeds max_pitch_deg.
    f32 pitch = s.pitch_offset_deg + p.pitch_per_shot_deg;
    if (pitch > p.max_pitch_deg) pitch = p.max_pitch_deg;
    s.pitch_offset_deg = pitch;

    // Advance the spray position for the next shot.
    s.shot_index += 1u;
}

void recoil_recover(RecoilState& s, const RecoilPattern& p, f32 dt_s) noexcept {
    if (dt_s <= 0.0f) return;  // no time passed -> nothing to recover

    const f32 step = p.recovery_per_s_deg * dt_s;
    s.pitch_offset_deg = decay_toward_zero(s.pitch_offset_deg, step);
    s.yaw_offset_deg   = decay_toward_zero(s.yaw_offset_deg, step);

    // Once the view has fully settled back to rest, restart the pattern so the
    // next burst climbs/weaves from the top instead of continuing the curve.
    if (s.pitch_offset_deg == 0.0f && s.yaw_offset_deg == 0.0f) {
        s.shot_index = 0u;
    }
}

void recoil_reset(RecoilState& s) noexcept {
    s.pitch_offset_deg = 0.0f;
    s.yaw_offset_deg   = 0.0f;
    s.shot_index       = 0u;
}

}  // namespace psynder::camera
