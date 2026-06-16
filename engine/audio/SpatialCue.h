// SPDX-License-Identifier: MIT
//
// engine/audio/SpatialCue.h
//
// Lane 14 — listener-relative spatial cues (the world-geometry-to-scalars
// front-end that feeds PositionalMix.h).
//
// PositionalMix.h provides the device-free mix math — pan_lr(azimuth) and
// occlusion_filter() — but it explicitly says the *caller* supplies the
// world-geometry-derived scalars (azimuth, distance attenuation). This header
// is that missing layer: given a listener pose and a source world position it
// computes the Euclidean distance, an inverse-distance attenuation gain, the
// listener-relative azimuth (ready for pan_lr), and a doppler pitch factor.
//
// SCOPE: pure cue math only — no device I/O, no rays, no clock. The functions
// are header-inline, branch-light, POD in / POD out, allocation-free, and
// `noexcept` so they are safe to call from the mixer pull path next to
// PositionalMix. This is COSMETIC positional mix math, NOT the authoritative
// lockstep tick, so transcendentals (sqrt/atan2) are fine — the audio lane's
// determinism guarantee is same-platform (no RNG, no wall-clock, stable
// evaluation order), matching PositionalMix.
//
// Units (matching PositionalMix.h): distances are metres (1 world unit = 1 m);
// azimuth is radians (0 = front, +x toward the right ear, -x toward the left,
// clamped to [-pi/2, +pi/2] to match pan_lr's front-hemisphere domain); gains
// are linear [0,1]; speeds are m/s; pitch is a unitless playback multiplier.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <cmath>

