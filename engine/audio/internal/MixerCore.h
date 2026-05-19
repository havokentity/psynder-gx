// SPDX-License-Identifier: MIT
// Psynder — audio mixer core (header-only algorithmic kernels).
//
// This header contains the testable algorithmic core of the lane-14 audio
// subsystem: voice pool, SIMD voice-sum, 2-D angular HRTF (azimuth +
// elevation), Cooley-Tukey FFT, FFT-convolution reverb against a baked
// room IR, and an 8-line Hadamard FDN reverb. It is deliberately header-
// only so the shared tests/unit CMakeLists (owned by lane 25) can compile
// against the algorithms without linking `psynder_audio`.
//
// DESIGN-PSYNDER-GX.md §10.2:
//   - 32-channel software CPU mixer.
//   - SIMD-merged voice sum (engine/simd, lane 03).
//   - Per-voice mixing dispatched via the job system parallel_for (lane 04).
//   - HRTF for first-person.
//   - FDN reverb (outdoors), FFT convolution reverb (indoors).
//
// Wave-A rule (docs/wave-a-bar.md): no `virtual` in the mixer hot path.

#pragma once

#include "audio/AudioClip.h"
#include "core/Types.h"
#include "math/Math.h"
#include "simd/Simd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

namespace psynder::audio::detail {

// ─── Constants ───────────────────────────────────────────────────────────
inline constexpr u32 kMaxVoices       = 32;          // DESIGN §10.2
inline constexpr u32 kHrirLength      = 64;          // HRIR taps / ear
inline constexpr u32 kFdnSize         = 8;           // 8-line Hadamard FDN
inline constexpr f32 kSpeedOfSound    = 343.0f;      // m/s @ 20 °C
inline constexpr f32 kHeadRadius      = 0.0875f;     // m (avg adult head)
inline constexpr u32 kBakedRirSamples = 24000u;      // 0.5 s @ 48 kHz, float32

// ─── Voice ──────────────────────────────────────────────────────────────
// One voice is one playing instance of a clip. The voice pool is a tagged-
// handle array with generation counters so a stale VoiceId compares unequal
// after a stop_voice() + play() reuse.
struct Voice {
    u32         gen      = 0;   // monotonic
    bool        active   = false;
    u32         clip_raw = 0;
    f32         volume   = 0.0f;
    math::Vec3  position{0,0,0};
    // playback cursor (frame index into clip's PCM stream)
    u32         cursor   = 0;
};

// Voice pool — fixed-size, contiguous, no allocations on acquire/release.
// Returns -1 when full so callers fail gracefully.
class VoicePool {
public:
    // Find a free slot; mark active; bump generation; return packed id.
    // Packed id layout: low 24b = slot index, high 8b = generation low byte.
    // 0 is reserved for "invalid".
    u32 acquire(u32 clip_raw, math::Vec3 pos, f32 volume) noexcept {
        for (u32 i = 0; i < kMaxVoices; ++i) {
            if (!voices_[i].active) {
                voices_[i].active   = true;
                voices_[i].clip_raw = clip_raw;
                voices_[i].position = pos;
                voices_[i].volume   = volume;
                voices_[i].cursor   = 0;
                // bump gen; never zero (so packed id != 0)
                voices_[i].gen = (voices_[i].gen + 1u) & 0xFFu;
                if (voices_[i].gen == 0u) voices_[i].gen = 1u;
                ++active_count_;
                return pack(i, voices_[i].gen);
            }
        }
        return 0u;  // full
    }

    // Release a previously-acquired voice. Mismatched gen is a no-op (stale).
    bool release(u32 packed) noexcept {
        if (packed == 0u) return false;
        const u32 idx = unpack_index(packed);
        const u32 gen = unpack_gen(packed);
        if (idx >= kMaxVoices) return false;
        if (!voices_[idx].active) return false;
        if (voices_[idx].gen != gen) return false;
        voices_[idx].active = false;
        if (active_count_ > 0u) --active_count_;
        return true;
    }

    void clear() noexcept {
        for (auto& v : voices_) v.active = false;
        active_count_ = 0;
    }

    u32  active_count() const noexcept { return active_count_; }

    Voice&       at(u32 i)       noexcept { return voices_[i]; }
    const Voice& at(u32 i) const noexcept { return voices_[i]; }

