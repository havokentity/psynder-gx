// SPDX-License-Identifier: MIT
//
// engine/audio/Attenuation.h
//
// Lane 14 — selectable distance-attenuation rolloff models.
//
// SpatialCue.h ships ONE fixed inverse-distance `distance_attenuation`; this
// header complements it with the standard SELECTABLE family of rolloff curves
// (linear, inverse, inverse-square, exponential) so a sound designer can pick
// the falloff per emitter. Each model maps a listener-to-source distance plus a
// (ref, max) range to a linear gain in [0,1], and there is a decibel companion.
//
// SCOPE: pure scalar curve math only — no device I/O, no state, no allocation.
// The two functions are `noexcept`, POD in / POD out, and safe to call from the
// mixer pull path next to PositionalMix / SpatialCue. This is COSMETIC mix math
// (NOT the authoritative lockstep tick), so transcendentals (pow/log) are fine;
// determinism is the lane's same-platform guarantee (no RNG, no wall-clock,
// stable evaluation order), matching the rest of engine/audio.
//
// Units (matching SpatialCue.h / PositionalMix.h): distances are metres
// (1 world unit = 1 m); gains are linear [0,1]; dB is dBFS-style 20*log10(gain)
// (<= 0, 0 dB == unity gain).
//
// SHARED CONTRACT for every model (the `attenuation` dispatcher and each curve):
//   * distance <= ref_dist          => 1.0 (full gain inside the reference
//                                      radius; distance < 0 is clamped to 0).
//   * distance >= max_dist          => 0.0 (silent at/beyond the cutoff).
//   * ref < distance < max          => the model's curve, clamped to [0,1] and
//                                      monotonically NON-INCREASING in distance.
//   * degenerate range (max <= ref) => clean documented fallback: a hard step,
//                                      1.0 at/under ref else 0.0 (the [ref,max]
//                                      band is empty so there is no curve to
//                                      evaluate).
//   * rolloff_factor                => steepness knob (>= 0; negative clamps to
//                                      0). Its exact role is per-model, below.

#pragma once

#include "core/Types.h"

namespace psynder::audio {

// ─── Rolloff model selector ────────────────────────────────────────────────
//
// Picked per emitter by the sound designer. Fixed underlying type so it is a
// stable serialisable tag.
enum class RolloffModel : u32 {
    Linear        = 0,  // straight-line gain across [ref, max]
    Inverse       = 1,  // ref / (ref + f*(d-ref)) — textbook inverse distance
    InverseSquare = 2,  // ref^2 / (ref^2 + f*(d-ref)^2) — steeper, physical-ish
    Exponential   = 3,  // exp(-f * (d-ref)/(max-ref)) — exponential decay
};

// Decibel floor returned by attenuation_db() for a zero (or sub-epsilon) linear
// gain, standing in for -infinity so the result is always finite and orderable.
// -120 dBFS is ~1e-6 linear: inaudibly silent yet a clean, comparable number.
inline constexpr f32 kAttenuationDbFloor = -120.0f;

// ─── Linear gain ───────────────────────────────────────────────────────────
//
// Compute the per-emitter linear distance gain in [0,1] for the chosen model.
//
// `model`          — which rolloff curve to apply (see RolloffModel).
// `distance_m`     — listener-to-source distance in metres (negative clamps 0).
// `ref_dist_m`     — radius of full gain; at/under it the gain is exactly 1.0.
// `max_dist_m`     — radius at/beyond which the gain is exactly 0.0.
// `rolloff_factor` — falloff steepness (>= 0; negative clamps to 0). Per model:
//                    * Linear:        unused (the straight line is fixed by the
//                                     [ref,max] endpoints); accepted for a
//                                     uniform signature.
//                    * Inverse:       1 == textbook inverse distance; larger ==
//                                     faster falloff (the gain at the midpoint
//                                     drops as the factor grows).
//                    * InverseSquare: same knob, squared distance term.
//                    * Exponential:   decay rate; larger == faster decay. With
//                                     the default 1.0 the gain at max is exp(-1)
//                                     (~0.368) before the hard cutoff snaps it
//                                     to 0; larger factors approach 0 sooner.
//
// Exact formulas (let x = (d - ref) and span = (max - ref), both > 0 here):
//   Linear:        g = 1 - x / span                       (== 0.5 at the midpoint)
//   Inverse:       g = ref / (ref + f * x)
//   InverseSquare: g = ref^2 / (ref^2 + f * x^2)
//   Exponential:   g = exp(-f * x / span)
// All four are clamped to [0,1]; for ref < d < max they are non-increasing in d.
// The hard `d >= max => 0` cutoff is applied to every model so a distant source
// is genuinely silent (Inverse/InverseSquare/Exponential are merely small, not
// exactly zero, at max — the cutoff guarantees true silence).
//
// Degenerate range (max <= ref): the [ref,max] band is empty, so we fall back to
// a hard step — 1.0 at/under ref, 0.0 beyond. (With max <= ref a distance past
// ref is also >= max, so the standard `d >= max => 0` already yields this.)
f32 attenuation(RolloffModel model,
                f32          distance_m,
                f32          ref_dist_m,
                f32          max_dist_m,
                f32          rolloff_factor) noexcept;

// ─── Decibel gain ──────────────────────────────────────────────────────────
//
// The same attenuation expressed as a dBFS-style level: 20*log10(gain). Unity
// gain (d <= ref) is 0 dB; the level falls negative with distance. Because a
// true zero gain is -infinity in dB, a zero/sub-epsilon linear gain is floored
// to kAttenuationDbFloor (-120 dB) so the result is always finite and orderable.
// All other inputs follow attenuation() exactly (call it, then convert).
f32 attenuation_db(RolloffModel model,
                   f32          distance_m,
                   f32          ref_dist_m,
                   f32          max_dist_m,
                   f32          rolloff_factor) noexcept;

}  // namespace psynder::audio
