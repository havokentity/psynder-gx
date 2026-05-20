// SPDX-License-Identifier: MIT
// Psynder — runtime (de)serialization for the cooked asset containers
// `.lmm` (mesh), `.lmt` (texture), `.lma` (audio). Lane 05 owns.
//
// `Formats.h` fixes the on-disk *struct* layout (the v1 wire contract).
// This header is the reference reader + an in-lane writer that round-trip
// those structs byte-for-byte. The canonical offline cookers live in lane
// 24 (`tools/lm_cook` writes .lmm/.lmt/.lma; `tools/lm_bake` writes baked
// lightmaps as .lmt). Both sides MUST agree with the byte layout pinned
// below — this is a cross-lane integration seam.
//
// Header-only + inline (same pattern as LmpakWriter.h) so it links into
// both `psynder_asset` and `psynder_unit` without a dedicated TU. The
// optional zstd dependency stays a single-TU concern: the audio path
// delegates to `lmpak::zstd_compress` / `lmpak::zstd_decompress`, defined
// in LmpakWriter.cpp.
//
// ─── On-disk byte layout (little-endian; every supported target is LE) ───
//
// Common: each file starts with the 16-byte `formats::FileHeader`:
//     off 0  u32 magic          format FOURCC (kLmm/Lmt/LmaMagic)
//     off 4  u16 version        kLmm/Lmt/LmaVersion
//     off 6  u16 flags          format-specific bits
//     off 8  u64 payload_size   == total_file_size - sizeof(FileHeader)
//                               i.e. every byte after this 16-byte preamble,
//                               INCLUDING the rest of the format header.
//
// .lmm  [LmmHeader 56B][LmmSubmesh × submesh_count][vertex bytes][index bytes]
//     - LmmHeader embeds FileHeader at off 0 (magic kLmmMagic).
//     - vertex bytes  = vertex_count * vertex_stride (stride matches fmt:
//                       Pos3N3UV2=32, Pos3N3T4UV2=48).
//     - index bytes   = index_count * (vertex_count<=65535 ? 2 : 4).
//     - sections are contiguous, no gaps; file ends exactly at index end.
//
// .lmt  [LmtHeader 32B][LmtMip × mip_count][palette?][mip0 px][mip1 px]...
//     - LmtMip table immediately follows the 32-byte header (off 32).
//     - palette present iff pixel_fmt==P8: 256*4=1024 bytes at
//       palette_offset (0 when absent).
//     - pixels_offset == LmtMip[0].offset; each LmtMip.offset is absolute
//       from the header start, byte_size == width*height*bytes_per_texel
//       (P8 stores 1-byte indices; palette is separate).
//     - the writer packs sections tightly; the reader trusts the stored
//       offsets and bounds-checks them (tolerates cooker alignment pads).
//
// .lma  [LmaHeader 40B][PCM payload]
//     - LmaHeader is 40 bytes on disk (sizeof, incl. 4 trailing zero pad
//       bytes); PCM payload begins at offset 40.
//     - uncompressed: payload == frame_count*channels*bytes_per_sample,
//       stored verbatim. zstd (kLmaFlagZstd): payload is one zstd frame
//       that inflates to exactly that size.

#pragma once

#include "Formats.h"
#include "LmpakWriter.h"
#include "core/Types.h"

#include <cstring>
#include <span>
#include <vector>