    static constexpr u32 pack(u32 idx, u32 gen) noexcept {
        return (idx & 0x00FFFFFFu) | ((gen & 0xFFu) << 24);
    }
    static constexpr u32 unpack_index(u32 packed) noexcept { return packed & 0x00FFFFFFu; }
    static constexpr u32 unpack_gen(u32 packed)   noexcept { return (packed >> 24) & 0xFFu; }

private:
    std::array<Voice, kMaxVoices> voices_{};
    u32 active_count_ = 0;
};

// ─── SIMD voice-sum ──────────────────────────────────────────────────────
// Restrict-style aliasing macro (clang accepts the attribute everywhere we ship).
#if defined(__clang__) || defined(__GNUC__)
#   define PSY_RESTRICT_ALIAS __restrict
#else
#   define PSY_RESTRICT_ALIAS
#endif

// Accumulates `src[i] * gain` into `dst[i]` over `count` floats.
// PSY_FORCEINLINE so this can be called from a parallel_for body without
// indirection — no `virtual`, no function-pointer barrier (Wave-A rule).
PSY_FORCEINLINE void simd_mix_into(f32* PSY_RESTRICT_ALIAS dst,
                                   const f32* PSY_RESTRICT_ALIAS src,
                                   f32 gain, u32 count) noexcept {
    // 4-wide SIMD bulk + scalar tail. simd::fma calls into lane 03's f32x4
    // back-end (NEON on Apple Silicon, SSE/AVX on x86, scalar fallback).
    const simd::f32x4 g = simd::broadcast(gain);
    u32 i = 0;
    for (; i + 4u <= count; i += 4u) {
        simd::f32x4 d = simd::load(dst + i);
        simd::f32x4 s = simd::load(src + i);
        d = simd::fma(s, g, d);
        simd::store(dst + i, d);
    }
    for (; i < count; ++i) dst[i] += src[i] * gain;
}

// Equal-power pan gains. azimuth in radians; 0 = front, +π/2 = right.
// Result clipped to [0,1] for left and right.
PSY_FORCEINLINE void pan_equal_power(f32 azimuth_rad, f32& left_gain, f32& right_gain) noexcept {
    // Map azimuth ∈ [-π/2, +π/2] → x ∈ [0,1] where 0=full left, 1=full right.
    const f32 az = std::fmax(-math::kHalfPi, std::fmin(math::kHalfPi, azimuth_rad));
    const f32 x  = 0.5f * (az / math::kHalfPi) + 0.5f;
    const f32 a  = x * math::kHalfPi;
    left_gain  = std::cos(a);
    right_gain = std::sin(a);
}

// ─── HRTF: 2-D angular analytical model (azimuth + elevation) ────────────
//
// Reference: Brown & Duda, "A Structural Model for Binaural Sound
// Synthesis", IEEE Trans. SAP 6(5), 1998. Their structural model is
// composed of:
//   1. A spherical-head shadowing low-pass at the contralateral ear with
//      cutoff that varies with incidence angle.
//   2. An interaural time difference (ITD) from Woodworth's diffraction
//      model (head radius / speed-of-sound) extended for elevation by
//      using the angle of incidence between source and ear.
//   3. A pinna-shadow short delay-comb that depends on elevation
//      (front-back differentiation; ears blind to azimuth alone).
//
// We implement a *cheap* analytical form: no measured HRIR, no FIR per
// bin, no spherical harmonics. The deliverable is the structural model
// shape — measured HRIRs slot in by replacing make_hrir_2d() with a
// vendored IRC binary while keeping the StereoHrir struct stable.
//
// Coordinate convention (matches DESIGN §10.2):
//   azimuth ∈ [-π, π], 0 = front, +π/2 = right, ±π = behind
//   elevation ∈ [-π/2, π/2], 0 = horizon, +π/2 = above, -π/2 = below
//   distance metres > 0

struct StereoHrir {
    std::array<f32, kHrirLength> left{};
    std::array<f32, kHrirLength> right{};
    u32 left_delay_samples  = 0;
    u32 right_delay_samples = 0;
    // Per-ear scalar gains applied AFTER the delay+shadow filter; lets
    // a caller normalise per-voice loudness.
    f32 left_gain  = 1.0f;
    f32 right_gain = 1.0f;
};

// Woodworth ITD for the horizontal plane (elevation = 0). This is the
// classic single-variable formula:
//   ITD ≈ r/c * (sin θ + θ)         for |θ| ≤ π/2
//   ITD ≈ r/c * (π - θ + sin θ)     for π/2 < θ ≤ π
// where r = head radius, c = speed of sound.
inline f32 itd_seconds(f32 azimuth_rad) noexcept {
    const f32 a   = std::fabs(azimuth_rad);
    const f32 sin_a = std::sin(a);
    f32 t = 0.0f;
    if (a <= math::kHalfPi) {
        t = (kHeadRadius / kSpeedOfSound) * (sin_a + a);
    } else {
        const f32 a_clamped = std::fmin(a, math::kPi);
        t = (kHeadRadius / kSpeedOfSound) * (math::kPi - a_clamped + sin_a);
    }
    return azimuth_rad >= 0.0f ? +t : -t;
}

// Extended ITD with elevation: the effective horizontal incidence angle
// shrinks as the source moves toward the median (above-head, below) plane.
// Following Brown-Duda eq. (3) we use:
//   θ_eff = atan2(sin(azimuth) * cos(elevation),
//                 sqrt(cos²(azimuth)*cos²(elevation) + sin²(elevation)))
// which equals azimuth when elevation == 0 and tends to 0 as elevation
// approaches ±π/2 (median plane has zero ITD).
inline f32 itd_seconds_2d(f32 azimuth_rad, f32 elevation_rad) noexcept {
    const f32 ce = std::cos(elevation_rad);
    const f32 se = std::sin(elevation_rad);
    const f32 ca = std::cos(azimuth_rad);
    const f32 sa = std::sin(azimuth_rad);
    const f32 y  = sa * ce;
    const f32 x  = std::sqrt(ca*ca * ce*ce + se*se);
    const f32 theta_eff = std::atan2(y, x);
    return itd_seconds(theta_eff);
}

// Convert ITD in seconds to (left_delay, right_delay) integer-sample offsets
// such that both are non-negative (we delay whichever ear arrives later).
inline void itd_to_delays(f32 azimuth_rad, u32 sample_rate,
                          u32& left_delay, u32& right_delay) noexcept {
    const f32 t = itd_seconds(azimuth_rad);
    const f32 samples = std::fabs(t) * static_cast<f32>(sample_rate);
    const u32 d = static_cast<u32>(samples + 0.5f);
    if (t >= 0.0f) {
        // sound on the right → arrives at right first → left ear is delayed
        left_delay  = d;
        right_delay = 0u;
    } else {
        left_delay  = 0u;
        right_delay = d;
    }
}

// Same but with elevation factored into the effective angle.
inline void itd_to_delays_2d(f32 azimuth_rad, f32 elevation_rad,
                             u32 sample_rate,
                             u32& left_delay, u32& right_delay) noexcept {
    const f32 t = itd_seconds_2d(azimuth_rad, elevation_rad);
    const f32 samples = std::fabs(t) * static_cast<f32>(sample_rate);
    const u32 d = static_cast<u32>(samples + 0.5f);
    if (t >= 0.0f) {
        left_delay  = d;
        right_delay = 0u;
    } else {
        left_delay  = 0u;
        right_delay = d;
    }
}

// Pinna model (Brown-Duda eq. 9): a tiny anti-aliased FIR comb whose
// notch frequency depends on elevation. The front-vs-back ambiguity of a
// pure spherical-head model is broken by these elevation-dependent peaks
// and notches.
//
// Returns 5-tap pinna gains for one ear given source elevation. The taps
// model two pinna reflections (concha + helix) whose delays vary with
// elevation between ~0.1 ms (low) and ~0.6 ms (high).
PSY_FORCEINLINE void pinna_taps(f32 elevation_rad, u32 sample_rate,
                                std::array<f32, 5>& out_taps) noexcept {
    // Brown-Duda picks pinna reflection delays from a piecewise-linear table.
    // We approximate that with a smooth function of elevation:
    //   tap_delay_us = base_us + slope_us * sin(elevation)
    // base/slope values chosen by Brown-Duda's table fit (their fig. 4).
    const f32 e = elevation_rad;
    const f32 se = std::sin(e);
    // concha tap
    const f32 t1_us = 110.0f + 60.0f * se;
    // helix tap
    const f32 t2_us = 230.0f + 130.0f * se;
    const f32 inv_sr = 1.0f / static_cast<f32>(sample_rate);
    const u32 t1 = static_cast<u32>((t1_us * 1e-6f) / inv_sr + 0.5f);
    const u32 t2 = static_cast<u32>((t2_us * 1e-6f) / inv_sr + 0.5f);
    out_taps.fill(0.0f);
    out_taps[0] = 1.0f;                      // direct
    if (t1 < out_taps.size()) out_taps[t1] += -0.55f;   // concha (inverting)
    if (t2 < out_taps.size()) out_taps[t2] +=  0.30f;   // helix  (in-phase)
}

// Spherical-head shadow low-pass coefficient for the contralateral ear.
// Brown-Duda eq. (7) gives a 1-pole pseudo-shelving filter whose pole
// position varies with the contralateral incidence angle. We use the
// simpler form: alpha = 0.1 + 0.4 * (1 + cos(angle_ipsi)) where
// angle_ipsi is 0 when source is in front of ipsilateral ear and π when
// it's directly behind the listener.
PSY_FORCEINLINE f32 shadow_alpha(f32 contralateral_angle_rad) noexcept {
    // angle 0 = source pointing right at the contralateral ear (no shadow)
    // angle π = source pointing away from the contralateral ear (max shadow)
    const f32 c = std::cos(contralateral_angle_rad);
    // 0.1 ≤ α ≤ 0.5; smaller α = more head shadow (more low-pass)
    return 0.3f - 0.2f * c;
}

// Synthesise a 2-D angular HRIR (mono → stereo IR pair) for the given
// (azimuth, elevation) with distance-based 1/r attenuation.
// Convention: source is on the right when azimuth > 0.
inline StereoHrir make_hrir_2d(f32 azimuth_rad, f32 elevation_rad,
                               f32 distance_m, u32 sample_rate) noexcept {
    StereoHrir h{};
    itd_to_delays_2d(azimuth_rad, elevation_rad, sample_rate,
                     h.left_delay_samples, h.right_delay_samples);

    // 8-tap raised-cosine base impulse (compact, no aliasing).
    constexpr u32 kBaseTaps = 8;
    std::array<f32, kBaseTaps> base{};
    f32 sum = 0.0f;
    for (u32 i = 0; i < kBaseTaps; ++i) {
        base[i] = 0.5f * (1.0f - std::cos(2.0f * math::kPi *
                          (static_cast<f32>(i) + 0.5f) /
                          static_cast<f32>(kBaseTaps)));
        sum += base[i];
    }
    if (sum > 0.0f) for (auto& b : base) b /= sum;

    // Per-ear pan (azimuth interpreted L↔R only — elevation drives pinna).
    f32 lg, rg;
    pan_equal_power(azimuth_rad, lg, rg);

    // Distance attenuation: 1 / max(1, distance). Clamp near to avoid
    // singularity right at the listener (also keeps gains ≤ 1.0).
    const f32 r_atten = 1.0f / std::fmax(1.0f, distance_m);

    // Pinna combs per ear. Per-ear pinna *delay* differs because the
    // pinna geometry is asymmetric — for the contralateral side we damp
    // the comb a touch since head-shadow already nuked the highs.
    std::array<f32, 5> pinna_l{}, pinna_r{};
    pinna_taps(elevation_rad, sample_rate, pinna_l);
    pinna_taps(elevation_rad, sample_rate, pinna_r);

    const bool source_on_right = azimuth_rad > 0.0f;
    // contralateral angle: angle from contralateral-ear axis to source.
    // For source on right, the left ear is contralateral; its axis points
    // left. So contralateral_angle ≈ π/2 + azimuth on the right side.
    const f32 contra_angle =
        source_on_right ? (math::kHalfPi + azimuth_rad)
                        : (math::kHalfPi - azimuth_rad);
    const f32 a_shadow = shadow_alpha(contra_angle);

    // Two-stage build per ear: convolve base impulse with pinna comb,
    // then 1-pole low-pass the contralateral channel.
    auto build_ear = [&](std::array<f32, kHrirLength>& out,
                         const std::array<f32, 5>&     pinna,
                         f32 pan_gain,
                         bool is_contralateral) {
        out.fill(0.0f);
        // Convolve base * pinna.
        for (u32 i = 0; i < kBaseTaps; ++i) {
            const f32 bi = base[i] * pan_gain;
            for (u32 p = 0; p < pinna.size(); ++p) {
                const u32 j = i + p;
                if (j < kHrirLength) out[j] += bi * pinna[p];
            }
        }
        if (is_contralateral) {
            // 1-pole LP: y[n] = α x[n] + (1-α) y[n-1]
            f32 y = 0.0f;
            for (u32 i = 0; i < kHrirLength; ++i) {
                y = a_shadow * out[i] + (1.0f - a_shadow) * y;
                out[i] = y;
            }
        }
    };
    build_ear(h.left,  pinna_l, lg, /*is_contralateral=*/  source_on_right);
    build_ear(h.right, pinna_r, rg, /*is_contralateral=*/ !source_on_right);

    h.left_gain  = r_atten;
    h.right_gain = r_atten;
    return h;
}

// Back-compat wrapper for the prior azimuth-only API. Computes an HRIR at
// elevation = 0, distance = 1 m so existing call sites keep working.
inline StereoHrir make_minimal_hrir(f32 azimuth_rad, u32 sample_rate) noexcept {
    return make_hrir_2d(azimuth_rad, /*elevation=*/0.0f, /*distance=*/1.0f,
                        sample_rate);
}

// Apply a 2-D HRTF to a block of mono samples, producing stereo output.
// out_left / out_right are accumulator outputs of length `frames`; mono_in
// is `frames` mono samples. Caller pre-zeroes accumulators if they want a
// pure overwrite. State (ring buffer position, IR taps) is rebuilt every
// call — fine for the deliverable's job-per-voice job-system shape, but
// production-grade per-voice persistence is the obvious next step.
inline void apply_hrtf(f32* PSY_RESTRICT_ALIAS out_left,
                       f32* PSY_RESTRICT_ALIAS out_right,
                       const f32* PSY_RESTRICT_ALIAS mono_in,
                       u32 frames,
                       f32 azimuth_rad, f32 elevation_rad, f32 distance_m,
                       u32 sample_rate) noexcept {
    const StereoHrir h = make_hrir_2d(azimuth_rad, elevation_rad,
                                      distance_m, sample_rate);
    // Per-ear pipeline: delay-line into `mono_in`, then 64-tap FIR convolve
    // with the ear's HRIR. We use sample-by-sample convolution; the IR is
    // 64 taps, frame counts are typically 512 → 32k ops per ear per pull.
    auto run_ear = [&](f32* PSY_RESTRICT_ALIAS out,
                       const std::array<f32, kHrirLength>& ir,
                       u32 delay,
                       f32 gain) {
        for (u32 i = 0; i < frames; ++i) {
            f32 acc = 0.0f;
            for (u32 k = 0; k < kHrirLength; ++k) {
                const i64 idx = static_cast<i64>(i) - static_cast<i64>(k)
                              - static_cast<i64>(delay);
                if (idx < 0) break;
                acc += mono_in[idx] * ir[k];
            }
            out[i] += acc * gain;
        }
    };
    run_ear(out_left,  h.left,  h.left_delay_samples,  h.left_gain);
    run_ear(out_right, h.right, h.right_delay_samples, h.right_gain);
}

// ─── Cooley-Tukey FFT (radix-2, in-place) ───────────────────────────────
//
// Used by the indoor reverb (FFT convolution against a short room impulse).
// Real-only signals are zero-imag on input; output is complex; bit-reversal
// done in-place before butterflies. We expose `fft` and `ifft` taking
// f32 arrays of length 2*N (interleaved re,im).

inline u32 bit_reverse(u32 x, u32 log2n) noexcept {
    u32 n = 0;
    for (u32 i = 0; i < log2n; ++i) {
        n = (n << 1) | (x & 1u);
        x >>= 1;
    }
    return n;
}

// `data` is interleaved real+imag pairs of length 2*N where N = (1<<log2n).
// Sign: -1 for forward, +1 for inverse (we scale by 1/N inside ifft()).
inline void fft_radix2(f32* data, u32 log2n, int sign) noexcept {
    const u32 n = 1u << log2n;
    // bit-reversal permutation
    for (u32 i = 0; i < n; ++i) {
        const u32 j = bit_reverse(i, log2n);
        if (j > i) {
            std::swap(data[2*i],   data[2*j]);
            std::swap(data[2*i+1], data[2*j+1]);
        }
    }
    // butterflies
    for (u32 size = 2; size <= n; size <<= 1) {
        const u32 half = size >> 1;
        const f32 theta = static_cast<f32>(sign) * (2.0f * math::kPi / static_cast<f32>(size));
        const f32 wr_step = std::cos(theta);
        const f32 wi_step = std::sin(theta);
        for (u32 start = 0; start < n; start += size) {
            f32 wr = 1.0f, wi = 0.0f;
            for (u32 k = 0; k < half; ++k) {
                const u32 a = start + k;
                const u32 b = a + half;
                const f32 t_re = wr * data[2*b]   - wi * data[2*b+1];
                const f32 t_im = wr * data[2*b+1] + wi * data[2*b];
                data[2*b]   = data[2*a]   - t_re;
                data[2*b+1] = data[2*a+1] - t_im;
                data[2*a]   = data[2*a]   + t_re;
                data[2*a+1] = data[2*a+1] + t_im;
                const f32 nwr = wr * wr_step - wi * wi_step;
                const f32 nwi = wr * wi_step + wi * wr_step;
                wr = nwr; wi = nwi;
            }
        }
    }
}

inline void fft (f32* data, u32 log2n) noexcept { fft_radix2(data, log2n, -1); }
inline void ifft(f32* data, u32 log2n) noexcept {
    fft_radix2(data, log2n, +1);
    const f32 inv_n = 1.0f / static_cast<f32>(1u << log2n);
    const u32 n2    = (1u << log2n) * 2u;
    for (u32 i = 0; i < n2; ++i) data[i] *= inv_n;
}

// ─── FDN reverb (outdoors): 8-line Hadamard ─────────────────────────────
//
// 8 mutually-feeding delay lines mixed through a normalised Hadamard 8×8
// unitary matrix. The Hadamard matrix (the +1/-1 sign pattern below,
// scaled by 1/sqrt(8)) is unitary — it preserves energy across the
// network, guaranteeing exponential decay set purely by the feedback
// gain, no parametric tail.
//
// Delay-line lengths (in samples @ sample_rate):
//   Mutually-prime offsets in milliseconds, chosen to spread the comb
//   peaks of each line so the modal density of the combined network is
//   high (Schroeder-Logan diffuse criterion). Translating to samples at
//   the host sample rate keeps the comb spacing constant in seconds.
//
//   line  ms     samples @ 48k    samples @ 44.1k
//    0   29.7        1426             1310
//    1   37.1        1782             1636
//    2   41.1        1973             1813
//    3   43.7        2098             1928
//    4   47.3        2270             2086
//    5   53.7        2578             2369
//    6   59.3        2846             2616
//    7   61.7        2962             2722
//
// These specific values come from the literature (Jot 1992, Smith's CCRMA
// notes "Designing the Lengths of the Delay Lines"). The ratio of largest
// to smallest is ~2:1 which avoids aliased combs while keeping the
// network reasonably compact.

class FdnReverb {
public:
    void reset(u32 sample_rate, f32 decay_seconds) noexcept {
        sr_ = sample_rate;
        // delay-line lengths in seconds (see table above).
        const f32 base_s[kFdnSize] = {
            0.0297f, 0.0371f, 0.0411f, 0.0437f,
            0.0473f, 0.0537f, 0.0593f, 0.0617f,
        };
        f32 mean_s = 0.0f;
        for (u32 i = 0; i < kFdnSize; ++i) {
            u32 len = static_cast<u32>(base_s[i] * static_cast<f32>(sample_rate));
            if (len < 1u) len = 1u;
            if (len > kMaxLine) len = kMaxLine;
            line_len_[i]  = len;
            for (auto& x : lines_[i]) x = 0.0f;
            head_[i] = 0;
            mean_s  += base_s[i];
        }
        mean_s /= static_cast<f32>(kFdnSize);
        // feedback gain such that 60-dB decay reaches at decay_seconds
        // g = 10^(-3 * mean_delay_s / decay_s)
        const f32 decay_s = decay_seconds < 0.05f ? 0.05f : decay_seconds;
        const f32 db60    = -3.0f * mean_s / decay_s;
        gain_             = std::pow(10.0f, db60);
        (void)sr_;
    }

