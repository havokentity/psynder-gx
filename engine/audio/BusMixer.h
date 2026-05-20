// SPDX-License-Identifier: MIT
//
// engine/audio/BusMixer.h
//
// Lane 23 — submix-graph DSP kernel.
//
// One BusMixer instance owns:
//   * kMaxBuses pre-sized stereo accumulators (one per bus).
//   * kMaxBuses BusState slots (current + target gains, LP filter state).
//   * A mono reverb-send accumulator summed across all buses.
//
// The audio callback's per-frame protocol:
//
//   1. begin_frame(frames)                — zero per-bus and reverb-send
//                                            accumulators; latch the
//                                            target gains under try_lock.
//   2. accumulate_voice(bus, src, frames) — sum a rendered voice's
//                                            stereo PCM into its bus.
//   3. mix_buses(master, frames)          — apply each bus's gain ramp +
//                                            LP filter, sum into master,
//                                            write reverb-send copies.
//
// The accumulators are tracked even between begin_frame and mix_buses so
// tests (via BusTestHook.h) can snapshot pre-gain state.
//
// This class is internal — the global instance lives behind the lane-23
// router singleton in BusApi.cpp. The free functions in Bus.h thunk
// through to that instance via internal/BusRouter.h. Standalone
// construction is supported (and recommended for unit tests).

#pragma once

#include "audio/Bus.h"
#include "core/Types.h"

#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace psynder::audio::detail {

// ─── Constants ───────────────────────────────────────────────────────────

// Gain-ramp length (samples). Long enough that a 1.0 → 0.0 instantaneous
// gain change is inaudible as a click; short enough that the lag isn't
// perceptible (256 samples ≈ 5.3 ms @ 48 kHz). DESIGN.md §10.2 names
// "zipper-free gain changes" as a non-functional requirement.
inline constexpr u32 kRampSamples = 256;

// LP-bypass threshold. A 1-pole LP at or above this cutoff is effectively
// transparent at 48 kHz sample rate (-3 dB > Nyquist), so we skip the
// filter math entirely.
inline constexpr f32 kLpBypassHz = 22000.0f;

// Maximum frames per audio callback. Mirrors DEFAULT desc.buffer_frames
// in Audio.cpp — large enough for any platform backend's worst-case pull.
inline constexpr u32 kMaxBlockFrames = 4096;

// ─── BusState (POD) ──────────────────────────────────────────────────────

// One bus's runtime state. POD layout — no constructors that require
// non-trivial destruction; safe to memset to zero on init().
struct BusState {
    // Current applied gains (interpolated toward `target` each block).
    BusGains current{};
    // Target gains set by set_bus_gains().
    BusGains target{};
    // 1-pole LP filter state (per-channel last output).
    f32      lp_prev_l = 0.0f;
    f32      lp_prev_r = 0.0f;

    // The per-bus stereo accumulator is held in the BusMixer's heap-
    // allocated `accum_storage_` (see private members below) — too big
    // to live inline at kMaxBuses * kMaxBlockFrames * 2 floats = ~512 KB.
    // Storing per-bus rather than per-voice keeps the hot path's working
    // set small (each bus is touched once during mix_buses, hot in L1).
};

// ─── BusMixer ─────────────────────────────────────────────────────────────

class BusMixer {
public:
    // Allocate accumulators sized for `max_frames`. Must be called before
    // any per-frame call. Idempotent — safe to call again with a larger
    // size if the backend renegotiates. Returns false if max_frames is
    // out of range.
    bool init(u32 sample_rate, u32 max_frames) noexcept;

    // Reset all bus state to defaults (unity gain, no LP, no reverb send).
    // Called from the lane-23 router_reset() — wired into Engine::start()
    // and Engine::stop() — so a fresh engine lifecycle starts from a
    // known-good state. Safe to call from tests between scenarios.
    void reset() noexcept;

    // ── Public bus parameter API (thread-safe) ─────────────────────────

    void     set_target(BusId bus, const BusGains& g) noexcept;
    BusGains get_target(BusId bus) const noexcept;

    // ── Audio-callback hot path (single-threaded, RT priority) ─────────

    // Step 1: zero accumulators, latch target gains. Call exactly once
    // per pull, before any accumulate_voice() calls.
    void begin_frame(u32 frames) noexcept;

    // Step 2: sum `src` (stereo interleaved, 2*frames floats) into the
    // bus's accumulator. Called once per active voice during a pull.
    // Out-of-range BusId is a no-op (early return).
    void accumulate_voice(BusId bus, const f32* src, u32 frames) noexcept;

    // Step 3: apply each bus's gain ramp + LP filter; sum into master_stereo
    // (2*frames floats); accumulate reverb sends into the shared
    // reverb_send_mono buffer (`frames` floats). The master buffer is added
    // to (not overwritten) so the engine can intermix non-bus content if it
    // wants to.
    void mix_buses(f32* master_stereo, f32* reverb_send_mono, u32 frames) noexcept;

    // ── Test hooks (used by BusTestHook.h) ─────────────────────────────

    u32         frames_in_flight() const noexcept { return last_frames_; }
    const f32*  bus_accumulator(BusId bus) const noexcept;
    const f32*  reverb_send_buffer() const noexcept {
        return reverb_send_.empty() ? nullptr : reverb_send_.data();
    }
    BusGains    current_gains(BusId bus) const noexcept;

private:
    // Per-bus state (PODs, sized inline). The PCM accumulators live on the
    // heap (allocated once in init()) — at kMaxBuses * kMaxBlockFrames * 2
    // floats = ~512 KB they'd blow a default 1 MB Windows thread stack if
    // BusMixer were ever stack-allocated (e.g. in unit tests).
    std::array<BusState, kMaxBuses>             states_{};

    // Flattened per-bus accumulators: layout [bus][frame*2 + L/R].
    // Sized to kMaxBuses * kMaxBlockFrames * 2 in init(); never reallocated
    // afterwards (the hot path requires zero allocations).
    std::vector<f32>                            accum_storage_{};

    // Mono reverb-send accumulator. Sized to kMaxBlockFrames in init().
    std::vector<f32>                            reverb_send_{};

    // Bus-target mutex. The audio callback try_locks; on contention it
    // uses last-frame's target (already latched at begin_frame).
    mutable std::mutex      target_mu_{};

    u32 sample_rate_ = 48000;
    u32 max_frames_  = 512;
    u32 last_frames_ = 0;  // frames passed to most-recent begin_frame()

    // Stride between consecutive bus accumulators (floats per bus).
    static constexpr u32 kBusAccumStride = kMaxBlockFrames * 2u;
    f32*       accum_ptr(u32 bus)       noexcept { return accum_storage_.data() + bus * kBusAccumStride; }
    const f32* accum_ptr(u32 bus) const noexcept { return accum_storage_.data() + bus * kBusAccumStride; }

    // Clamp BusGains to safe ranges (gain ∈ [0, 4], lp_hz ∈ [10, 22000],
    // reverb_send ∈ [0, 1]). Called inside set_target().
    static BusGains clamp_gains(BusGains g) noexcept;

    // Apply one block of gain ramp + LP filter on a bus's accumulator,
    // writing the gain-applied output into `out_stereo` (length 2*frames)
    // and updating the bus's LP state + current gains.
    void process_bus_block(BusState& s, f32* accum_stereo,
                           u32 frames, f32* out_stereo) noexcept;
};

}  // namespace psynder::audio::detail
