// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — deterministic fixed-point quantization. Lane 18 (net).
//
// See Quantize.h for the contract and determinism rationale. The divide and
// floor are performed in double precision: this widens the f32 inputs exactly
// (f32 ⊂ f64) and keeps the round-half-up step bit-reproducible across
// platforms for finite inputs in the strict-FP net lane.

#include "net/Quantize.h"

#include "core/Types.h"

#include <cmath>
#include <cstdint>

namespace psynder::net {

i32 quantize(f32 value, f32 resolution_m) noexcept {
    if (resolution_m <= 0.f) {
        return 0;  // invalid resolution — defined fallback, not UB
    }
    // double divide + floor(x + 0.5): deterministic round-half-up.
    const double steps = static_cast<double>(value) / static_cast<double>(resolution_m);
    return static_cast<i32>(std::floor(steps + 0.5));
}

f32 dequantize(i32 q, f32 resolution_m) noexcept {
    return static_cast<f32>(static_cast<double>(q) * static_cast<double>(resolution_m));
}

void quantize_pos(const f32 in[3], f32 resolution_m, i32 out[3]) noexcept {
    out[0] = quantize(in[0], resolution_m);
    out[1] = quantize(in[1], resolution_m);
    out[2] = quantize(in[2], resolution_m);
}

void dequantize_pos(const i32 in[3], f32 resolution_m, f32 out[3]) noexcept {
    out[0] = dequantize(in[0], resolution_m);
    out[1] = dequantize(in[1], resolution_m);
    out[2] = dequantize(in[2], resolution_m);
}

}  // namespace psynder::net