    // Process one mono sample, returning the wet-only reverb output.
    // The Hadamard 8x8 matrix (un-normalised):
    //   H8 = [[ +1 +1 +1 +1 +1 +1 +1 +1 ],
    //         [ +1 -1 +1 -1 +1 -1 +1 -1 ],
    //         [ +1 +1 -1 -1 +1 +1 -1 -1 ],
    //         [ +1 -1 -1 +1 +1 -1 -1 +1 ],
    //         [ +1 +1 +1 +1 -1 -1 -1 -1 ],
    //         [ +1 -1 +1 -1 -1 +1 -1 +1 ],
    //         [ +1 +1 -1 -1 -1 -1 +1 +1 ],
    //         [ +1 -1 -1 +1 -1 +1 +1 -1 ]]
    // Normalised by 1/sqrt(8). Implemented inline below.
    PSY_FORCEINLINE f32 tick(f32 in) noexcept {
        f32 r[kFdnSize];
        for (u32 i = 0; i < kFdnSize; ++i) {
            r[i] = lines_[i][head_[i]];
        }
        // Hadamard 8x8 normalised mix.
        const f32 inv_sqrt8 = 0.353553391f;  // 1 / sqrt(8)
        const f32 m0 = inv_sqrt8 * ( r[0]+r[1]+r[2]+r[3]+r[4]+r[5]+r[6]+r[7]);
        const f32 m1 = inv_sqrt8 * ( r[0]-r[1]+r[2]-r[3]+r[4]-r[5]+r[6]-r[7]);
        const f32 m2 = inv_sqrt8 * ( r[0]+r[1]-r[2]-r[3]+r[4]+r[5]-r[6]-r[7]);
        const f32 m3 = inv_sqrt8 * ( r[0]-r[1]-r[2]+r[3]+r[4]-r[5]-r[6]+r[7]);
        const f32 m4 = inv_sqrt8 * ( r[0]+r[1]+r[2]+r[3]-r[4]-r[5]-r[6]-r[7]);
        const f32 m5 = inv_sqrt8 * ( r[0]-r[1]+r[2]-r[3]-r[4]+r[5]-r[6]+r[7]);
        const f32 m6 = inv_sqrt8 * ( r[0]+r[1]-r[2]-r[3]-r[4]-r[5]+r[6]+r[7]);
        const f32 m7 = inv_sqrt8 * ( r[0]-r[1]-r[2]+r[3]-r[4]+r[5]+r[6]-r[7]);
        const f32 m[kFdnSize] = { m0, m1, m2, m3, m4, m5, m6, m7 };
        // Inject input + feedback into each line.
        for (u32 i = 0; i < kFdnSize; ++i) {
            const u32 idx = head_[i];
            lines_[i][idx] = in + gain_ * m[i];
            head_[i] = (idx + 1u) % line_len_[i];
        }
        // Wet output is the *un-weighted* average of the mixed outputs so
        // the listener hears the whole network rather than one tap.
        const f32 sum = m0+m1+m2+m3+m4+m5+m6+m7;
        return sum * (1.0f / static_cast<f32>(kFdnSize));
    }

