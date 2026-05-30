// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — per-tick send byte budget. Lane 18 (net).
//
// CongestionControl.h decides the send *rate* (Hz) and PriorityAccumulator.h
// decides the *order* in which relevant entities want a slot in the packet.
// This header is the third corner: a per-tick *byte* budget. A send tick can
// only put so many bytes on the wire — the link's bytes/second divided across
// the tickrate, i.e. (send rate * MTU). As the scheduler walks the priority
// selection and packs entity records, it must stop once that byte budget is
// spent so a busy frame cannot overrun the link. BandwidthBudget is exactly
// that gate: a classic token bucket measured in BYTES.
//
//   refill(dt)        — pour bytes_per_s * dt into the bucket, capped at burst.
//   try_spend(bytes)  — pack a record iff the bytes are available; else stop.
//
// The bucket carries unspent budget forward up to the burst cap, so a quiet
// tick lets the next busy tick send a little more — the standard token-bucket
// smoothing — while the cap bounds how bursty the stream can ever get.
//
// SCOPE: this is a *scheduler* gate, not authoritative sim state. It governs
// how many replication bytes the server ships, but never feeds back into the
// lockstep simulation, so it cannot change the deterministic game outcome. It
// still lives in the strict-FP net lane (cmake/HotLane.cmake builds it
// -fno-fast-math / -ffp-contract=off, DESIGN-PSYNDER-GX.md §14), so for a given
// (config, dt + spend sequence) the bucket state is bit-reproducible across
// runs and platforms.
//
// Determinism / safety: pure algebra — no RNG, no transcendentals, no library
// call beyond a finiteness guard. The fractional fill is accumulated in f64 so
// repeated small refills do not drift; spends are integer byte counts. No
// per-call heap: the whole state is a single f64 field. A non-finite or
// non-positive dt is ignored, so a stalled / garbage clock cannot corrupt the
// bucket.
//
// Compile flag: net TUs that include this header MUST be compiled with
//   -fno-fast-math -ffp-contract=off
// (DESIGN-PSYNDER-GX.md §14, lockstep determinism).

#pragma once

#include "core/Types.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// BandwidthBudget — a token-bucket byte budget for one send tick. The bucket
// holds fractional bytes (f64) so a slow refill rate over many ticks does not
// round away; spends are whole-byte integer amounts. Default-constructed it is
// a zero budget (rate 0, cap 0, empty) that spends nothing until configure()
// gives it a rate and a burst cap. POD-ish state (three scalars), copy freely.
// NOT thread-safe: refill/try_spend mutate shared state, so a single budget
// must be driven from one thread at a time.
// ──────────────────────────────────────────────────────────────────────────
class BandwidthBudget {
public:
    // Zero budget: rate 0, burst 0, empty bucket. Spends nothing until
    // configure() is called.
    BandwidthBudget() noexcept;

    // Set the refill rate (bytes/second) and the burst cap (the maximum bytes
    // the bucket can ever hold, i.e. the bucket size). A negative or non-finite
    // rate is clamped to 0. The current fill is re-clamped to the new cap so a
    // shrink takes effect immediately.
    void configure(f32 bytes_per_s, usize burst_bytes) noexcept;

    // Empty the bucket (fill -> 0) without changing the configured rate/cap.
    void reset() noexcept;

    // Add bytes_per_s * dt_s bytes to the bucket, saturating at the burst cap.
    // A non-finite or non-positive dt_s is ignored (the bucket is left
    // untouched), so a stalled / garbage clock can never inflate the budget.
    void refill(f32 dt_s) noexcept;

    // True iff the bucket currently holds at least `bytes` bytes.
    bool can_spend(usize bytes) const noexcept;

    // If can_spend(bytes), subtract `bytes` from the bucket and return true;
    // otherwise return false and leave the bucket UNCHANGED (all-or-nothing, so
    // a partial record is never charged).
    bool try_spend(usize bytes) noexcept;

    // Current bytes in the bucket (fractional).
    f32 available() const noexcept;

    // floor(available()) — the whole bytes currently spendable.
    usize available_bytes() const noexcept;

    // The configured refill rate (bytes/second).
    f32 bytes_per_s() const noexcept { return bytes_per_s_; }

    // The configured burst cap (bucket size, bytes).
    usize burst_bytes() const noexcept { return burst_bytes_; }

private:
    f64   fill_bytes_;   // current bucket level, fractional bytes [0, burst].
    f32   bytes_per_s_;  // refill rate, bytes per second (>= 0).
    usize burst_bytes_;  // bucket capacity, bytes.
};

}  // namespace psynder::net