namespace psynder::audio {

// Default speed of sound in dry air at ~20 C, in metres/second. Exposed as a
// parameter on doppler_pitch() so callers can model other media; this is the
// sensible default.
inline constexpr f32 kSpeedOfSoundMps = 343.0f;

// Doppler pitch is clamped to this musically-sane range so a fast fly-by (or a
// near-singularity closing speed approaching the speed of sound) can never
// produce a runaway or inverted playback rate.
inline constexpr f32 kDopplerMinPitch = 0.5f;
inline constexpr f32 kDopplerMaxPitch = 2.0f;

// ─── Listener pose ─────────────────────────────────────────────────────────
//
// The listener's world-space position and orientation basis. `forward` and
// `right` are unit vectors (+y is world up); they need not be perfectly
// orthonormal but are assumed unit-length — the azimuth is taken from the
// source direction's projections onto them.
struct ListenerPose {
    math::Vec3 position;  // world-space ears origin, metres
    math::Vec3 forward;   // unit world-space facing direction
    math::Vec3 right;     // unit world-space right-ear direction
};

// ─── Distance ──────────────────────────────────────────────────────────────
//
// Euclidean distance in metres from the listener to a source world position.
inline f32 source_distance_m(const ListenerPose& l, math::Vec3 source) noexcept {
    return math::length(math::sub(source, l.position));
}

// ─── Distance attenuation ──────────────────────────────────────────────────
//
// Inverse-distance-style rolloff, clamped to [0,1].
//
// `distance_m` — listener-to-source distance in metres.
// `ref_dist_m` — radius of full gain; at or under it the source is at unity.
// `max_dist_m` — radius at/beyond which the source is fully silent.
// `rolloff`    — falloff steepness (>= 0; 1 = textbook inverse distance, larger
//                = faster falloff). Negative is clamped to 0.
//
// Formula (for ref < d < max):
//     gain = ref / (ref + rolloff * (d - ref))
// which is 1.0 at d == ref and decreases monotonically as d grows. The result
// is then clamped to [0,1] and forced to exactly 0 at/beyond max_dist_m so a
// distant source is silent (and the [ref,max] band is continuous because the
// curve is already small near max for typical rolloff).
//
// Degenerate guards (clean, documented fallbacks):
//   * d <= ref            => 1.0 (full gain inside the reference radius).
//   * ref <= 0            => no inner plateau is meaningful; fall back to 1.0 at
//                            d == 0 and 0 beyond max — i.e. treat as a point
//                            source with the max-distance cutoff only.
//   * max <= ref          => the band is empty; return 1.0 inside ref and 0.0
//                            outside (a hard on/off at ref).
inline f32 distance_attenuation(f32 distance_m,
                                f32 ref_dist_m,
                                f32 max_dist_m,
                                f32 rolloff) noexcept {
    f32 d = distance_m;
    if (d < 0.0f) d = 0.0f;  // distance can never be negative

    // At or beyond the max radius the source is silent. (Also covers max <= ref
    // outside the reference radius below, since that path returns 0 there too.)
    if (d >= max_dist_m) {
        return 0.0f;
    }

    // Inside the reference radius: full gain. Also the natural answer when
    // ref <= 0 and d == 0.
    if (d <= ref_dist_m) {
        return 1.0f;
    }

    // ref_dist_m here is > 0 (d > ref >= ... and d <= ref handled above), and
    // max > ref (else d >= max already returned). Apply the rolloff curve.
    f32 k = rolloff;
    if (k < 0.0f) k = 0.0f;

    const f32 denom = ref_dist_m + k * (d - ref_dist_m);
    f32 g = denom > 0.0f ? (ref_dist_m / denom) : 1.0f;

    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    return g;
}

// ─── Listener-relative azimuth ─────────────────────────────────────────────
//
// The horizontal angle of the source relative to the listener's facing, in
// radians, ready to feed pan_lr(): 0 = dead ahead (along `forward`), positive
// toward `right` (right ear), negative toward the left, +-pi/2 at the sides.
//
// We project the listener->source direction onto the forward/right basis and
// take atan2(dot(dir,right), dot(dir,forward)). That raw angle covers the full
// circle [-pi, +pi]; pan_lr only models the front hemisphere [-pi/2, +pi/2].
//
// REAR FOLDING: a source behind the listener is folded onto the *same side's*
// hard-pan edge rather than wrapping past it. Concretely we clamp the raw
// azimuth into [-pi/2, +pi/2]: anything in (+pi/2, +pi] (behind-right) clamps
// to +pi/2 (hard right), anything in [-pi, -pi/2) (behind-left) clamps to
// -pi/2 (hard left). This preserves the left/right SIGN of a rear source (a
// sound behind-and-to-the-right still pans right, never nonsensically flips to
// the left) and degrades a behind source to a full side-pan — the honest best
// the front-hemisphere law can do until the HRTF dataset lands. A source at the
// listener position has a zero direction vector => atan2(0,0) = 0 => centre.
inline f32 listener_azimuth(const ListenerPose& l, math::Vec3 source) noexcept {
    const math::Vec3 dir = math::sub(source, l.position);

    // Projections onto the listener's basis. forward/right are unit vectors so
    // these are signed metres of front/right offset.
    const f32 fwd = math::dot(dir, l.forward);
    const f32 rgt = math::dot(dir, l.right);

    // atan2(right, forward): 0 dead ahead, +pi/2 hard right, -pi/2 hard left,
    // +-pi directly behind. A zero direction (source at listener) gives 0.
    f32 az = std::atan2(rgt, fwd);

    // Fold the rear hemisphere onto the near side edge (see REAR FOLDING above),
    // identical to pan_lr's own clamp so the value is in its valid domain.
    if (az < -math::kHalfPi) az = -math::kHalfPi;
    if (az >  math::kHalfPi) az =  math::kHalfPi;
    return az;
}

// ─── Doppler pitch ─────────────────────────────────────────────────────────
//
// Classic doppler playback-rate multiplier from the relative radial (closing)
// speed between source and listener.
//
// `closing_speed_mps`  — positive = source and listener APPROACHING (pitch up);
//                        negative = receding (pitch down); 0 = constant range
//                        (pitch 1.0). This is the radial component of relative
//                        velocity; the caller resolves it from velocities.
// `speed_of_sound_mps` — propagation speed of the medium (default
//                        kSpeedOfSoundMps = 343 m/s).
//
// Formula:  pitch = c / (c - closing_speed)
// With closing > 0 the denominator shrinks => pitch > 1 (approaching raises
// pitch); with closing < 0 it grows => pitch < 1 (receding lowers pitch).
//
// Guards: c <= 0 is unphysical => return 1.0 (no shift). As closing_speed
// approaches c the denominator approaches zero (the sonic-boom singularity);
// the final clamp to [kDopplerMinPitch, kDopplerMaxPitch] tames both that
// singularity and any supersonic input to a musically-sane range.
inline f32 doppler_pitch(f32 closing_speed_mps,
                         f32 speed_of_sound_mps = kSpeedOfSoundMps) noexcept {
    const f32 c = speed_of_sound_mps;
    if (c <= 0.0f) {
        return 1.0f;  // no medium => no shift
    }

    const f32 denom = c - closing_speed_mps;
    // Approaching/at the singularity (denom <= 0) means the source is meeting or
    // outrunning its own wavefront; pin to the max pitch and let the clamp own
    // the ceiling rather than dividing by zero or going negative.
    f32 pitch = denom > 0.0f ? (c / denom) : kDopplerMaxPitch;

    if (pitch < kDopplerMinPitch) pitch = kDopplerMinPitch;
    if (pitch > kDopplerMaxPitch) pitch = kDopplerMaxPitch;
    return pitch;
}

// ─── One-call convenience ──────────────────────────────────────────────────
//
// Bundles distance + attenuation + azimuth for the common case of preparing a
// source for the positional mix. (Doppler is left separate because it needs
// velocities, not just positions.) Feed `azimuth_rad` straight into pan_lr and
// multiply the per-source gain by `attenuation`.
struct SpatialResult {
    f32 distance_m;    // Euclidean listener-to-source distance, metres
    f32 attenuation;   // linear distance gain in [0,1]
    f32 azimuth_rad;   // listener-relative azimuth in [-pi/2, +pi/2]
};

inline SpatialResult spatialize(const ListenerPose& l,
                                math::Vec3          source,
                                f32                 ref_dist_m,
                                f32                 max_dist_m,
                                f32                 rolloff) noexcept {
    const f32 d = source_distance_m(l, source);
    return SpatialResult{
        d,
        distance_attenuation(d, ref_dist_m, max_dist_m, rolloff),
        listener_azimuth(l, source),
    };
}

}  // namespace psynder::audio
