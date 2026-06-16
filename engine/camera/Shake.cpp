// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Shake.cpp — see Shake.h.
//
// No <cmath> here on purpose: the whole point of the model is that it needs no
// trigonometry and no libm. Everything is +,-,*,/ on f32 plus an integer
// splitmix64 hash, so the shake is bit-identical for a given (state, params)
// and replay-safe on the lane's strict-FP build. The textbook "sin(time*freq)"
// shake is replaced by VALUE NOISE: a hashed [-1,1] lattice smoothstep-lerped
// over the noise phase, which has no trig and is exactly reproducible.

#include "camera/Shake.h"

namespace psynder::camera {

namespace {

// ─── Local splitmix64 hash (lockstep-safe pseudo-randomness) ─────────────
// Reimplemented locally — deliberately NOT a cross-lane dependency on Recoil's
// hash or on gameplay::Rng. Same well-known avalanche constants (Steele / Lea /
// Flood); all operations are exact on u64, so the hash is identical on every
// platform.
u64 splitmix64(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Hash a (seed, axis, lattice_index) triple into one u64. We fold the seed in
// first, then avalanche the axis (offset by an odd constant so axis==0 isn't a
// degenerate input), then the lattice index (offset by a different odd constant
// for the same reason), mixing after each so adjacent lattice indices and
// neighbouring axes produce uncorrelated lattice values. Pure integer ops —
// `i32 lattice` is widened through u32 first so a negative index (time before
// t=0) folds deterministically into the u64 space instead of sign-extending
// into a different bucket on different platforms.
u64 noise_hash(u32 seed, u32 axis, i32 lattice) noexcept {
    u64 h = splitmix64(static_cast<u64>(seed));
    h ^= splitmix64(static_cast<u64>(axis) + 0x632BE59BD9B4E019ull);
    const u64 li = static_cast<u64>(static_cast<u32>(lattice));
    h ^= splitmix64(li + 0xD6E8FEB86659FD93ull);
    return splitmix64(h);
}

// Map a hash to a deterministic lattice value in [-1, 1] with NO trig and NO
// float RNG. We take the top 25 bits of the hash: the low 24 bits become the
// magnitude and the next bit becomes the sign. 24-bit integers are exactly
// representable in f32 and 1/2^24 is exact, so magnitude = bits24 / 2^24 lands
// in [0, 1) identically on every platform; the sign flip keeps it exact. The
// result therefore lies in (-1, 1] / [-1, 1) — a real two-sided lattice value.
f32 lattice_value(u64 hash) noexcept {
    const u32 bits24 = static_cast<u32>(hash >> 40);                    // top 24 bits -> [0, 2^24)
    const f32 mag    = static_cast<f32>(bits24) * (1.0f / 16777216.0f); // /2^24, exact
    const bool neg   = ((hash >> 39) & 1ull) != 0ull;                   // next bit -> sign
    return neg ? -mag : mag;
}

// Floor of a non-negative-or-negative f32 to the largest integer <= x, without
// <cmath>. The noise phase is time_s * frequency_hz; time_s only grows from 0,
// but we still handle negatives correctly (truncation rounds toward zero, so a
// negative non-integer needs a -1 correction) to keep the lattice well-defined
// for any input. Phases beyond i32 range are clamped to the bounds so the cast
// stays defined; in practice time_s * frequency_hz never approaches 2^31.
i32 floor_to_int(f32 x) noexcept {
    if (x >= 2147483520.0f) return 2147483520;      // near INT32_MAX, f32-exact
    if (x <= -2147483520.0f) return -2147483520;
    i32 t = static_cast<i32>(x);                    // truncates toward zero
    if (static_cast<f32>(t) > x) t -= 1;            // correct for negatives
    return t;
}

// Smoothstep easing of the fractional phase: 3t^2 - 2t^3. Maps [0,1] -> [0,1]
// with zero slope at both ends, so the value-noise lerp is C1-continuous across
// lattice boundaries (no velocity discontinuity / visible "kink" when the
// phase crosses an integer). Pure polynomial — exact and deterministic.
f32 smoothstep01(f32 t) noexcept {
    return t * t * (3.0f - 2.0f * t);
}

// One axis of value noise in [-1, 1]: hash the lattice point at floor(phase)
// and at floor(phase)+1, then lerp between them by the smoothstepped fraction.
// Adjacent samples (small phase steps) differ only a little — it is a lerp, not
// a per-frame re-roll — which is what makes the shake read as a smooth wobble.
f32 value_noise(u32 seed, u32 axis, f32 phase) noexcept {
    const i32 i0   = floor_to_int(phase);
    const f32 frac = phase - static_cast<f32>(i0);          // in [0, 1)
    const f32 v0   = lattice_value(noise_hash(seed, axis, i0));
    const f32 v1   = lattice_value(noise_hash(seed, axis, i0 + 1));
    const f32 w    = smoothstep01(frac);
    return v0 + (v1 - v0) * w;                              // lerp, stays in [-1, 1]
}

// Axis identifiers for the per-axis noise — distinct constants so the six
// channels are uncorrelated. Kept local; not part of the public ABI.
constexpr u32 kAxisPitch = 0u;
constexpr u32 kAxisYaw   = 1u;
constexpr u32 kAxisRoll  = 2u;
constexpr u32 kAxisPosX  = 3u;
constexpr u32 kAxisPosY  = 4u;
constexpr u32 kAxisPosZ  = 5u;

// True iff finite (no <cmath>): NaN != itself; Inf - Inf is NaN, so the
// self-subtraction trick rejects both NaN and the infinities in one test.
bool is_finite(f32 x) noexcept {
    return (x == x) && ((x - x) == 0.0f);
}

}  // namespace

void shake_add_trauma(ShakeState& s, f32 amount) noexcept {
    if (!is_finite(amount)) return;            // ignore NaN/Inf impacts
    f32 t = s.trauma + amount;
    if (t < 0.0f) t = 0.0f;                    // a stray negative can't sink it
    if (t > 1.0f) t = 1.0f;                    // saturate at full energy
    s.trauma = t;
}

void shake_tick(ShakeState& s, f32 dt_s, const ShakeParams& p) noexcept {
    if (dt_s <= 0.0f) return;                  // no time passed -> nothing to do

    // Advance the noise phase clock (only ever grows).
    s.time_s += dt_s;

    // Decay trauma toward rest, clamping AT zero (never negative).
    f32 t = s.trauma - p.decay_per_s * dt_s;
    if (t < 0.0f) t = 0.0f;
    s.trauma = t;
}

ShakeOffset shake_sample(const ShakeState& s, const ShakeParams& p) noexcept {
    ShakeOffset out{};

    // magnitude = trauma^2 — a smooth ease-out (the shake fades gently as
    // trauma drains). At trauma == 0 this is exactly 0, so every field below is
    // exactly 0 and we short-circuit the noise work.
    const f32 magnitude = s.trauma * s.trauma;
    if (magnitude <= 0.0f) return out;

    // The noise phase: lattice points advance at `frequency_hz` per second.
    const f32 phase = s.time_s * p.frequency_hz;

    // Each axis draws an INDEPENDENT value-noise sample in [-1, 1], scaled by
    // magnitude and the axis's peak displacement.
    out.pitch_deg = magnitude * p.max_pitch_deg * value_noise(s.seed, kAxisPitch, phase);
    out.yaw_deg   = magnitude * p.max_yaw_deg   * value_noise(s.seed, kAxisYaw,   phase);
    out.roll_deg  = magnitude * p.max_roll_deg  * value_noise(s.seed, kAxisRoll,  phase);
    out.pos[0]    = magnitude * p.max_pos_m     * value_noise(s.seed, kAxisPosX,  phase);
    out.pos[1]    = magnitude * p.max_pos_m     * value_noise(s.seed, kAxisPosY,  phase);
    out.pos[2]    = magnitude * p.max_pos_m     * value_noise(s.seed, kAxisPosZ,  phase);
    return out;
}

void shake_reset(ShakeState& s) noexcept {
    s.trauma = 0.0f;
    s.time_s = 0.0f;
    // seed is intentionally preserved (see header) so the camera keeps its
    // assigned noise pattern across resets.
}

}  // namespace psynder::camera
