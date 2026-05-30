// SPDX-License-Identifier: MIT
//
// engine/audio/DistanceDelay.h
//
// Lane 14 — speed-of-sound propagation delay + air absorption for distant
// sounds. A faraway event (a distant gunshot, an explosion across the map)
// arrives LATER than you see it — sound travels ~343 m/s, so a 343 m shot is
// heard ~1 s after the muzzle flash — and arrives DULLER and quieter as the air
// absorbs it over distance. This header turns a source distance into (a) the
// playback delay (in seconds and in mixer samples) the scheduler should defer
// the sound by, and (b) an air-absorption linear gain.
//
// SCOPE: pure scalar math — no device I/O, no event queue. The scheduler /
// mixer consumes these numbers. Cosmetic (not the authoritative lockstep tick),
// allocation-free, noexcept, deterministic (the integer sample rounding is
// stable). Units: distance metres, delay seconds, gain linear [0,1], 1 unit = 1 m.

#pragma once

#include "core/Types.h"

namespace psynder::audio {

// Dry-air speed of sound at ~20 C, metres/second.
inline constexpr f32 kSpeedOfSoundMps = 343.0f;

// Propagation delay (seconds) for a sound originating `distance_m` away:
// distance / speed_of_sound. A non-positive speed_of_sound returns 0 (guard);
// a negative distance is treated as 0 (at the listener).
f32 propagation_delay_s(f32 distance_m,
                        f32 speed_of_sound_mps = kSpeedOfSoundMps) noexcept;

// The integer mixer-sample offset to schedule the sound at:
// round(propagation_delay_s * sample_rate_hz). Rounds half away from zero via
// floor(x + 0.5). Never negative.
usize delay_samples(f32 distance_m, u32 sample_rate_hz,
                    f32 speed_of_sound_mps = kSpeedOfSoundMps) noexcept;

// Air-absorption linear gain in (0, 1]: 1 / (1 + absorption_per_m * distance).
// Unity at the listener (distance 0), monotonically decreasing with distance,
// clamped to [0, 1]. A negative absorption_per_m or distance is clamped to 0
// (no absorption / at the listener), so the result never exceeds 1 or goes
// negative.
f32 air_absorption_gain(f32 distance_m, f32 absorption_per_m) noexcept;

// Bundle of the three arrival cues for one distant source.
struct ArrivalCue {
    f32   delay_s;          // propagation delay (seconds)
    usize delay_samples;    // the same delay in mixer samples
    f32   absorption_gain;  // air-absorption linear gain [0,1]
};

// Compute all three cues at once for a source `distance_m` away.
ArrivalCue distant_arrival(f32 distance_m, u32 sample_rate_hz,
                           f32 absorption_per_m,
                           f32 speed_of_sound_mps = kSpeedOfSoundMps) noexcept;

}  // namespace psynder::audio