namespace psynder::asset::formats {

// ─── Compile-time wire-size locks ────────────────────────────────────────
// Pin the on-disk header sizes so a stray field edit in Formats.h (the
// frozen v1 contract) cannot silently desync this lane from lane 24.
static_assert(sizeof(FileHeader) == 16, "FileHeader v1 wire size");
static_assert(sizeof(LmmHeader) == 56, "LmmHeader v1 wire size");
static_assert(sizeof(LmmSubmesh) == 16, "LmmSubmesh v1 wire size");
static_assert(sizeof(LmtHeader) == 32, "LmtHeader v1 wire size");
static_assert(sizeof(LmtMip) == 16, "LmtMip v1 wire size");
static_assert(sizeof(LmaHeader) == 40, "LmaHeader v1 wire size");

// ─── Format helpers ──────────────────────────────────────────────────────

// Index element size in bytes: 16-bit while the mesh fits in 65535 verts,
// else 32-bit. A vertex_count of 0 still implies 16-bit (degenerate mesh).
inline constexpr u32 lmm_index_stride(u32 vertex_count) noexcept {
    return vertex_count <= 0xFFFFu ? 2u : 4u;
}

// Canonical bytes-per-vertex for a vertex format. Returns 0 for an
// unrecognized format so callers can reject unknown v1 enum values.
inline constexpr u32 lmm_vertex_stride(LmmVertexFmt fmt) noexcept {
    switch (fmt) {
        case LmmVertexFmt::Pos3N3UV2:
            return 32u;  // float3 pos + float3 normal + float2 uv
        case LmmVertexFmt::Pos3N3T4UV2:
            return 48u;  // + float4 tangent
    }
    return 0u;
}

// Bytes per texel (P8 = 1-byte palette index). Returns 0 for unknown.
inline constexpr u32 lmt_bytes_per_texel(LmtPixelFmt fmt) noexcept {
    switch (fmt) {
        case LmtPixelFmt::P8:
            return 1u;
        case LmtPixelFmt::RGB565:
        case LmtPixelFmt::RG88:
            return 2u;
        case LmtPixelFmt::RGBA8:
            return 4u;
    }
    return 0u;
}

inline constexpr u32 kLmtPaletteBytes = 256u * 4u;  // 256-entry RGBA8 palette

// Bytes per sample for an audio sample format. Returns 0 for unknown.
inline constexpr u32 lma_bytes_per_sample(LmaSampleFmt fmt) noexcept {
    switch (fmt) {
        case LmaSampleFmt::PCM_S16:
            return 2u;
        case LmaSampleFmt::PCM_F32:
            return 4u;
    }
    return 0u;
}

// ─── Readers (zero-copy views into the source blob) ──────────────────────
//
// All `parse_*` are noexcept, allocate nothing, and return spans pointing
// into `data`. The returned view is valid only while `data` outlives it.
// Size math is done in u64 against the remaining byte budget so a hostile
// or truncated file cannot overflow into an out-of-bounds span.

struct LmmView {
    LmmHeader header{};
    std::span<const LmmSubmesh> submeshes{};
    std::span<const u8> vertices{};  // vertex_count * vertex_stride bytes
    std::span<const u8> indices{};   // index_count * index_stride bytes
    u32 index_stride = 0;            // 2 or 4
    bool valid = false;
    explicit operator bool() const noexcept { return valid; }
};

inline bool parse_lmm(const u8* data, usize bytes, LmmView& out) noexcept {
    out = LmmView{};
    if (!data || bytes < sizeof(LmmHeader)) return false;

    LmmHeader h{};
    std::memcpy(&h, data, sizeof(h));
    if (h.file.magic != kLmmMagic || h.file.version != kLmmVersion) return false;
    if (h.file.payload_size != u64(bytes) - sizeof(FileHeader)) return false;

    const u32 vstride = lmm_vertex_stride(h.vertex_fmt);
    if (vstride == 0 || vstride != h.vertex_stride) return false;
    const u32 istride = lmm_index_stride(h.vertex_count);

    const u64 sub_bytes = u64(h.submesh_count) * sizeof(LmmSubmesh);
    const u64 vtx_bytes = u64(h.vertex_count) * u64(vstride);
    const u64 idx_bytes = u64(h.index_count) * u64(istride);

    // Walk the contiguous sections, charging each against the remaining
    // budget (subtraction avoids u64 overflow on hostile counts).
    u64 cursor = sizeof(LmmHeader);
    if (sub_bytes > u64(bytes) - cursor) return false;
    const u64 sub_off = cursor;
    cursor += sub_bytes;
    if (vtx_bytes > u64(bytes) - cursor) return false;
    const u64 vtx_off = cursor;
    cursor += vtx_bytes;
    if (idx_bytes > u64(bytes) - cursor) return false;
    const u64 idx_off = cursor;
    cursor += idx_bytes;
    if (cursor != u64(bytes)) return false;  // no trailing slack

    out.header = h;
    out.submeshes = std::span<const LmmSubmesh>(
        reinterpret_cast<const LmmSubmesh*>(data + sub_off), h.submesh_count);
    out.vertices = std::span<const u8>(data + vtx_off, static_cast<usize>(vtx_bytes));
    out.indices = std::span<const u8>(data + idx_off, static_cast<usize>(idx_bytes));
    out.index_stride = istride;
    out.valid = true;
    return true;
}

struct LmtView {
    LmtHeader header{};
    std::span<const u8> palette{};    // kLmtPaletteBytes when P8, else empty
    std::span<const LmtMip> mips{};
    const u8* base = nullptr;         // header start; mip i px at base+mip.offset
    bool valid = false;

