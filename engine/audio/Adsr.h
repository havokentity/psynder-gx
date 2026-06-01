// SPDX-License-Identifier: MIT
//
// engine/audio/Adsr.h
//
// Lane 14 — the classic 4-stage ADSR amplitude envelope for sound events. On
// note-on the gain ramps 0 -> 1 over `attack`, decays 1 -> sustain over `decay`,
// holds at `sustain` while the note is held, then on note-off ramps from the
// sustain level down to 0 over `release`. `adsr_gain` is a pure function of the
// timeline (no per-sample state); a tiny `AdsrVoice` is the stateful convenience.
//
// Linear ramps only — pure +,-,*,/ and clamp, no transcendentals, no RNG. Audio
// is cosmetic; this is still deterministic so the same timeline yields the same
// gain. Gains are linear in [0,1]; times in seconds.

#pragma once

#include "core/Types.h"

namespace psynder::audio {

struct AdsrParams {
    f32 attack_s = 0.01f;       // 0 -> 1 ramp time
    f32 decay_s = 0.1f;         // 1 -> sustain ramp time
    f32 sustain_level = 0.7f;   // held level in [0,1]
    f32 release_s = 0.2f;       // sustain -> 0 ramp time after note-off
};

// Envelope gain in [0,1] at a point on the timeline. While NOT released: attack
// ramp (t < attack), decay ramp (attack <= t < attack+decay), else sustain. A
// zero-length attack jumps straight to 1; a zero-length decay jumps straight to
// sustain. While released: a linear ramp from sustain_level down to 0 over
// release_s (release-from-sustain model), reaching 0 at/after time_since_off ==
// release_s. Inputs are clamped (negative times -> 0); sustain_level is clamped
// to [0,1]; the result is clamped to [0,1].
f32 adsr_gain(const AdsrParams& p, f32 time_since_on_s, bool released,
              f32 time_since_off_s) noexcept;

// Stateful one-voice convenience: drive it with note_on/note_off/advance and
// read adsr_voice_gain each frame.
struct AdsrVoice {
    f32  time_on_s = 0.0f;   // seconds since note-on
    bool released = false;   // note-off received
    f32  time_off_s = 0.0f;  // seconds since note-off (valid when released)
};

void adsr_note_on(AdsrVoice& v) noexcept;
void adsr_note_off(AdsrVoice& v) noexcept;
void adsr_advance(AdsrVoice& v, f32 dt_s) noexcept;
f32  adsr_voice_gain(const AdsrVoice& v, const AdsrParams& p) noexcept;

// True once the voice has been released and has passed its release_s — the sound
// is silent and the voice can be reclaimed.
bool adsr_finished(const AdsrVoice& v, const AdsrParams& p) noexcept;

}  // namespace psynder::audio
