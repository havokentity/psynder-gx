// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 24 tools: lm_mapimport (.map -> .psimport scene).
//
// A one-way bridge that imports a Quake / TrenchBroom ".map" into a
// documented, versioned binary scene file (".psimport") carrying the brush
// set (as convex plane-sets, with full texture projection) and the entity
// key/value dictionaries. It is the "courtesy bridge" referenced by
// tools/CMakeLists.txt + DESIGN §10.8 — the editor's native ".psylevel"
// (engine/editor/core/Serialization.h, magic 'PSLV') is the eventual home,
// but that is an INTERNAL lane-18 header that requires psynder_editor_core
// (off in default + CI builds), so lm_mapimport emits its own self-contained
// intermediate and the orchestrator reconciles at integration (see
// INTEGRATION.txt).
//
// Header-only (every free function `inline`) so the unit tests drive it
// through a relative-path include and run in the default (TOOLS=OFF) build.
//
// ──────────────────────────────────────────────────────────────────────────
//  .psimport wire format (little-endian)
// ──────────────────────────────────────────────────────────────────────────
//
//   PsImportHeader (16 bytes):
//     u32 magic        'PSIM' (0x4D495350)
//     u16 version      kPsImportVersion
//     u16 flags        reserved (0)
//     u32 entity_count
//     u32 payload_size bytes following this header (sanity check)
//   then entity_count entities, each:
//     u32 prop_count
//     prop_count * { u32 key_len; key bytes; u32 val_len; value bytes }
//     u32 brush_count
//     brush_count * Brush:
//        u32 face_count
//        face_count * Face:
//           f32 nx, ny, nz, d           (outward plane, dot(n,p) = d)
//           u8  valve220; u8 pad[3]
//           f32 u_axis[3]; f32 u_offset
//           f32 v_axis[3]; f32 v_offset
//           f32 rotation; f32 scale_x; f32 scale_y
//           u32 tex_len; texture bytes
//
//  Strings are length-prefixed (no terminator). The format is faithful: the
//  brush planes are outward-oriented and the texture projection is preserved
//  so a consumer can re-derive both geometry and UVs.
//
//  Units: metres. --scale multiplies brush plane distances (geometry) but
//  leaves entity property strings (e.g. "origin") verbatim.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "../lm_qbsp/MapSource.h"

namespace psy::lm_mapimport {

inline constexpr std::uint32_t kPsImportMagic = 0x4D495350u;  // 'P','S','I','M' LE
inline constexpr std::uint16_t kPsImportVersion = 1u;

struct ImportOptions {
    std::string input_path;
    std::string output_path;  // "" => no file write
    float scale = 1.0f;
    bool force_overwrite = false;
    bool quiet = false;
    bool print_stats = false;
};

struct ImportStats {
    std::uint64_t bytes_written = 0;
    std::uint32_t entity_count = 0;
    std::uint32_t brush_count = 0;
    std::uint32_t face_count = 0;
    std::uint32_t point_entity_count = 0;  // entities with no brushes
    std::uint32_t source_hash = 0;
};

// ─────────────────────────────────────────────────────────────────────────
// Low-level append helpers (little-endian, length-prefixed strings).
// ─────────────────────────────────────────────────────────────────────────

inline void put_bytes(std::vector<std::uint8_t>& out, const void* src, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(src);
    out.insert(out.end(), p, p + n);
}
inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put_bytes(out, &v, sizeof(v));
}
inline void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    put_bytes(out, &v, sizeof(v));
}
inline void put_f32(std::vector<std::uint8_t>& out, float v) {
    put_bytes(out, &v, sizeof(v));
}
inline void put_string(std::vector<std::uint8_t>& out, std::string_view s) {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    put_bytes(out, s.data(), s.size());
}

// ─────────────────────────────────────────────────────────────────────────
// FNV-1a (source-blob staleness hash).
// ─────────────────────────────────────────────────────────────────────────
inline std::uint32_t fnv1a32(const std::uint8_t* bytes, std::size_t len) noexcept {
    std::uint32_t h = 0x811C9DC5u;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint32_t>(bytes[i]);
        h *= 0x01000193u;
    }
    return h;
}

// ─────────────────────────────────────────────────────────────────────────
// Core import: parsed map -> .psimport blob (no file I/O, for tests).
// ─────────────────────────────────────────────────────────────────────────