    std::span<const u8> mip_pixels(u32 i) const noexcept {
        if (!valid || i >= mips.size()) return {};
        return std::span<const u8>(base + mips[i].offset, mips[i].byte_size);
    }
    explicit operator bool() const noexcept { return valid; }
};

inline bool parse_lmt(const u8* data, usize bytes, LmtView& out) noexcept {
    out = LmtView{};
    if (!data || bytes < sizeof(LmtHeader)) return false;

    LmtHeader h{};
    std::memcpy(&h, data, sizeof(h));
    if (h.file.magic != kLmtMagic || h.file.version != kLmtVersion) return false;
    if (h.file.payload_size != u64(bytes) - sizeof(FileHeader)) return false;

    const u32 bpt = lmt_bytes_per_texel(h.pixel_fmt);
    if (bpt == 0) return false;
    if (h.mip_count < 1 || h.mip_count > 16) return false;

    // Mip table sits immediately after the fixed header.
    const u64 table_off = sizeof(LmtHeader);
    const u64 table_bytes = u64(h.mip_count) * sizeof(LmtMip);
    if (table_bytes > u64(bytes) - table_off) return false;
    const auto* mips = reinterpret_cast<const LmtMip*>(data + table_off);

    // Palette: present iff P8. Bounds-check the stored offset.
    const bool is_p8 = (h.pixel_fmt == LmtPixelFmt::P8);
    if (is_p8) {
        if (h.palette_offset == 0) return false;
        if (u64(h.palette_offset) > u64(bytes)) return false;
        if (u64(kLmtPaletteBytes) > u64(bytes) - h.palette_offset) return false;
    } else if (h.palette_offset != 0) {
        return false;  // non-paletted formats must not carry a palette
    }

    // Validate each mip: dimensions self-consistent, payload in bounds.
    const u32 mip_count = h.mip_count;
    for (u32 i = 0; i < mip_count; ++i) {
        const LmtMip& m = mips[i];
        const u64 expect = u64(m.width) * u64(m.height) * u64(bpt);
        if (m.byte_size != expect) return false;
        if (u64(m.offset) > u64(bytes)) return false;
        if (u64(m.byte_size) > u64(bytes) - m.offset) return false;
    }
    // mip 0 dimensions must match the header, and pixels_offset must point
    // at mip 0's payload.
    if (mips[0].width != u32(h.width) || mips[0].height != u32(h.height)) return false;
    if (h.pixels_offset != mips[0].offset) return false;

    out.header = h;
    out.mips = std::span<const LmtMip>(mips, h.mip_count);
    if (is_p8) {
        out.palette = std::span<const u8>(data + h.palette_offset, kLmtPaletteBytes);
    }
    out.base = data;
    out.valid = true;
    return true;
}

struct LmaView {
    LmaHeader header{};
    std::span<const u8> stored{};  // on-disk payload (zstd frame if compressed)
    bool zstd = false;
    bool valid = false;

    // Decoded PCM size: frame_count * channels * bytes_per_sample.
    u64 decoded_bytes() const noexcept {
        return u64(header.frame_count) * u64(header.channels) *
               u64(lma_bytes_per_sample(header.sample_fmt));
    }

