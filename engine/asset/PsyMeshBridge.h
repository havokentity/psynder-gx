// SPDX-License-Identifier: MIT
// Psynder-GX — runtime bridge from psymesh_cook output to .lmm.
//
// The offline `tools/psymesh_cook` path emits `.psymesh`: a deterministic,
// mmap-friendly SoA mesh. The runtime/static-mesh container already consumed
// by lane 05 is `.lmm`: interleaved vertices plus an index stream. This
// header validates the `.psymesh` v1 wire layout and repacks static meshes
// into `.lmm` bytes without depending on the offline tool target.

#pragma once

#include "FormatsIo.h"
#include "core/Types.h"

#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace psynder::asset::psymesh {

// Mirror of tools/psymesh_cook/PsyMeshFormat.h v1. Runtime code keeps its
// own copy so it can load shipped assets without linking the cooker.
inline constexpr u32 kPsyMeshMagic = 0x4D595350u;  // 'P','S','Y','M' LE
inline constexpr u16 kPsyMeshVersion = 1u;

inline constexpr u32 kAttrTangent = 1u << 0;
inline constexpr u32 kAttrUv1 = 1u << 1;
inline constexpr u32 kAttrColor = 1u << 2;
inline constexpr u32 kAttrSkin = 1u << 3;
inline constexpr u32 kKnownAttrMask = kAttrTangent | kAttrUv1 | kAttrColor | kAttrSkin;

#pragma pack(push, 1)

struct PsyMeshFileHeader {
    u32 magic;
    u16 version;
    u16 reserved0;
    u32 vertex_count;
    u32 index_count;
    u32 submesh_count;
    u32 attr_mask;
    f32 aabb_min[3];
    f32 aabb_max[3];
    u32 source_hash;
};

struct PsyMeshSubmesh {
    u32 index_first;
    u32 index_count;
    u32 material_slot;
    u32 reserved;
};

#pragma pack(pop)

static_assert(sizeof(PsyMeshFileHeader) == 52, "PsyMeshFileHeader v1 wire size");
static_assert(sizeof(PsyMeshSubmesh) == 16, "PsyMeshSubmesh v1 wire size");
static_assert(std::endian::native == std::endian::little,
              "psymesh v1 stores little-endian native fields");
static_assert(std::numeric_limits<f32>::is_iec559,
              "psymesh v1 stores IEEE-754 binary32 floats");

struct PsyMeshView {
    PsyMeshFileHeader header{};
    std::span<const u8> positions{};  // vertex_count * f32[3]
    std::span<const u8> normals{};    // vertex_count * f32[3]
    std::span<const u8> uv0{};        // vertex_count * f32[2]
    std::span<const u8> tangents{};   // optional vertex_count * f32[4]
    std::span<const u8> uv1{};        // optional vertex_count * f32[2]
    std::span<const u8> colors{};     // optional vertex_count * u8[4]
    std::span<const u8> skin{};       // optional vertex_count * {u8[4], f32[4]}
    std::span<const u8> indices{};    // index_count * u32
    const u8* submesh_data = nullptr; // submesh_count * PsyMeshSubmesh
    bool valid = false;

    PsyMeshSubmesh submesh(u32 i) const noexcept {
        PsyMeshSubmesh sm{};
        if (i < header.submesh_count) {
            std::memcpy(&sm, submesh_data + usize(i) * sizeof(PsyMeshSubmesh), sizeof(sm));
        }
        return sm;
    }

    explicit operator bool() const noexcept { return valid; }
};

inline bool checked_span(const u8* data, usize bytes, u64& cursor, u64 size,
                         std::span<const u8>& out) noexcept {
    if (size > u64(bytes) - cursor) return false;
    out = std::span<const u8>(data + cursor, static_cast<usize>(size));
    cursor += size;
    return true;
}

