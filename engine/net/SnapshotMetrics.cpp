// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — snapshot-stream bandwidth metrics. Lane 18 (net). Milestone P.
//
// Implementation of the bandwidth accounting declared in SnapshotMetrics.h: pure
// integer + f64 arithmetic, every divide guarded against a zero denominator. No
// allocation, no transcendentals, no RNG. This is measurement tooling — it never
// feeds the authoritative sim — but it still lives in the strict-FP net lane so
// the f64 derivations are bit-reproducible across runs and platforms.

#include "net/SnapshotMetrics.h"

#include "net/SnapshotPack.h"  // net::packed_size — the full-snapshot baseline.

namespace psynder::net {

void meter_record(BandwidthMeter& m, usize frame_bytes) noexcept {
    const u64 bytes = static_cast<u64>(frame_bytes);
    m.total_bytes += bytes;
    ++m.frames;
    if (bytes > m.peak_frame_bytes) m.peak_frame_bytes = bytes;
}

void meter_reset(BandwidthMeter& m) noexcept {
    m.total_bytes      = 0;
    m.frames           = 0;
    m.peak_frame_bytes = 0;
}

f64 mean_bytes_per_tick(const BandwidthMeter& m) noexcept {
    if (m.frames == 0) return 0.0;  // guard the divide on an empty meter.
    return static_cast<f64>(m.total_bytes) / static_cast<f64>(m.frames);
}

f64 bytes_per_second(const BandwidthMeter& m, f32 tickrate_hz) noexcept {
    // mean is 0 for an empty meter, so the product is 0 with no extra guard.
    return mean_bytes_per_tick(m) * static_cast<f64>(tickrate_hz);
}

f64 bits_per_second(const BandwidthMeter& m, f32 tickrate_hz) noexcept {
    return bytes_per_second(m, tickrate_hz) * 8.0;
}

f64 compression_ratio(usize delta_bytes, usize full_bytes) noexcept {
    // No baseline to compare against — treat as "no compression" (ratio 1.0)
    // rather than dividing by zero.
    if (full_bytes == 0) return 1.0;
    return static_cast<f64>(delta_bytes) / static_cast<f64>(full_bytes);
}

usize full_snapshot_bytes(usize entity_count) noexcept {
    // The full quantized snapshot size (4 + 20 * entity_count) — the denominator
    // a delta is measured against. Delegated so the two stay in lock-step.
    return net::packed_size(entity_count);
}

f64 savings_fraction(usize delta_bytes, usize full_bytes) noexcept {
    // Fraction of bytes saved vs a full snapshot. Deliberately NOT clamped: a
    // delta larger than the full snapshot yields a negative result so a
    // regression past full-snapshot size stays visible to the caller.
    return 1.0 - compression_ratio(delta_bytes, full_bytes);
}

}  // namespace psynder::net
