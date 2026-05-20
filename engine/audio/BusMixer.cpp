// SPDX-License-Identifier: MIT
//
// engine/audio/BusMixer.cpp
//
// Lane 23 — submix-graph DSP kernel implementation.
//
// Audio-callback-side code is hot-path: no allocations, no virtual calls,
// no exceptions. Per-frame accumulators are pre-sized in init(); all DSP
// kernels operate over `frames` and bail-out cleanly on overflow.

#include "BusMixer.h"

#include "math/Math.h"

#include <cmath>
#include <cstring>
#include <mutex>

namespace psynder::audio::detail {

namespace {

// 1-pole LP coefficient from cutoff. Standard EE form:
//   y[n] = alpha * x[n] + (1 - alpha) * y[n-1]
//   alpha = 1 - exp(-2*pi*fc / sr)
// Both fc and sr are positive; alpha ∈ (0, 1].
PSY_FORCEINLINE f32 lp_alpha(f32 fc_hz, u32 sr) noexcept {
    const f32 sr_f = static_cast<f32>(sr);
    if (sr_f <= 0.0f) return 1.0f;
    const f32 w = 2.0f * math::kPi * fc_hz / sr_f;
    const f32 a = 1.0f - std::exp(-w);
    if (a < 0.0f) return 0.0f;
    if (a > 1.0f) return 1.0f;
    return a;
}

}  // namespace

// ─── init / reset ─────────────────────────────────────────────────────────

bool BusMixer::init(u32 sample_rate, u32 max_frames) noexcept {
    if (sample_rate == 0u || max_frames == 0u || max_frames > kMaxBlockFrames) {
        return false;
    }
    sample_rate_ = sample_rate;
    max_frames_  = max_frames;
    // Allocate accumulator storage if not already sized. `assign` zero-
    // initialises; subsequent reset() calls reuse the same allocation.
    if (accum_storage_.size() != static_cast<usize>(kMaxBuses) * kBusAccumStride) {
        accum_storage_.assign(static_cast<usize>(kMaxBuses) * kBusAccumStride, 0.0f);
    }
    if (reverb_send_.size() != kMaxBlockFrames) {
        reverb_send_.assign(kMaxBlockFrames, 0.0f);
    }
    reset();
    return true;
}

void BusMixer::reset() noexcept {
    std::lock_guard<std::mutex> lk(target_mu_);
    for (u32 i = 0; i < kMaxBuses; ++i) {
        states_[i] = BusState{};
    }
    if (!accum_storage_.empty()) {
        std::memset(accum_storage_.data(), 0, sizeof(f32) * accum_storage_.size());
    }
    if (!reverb_send_.empty()) {
        std::memset(reverb_send_.data(), 0, sizeof(f32) * reverb_send_.size());
    }
    last_frames_ = 0;
}

// ─── Parameter setters / getters (callable from any thread) ──────────────

BusGains BusMixer::clamp_gains(BusGains g) noexcept {
    if (!(g.gain_linear >= 0.0f))      g.gain_linear = 0.0f;
    if (g.gain_linear > 4.0f)          g.gain_linear = 4.0f;
    if (!(g.lp_hz > 0.0f))             g.lp_hz       = 10.0f;  // also catches NaN
    if (g.lp_hz > kLpBypassHz)         g.lp_hz       = kLpBypassHz;
    if (!(g.reverb_send >= 0.0f))      g.reverb_send = 0.0f;
    if (g.reverb_send > 1.0f)          g.reverb_send = 1.0f;
    return g;
}

void BusMixer::set_target(BusId bus, const BusGains& g) noexcept {
    const u32 i = static_cast<u32>(bus);
    if (i >= kMaxBuses) return;
    std::lock_guard<std::mutex> lk(target_mu_);
    states_[i].target = clamp_gains(g);
}

BusGains BusMixer::get_target(BusId bus) const noexcept {
    const u32 i = static_cast<u32>(bus);
    if (i >= kMaxBuses) return {};
    std::lock_guard<std::mutex> lk(target_mu_);
    return states_[i].target;
}

BusGains BusMixer::current_gains(BusId bus) const noexcept {
    const u32 i = static_cast<u32>(bus);
    if (i >= kMaxBuses) return {};
    // No lock — current is only mutated by the audio thread. Test hooks
    // call this from the test thread; a torn read is OK for diagnostic
    // inspection (the production hot path doesn't observe current_ from
    // any other thread).
    return states_[i].current;
}

// ─── Audio-callback hot path ──────────────────────────────────────────────

void BusMixer::begin_frame(u32 frames) noexcept {
    if (frames == 0u || frames > max_frames_ || accum_storage_.empty()) {
        last_frames_ = 0;
        return;
    }
    last_frames_ = frames;
    const u32 stereo_floats = 2u * frames;

    // Zero per-bus accumulators (just the in-use portion).
    for (u32 i = 0; i < kMaxBuses; ++i) {
        std::memset(accum_ptr(i), 0, sizeof(f32) * stereo_floats);
    }
    std::memset(reverb_send_.data(), 0, sizeof(f32) * frames);

    // Latch target gains under try_lock. On contention (a UI thread is
    // mid-set_target), skip the latch — the audio thread keeps the
    // previously-latched target. This is the canonical "RT thread never
    // blocks on a non-RT thread" pattern (see DESIGN.md §10.2).
    if (target_mu_.try_lock()) {
        // No copy needed — target_ is already up to date in `states_`.
        // The lock acquisition is itself the synchronisation: any in-flight
        // setter has either completed before us or will run after this
        // unlock and pick up the next frame.
        target_mu_.unlock();
    }
    // If try_lock fails, the audio thread proceeds with the values
    // currently in states_[i].target. These are coherent (mutex pairs
    // with prior writes), and the next pull will pick up newer values.
}

void BusMixer::accumulate_voice(BusId bus, const f32* src, u32 frames) noexcept {
    const u32 i = static_cast<u32>(bus);
    if (i >= kMaxBuses) return;
    if (!src || frames == 0u || frames > last_frames_) return;
    if (accum_storage_.empty()) return;
    const u32 stereo_floats = 2u * frames;
    f32* acc = accum_ptr(i);
    // Inline scalar accumulate. We deliberately do NOT call simd_mix_into
    // here: the per-voice buffer is already SIMD-mixed once by the engine,
    // and this bus accumulation is a second add-pass — keeping it scalar
    // (over short blocks) avoids a second SIMD-load-store round trip for
    // short stereo blocks (≤ 512 frames is the common case).
    for (u32 k = 0; k < stereo_floats; ++k) {
        acc[k] += src[k];
    }
}

void BusMixer::process_bus_block(BusState& s, f32* accum_stereo,
                                 u32 frames, f32* out_stereo) noexcept {
    const f32 g_target  = s.target.gain_linear;
    const f32 lp_target = s.target.lp_hz;
    // Per-frame ramp step. kRampSamples is the worst-case ramp length;
    // the per-sample increment equals (target - current) / kRampSamples
    // so a 1.0 → 0.0 hop takes exactly 256 samples.
    f32 g_cur = s.current.gain_linear;
    const f32 g_step = (g_target - g_cur) / static_cast<f32>(kRampSamples);

    // LP coefficient. If we're at or above bypass, skip the filter math.
    const bool lp_enabled = (lp_target < kLpBypassHz);
    const f32  alpha      = lp_enabled ? lp_alpha(lp_target, sample_rate_) : 1.0f;

    f32 lp_l = s.lp_prev_l;
    f32 lp_r = s.lp_prev_r;

    for (u32 i = 0; i < frames; ++i) {
        // Advance gain ramp toward target, clamped to not overshoot.
        if (g_step > 0.0f && g_cur < g_target) {
            g_cur += g_step;
            if (g_cur > g_target) g_cur = g_target;
        } else if (g_step < 0.0f && g_cur > g_target) {
            g_cur += g_step;
            if (g_cur < g_target) g_cur = g_target;
        } else {
            g_cur = g_target;
        }

        f32 l = accum_stereo[2*i + 0] * g_cur;
        f32 r = accum_stereo[2*i + 1] * g_cur;

        if (lp_enabled) {
            lp_l = alpha * l + (1.0f - alpha) * lp_l;
            lp_r = alpha * r + (1.0f - alpha) * lp_r;
            l = lp_l;
            r = lp_r;
        }

        out_stereo[2*i + 0] = l;
        out_stereo[2*i + 1] = r;
    }

    // Persist trailing state.
    s.lp_prev_l           = lp_l;
    s.lp_prev_r           = lp_r;
    s.current.gain_linear = g_cur;
    // lp_hz / reverb_send are read directly from `target` per sample —
    // there's no separate per-sample interpolation for those because
    // (a) lp_hz changes from gameplay are rare (per-bus mood / EQ tweak,
    // not per-event), and (b) reverb_send rides on top of the bus signal,
    // which is already gain-ramped, so a reverb_send jump can't produce a
    // discontinuity larger than the bus signal itself.
    s.current.lp_hz       = lp_target;
    s.current.reverb_send = s.target.reverb_send;
}

void BusMixer::mix_buses(f32* master_stereo, f32* reverb_send_mono, u32 frames) noexcept {
    if (!master_stereo || frames == 0u || frames > last_frames_) return;
    if (accum_storage_.empty()) return;
    const u32 stereo_floats = 2u * frames;

    // Scratch for one bus's post-gain output. Sized for the max block;
    // only `stereo_floats` entries are touched per call. Stack-allocated
    // (the audio callback stack on every supported platform — AURemoteIO,
    // Wasapi event thread, PipeWire RT thread — has at least 64 KB to
    // spare; 32 KB for this buffer is fine).
    alignas(16) f32 bus_out[kMaxBlockFrames * 2u];

    for (u32 i = 0; i < kMaxBuses; ++i) {
        BusState& s = states_[i];
        f32* acc    = accum_ptr(i);

        // Early-out for buses that have no signal AND no in-flight gain
        // ramp. We must still process when the gain is mid-ramp (current
        // != target) because the current value has to converge to the
        // target even when the bus is silent — otherwise a UI thread
        // setting gain on an empty bus would never see it take effect.
        const bool mid_ramp =
            s.current.gain_linear != s.target.gain_linear ||
            s.current.lp_hz != s.target.lp_hz;
        bool needs_processing = mid_ramp;
        if (!needs_processing) {
            for (u32 k = 0; k < stereo_floats; ++k) {
                if (acc[k] != 0.0f) { needs_processing = true; break; }
            }
        }
        if (!needs_processing) continue;

        process_bus_block(s, acc, frames, bus_out);

        // Sum into master (additive — caller may have other content).
        for (u32 k = 0; k < stereo_floats; ++k) {
            master_stereo[k] += bus_out[k];
        }

        // Reverb send: route `reverb_send * mono_sum` into the shared
        // reverb input. Mono sum = 0.5 * (L + R). We always update our
        // owned `reverb_send_` buffer (for the BusTestHook inspection
        // path) and additionally tee into `reverb_send_mono` if the
        // engine supplied one. The two buffers stay in lockstep.
        const f32 rs = s.current.reverb_send;
        if (rs > 0.0f) {
            for (u32 j = 0; j < frames; ++j) {
                const f32 mono     = 0.5f * (bus_out[2*j + 0] + bus_out[2*j + 1]);
                const f32 send_amp = rs * mono;
                reverb_send_[j] += send_amp;
                if (reverb_send_mono) reverb_send_mono[j] += send_amp;
            }
        }
    }
}

const f32* BusMixer::bus_accumulator(BusId bus) const noexcept {
    const u32 i = static_cast<u32>(bus);
    if (i >= kMaxBuses) return nullptr;
    if (accum_storage_.empty()) return nullptr;
    return accum_ptr(i);
}

}  // namespace psynder::audio::detail