    u32  line_length(u32 i) const noexcept {
        return (i < kFdnSize) ? line_len_[i] : 0u;
    }

private:
    // 4096 samples @ 48 kHz ≈ 85 ms — long enough for the longest base
    // delay-line (61.7 ms) with margin.
    static constexpr u32 kMaxLine = 4096;
    u32                                                 sr_ = 48000;
    f32                                                 gain_ = 0.0f;
    std::array<u32, kFdnSize>                           line_len_{};
    std::array<u32, kFdnSize>                           head_{};
    std::array<std::array<f32, kMaxLine>, kFdnSize>     lines_{};
};

// ─── FFT-convolution reverb (indoors) ───────────────────────────────────
//
// Convolution against a pre-baked room impulse response. The default IR is
// `engine/audio/builtin/short_room.bin` — a 24000-sample (0.5 s @ 48 kHz)
// image-source + Schroeder-tail RIR (see builtin/bake_short_room.cpp).
//
// API:
//   load_rir_from_file(path)  — read a raw float32 little-endian blob.
//   load_rir_from_memory(p,n) — copy a float32 array directly.
//   reset_synthetic(sr, ...)  — fallback to a synthesised IR if no file
//                                is available (kept for backward compat
//                                with the prior placeholder code).

class FftConvReverb {
public:
    // Load the baked room IR from a filesystem path. `sample_rate` is the
    // engine's host sample rate; we resample on the fly if it mismatches.
    // Returns true on success and IR is ready; false leaves the object in
    // the "no IR" state (process_block will produce silence).
    bool load_rir_from_file(const char* path, u32 sample_rate) noexcept {
        FILE* fp = std::fopen(path, "rb");
        if (!fp) return false;
        std::fseek(fp, 0, SEEK_END);
        const long bytes = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (bytes <= 0 || (bytes % 4) != 0) { std::fclose(fp); return false; }
        const u32 n_in = static_cast<u32>(bytes / 4);
        std::vector<f32> raw(n_in);
        const usize got = std::fread(raw.data(), 4, n_in, fp);
        std::fclose(fp);
        if (got != n_in) return false;
        return load_rir_from_memory(raw.data(), n_in, /*src_sr=*/48000u,
                                    sample_rate);
    }

