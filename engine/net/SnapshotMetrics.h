// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — snapshot-stream bandwidth metrics. Lane 18 (net). Milestone P.
//
// SnapshotPack.h ships a full quantized snapshot every tick; SnapshotPackDelta.h
// collapses the steady state to only what changed. To *tune* that budget we need
// to MEASURE it: how many bytes the stream actually costs per tick, the peak
// frame, the bytes/bits per second at a given tickrate, and how much a delta
// saves versus a full snapshot. This header is that accounting layer.
//
// SCOPE: this is measurement / tooling, NOT authoritative sim state. A
// BandwidthMeter is a side-channel counter the server or a profiler feeds; it
// never feeds back into the lockstep simulation, so nothing here affects the
// deterministic game outcome. (It still lives in the strict-FP net lane, so the
// f64 derivations below are themselves deterministic — identical record
// sequences yield identical means — but that is a property, not a requirement of
// the sim contract.)
//
// Determinism / safety: pure integer + f64 arithmetic. No allocation, no
// transcendentals, no RNG, no per-call heap growth. Every divide is guarded
// against a zero denominator (an empty meter and a zero-byte full snapshot both
// return well-defined values rather than dividing by zero). The net lane builds
// with -fno-fast-math / -ffp-contract=off (cmake/HotLane.cmake,
// DESIGN-PSYNDER-GX.md §14), so the f64 algebra here is bit-reproducible.

#pragma once

#include "core/Types.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// BandwidthMeter — a running tally of the per-tick snapshot byte cost. POD /
// trivially copyable (no methods, default member initialisers only) so it packs
// into a stats block and copies freely. `total_bytes` is the sum over every
// recorded frame, `frames` the number of recorded frames, `peak_frame_bytes` the
// largest single frame seen. u64 counters so a long capture cannot overflow at
// realistic snapshot sizes.
// ──────────────────────────────────────────────────────────────────────────
struct BandwidthMeter {
    u64 total_bytes      = 0;
    u64 frames           = 0;
    u64 peak_frame_bytes = 0;
};

// ──────────────────────────────────────────────────────────────────────────
// meter_record — fold one frame's snapshot byte cost into the meter: add
// `frame_bytes` to total_bytes, increment frames, and lift peak_frame_bytes to
// max(peak, frame_bytes). Pure integer accumulation — no allocation.
// ──────────────────────────────────────────────────────────────────────────
void meter_record(BandwidthMeter& m, usize frame_bytes) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// meter_reset — zero every field, returning the meter to its just-constructed
// state so a fresh capture window can begin.
// ──────────────────────────────────────────────────────────────────────────
void meter_reset(BandwidthMeter& m) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// mean_bytes_per_tick — total_bytes / frames, the average per-tick byte cost.
// Returns 0 for an empty meter (frames == 0) rather than dividing by zero.
// ──────────────────────────────────────────────────────────────────────────
f64 mean_bytes_per_tick(const BandwidthMeter& m) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// bytes_per_second — the mean per-tick cost projected to a wall-clock rate:
// mean_bytes_per_tick(m) * tickrate_hz. Returns 0 for an empty meter (the mean
// is already 0, so the product is 0 — no separate guard needed).
// ──────────────────────────────────────────────────────────────────────────
f64 bytes_per_second(const BandwidthMeter& m, f32 tickrate_hz) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// bits_per_second — bytes_per_second(m, tickrate_hz) * 8, the figure usually
// quoted for a network budget. Returns 0 for an empty meter.
// ──────────────────────────────────────────────────────────────────────────
f64 bits_per_second(const BandwidthMeter& m, f32 tickrate_hz) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// compression_ratio — delta_bytes / full_bytes as a fraction: how large a delta
// is relative to the full snapshot it replaces. Smaller is better (0.05 == the
// delta is 5% of a full snapshot). Returns 1.0 when full_bytes == 0 (no baseline
// to compare against — treat as "no compression") rather than dividing by zero.
// A pathological delta that exceeds the full snapshot yields a ratio > 1.
// ──────────────────────────────────────────────────────────────────────────
f64 compression_ratio(usize delta_bytes, usize full_bytes) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// full_snapshot_bytes — the size of the full quantized snapshot for
// `entity_count` entities (= net::packed_size(entity_count), i.e.
// 4 + 20 * entity_count). This is the denominator a delta is measured against in
// compression_ratio / savings_fraction.
// ──────────────────────────────────────────────────────────────────────────
usize full_snapshot_bytes(usize entity_count) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// savings_fraction — 1 - compression_ratio(delta_bytes, full_bytes): the
// fraction of bytes saved by sending the delta instead of a full snapshot
// (0.95 == 95% saved). Returns 0 when full_bytes == 0 (ratio is 1). Note: a
// delta larger than the full snapshot yields a NEGATIVE savings fraction — it is
// intentionally NOT clamped to [0, 1] so a regression past the full-snapshot
// size stays visible to the caller.
// ──────────────────────────────────────────────────────────────────────────
f64 savings_fraction(usize delta_bytes, usize full_bytes) noexcept;

}  // namespace psynder::net
