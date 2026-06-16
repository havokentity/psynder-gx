// SPDX-License-Identifier: MIT
//
// engine/audio/Echo.h
//
// Lane 14 — feedback echo (delay line) parameter model. An "echo" is a single
// delay tapped repeatedly: the dry sound, then a quieter copy one delay later,
// then a quieter-still copy after another delay, and so on — each successive
// tap multiplied by the feedback gain (a geometric decay) until it falls below
// audibility. Sound designers set a delay time and a feedback amount; this
// header turns that into the discrete tap schedule the mixer plays: how many
// samples each tap is delayed, the gain of each tap, when each tap arrives, and
// how many taps stay audible. Canyon / cave slap-back echoes and weapon tails.
//
// SCOPE: pure scalar math — no delay-line buffer, no device I/O, no event
// queue. The mixer consumes the tap schedule. Cosmetic (not the authoritative
// lockstep tick), so f32 + pow/log is fine; allocation-free, noexcept,
// same-platform deterministic (pure algebra + pow/log, no RNG / wall-clock).
//
// TAP INDEXING CONVENTION: tap 0 is the DRY (direct) signal — gain 1.0, time
// offset 0. Tap 1 is the FIRST echo (delayed by one delay_time, gain feedback),
// tap 2 is the echo of the echo (gain feedback^2), etc. So tap n has gain
// feedback^n and arrives n * delay_time_s after the dry hit.
//
// Units: delay_time_s is seconds, sample_rate_hz is Hz, gains are linear, the
// audible_floor is a linear gain (e.g. 0.01 ~ -40 dB). 1 world unit = 1 metre.

#pragma once

#include "core/Types.h"

namespace psynder::audio {

// Maximum feedback gain. A feedback >= 1 would never decay (an infinite, even
// growing, echo train), so we clamp just under 1 to guarantee the tail always
// dies out. 0.999 keeps a very long but finite tail.
inline constexpr f32 kEchoMaxFeedback = 0.999f;

// Hard cap on the number of audible taps echo_audible_taps will report, so a
// near-unity feedback with a tiny floor can't ask the mixer for an unbounded
// tap count. 64 taps is already a very long slap-back train.
inline constexpr u32 kEchoMaxTaps = 64u;

// The per-tap delay length in mixer samples: round(delay_time_s * sample_rate).
// Rounds half away from zero via floor(x + 0.5). A non-positive delay time is
// clamped to 0 (no delay). Never negative.
usize echo_delay_samples(f32 delay_time_s, u32 sample_rate_hz) noexcept;

// The linear gain of tap `tap_index` under the indexing convention above:
// feedback^tap_index (geometric decay). Tap 0 (the dry signal) always returns
// exactly 1.0. `feedback` is clamped to [0, kEchoMaxFeedback] first, so a
// feedback >= 1 still produces a decaying train rather than a sustaining one.
f32 echo_tap_gain(f32 feedback, u32 tap_index) noexcept;

// How many taps stay at or above `audible_floor` (a linear gain) — the largest
// n for which feedback^n >= audible_floor. Counts only the echoes that the
// mixer must actually render. Returns 0 for feedback <= 0 (the dry tap aside,
// no echo survives) and for a non-positive floor it returns the cap. Capped at
// kEchoMaxTaps. The result is consistent with echo_tap_gain: tap `n` is audible
// (gain >= floor) for n < the returned count.
u32 echo_audible_taps(f32 feedback, f32 audible_floor) noexcept;

// One echo's authoring parameters.
//   delay_time_s — the delay length between successive taps, seconds (>= 0).
//   feedback     — the per-tap decay gain, linear (clamped to
//                  [0, kEchoMaxFeedback] where it is consumed).
//   wet          — the echo send / wet-mix level, linear [0,1].
struct EchoParams {
    f32 delay_time_s;
    f32 feedback;
    f32 wet;
};

// The time offset (seconds) at which tap `tap_index` arrives:
// tap_index * delay_time_s. Tap 0 (dry) arrives at 0; tap 1 one delay later;
// tap n after n delays. A negative delay time is treated as 0.
f32 echo_tap_time_s(const EchoParams& p, u32 tap_index) noexcept;

}  // namespace psynder::audio
