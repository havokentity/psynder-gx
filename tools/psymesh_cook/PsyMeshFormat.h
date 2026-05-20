// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 05 mesh cooker .psymesh wire format (v1).
//
// This header is the frozen contract between the offline cooker
// (tools/psymesh_cook, this lane) and the runtime loader (lane 09 /
// lane 10, not yet shipped). The loader is expected to live under
// engine/asset/PsyMeshFormat.h and engine/asset/PsyMeshLoad.{h,cpp};
// until that lane lands, this header is the single source of truth.
//
// ──────────────────────────────────────────────────────────────────────────
//  Byte layout (little-endian, all multi-byte fields LE on disk)
// ──────────────────────────────────────────────────────────────────────────
//
//   ┌──────────────────────────────────────────┐
//   │  PsyMeshFileHeader (52 bytes, see below) │  <-- mmap target #0
//   ├──────────────────────────────────────────┤
//   │  vertex_count × f32[3] positions         │  ALWAYS — 12 B/vertex
//   ├──────────────────────────────────────────┤
//   │  vertex_count × f32[3] normals           │  ALWAYS — 12 B/vertex
//   ├──────────────────────────────────────────┤
//   │  vertex_count × f32[2] uv0               │  ALWAYS — 8  B/vertex
//   ├──────────────────────────────────────────┤
//   │  vertex_count × f32[4] tangents          │  if attr_mask bit 0 — 16 B
//   ├──────────────────────────────────────────┤
//   │  vertex_count × f32[2] uv1               │  if attr_mask bit 1 — 8  B
//   ├──────────────────────────────────────────┤
//   │  vertex_count × u8[4]  rgba              │  if attr_mask bit 2 — 4  B
//   ├──────────────────────────────────────────┤
//   │  vertex_count × { u8[4] joint_idx;       │  if attr_mask bit 3 — 20 B
//   │                   f32[4] joint_w }       │
//   ├──────────────────────────────────────────┤
//   │  index_count   × u32 indices             │
//   ├──────────────────────────────────────────┤
//   │  submesh_count × PsyMeshSubmesh (16 B)   │
//   └──────────────────────────────────────────┘
//
//  The vertex streams are kept separate (SoA per-attribute) so the engine
//  can mmap the file and skip cold attributes (e.g. skinning data when
//  rendering a static instance, lightmap UV1 in a non-lightmapped scene).
//
// ──────────────────────────────────────────────────────────────────────────
//  Version policy
// ──────────────────────────────────────────────────────────────────────────
//
//   v1 (M2, this PR) — initial schema. Bump on any incompatible field add
//   or reorder. Loaders MUST reject mismatched magic + version pairs.
//
// ──────────────────────────────────────────────────────────────────────────
//  Determinism contract
// ──────────────────────────────────────────────────────────────────────────
//
//   The cooker is required to produce byte-identical output for byte-
//   identical input across runs on any LITTLE-ENDIAN, IEEE-754, x86-64
//   / aarch64 host (the project's supported targets — see DESIGN §1.2).
//   The on-disk format is explicitly little-endian and the writer memcpys
//   native u32 / f32 values without byte-swapping; the static_assert at
//   the bottom of this header enforces the matching host expectation at
//   compile time so a hypothetical big-endian build trips the build
//   rather than silently mis-encoding bytes.
//
//   Specifically:
//     * No std::random_device / std::chrono / process-id mixing.
//     * Vertex dedup uses an ordered map keyed on the (pos, normal, uv0)
//       bit-pattern; first-occurrence wins. Insertion order = stream order.
//     * If a helper (e.g. xatlas for UV1 lightmap charts) is added in a
//       future PR, it must be invoked with a fixed seed.
//
//  The header's `source_hash` is FNV-1a over the raw input file bytes
//  (the .obj / .gltf / .glb on disk) and exists for build-system
//  staleness checks, not as a content hash of the cooked output.
//
// ──────────────────────────────────────────────────────────────────────────
//  Unit contract
// ──────────────────────────────────────────────────────────────────────────
//
//   Positions in the file are in METRES. The cooker's --scale option
//   multiplies input positions by a uniform factor; pass 1.0 if the input
//   DCC file already uses metres, or e.g. 0.01 if the input is in cm
//   (most .obj convention for some pipelines).
//
//   Coordinate convention: right-handed, +Y up (Blender / glTF). .obj
//   files are read with the same convention — the cooker does not flip
//   axes.
//
// ──────────────────────────────────────────────────────────────────────────