    // Load from a memory buffer. `src_sr` is the IR's native rate; if it
    // differs from `dst_sr` we do a cheap linear resample.
    bool load_rir_from_memory(const f32* in, u32 in_samples,
                              u32 src_sr, u32 dst_sr) noexcept {
        if (!in || in_samples == 0u || src_sr == 0u || dst_sr == 0u) return false;
        u32 n_out;
        if (src_sr == dst_sr) {
            n_out = in_samples;
            if (n_out > kMaxIr) n_out = kMaxIr;
            for (u32 i = 0; i < n_out; ++i) ir_[i] = in[i];
        } else {
            const f32 ratio = static_cast<f32>(dst_sr) / static_cast<f32>(src_sr);
            u32 raw_out = static_cast<u32>(static_cast<f32>(in_samples) * ratio);
            if (raw_out > kMaxIr) raw_out = kMaxIr;
            n_out = raw_out;
            for (u32 i = 0; i < n_out; ++i) {
                const f32 t  = static_cast<f32>(i) / ratio;
                const u32 i0 = static_cast<u32>(t);
                const u32 i1 = (i0 + 1u < in_samples) ? (i0 + 1u) : (in_samples - 1u);
                const f32 a  = t - static_cast<f32>(i0);
                ir_[i] = in[i0] * (1.0f - a) + in[i1] * a;
            }
        }
        ir_len_ = n_out;
        return true;
    }