    // Materialize the PCM into `pcm` (resized to decoded_bytes()),
    // decompressing if needed. Returns false on a zstd/size error.
    bool decode(std::vector<u8>& pcm) const {
        if (!valid) return false;
        const u64 need = decoded_bytes();
        pcm.resize(static_cast<usize>(need));
        if (!zstd) {
            if (stored.size() != need) return false;
            if (need) std::memcpy(pcm.data(), stored.data(), static_cast<usize>(need));
            return true;
        }
        return lmpak::zstd_decompress(stored.data(), stored.size(), pcm.data(),
                                      static_cast<usize>(need));
    }
    explicit operator bool() const noexcept { return valid; }
};

inline bool parse_lma(const u8* data, usize bytes, LmaView& out) noexcept {
    out = LmaView{};
    if (!data || bytes < sizeof(LmaHeader)) return false;

    LmaHeader h{};
    std::memcpy(&h, data, sizeof(h));
    if (h.file.magic != kLmaMagic || h.file.version != kLmaVersion) return false;
    if (h.file.payload_size != u64(bytes) - sizeof(FileHeader)) return false;

    if (lma_bytes_per_sample(h.sample_fmt) == 0) return false;
    if (h.channels != 1 && h.channels != 2) return false;

    const bool is_zstd = (h.file.flags & kLmaFlagZstd) != 0;
    const u64 stored_bytes = u64(bytes) - sizeof(LmaHeader);
    const u64 decoded =
        u64(h.frame_count) * u64(h.channels) * u64(lma_bytes_per_sample(h.sample_fmt));
    if (!is_zstd && stored_bytes != decoded) return false;
    if ((h.file.flags & kLmaFlagLoop) != 0) {
        if (h.loop_start >= h.loop_end || h.loop_end > h.frame_count) return false;
    }

    out.header = h;
    out.stored = std::span<const u8>(data + sizeof(LmaHeader), static_cast<usize>(stored_bytes));
    out.zstd = is_zstd;
    out.valid = true;
    return true;
}

// ─── Writers (build the canonical byte-exact layout) ─────────────────────
//
// These serialize already-cooked data — they are NOT the obj/png/wav
// cookers (those are lane 24). Headers are value-initialized so reserved /
// padding bytes are deterministically zero, making round-trips bit-stable.

struct LmmWriter {
    LmmVertexFmt vertex_fmt = LmmVertexFmt::Pos3N3UV2;
    u32 vertex_count = 0;
    u32 index_count = 0;
    std::vector<u8> vertex_data;  // must equal vertex_count * vertex_stride
    std::vector<u8> index_data;   // must equal index_count * index_stride
    std::vector<LmmSubmesh> submeshes;
    f32 bbox_min[3] = {0.0f, 0.0f, 0.0f};
    f32 bbox_max[3] = {0.0f, 0.0f, 0.0f};
    u16 flags = 0;

    bool build(std::vector<u8>& out) const {
        const u32 vstride = lmm_vertex_stride(vertex_fmt);
        if (vstride == 0) return false;
        if (submeshes.size() > 0xFFFFu) return false;
        if (vertex_data.size() != u64(vertex_count) * vstride) return false;
        if (index_data.size() != u64(index_count) * lmm_index_stride(vertex_count)) return false;

        const u64 sub_bytes = u64(submeshes.size()) * sizeof(LmmSubmesh);
        const u64 total =
            u64(sizeof(LmmHeader)) + sub_bytes + vertex_data.size() + index_data.size();

        LmmHeader h{};
        h.file.magic = kLmmMagic;
        h.file.version = kLmmVersion;
        h.file.flags = flags;
        h.file.payload_size = total - sizeof(FileHeader);
        h.vertex_count = vertex_count;
        h.index_count = index_count;
        h.vertex_fmt = vertex_fmt;
        h.submesh_count = static_cast<u16>(submeshes.size());
        h.vertex_stride = vstride;
        std::memcpy(h.bbox_min, bbox_min, sizeof(bbox_min));
        std::memcpy(h.bbox_max, bbox_max, sizeof(bbox_max));

        out.assign(static_cast<usize>(total), u8{0});
        usize w = 0;
        std::memcpy(out.data() + w, &h, sizeof(h));
        w += sizeof(h);
        if (!submeshes.empty()) {
            std::memcpy(out.data() + w, submeshes.data(), static_cast<usize>(sub_bytes));
            w += static_cast<usize>(sub_bytes);
        }
        if (!vertex_data.empty()) {
            std::memcpy(out.data() + w, vertex_data.data(), vertex_data.size());
            w += vertex_data.size();
        }
        if (!index_data.empty()) {
            std::memcpy(out.data() + w, index_data.data(), index_data.size());
        }
        return true;
    }
};

struct LmtMipSrc {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels;  // width * height * bytes_per_texel
};

struct LmtWriter {
    LmtPixelFmt pixel_fmt = LmtPixelFmt::RGBA8;
    u16 flags = 0;
    std::vector<u8> palette;       // exactly kLmtPaletteBytes when P8, else empty
    std::vector<LmtMipSrc> mips;   // mip 0 first; 1..16 entries