inline u32 read_u32_unaligned(const u8* p) noexcept {
    u32 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline bool parse_psymesh(const u8* data, usize bytes, PsyMeshView& out) noexcept {
    out = PsyMeshView{};
    if (!data || bytes < sizeof(PsyMeshFileHeader)) return false;

    PsyMeshFileHeader h{};
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kPsyMeshMagic || h.version != kPsyMeshVersion) return false;
    if (h.reserved0 != 0) return false;
    if ((h.attr_mask & ~kKnownAttrMask) != 0) return false;
    if (h.vertex_count == 0 || h.index_count == 0 || h.submesh_count == 0) return false;
    if (h.index_count % 3u != 0u) return false;

    u64 cursor = sizeof(PsyMeshFileHeader);
    const u64 vc = h.vertex_count;
    if (!checked_span(data, bytes, cursor, vc * 12u, out.positions)) return false;
    if (!checked_span(data, bytes, cursor, vc * 12u, out.normals)) return false;
    if (!checked_span(data, bytes, cursor, vc * 8u, out.uv0)) return false;
    if ((h.attr_mask & kAttrTangent) != 0 &&
        !checked_span(data, bytes, cursor, vc * 16u, out.tangents)) return false;
    if ((h.attr_mask & kAttrUv1) != 0 &&
        !checked_span(data, bytes, cursor, vc * 8u, out.uv1)) return false;
    if ((h.attr_mask & kAttrColor) != 0 &&
        !checked_span(data, bytes, cursor, vc * 4u, out.colors)) return false;
    if ((h.attr_mask & kAttrSkin) != 0 &&
        !checked_span(data, bytes, cursor, vc * 20u, out.skin)) return false;
    if (!checked_span(data, bytes, cursor, u64(h.index_count) * 4u, out.indices)) return false;

    const u64 submesh_bytes = u64(h.submesh_count) * sizeof(PsyMeshSubmesh);
    if (submesh_bytes > u64(bytes) - cursor) return false;
    out.submesh_data = data + cursor;
    cursor += submesh_bytes;
    if (cursor != u64(bytes)) return false;

    for (u32 i = 0; i < h.index_count; ++i) {
        if (read_u32_unaligned(out.indices.data() + usize(i) * 4u) >= h.vertex_count) return false;
    }
    for (u32 i = 0; i < h.submesh_count; ++i) {
        const PsyMeshSubmesh sm = out.submesh(i);
        if (sm.reserved != 0) return false;
        if (u64(sm.index_first) + u64(sm.index_count) > u64(h.index_count)) return false;
        if (sm.index_count % 3u != 0u) return false;
    }

    out.header = h;
    out.valid = true;
    return true;
}

struct LmmBridgeOptions {
    // Used when material_hashes is empty. Pass formats::material_name_hash("crate")
    // to bind the resulting .lmm submesh to a matching crate .lmt material.
    u32 default_material_hash = 0;
    std::span<const u32> material_hashes{};
};

inline bool build_lmm_from_psymesh(const PsyMeshView& src, std::vector<u8>& out,
                                   LmmBridgeOptions opts = {}) {
    out.clear();
    if (!src.valid) return false;
    if (!src.colors.empty() || !src.skin.empty()) return false;  // not representable in .lmm v1
    if (!opts.material_hashes.empty() &&
        opts.material_hashes.size() < src.header.submesh_count) return false;

    formats::LmmWriter w;
    w.vertex_fmt = src.tangents.empty()
        ? formats::LmmVertexFmt::Pos3N3UV2
        : formats::LmmVertexFmt::Pos3N3T4UV2;
    w.vertex_count = src.header.vertex_count;
    w.index_count = src.header.index_count;
    std::memcpy(w.bbox_min, src.header.aabb_min, sizeof(w.bbox_min));
    std::memcpy(w.bbox_max, src.header.aabb_max, sizeof(w.bbox_max));

    const u32 stride = formats::lmm_vertex_stride(w.vertex_fmt);
    w.vertex_data.assign(usize(w.vertex_count) * stride, u8{0});
    for (u32 i = 0; i < w.vertex_count; ++i) {
        u8* dst = w.vertex_data.data() + usize(i) * stride;
        const usize pos = usize(i) * 12u;
        const usize uv = usize(i) * 8u;
        std::memcpy(dst + 0, src.positions.data() + pos, 12u);
        std::memcpy(dst + 12, src.normals.data() + pos, 12u);
        if (!src.tangents.empty()) {
            std::memcpy(dst + 24, src.tangents.data() + usize(i) * 16u, 16u);
            std::memcpy(dst + 40, src.uv0.data() + uv, 8u);
        } else {
            std::memcpy(dst + 24, src.uv0.data() + uv, 8u);
        }
    }

    const u32 index_stride = formats::lmm_index_stride(w.vertex_count);
    w.index_data.assign(usize(w.index_count) * index_stride, u8{0});
    for (u32 i = 0; i < w.index_count; ++i) {
        const u32 idx = read_u32_unaligned(src.indices.data() + usize(i) * 4u);
        if (index_stride == 2u) {
            const u16 idx16 = static_cast<u16>(idx);
            std::memcpy(w.index_data.data() + usize(i) * 2u, &idx16, sizeof(idx16));
        } else {
            std::memcpy(w.index_data.data() + usize(i) * 4u, &idx, sizeof(idx));
        }
    }

    w.submeshes.reserve(src.header.submesh_count);
    for (u32 i = 0; i < src.header.submesh_count; ++i) {
        const PsyMeshSubmesh sm = src.submesh(i);
        const u32 material_hash =
            opts.material_hashes.empty() ? opts.default_material_hash : opts.material_hashes[i];
        w.submeshes.push_back(formats::LmmSubmesh{sm.index_first, sm.index_count,
                                                  material_hash, 0});
    }

    return w.build(out);
}

inline bool build_lmm_from_psymesh(const u8* data, usize bytes, std::vector<u8>& out,
                                   LmmBridgeOptions opts = {}) {
    PsyMeshView view;
    return parse_psymesh(data, bytes, view) && build_lmm_from_psymesh(view, out, opts);
}

}  // namespace psynder::asset::psymesh
