// SPDX-License-Identifier: MIT
//
// engine/audio/Echo.cpp — feedback echo (delay line) parameter model. See
// Echo.h.

#include "audio/Echo.h"

#include <algorithm>  // std::clamp
#include <cmath>      // std::floor, std::pow, std::log

namespace psynder::audio {

usize echo_delay_samples(f32 delay_time_s, u32 sample_rate_hz) noexcept {
    if (delay_time_s <= 0.0f) return 0;
    const f32 samples = delay_time_s * static_cast<f32>(sample_rate_hz);
    if (samples <= 0.0f) return 0;
    // Round half away from zero (samples is non-negative here).
    return static_cast<usize>(std::floor(samples + 0.5f));
}

f32 echo_tap_gain(f32 feedback, u32 tap_index) noexcept {
    // Tap 0 is the dry signal — always full gain regardless of feedback.
    if (tap_index == 0u) return 1.0f;
    // Clamp feedback so the train always decays (feedback >= 1 would not).
    const f32 fb = std::clamp(feedback, 0.0f, kEchoMaxFeedback);
    return std::pow(fb, static_cast<f32>(tap_index));
}

u32 echo_audible_taps(f32 feedback, f32 audible_floor) noexcept {
    // No feedback => no echo tap survives above any positive floor.
    if (feedback <= 0.0f) return 0u;
    // A non-positive floor admits every tap; report the cap.
    if (audible_floor <= 0.0f) return kEchoMaxTaps;

    const f32 fb = std::clamp(feedback, 0.0f, kEchoMaxFeedback);

    // Count taps whose gain (fb^n) stays at or above the floor. Solving
    // fb^n >= floor gives n <= log(floor)/log(fb) (log(fb) < 0 flips the
    // inequality), so the count is floor(log(floor)/log(fb)) + 1. We use a
    // bounded loop instead for clarity and to share exactly the same gain
    // expression as echo_tap_gain (keeping the two functions consistent).
    u32 count = 0u;
    for (u32 n = 1u; n <= kEchoMaxTaps; ++n) {
        if (std::pow(fb, static_cast<f32>(n)) < audible_floor) break;
        ++count;
    }
    return count;
}

f32 echo_tap_time_s(const EchoParams& p, u32 tap_index) noexcept {
    const f32 dt = p.delay_time_s > 0.0f ? p.delay_time_s : 0.0f;
    return static_cast<f32>(tap_index) * dt;
}

}  // namespace psynder::audio