#pragma once

#include <bit>
#include <cstdint>
#include <limits>

namespace psy::psymesh {

// 'PSYM' little-endian on disk.
inline constexpr std::uint32_t kPsyMeshMagic   = 0x4D595350u;  // 'P','S','Y','M' as LE u32
inline constexpr std::uint16_t kPsyMeshVersion = 1u;

// Attribute mask bits — which optional streams follow the always-on
// pos/normal/uv0 streams.
inline constexpr std::uint32_t kAttrTangent = 1u << 0;
inline constexpr std::uint32_t kAttrUv1     = 1u << 1;
inline constexpr std::uint32_t kAttrColor   = 1u << 2;
inline constexpr std::uint32_t kAttrSkin    = 1u << 3;

#pragma pack(push, 1)

struct PsyMeshFileHeader {
    std::uint32_t magic;          // == kPsyMeshMagic
    std::uint16_t version;        // == kPsyMeshVersion
    std::uint16_t reserved0;      // must be 0
    std::uint32_t vertex_count;
    std::uint32_t index_count;
    std::uint32_t submesh_count;
    std::uint32_t attr_mask;      // bitfield of kAttr* flags above
    float         aabb_min[3];    // post-transform (i.e. after --scale and --center-pivot)
    float         aabb_max[3];
    std::uint32_t source_hash;    // FNV-1a over raw input file bytes (build staleness)
};

struct PsyMeshSubmesh {
    std::uint32_t index_first;
    std::uint32_t index_count;
    std::uint32_t material_slot;  // cooker passes through (0 unless overridden by source)
    std::uint32_t reserved;       // must be 0
};

#pragma pack(pop)

static_assert(sizeof(PsyMeshFileHeader) == 52, "PsyMeshFileHeader v1 wire size is 52 bytes");
static_assert(sizeof(PsyMeshSubmesh)    == 16, "PsyMeshSubmesh v1 wire size is 16 bytes");

// Endianness + IEEE-754 host expectations.  The writer memcpys native
// u32 / f32 values into the blob without byte-swapping; the format is
// explicitly little-endian (DESIGN §1.2 supported targets).  A
// hypothetical big-endian or non-IEEE-754 host would silently mis-encode
// bytes — trip the build instead.  Copilot PR #18 review caught the
// implicit assumption.
static_assert(std::endian::native == std::endian::little,
              "psymesh wire format is little-endian; this host must be too "
              "(supported targets per DESIGN §1.2 are all LE)");
static_assert(std::numeric_limits<float>::is_iec559,
              "psymesh wire format stores native f32 bit-patterns; host "
              "float MUST be IEEE-754 binary32 (every supported target is)");

// Per-vertex byte sizes for each stream — useful when computing the
// expected file size or scanning for a specific attribute.
inline constexpr std::uint32_t kPosBytes     = 12;   // f32[3]
inline constexpr std::uint32_t kNormalBytes  = 12;   // f32[3]
inline constexpr std::uint32_t kUv0Bytes     = 8;    // f32[2]
inline constexpr std::uint32_t kTangentBytes = 16;   // f32[4]
inline constexpr std::uint32_t kUv1Bytes     = 8;    // f32[2]
inline constexpr std::uint32_t kColorBytes   = 4;    // u8[4] (RGBA)
inline constexpr std::uint32_t kSkinBytes    = 20;   // u8[4] joint_idx + f32[4] joint_w

}  // namespace psy::psymesh