    bool build(std::vector<u8>& out) const {
        const u32 bpt = lmt_bytes_per_texel(pixel_fmt);
        if (bpt == 0) return false;
        if (mips.empty() || mips.size() > 16) return false;

        const bool is_p8 = (pixel_fmt == LmtPixelFmt::P8);
        if (is_p8) {
            if (palette.size() != kLmtPaletteBytes) return false;
        } else if (!palette.empty()) {
            return false;
        }
        for (const auto& m : mips) {
            if (m.width > 0xFFFFu || m.height > 0xFFFFu) return false;  // header dims are u16
            if (m.pixels.size() != u64(m.width) * u64(m.height) * bpt) return false;
        }

        const u64 mip_count = mips.size();
        const u64 table_off = sizeof(LmtHeader);
        const u64 palette_off = is_p8 ? table_off + mip_count * sizeof(LmtMip) : 0;
        const u64 pixels_off = table_off + mip_count * sizeof(LmtMip) + (is_p8 ? kLmtPaletteBytes : 0);

        u64 total = pixels_off;
        for (const auto& m : mips) total += m.pixels.size();
        if (total > 0xFFFFFFFFull) return false;  // palette/pixels/mip offsets are u32

        LmtHeader h{};
        h.file.magic = kLmtMagic;
        h.file.version = kLmtVersion;
        h.file.flags = flags;
        h.file.payload_size = total - sizeof(FileHeader);
        h.width = static_cast<u16>(mips[0].width);
        h.height = static_cast<u16>(mips[0].height);
        h.mip_count = static_cast<u8>(mip_count);
        h.pixel_fmt = pixel_fmt;
        h.palette_offset = static_cast<u32>(palette_off);
        h.pixels_offset = static_cast<u32>(pixels_off);

        out.assign(static_cast<usize>(total), u8{0});
        std::memcpy(out.data(), &h, sizeof(h));

        u64 cursor = pixels_off;
        u8* table = out.data() + table_off;
        for (u64 i = 0; i < mip_count; ++i) {
            LmtMip m{};
            m.width = mips[i].width;
            m.height = mips[i].height;
            m.offset = static_cast<u32>(cursor);
            m.byte_size = static_cast<u32>(mips[i].pixels.size());
            std::memcpy(table + i * sizeof(LmtMip), &m, sizeof(m));
            if (!mips[i].pixels.empty()) {
                std::memcpy(out.data() + cursor, mips[i].pixels.data(), mips[i].pixels.size());
            }
            cursor += mips[i].pixels.size();
        }
        if (is_p8) {
            std::memcpy(out.data() + palette_off, palette.data(), kLmtPaletteBytes);
        }
        return true;
    }
};

struct LmaWriter {
    u32 sample_rate = 48000;
    LmaSampleFmt sample_fmt = LmaSampleFmt::PCM_S16;
    u16 channels = 1;
    u32 loop_start = 0;
    u32 loop_end = 0;
    bool loop = false;
    bool streamed = false;
    bool compress = false;  // request zstd; build() fails if unavailable
    int zstd_level = 9;
    std::vector<u8> pcm;  // frame_count * channels * bytes_per_sample

    bool build(std::vector<u8>& out) const {
        const u32 bps = lma_bytes_per_sample(sample_fmt);
        if (bps == 0) return false;
        if (channels != 1 && channels != 2) return false;
        const u64 frame_bytes = u64(channels) * bps;
        if (frame_bytes == 0 || pcm.size() % frame_bytes != 0) return false;
        const u64 frame_count = pcm.size() / frame_bytes;
        if (frame_count > 0xFFFFFFFFull) return false;
        if (loop) {
            if (loop_start >= loop_end || u64(loop_end) > frame_count) return false;
        }

        u16 flags = 0;
        if (loop) flags |= kLmaFlagLoop;
        if (streamed) flags |= kLmaFlagStreamed;

        std::vector<u8> payload;
        if (compress) {
            if (!lmpak::zstd_compress(pcm.data(), pcm.size(), zstd_level, payload)) return false;
            flags |= kLmaFlagZstd;
        } else {
            payload = pcm;
        }

        const u64 total = u64(sizeof(LmaHeader)) + payload.size();

        LmaHeader h{};
        h.file.magic = kLmaMagic;
        h.file.version = kLmaVersion;
        h.file.flags = flags;
        h.file.payload_size = total - sizeof(FileHeader);
        h.sample_rate = sample_rate;
        h.frame_count = static_cast<u32>(frame_count);
        h.sample_fmt = sample_fmt;
        h.channels = channels;
        h.loop_start = loop ? loop_start : 0;
        h.loop_end = loop ? loop_end : 0;

        out.assign(static_cast<usize>(total), u8{0});
        std::memcpy(out.data(), &h, sizeof(h));
        if (!payload.empty()) {
            std::memcpy(out.data() + sizeof(LmaHeader), payload.data(), payload.size());
        }
        return true;
    }
};

}  // namespace psynder::asset::formats