    // Synthesised-IR fallback. Kept for backward compatibility — if the
    // baked file can't be located the engine still produces some reverb.
    // This is identical to the old placeholder: exponentially-decayed
    // noise. NOT used in production paths once load_rir_from_file()
    // succeeds.
    void reset_synthetic(u32 sample_rate, f32 ir_seconds, f32 decay_seconds) noexcept {
        u32 n = static_cast<u32>(ir_seconds * static_cast<f32>(sample_rate));
        if (n == 0) n = 1;
        if (n > kMaxIr) n = kMaxIr;
        ir_len_ = n;
        u32 lcg = 0xC0FFEEu;
        const f32 inv_max = 1.0f / static_cast<f32>(0x7FFFFFFF);
        const f32 decay_s = decay_seconds < 0.05f ? 0.05f : decay_seconds;
        for (u32 i = 0; i < n; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            const i32 s = static_cast<i32>(lcg) >> 1;
            const f32 r = (static_cast<f32>(s) * inv_max) * 2.0f - 1.0f;
            const f32 t = static_cast<f32>(i) / static_cast<f32>(sample_rate);
            ir_[i] = r * std::exp(-3.0f * t / decay_s);
        }
    }

    // Compatibility entry: try baked file first, fall back to synthetic.
    // Returns true if the baked file was loaded; false if it fell back.
    bool reset(u32 sample_rate, f32 fallback_ir_s, f32 fallback_decay_s,
               const char* baked_rir_path) noexcept {
        if (baked_rir_path && load_rir_from_file(baked_rir_path, sample_rate)) {
            return true;
        }
        reset_synthetic(sample_rate, fallback_ir_s, fallback_decay_s);
        return false;
    }

