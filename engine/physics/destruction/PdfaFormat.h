// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/destruction/PdfaFormat.h
//
// Lane 17 — .pdfa (Psynder Destruction Asset) file format descriptor.
//
// This header defines the on-disk layout of pre-fractured destruction assets.
// It is a FORMAT DESCRIPTOR only — no parser, no serializer. Those come in M7.
//
// Unit conventions (DESIGN-PSYNDER-GX.md §14 + AGENTS.md):
//   - Chunk masses: kilograms (kg).
//   - Joint strengths: Newtons (N) for translational joints;
//     Newton-metres (N·m) for torsional joints (see JointStrengthPrecision).
//   - Positions: metres (m), matching 1 world unit = 1 metre.
//   - -fno-fast-math is enforced by the lane CMakeLists.txt for all physics TUs.
//
// File layout (little-endian, no padding between sections):
//   PdfaFileHeader
//   PdfaChunkDesc[header.chunk_count]
//   PdfaJointDesc[header.joint_count]
//
// See DESIGN-PSYNDER-GX.md §10.1 for the authoring pipeline description.

#pragma once

#include <cstdint>

namespace psynder::physics::destruction {

// ─────────────────────────────────────────────────────────────────────────────
// Four-character codes
// ─────────────────────────────────────────────────────────────────────────────

// Magic: "PDFA" (0x41464450 in little-endian u32)
inline constexpr std::uint32_t kPdfaMagic  = 0x41464450u;
inline constexpr std::uint32_t kPdfaVersion = 1u;

// ─────────────────────────────────────────────────────────────────────────────
// Joint-strength precision tag
//
// Records whether the joint_strength_raw values in PdfaJointDesc are stored as
// force (N) or torque (N·m).  Per-asset: all joints in one file use the same
// precision tag (authors choose the dominant mode at export time).
// ─────────────────────────────────────────────────────────────────────────────
enum class JointStrengthPrecision : std::uint8_t {
    // joint_strength_raw is a 32-bit IEEE float in Newtons (N).
    // Used for translational breakage (tension / compression).
    ForceNewtons    = 0,

    // joint_strength_raw is a 32-bit IEEE float in Newton-metres (N·m).
    // Used for torsional/bending breakage (e.g. structural columns).
    TorqueNewtonMetres = 1,
};

// ─────────────────────────────────────────────────────────────────────────────
// PdfaFileHeader — first sizeof(PdfaFileHeader) bytes of the file.
// ─────────────────────────────────────────────────────────────────────────────
struct PdfaFileHeader {
    // Magic number: must equal kPdfaMagic.
    std::uint32_t magic;

    // Format version: must equal kPdfaVersion.
    std::uint32_t version;

    // Number of PdfaChunkDesc entries immediately following this header.
    std::uint32_t chunk_count;

    // Number of PdfaJointDesc entries following the chunk array.
    std::uint32_t joint_count;

    // Strength precision shared by all joints in this asset.
    JointStrengthPrecision strength_precision;

    // Reserved; must be zero on write, ignored on read.
    std::uint8_t  reserved[3];

    // CRC-32 over bytes [sizeof(PdfaFileHeader), end-of-file).
    // Zero means "not checked" (debug exports).
    std::uint32_t payload_crc32;
};
static_assert(sizeof(PdfaFileHeader) == 24,
    "PdfaFileHeader size must be 24 bytes; update version if you change this.");

// ─────────────────────────────────────────────────────────────────────────────
// PdfaChunkDesc — describes one pre-fractured rigid-body chunk.
//
// Indexing: chunk 0 is the "root" / top-level piece. The connectivity graph
// (joints) references chunks by their 0-based index into this array.
// ─────────────────────────────────────────────────────────────────────────────
struct PdfaChunkDesc {
    // Reference to the mesh for this chunk, stored as a 64-bit hash of the VFS
    // path to the cooked mesh asset (e.g. the .lmpak entry path).
    // Zero means "no mesh" (invisible structural phantom chunk).
    std::uint64_t mesh_ref_hash;

    // Mass of this chunk in kilograms (kg).  Must be > 0.
    // Derived from authored volume × material density at export time.
    float mass_kg;

    // Rest-pose centre-of-mass position in asset-local space (metres).
    float local_position[3];   // [x, y, z]

    // Rest-pose orientation as a unit quaternion in asset-local space.
    // Layout: [x, y, z, w] (matches Jolt's JoltPhysics convention).
    float local_quat[4];       // [x, y, z, w]

    // Load-bearing flag: 1 if removing this chunk could trigger gravity-driven
    // cascade even with no impulse applied.  Used by the structural solver.
    std::uint8_t  load_bearing;

    // Reserved; must be zero on write.
    std::uint8_t  reserved[3];
};
static_assert(sizeof(PdfaChunkDesc) == 48,
    "PdfaChunkDesc size must be 48 bytes; update version if you change this.");

// ─────────────────────────────────────────────────────────────────────────────
// PdfaJointDesc — describes one breakable joint connecting two chunks.
//
// A joint is directional only in the sense that chunk_a < chunk_b is a soft
// convention (not enforced); the solver treats them symmetrically.
// ─────────────────────────────────────────────────────────────────────────────
struct PdfaJointDesc {
    // Indices of the two connected chunks (0-based into the chunk array).
    std::uint32_t chunk_a;
    std::uint32_t chunk_b;

    // Strength threshold: in N (if strength_precision == ForceNewtons) or
    // N·m (if strength_precision == TorqueNewtonMetres).
    // Joint breaks when accumulated stress exceeds this value in a single tick.
    float joint_strength_raw;

    // Damping coefficient applied to stress propagation across this joint.
    // Range [0, 1]: 0 = no damping (full pass-through), 1 = full absorption.
    float stress_damping;

    // Reserved; must be zero on write.
    std::uint32_t reserved;
};
static_assert(sizeof(PdfaJointDesc) == 20,
    "PdfaJointDesc size must be 20 bytes; update version if you change this.");

} // namespace psynder::physics::destruction