inline bool import_map(const lmtools::MapFile& map,
                       const ImportOptions& opts,
                       std::vector<std::uint8_t>& blob,
                       ImportStats* out_stats) {
    std::vector<std::uint8_t> body;
    std::uint32_t brush_total = 0;
    std::uint32_t face_total = 0;
    std::uint32_t point_entities = 0;

    for (const lmtools::MapEntity& ent : map.entities) {
        put_u32(body, static_cast<std::uint32_t>(ent.props.size()));
        for (const auto& kv : ent.props) {
            put_string(body, kv.first);
            put_string(body, kv.second);
        }
        put_u32(body, static_cast<std::uint32_t>(ent.brushes.size()));
        if (ent.brushes.empty()) {
            ++point_entities;
        }
        for (const lmtools::MapBrush& brush : ent.brushes) {
            ++brush_total;
            put_u32(body, static_cast<std::uint32_t>(brush.faces.size()));
            for (const lmtools::MapFace& face : brush.faces) {
                ++face_total;
                put_f32(body, face.plane.n.x);
                put_f32(body, face.plane.n.y);
                put_f32(body, face.plane.n.z);
                put_f32(body, face.plane.d * opts.scale);
                const std::uint8_t valve = face.valve220 ? 1u : 0u;
                const std::uint8_t pad[3] = {0u, 0u, 0u};
                put_bytes(body, &valve, sizeof(valve));
                put_bytes(body, pad, sizeof(pad));
                put_f32(body, face.u_axis.x);
                put_f32(body, face.u_axis.y);
                put_f32(body, face.u_axis.z);
                put_f32(body, face.u_offset);
                put_f32(body, face.v_axis.x);
                put_f32(body, face.v_axis.y);
                put_f32(body, face.v_axis.z);
                put_f32(body, face.v_offset);
                put_f32(body, face.rotation);
                put_f32(body, face.scale_x);
                put_f32(body, face.scale_y);
                put_string(body, face.texture);
            }
        }
    }

    blob.clear();
    put_u32(blob, kPsImportMagic);
    put_u16(blob, kPsImportVersion);
    put_u16(blob, 0u);  // flags
    put_u32(blob, static_cast<std::uint32_t>(map.entities.size()));
    put_u32(blob, static_cast<std::uint32_t>(body.size()));
    put_bytes(blob, body.data(), body.size());

    if (out_stats != nullptr) {
        out_stats->bytes_written = blob.size();
        out_stats->entity_count = static_cast<std::uint32_t>(map.entities.size());
        out_stats->brush_count = brush_total;
        out_stats->face_count = face_total;
        out_stats->point_entity_count = point_entities;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// File-driven import.
// ─────────────────────────────────────────────────────────────────────────

inline bool slurp_file(const std::string& path, std::vector<std::uint8_t>& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        err = "cannot open input file '" + path + "': " + std::strerror(errno);
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) {
        err = "cannot stat input file '" + path + "'";
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (!f) {
            err = "short read from '" + path + "'";
            return false;
        }
    }
    return true;
}

inline bool import(const ImportOptions& opts,
                   ImportStats* out_stats = nullptr,
                   std::vector<std::uint8_t>* out_blob = nullptr) {
    auto log_err = [&](const std::string& msg) {
        std::fprintf(stderr, "lm_mapimport: error: %s\n", msg.c_str());
    };

    std::vector<std::uint8_t> raw;
    std::string err;
    if (!slurp_file(opts.input_path, raw, err)) {
        log_err(err);
        return false;
    }
    const std::uint32_t source_hash = fnv1a32(raw.data(), raw.size());

    lmtools::MapFile map;
    lmtools::MapParseError perr;
    if (!lmtools::parse_map(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()),
                            map,
                            perr)) {
        log_err("parse failed at line " + std::to_string(perr.line) + ": " + perr.message);
        return false;
    }

    std::vector<std::uint8_t> blob;
    ImportStats stats;
    if (!import_map(map, opts, blob, &stats)) {
        log_err("import failed");
        return false;
    }
    stats.source_hash = source_hash;

    if (!opts.output_path.empty()) {
        const std::filesystem::path out_path(opts.output_path);
        std::error_code ec;
        if (std::filesystem::exists(out_path, ec) && !opts.force_overwrite) {
            log_err("output file '" + opts.output_path + "' already exists; pass --force");
            return false;
        }
        if (out_path.has_parent_path()) {
            std::filesystem::create_directories(out_path.parent_path(), ec);
        }
        std::ofstream f(opts.output_path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            log_err("cannot open output file '" + opts.output_path + "': " + std::strerror(errno));
            return false;
        }
        f.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        if (!f) {
            log_err("failed to write output file '" + opts.output_path + "'");
            return false;
        }
    }

    if (out_blob != nullptr) {
        out_blob->insert(out_blob->end(), blob.begin(), blob.end());
    }
    if (out_stats != nullptr) {
        *out_stats = stats;
    }
    return true;
}

}  // namespace psy::lm_mapimport