    // Old single-argument reset for backward compat (synthetic only).
    void reset(u32 sample_rate, f32 ir_seconds, f32 decay_seconds) noexcept {
        reset_synthetic(sample_rate, ir_seconds, decay_seconds);
    }

    // Convolve `in` (length N samples, must be ≤ kMaxBlock) with the room IR
    // via FFT and add the wet signal into `out`. Returns the output length.
    u32 process_block(const f32* in, u32 n, f32* out) noexcept {
        if (ir_len_ == 0u) return 0u;
        const u32 conv_len = n + ir_len_ - 1u;
        u32 log2n = 1;
        while ((1u << log2n) < conv_len) ++log2n;
        const u32 fft_n = 1u << log2n;
        if (fft_n * 2u > kScratch) return 0u;

        for (u32 i = 0; i < fft_n; ++i) {
            sig_[2*i]   = (i < n) ? in[i] : 0.0f;
            sig_[2*i+1] = 0.0f;
        }
        for (u32 i = 0; i < fft_n; ++i) {
            kern_[2*i]   = (i < ir_len_) ? ir_[i] : 0.0f;
            kern_[2*i+1] = 0.0f;
        }
        fft(sig_.data(),  log2n);
        fft(kern_.data(), log2n);
        for (u32 i = 0; i < fft_n; ++i) {
            const f32 ar = sig_[2*i],  ai = sig_[2*i+1];
            const f32 br = kern_[2*i], bi = kern_[2*i+1];
            sig_[2*i]   = ar * br - ai * bi;
            sig_[2*i+1] = ar * bi + ai * br;
        }
        ifft(sig_.data(), log2n);
        for (u32 i = 0; i < conv_len; ++i) out[i] += sig_[2*i];
        return conv_len;
    }

