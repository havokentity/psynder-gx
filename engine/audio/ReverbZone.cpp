// SPDX-License-Identifier: MIT
//
// engine/audio/ReverbZone.cpp — acoustic-space reverb parameter model. See
// ReverbZone.h.

#include "audio/ReverbZone.h"

#include <algorithm>  // std::clamp

namespace psynder::audio {

ReverbZoneParams reverb_from_size(f32 room_volume_m3, f32 max_wet,
                                  f32 max_decay_s) noexcept {
    // Negative volume is meaningless; treat it as an empty (dry) space.
    const f32 vol = room_volume_m3 > 0.0f ? room_volume_m3 : 0.0f;

    // Saturating size parameter in [0,1): grows with volume, asymptotes to 1.
    // The denominator (vol + reference) is always > 0, so this never divides by
    // zero and never exceeds 1.
    const f32 t = vol / (vol + kReverbReferenceVolumeM3);

    ReverbZoneParams p{};
    p.wet     = std::clamp(max_wet * t, 0.0f, 1.0f);
    p.decay_s = max_decay_s * t;
    if (p.decay_s < 0.0f) p.decay_s = 0.0f;  // decay is a non-negative length
    return p;
}

f32 reverb_blend(f32 a, f32 b, f32 t) noexcept {
    const f32 u = std::clamp(t, 0.0f, 1.0f);
    return a + (b - a) * u;
}

ReverbZoneParams reverb_lerp(const ReverbZoneParams& a,
                             const ReverbZoneParams& b, f32 t) noexcept {
    ReverbZoneParams p{};
    p.wet     = reverb_blend(a.wet,     b.wet,     t);
    p.decay_s = reverb_blend(a.decay_s, b.decay_s, t);
    return p;
}

f32 reverb_dry_gain(const ReverbZoneParams& p) noexcept {
    return std::clamp(1.0f - p.wet, 0.0f, 1.0f);
}

}  // namespace psynder::audio
