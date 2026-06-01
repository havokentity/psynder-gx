// SPDX-License-Identifier: MIT
//
// engine/audio/Adsr.cpp — see Adsr.h.

#include "audio/Adsr.h"

#include <algorithm>  // std::clamp

namespace psynder::audio {

f32 adsr_gain(const AdsrParams& p, f32 time_since_on_s, bool released,
              f32 time_since_off_s) noexcept {
    const f32 sustain = std::clamp(p.sustain_level, 0.0f, 1.0f);

    f32 g;
    if (!released) {
        const f32 t = time_since_on_s > 0.0f ? time_since_on_s : 0.0f;
        if (t < p.attack_s) {
            // Attack ramp 0 -> 1. (t < attack_s implies attack_s > 0.)
            g = t / p.attack_s;
        } else {
            const f32 td = t - p.attack_s;  // time into the decay/sustain region
            if (td < p.decay_s) {
                // Decay ramp 1 -> sustain. (td < decay_s implies decay_s > 0.)
                g = 1.0f - (1.0f - sustain) * (td / p.decay_s);
            } else {
                g = sustain;  // held
            }
        }
    } else {
        // Release ramp from the sustain level down to 0.
        if (p.release_s <= 0.0f) {
            g = 0.0f;
        } else {
            const f32 to = time_since_off_s > 0.0f ? time_since_off_s : 0.0f;
            g = (to >= p.release_s) ? 0.0f : sustain * (1.0f - to / p.release_s);
        }
    }

    return std::clamp(g, 0.0f, 1.0f);
}

void adsr_note_on(AdsrVoice& v) noexcept {
    v.time_on_s = 0.0f;
    v.released = false;
    v.time_off_s = 0.0f;
}

void adsr_note_off(AdsrVoice& v) noexcept {
    if (!v.released) {
        v.released = true;
        v.time_off_s = 0.0f;
    }
}

void adsr_advance(AdsrVoice& v, f32 dt_s) noexcept {
    if (dt_s <= 0.0f) return;
    v.time_on_s += dt_s;
    if (v.released) v.time_off_s += dt_s;
}

f32 adsr_voice_gain(const AdsrVoice& v, const AdsrParams& p) noexcept {
    return adsr_gain(p, v.time_on_s, v.released, v.time_off_s);
}

bool adsr_finished(const AdsrVoice& v, const AdsrParams& p) noexcept {
    return v.released && v.time_off_s >= p.release_s;
}

}  // namespace psynder::audio