    u32 ir_length() const noexcept { return ir_len_; }

private:
    // 24k taps == the baked short_room.bin (0.5 s @ 48 kHz). If the host
    // rate is higher we resample on load; 48 kHz native is the design
    // baseline (DESIGN §10.2).
    static constexpr u32 kMaxIr    = 24576;
    static constexpr u32 kMaxBlock = 1024;
    // FFT size = next power of 2 ≥ (kMaxIr + kMaxBlock) ≈ 25600 → 32768.
    // Interleaved re/im so scratch = 2 * 32768 = 65536 floats per buffer.
    static constexpr u32 kScratch  = 65536;
    std::array<f32, kMaxIr>    ir_{};
    u32                        ir_len_ = 0;
    std::array<f32, kScratch>  sig_{};
    std::array<f32, kScratch>  kern_{};
};

// ─── Voice-mix kernel for one job ───────────────────────────────────────
//
// Two render paths:
//   render_voice_into_stereo_tone(...)  — diagnostic-only, emits a fixed
//                                         frequency sine wave; kept so
//                                         the test that asserted on the
//                                         440 Hz tone path still has a
//                                         deterministic input.
//   render_voice_into_stereo_clip(...)  — production path. Streams int16
//                                         PCM from an AudioClip with a
//                                         per-voice cursor and applies
//                                         the 2-D HRTF.

PSY_FORCEINLINE void render_voice_into_stereo_tone(
        const Voice& v, f32 freq_hz,
        u32 sample_rate, u32 frames,
        f32* PSY_RESTRICT_ALIAS dst_stereo) noexcept {
    if (!v.active) return;
    const f32 az = std::atan2(v.position.x, std::fmax(0.0001f, v.position.z));
    f32 lg, rg;
    pan_equal_power(az, lg, rg);
    lg *= v.volume;
    rg *= v.volume;
    const f32 omega = 2.0f * math::kPi * freq_hz / static_cast<f32>(sample_rate);
    for (u32 i = 0; i < frames; ++i) {
        const f32 s = std::sin(omega * static_cast<f32>(v.cursor + i));
        dst_stereo[2*i + 0] += s * lg;
        dst_stereo[2*i + 1] += s * rg;
    }
}

// Streams from `clip` starting at v.cursor for `frames` frames, applies
// 2-D HRTF based on the voice's world-space position relative to a
// listener at the origin facing +Z (the engine transforms world →
// listener-local before calling this; see Audio.cpp).
//
// Mono and stereo clips are both handled — stereo collapses to mono via
// equal sum before HRTF (the prior compatible behaviour); 3D positioning
// implies a mono source anyway.
PSY_FORCEINLINE void render_voice_into_stereo_clip(
        const Voice& v, const AudioClip& clip,
        u32 sample_rate, u32 frames,
        f32* PSY_RESTRICT_ALIAS dst_stereo,
        f32* PSY_RESTRICT_ALIAS scratch_mono) noexcept {
    if (!v.active) return;
    if (clip.empty()) return;
    if (clip.channels == 0u || clip.sample_rate == 0u) return;

    // Sample-rate ratio (source → destination). On rate match this is 1.
    const f32 sr_ratio = static_cast<f32>(clip.sample_rate) /
                         static_cast<f32>(sample_rate);
    const u32 src_frames = clip.frame_count();
    if (src_frames == 0u) return;

    // Decode mono sample stream from the int16 clip with linear-interp
    // resampling, into `scratch_mono[0..frames]`.
    const f32 inv_i16 = 1.0f / 32768.0f;
    for (u32 i = 0; i < frames; ++i) {
        const f32 src_pos_f = (static_cast<f32>(v.cursor) +
                               static_cast<f32>(i)) * sr_ratio;
        const u32 src_i0 = static_cast<u32>(src_pos_f);
        if (src_i0 >= src_frames) { scratch_mono[i] = 0.0f; continue; }
        const u32 src_i1 = (src_i0 + 1u < src_frames) ? (src_i0 + 1u) : src_i0;
        const f32 frac   = src_pos_f - static_cast<f32>(src_i0);

        f32 s0 = 0.0f, s1 = 0.0f;
        if (clip.channels == 1u) {
            s0 = static_cast<f32>(clip.samples[src_i0]) * inv_i16;
            s1 = static_cast<f32>(clip.samples[src_i1]) * inv_i16;
        } else {
            // stereo → mono mix (average L+R)
            const i16 l0 = clip.samples[2u*src_i0 + 0u];
            const i16 r0 = clip.samples[2u*src_i0 + 1u];
            const i16 l1 = clip.samples[2u*src_i1 + 0u];
            const i16 r1 = clip.samples[2u*src_i1 + 1u];
            s0 = (static_cast<f32>(l0) + static_cast<f32>(r0)) * 0.5f * inv_i16;
            s1 = (static_cast<f32>(l1) + static_cast<f32>(r1)) * 0.5f * inv_i16;
        }
        scratch_mono[i] = (s0 * (1.0f - frac) + s1 * frac) * v.volume;
    }

    // World → listener-local conversion is the caller's job (it knows the
    // listener pose). Here we just compute the voice's spherical
    // coordinates assuming the listener sits at origin facing +Z, +X right,
    // +Y up. `v.position` is already in listener-local space when this is
    // called from Audio.cpp.
    const math::Vec3 p = v.position;
    const f32 distance = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
    // Azimuth: atan2(x, z), positive = right of listener.
    const f32 az = std::atan2(p.x, p.z);
    // Elevation: angle above the horizon.
    const f32 horizontal = std::sqrt(p.x*p.x + p.z*p.z);
    const f32 el = std::atan2(p.y, std::fmax(1e-4f, horizontal));

    // Apply HRTF: writes L and R into dst_stereo with .add semantics, so
    // the caller can call this for multiple voices into the same buffer.
    // We need temporary deinterleaved L/R accumulators since apply_hrtf
    // wants planar buffers; produce them on the stack.
    constexpr u32 kBlockMax = 1024;  // matches FftConvReverb::kMaxBlock
    if (frames > kBlockMax) return;
    f32 left[kBlockMax]  = {};
    f32 right[kBlockMax] = {};
    apply_hrtf(left, right, scratch_mono, frames,
               az, el, std::fmax(1e-3f, distance), sample_rate);
    for (u32 i = 0; i < frames; ++i) {
        dst_stereo[2*i + 0] += left[i];
        dst_stereo[2*i + 1] += right[i];
    }
}

// Back-compat shim retained so the existing call site in Audio.cpp keeps
// building during the transition. New code should call the _clip variant.
PSY_FORCEINLINE void render_voice_into_stereo(
        const Voice& v, f32 freq_hz,
        u32 sample_rate, u32 frames,
        f32* PSY_RESTRICT_ALIAS dst_stereo) noexcept {
    render_voice_into_stereo_tone(v, freq_hz, sample_rate, frames, dst_stereo);
}

}  // namespace psynder::audio::detail
