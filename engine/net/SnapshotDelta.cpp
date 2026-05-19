// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — XOR + RLE snapshot delta codec impl. Lane 18 (net).

#include "SnapshotDelta.h"

#include <algorithm>

namespace psynder::net {

namespace {

// Emit a zero run of length `n` into `out`. `n` must be > 0. Splits into
// multiple `0x00 length-1` chunks of up to 256 zeros each.
void emit_zero_run(std::vector<u8>& out, usize n) noexcept {
    while (n > 0) {
        const usize chunk = std::min<usize>(n, 256);
        out.push_back(0x00u);
        out.push_back(static_cast<u8>(chunk - 1));
        n -= chunk;
    }
}

}  // namespace

usize xor_delta_encode(std::span<const u8> baseline,
                       std::span<const u8> current,
                       std::vector<u8>&    out) noexcept {
    const usize n   = std::max(baseline.size(), current.size());
    const usize bn  = baseline.size();
    const usize cn  = current.size();
    const usize start_size = out.size();

    // Reserve a conservative upper bound. Worst case (no zeros at all) is
    // 1:1 with input.
    out.reserve(start_size + n + 8);

    usize zero_run = 0;
    for (usize i = 0; i < n; ++i) {
        const u8 a = (i < bn) ? baseline[i] : 0u;
        const u8 b = (i < cn) ? current[i]  : 0u;
        const u8 x = a ^ b;
        if (x == 0u) {
            ++zero_run;
            continue;
        }
        if (zero_run != 0) {
            emit_zero_run(out, zero_run);
            zero_run = 0;
        }
        out.push_back(x);
    }
    if (zero_run != 0) {
        emit_zero_run(out, zero_run);
    }
    return out.size() - start_size;
}

bool xor_delta_decode(std::span<const u8> baseline,
                      std::span<const u8> delta,
                      std::vector<u8>&    out) noexcept {
    const usize bn = baseline.size();
    out.assign(bn, 0u);
    usize cursor = 0;  // index into `out`
    usize i      = 0;  // index into `delta`
    while (i < delta.size()) {
        const u8 tag = delta[i++];
        if (tag == 0x00u) {
            // Zero-run escape: next byte is run_length-1.
            if (i >= delta.size()) return false;       // truncated
            const usize run = static_cast<usize>(delta[i++]) + 1u;
            if (cursor + run > bn) {
                // Run extends past baseline. We allow zero-extending in
                // the encoder, so the decoder must tolerate it here.
                // Cap to baseline size; the trailing zeros are no-ops.
                const usize fill = bn - cursor;
                for (usize k = 0; k < fill; ++k) out[cursor + k] = baseline[cursor + k];
                cursor = bn;
                // Skip any remaining run beyond bn — nothing to write.
                continue;
            }
            for (usize k = 0; k < run; ++k) {
                out[cursor + k] = baseline[cursor + k];
            }
            cursor += run;
        } else {
            if (cursor >= bn) {
                // Delta has extra bytes past baseline end. Either the
                // baseline was shorter than `current` at encode time, or
                // the wire data is corrupt. We grow `out` to absorb the
                // tail so the round-trip property holds for the
                // size-mismatch case.
                out.push_back(tag);
                ++cursor;
            } else {
                out[cursor] = baseline[cursor] ^ tag;
                ++cursor;
            }
        }
    }
    // Any tail of `out` past `cursor` keeps its zero-init (== baseline XOR
    // implicit zero, then XOR'd against baseline above leaves the value at
    // baseline[i]). Actually we initialised `out` to all zeros and didn't
    // touch [cursor..bn) above when the delta stream ended early. The
    // correct value for an "all-zero tail in the XOR stream" is identical
    // to the baseline, so we copy it through.
    if (cursor < bn) {
        for (usize k = cursor; k < bn; ++k) {
            out[k] = baseline[k];
        }
    }
    return true;
}

}  // namespace psynder::net
