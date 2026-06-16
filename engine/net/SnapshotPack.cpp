// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — portable little-endian wire packing for quantized snapshots.
// Lane 18 (net). See SnapshotPack.h for the wire format + determinism notes.

#include "net/SnapshotPack.h"

#include "net/Quantize.h"
#include "net/SnapshotQuantized.h"

#include <cstring>

namespace psynder::net {

namespace {

// On-wire layout constants (all integers, explicit little-endian).
constexpr usize kHeaderBytes = sizeof(u32);                  // u32 count
constexpr usize kRecordBytes =
    sizeof(u32) + 3 * sizeof(i32) + sizeof(i32);             // id + pos[3] + yaw

// Append a u32 to `out` in little-endian byte order, independent of host
// endianness. We never memcpy the integer (that would be host-order); we shift
// out each byte so the stream is bit-identical on every platform.
inline void put_u32_le(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

// Append an i32 as little-endian. Reinterpret the bit pattern as u32 first so
// the byte layout is well-defined for negative values (two's-complement bits
// preserved) without relying on signed-shift behaviour.
inline void put_i32_le(std::vector<u8>& out, i32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32_le(out, bits);
}

// Read a little-endian u32 from `p` (caller guarantees 4 readable bytes).
inline u32 read_u32_le(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

// Read a little-endian i32 from `p` (caller guarantees 4 readable bytes).
inline i32 read_i32_le(const u8* p) noexcept {
    const u32 bits = read_u32_le(p);
    i32 v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

}  // namespace

void pack_quantized(std::span<const EntityState> states,
                    f32                          pos_resolution_m,
                    std::vector<u8>&             out) {
    out.clear();
    out.reserve(packed_size(states.size()));

    put_u32_le(out, static_cast<u32>(states.size()));

    for (const EntityState& s : states) {
        const QuantizedState q = quantize_state(s, pos_resolution_m);
        put_u32_le(out, q.id);
        put_i32_le(out, q.pos[0]);
        put_i32_le(out, q.pos[1]);
        put_i32_le(out, q.pos[2]);
        put_i32_le(out, q.yaw_milli);
    }
}

bool unpack_quantized(std::span<const u8>       bytes,
                      f32                       pos_resolution_m,
                      std::vector<EntityState>& out) {
    if (bytes.size() < kHeaderBytes) return false;  // header missing

    const u32 count = read_u32_le(bytes.data());

    // Total bytes the declared record count requires; reject truncated streams.
    const usize need = kHeaderBytes + static_cast<usize>(count) * kRecordBytes;
    if (bytes.size() < need) return false;

    out.clear();
    out.reserve(count);

    usize off = kHeaderBytes;
    for (u32 i = 0; i < count; ++i) {
        QuantizedState q{};
        q.id        = read_u32_le(bytes.data() + off); off += sizeof(u32);
        q.pos[0]    = read_i32_le(bytes.data() + off); off += sizeof(i32);
        q.pos[1]    = read_i32_le(bytes.data() + off); off += sizeof(i32);
        q.pos[2]    = read_i32_le(bytes.data() + off); off += sizeof(i32);
        q.yaw_milli = read_i32_le(bytes.data() + off); off += sizeof(i32);

        out.push_back(dequantize_state(q, pos_resolution_m));
    }
    return true;
}

usize packed_size(usize entity_count) noexcept {
    return kHeaderBytes + entity_count * kRecordBytes;  // 4 + 20 * count
}

}  // namespace psynder::net
