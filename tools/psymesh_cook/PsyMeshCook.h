// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 05 mesh cooker public API.
//
// The cooker takes a raw .obj / .gltf / .glb input file and emits a
// deterministic .psymesh binary (see PsyMeshFormat.h for the wire layout).
//
// This header exposes a programmatic entry point so tests can drive the
// cooker without spawning the CLI binary. The CLI's main.cpp is a thin
// argv-parsing shell around `psy::psymesh::cook()`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace psy::psymesh {

enum class InputKind : std::uint32_t {
    Obj  = 0,
    Gltf = 1,
};

struct CookOptions {
    InputKind   input_kind         = InputKind::Obj;
    std::string input_path;            // path on disk; the cooker reads it itself
    std::string output_path;           // where to write the .psymesh; "" = no write
    float       scale                 = 1.0f;    // uniform metric scale
    bool        center_pivot          = false;   // translate AABB centre to origin
    bool        compute_tangents      = true;    // MikkT; default ON
    bool        gen_uv2_lightmap_stub = false;   // M2 stub — emits UV1 mirroring UV0
    bool        force_overwrite       = false;
    bool        print_stats           = false;
    bool        quiet                 = false;
};

struct CookStats {
    std::uint64_t bytes_written        = 0;
    std::uint32_t source_vertex_count  = 0;  // pre-dedup
    std::uint32_t cooked_vertex_count  = 0;  // post-dedup
    std::uint32_t index_count          = 0;
    std::uint32_t submesh_count        = 0;
    std::uint32_t attr_mask            = 0;
    float         aabb_min[3]          = {0, 0, 0};
    float         aabb_max[3]          = {0, 0, 0};
    std::uint32_t source_hash          = 0;
    double        vertex_dedup_ratio   = 0.0;  // cooked / source (1.0 = no shared verts)
    bool          tangent_fallback_used = false;
};

// Cooks the input file into a .psymesh blob.
// On success returns true and (if `out_blob` is non-null) appends the
// cooked file bytes to *out_blob. The blob is the same byte sequence that
// would be written to output_path when output_path is non-empty.
//
// All diagnostic messages go to stderr; final summary (when not quiet)
// goes to stdout.
//
// Returns false on any parser / write / validation error.
bool cook(const CookOptions&  opts,
          CookStats*           out_stats = nullptr,
          std::vector<std::uint8_t>* out_blob = nullptr);

// FNV-1a over a byte range. Exposed for tests + so the CLI can stamp the
// source_hash from the read-in buffer without re-reading the file.
std::uint32_t fnv1a32(const std::uint8_t* bytes, std::size_t len) noexcept;

}  // namespace psy::psymesh
