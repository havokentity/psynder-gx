// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — bandwidth-efficient quantized snapshot states. Lane 18 (net).

#include "net/SnapshotQuantized.h"

#include "net/Quantize.h"

#include <cmath>

namespace psynder::net {

namespace {

// Yaw step: one milli-degree per integer count. round(yaw_deg * 1000) via
// floor(x + 0.5) in double — deterministic, half-up ties (matching the
// floor(x + 0.5) rounding used by net::quantize), no transcendentals.
constexpr double kYawMilliPerDeg = 1000.0;

}  // namespace

QuantizedState quantize_state(const EntityState& s, f32 pos_resolution_m) noexcept {
    QuantizedState q{};
    q.id = s.id;
    q.pos[0] = quantize(s.pos[0], pos_resolution_m);
    q.pos[1] = quantize(s.pos[1], pos_resolution_m);
    q.pos[2] = quantize(s.pos[2], pos_resolution_m);

    const double scaled = static_cast<double>(s.yaw_deg) * kYawMilliPerDeg;
    q.yaw_milli = static_cast<i32>(std::floor(scaled + 0.5));
    return q;
}

EntityState dequantize_state(const QuantizedState& q, f32 pos_resolution_m) noexcept {
    EntityState s{};
    s.id = q.id;
    s.pos[0] = dequantize(q.pos[0], pos_resolution_m);
    s.pos[1] = dequantize(q.pos[1], pos_resolution_m);
    s.pos[2] = dequantize(q.pos[2], pos_resolution_m);

    s.yaw_deg = static_cast<f32>(static_cast<double>(q.yaw_milli) / kYawMilliPerDeg);
    return s;
}

usize quantized_wire_bytes() noexcept {
    return sizeof(QuantizedState);
}

}  // namespace psynder::net
