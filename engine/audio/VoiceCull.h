// SPDX-License-Identifier: MIT
//
// engine/audio/VoiceCull.h
//
// Lane 14 — deterministic voice culling (finite-voice-budget selection).
//
// Hardware and the software mixer expose a finite number of voices: when more
// sound emitters want to play than there are voices, something must be dropped.
// This header decides WHICH ones, deterministically: each candidate emitter is
// scored by its authored priority times a distance-attenuation (how audible it
// is at the listener), and the top-K by that score are kept; the rest are
// culled. An inaudible emitter (at/beyond the max audible radius) is never kept
// even when there is spare budget — there is no point spending a voice on
// silence.
//
// SCOPE: pure selection math only — no device I/O, no clock, no RNG. Like its
// neighbours (PositionalMix.h, SpatialCue.h) this is COSMETIC mix-side math,
// NOT the authoritative lockstep tick, so floats are fine; the determinism
// guarantee is same-platform (no wall-clock, no RNG, stable evaluation order).
// `voice_score` / `is_audible` are header-inline POD-in/POD-out and `noexcept`;
// `select_voices` is `noexcept` and allocation-light (it reuses one amortised
// thread_local scratch buffer and the caller-owned `out_kept`, no exotic
// per-call allocation beyond those reused vectors).
//
// Determinism is a hard pillar: scoring is pure algebra and all ordering ties
// break to the LOWER candidate id, so the same candidate set always yields the
// same kept set in the same order.
//
// The distance-attenuation form is the same inverse-distance rolloff
// SpatialCue::distance_attenuation uses, reimplemented inline here so this
// header stays self-contained (no cross-include of SpatialCue.h).
//
// Units (matching PositionalMix.h / SpatialCue.h): distances are metres
// (1 world unit = 1 m); priority and the resulting score are unitless and
// linear (higher = more important / more audible).

#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::audio {

// ─── A candidate emitter competing for a voice ─────────────────────────────
//
// POD. `priority` is the authored importance (higher = more important, e.g. a
// gunshot above ambient wind); `distance_m` is the listener-to-emitter
// distance in metres (nearer = louder). `id` is a caller-stable identifier
// used both as the kept-set payload and as the deterministic tie-break key.
struct VoiceCandidate {
    u32 id;
    f32 priority;
    f32 distance_m;
};

// ─── Audibility quick-reject ───────────────────────────────────────────────
//
// True iff the emitter is strictly nearer than the max audible radius. At or
// beyond `max_dist_m` it is treated as silent (distance_attenuation returns 0
// there), so such a candidate can be rejected without scoring.
inline bool is_audible(const VoiceCandidate& v, f32 max_dist_m) noexcept {
    return v.distance_m < max_dist_m;
}

// ─── Audibility score ──────────────────────────────────────────────────────
//
// score = priority * distance_attenuation(distance, ref, max, rolloff).
//
// Higher = keep. The attenuation factor is the same inverse-distance rolloff as
// SpatialCue::distance_attenuation (reimplemented here to keep this header
// self-contained):
//   * d <= ref            => 1.0 (full gain inside the reference radius);
//   * ref < d < max       => ref / (ref + rolloff * (d - ref)), in (0,1);
//   * d >= max            => 0.0 (inaudible).
// Degenerate guards mirror SpatialCue: negative distance clamps to 0, negative
// rolloff clamps to 0, and a non-positive denominator falls back to 1.0.
//
// A candidate at or beyond `max_dist_m` therefore scores 0 regardless of
// priority (inaudible). Note a non-positive priority also yields a non-positive
// score and so is never kept by select_voices (see below).
inline f32 voice_score(const VoiceCandidate& v,
                       f32 ref_dist_m,
                       f32 max_dist_m,
                       f32 rolloff) noexcept {
    f32 d = v.distance_m;
    if (d < 0.0f) d = 0.0f;  // distance can never be negative

    // At/beyond the max radius the emitter is silent => zero score.
    if (d >= max_dist_m) {
        return 0.0f;
    }

    // Inside the reference radius: full attenuation (unity).
    f32 atten;
    if (d <= ref_dist_m) {
        atten = 1.0f;
    } else {
        f32 k = rolloff;
        if (k < 0.0f) k = 0.0f;
        const f32 denom = ref_dist_m + k * (d - ref_dist_m);
        atten = denom > 0.0f ? (ref_dist_m / denom) : 1.0f;
        if (atten < 0.0f) atten = 0.0f;
        if (atten > 1.0f) atten = 1.0f;
    }

    return v.priority * atten;
}

// ─── Top-K voice selection ─────────────────────────────────────────────────
//
// Score every candidate with voice_score() and keep the `max_voices` most
// audible ones, deterministically.
//
// Contract:
//   * `out_kept` is cleared, then filled with the kept candidate ids in
//     DESCENDING score order. Ties (equal score) break to the LOWER id, both
//     for WHICH candidates are kept and for their order in `out_kept` (so a
//     score tie lists the smaller id first).
//   * If candidates.size() <= max_voices, all AUDIBLE candidates are kept
//     (still ordered by descending score) — see the audibility rule next.
//   * A candidate whose score is <= 0 (inaudible: at/beyond max_dist_m, or a
//     non-positive priority) is NEVER kept, even when there is spare budget:
//     a voice spent on silence is wasted. So `out_kept` can be shorter than
//     both `max_voices` and `candidates.size()`.
//   * `max_voices == 0` or an empty input => empty `out_kept`.
//
// Determinism: pure algebra plus a stable lowest-id tie-break, so the same
// candidate set always produces the same kept ids in the same order.
//
// Allocation: reuses one amortised thread_local scratch buffer plus the
// caller-owned `out_kept`; no exotic per-call allocation. `noexcept` — on the
// (pathological) event a scratch growth would throw bad_alloc it terminates,
// matching the lane's noexcept mixer-path contract.
void select_voices(std::span<const VoiceCandidate> candidates,
                   usize max_voices,
                   f32 ref_dist_m,
                   f32 max_dist_m,
                   f32 rolloff,
                   std::vector<u32>& out_kept) noexcept;

}  // namespace psynder::audio
