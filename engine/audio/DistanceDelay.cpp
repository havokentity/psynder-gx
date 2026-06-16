// SPDX-License-Identifier: MIT
//
// engine/audio/DistanceDelay.cpp — propagation delay + air absorption. See
// DistanceDelay.h.

#include "audio/DistanceDelay.h"

#include <algorithm>  // std::clamp
#include <cmath>      // std::floor

namespace psynder::audio {

f32 propagation_delay_s(f32 distance_m, f32 speed_of_sound_mps) noexcept {
    if (speed_of_sound_mps <= 0.0f) return 0.0f;
    const f32 d = distance_m > 0.0f ? distance_m : 0.0f;
    return d / speed_of_sound_mps;
}

usize delay_samples(f32 distance_m, u32 sample_rate_hz,
                    f32 speed_of_sound_mps) noexcept {
    const f32 secs = propagation_delay_s(distance_m, speed_of_sound_mps);
    const f32 samples = secs * static_cast<f32>(sample_rate_hz);
    if (samples <= 0.0f) return 0;
    // Round half away from zero (samples is non-negative here).
    return static_cast<usize>(std::floor(samples + 0.5f));
}

f32 air_absorption_gain(f32 distance_m, f32 absorption_per_m) noexcept {
    const f32 d = distance_m > 0.0f ? distance_m : 0.0f;
    const f32 a = absorption_per_m > 0.0f ? absorption_per_m : 0.0f;
    const f32 gain = 1.0f / (1.0f + a * d);
    return std::clamp(gain, 0.0f, 1.0f);
}

ArrivalCue distant_arrival(f32 distance_m, u32 sample_rate_hz,
                           f32 absorption_per_m,
                           f32 speed_of_sound_mps) noexcept {
    ArrivalCue cue{};
    cue.delay_s = propagation_delay_s(distance_m, speed_of_sound_mps);
    cue.delay_samples = delay_samples(distance_m, sample_rate_hz, speed_of_sound_mps);
    cue.absorption_gain = air_absorption_gain(distance_m, absorption_per_m);
    return cue;
}

}  // namespace psynder::audio
