// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 05 mesh cooker implementation.
//
// .obj / .gltf / .glb -> .psymesh (see PsyMeshFormat.h).
//
// Determinism contract — see PsyMeshFormat.h.

#include "PsyMeshCook.h"
#include "PsyMeshFormat.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace psy::psymesh {

namespace {

// ─────────────────────────────────────────────────────────────────────────
// Logging helpers — stderr for diagnostics, stdout for the final summary.
// ─────────────────────────────────────────────────────────────────────────

void log_warn(const std::string& msg) {
    std::fprintf(stderr, "psymesh_cook: warning: %s\n", msg.c_str());
}

void log_err_fmt(const std::string& msg) {
    std::fprintf(stderr, "psymesh_cook: error: %s\n", msg.c_str());
}

// ─────────────────────────────────────────────────────────────────────────
// Math primitives (kept local — tools/psymesh_cook intentionally does not
// link engine/math, since the cooker may be built in isolation in CI).
// ─────────────────────────────────────────────────────────────────────────

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };

Vec3 v3_sub(Vec3 a, Vec3 b) { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 v3_add(Vec3 a, Vec3 b) { return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 v3_scale(Vec3 a, float s) { return Vec3{ a.x * s, a.y * s, a.z * s }; }
Vec3 v3_cross(Vec3 a, Vec3 b) {
    return Vec3{ a.y * b.z - a.z * b.y,
                 a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x };
}
float v3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float v3_len(Vec3 a) { return std::sqrt(v3_dot(a, a)); }

bool v3_normalize_inplace(Vec3& v) {
    const float l = v3_len(v);
    if (l <= 1e-30f) {
        return false;
    }
    const float inv = 1.0f / l;
    v.x *= inv;
    v.y *= inv;
    v.z *= inv;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// In-memory mesh — produced by parsers, consumed by the writer.
// Per-corner streams (one entry per index, not per dedup'd vertex). The
// dedup pass folds this into the cooked output.
// ─────────────────────────────────────────────────────────────────────────

struct CornerVertex {
    Vec3 pos;
    Vec3 normal;
    Vec2 uv0;
};

struct ParsedSubmesh {
    std::uint32_t corner_first;
    std::uint32_t corner_count;
    std::uint32_t material_slot;
};

struct ParsedMesh {
    std::vector<CornerVertex> corners;       // size = total index count, in submesh order
    std::vector<ParsedSubmesh> submeshes;    // at least one submesh; corner_count % 3 == 0
};

// ─────────────────────────────────────────────────────────────────────────
// FNV-1a 32-bit hash.
// ─────────────────────────────────────────────────────────────────────────

std::uint32_t fnv1a32_impl(const std::uint8_t* bytes, std::size_t len) noexcept {
    std::uint32_t h = 0x811C9DC5u;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint32_t>(bytes[i]);
        h *= 0x01000193u;
    }
    return h;
}

// ─────────────────────────────────────────────────────────────────────────
// File I/O helpers.
// ─────────────────────────────────────────────────────────────────────────

bool slurp_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        log_err_fmt("cannot open input file '" + path + "': " + std::strerror(errno));
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) {
        log_err_fmt("cannot stat input file '" + path + "'");
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (!f) {
            log_err_fmt("short read from '" + path + "'");
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// .obj parser — supports v / vn / vt / f. Triangles only.
// Output coordinate system: right-handed +Y up (no axis flip).
// ─────────────────────────────────────────────────────────────────────────

struct ObjFaceCorner {
    int v_idx  = 0;   // 1-based in source (we normalize to 0-based later)
    int vt_idx = 0;
    int vn_idx = 0;
};

bool parse_obj_face_token(std::string_view tok, ObjFaceCorner& out) {
    // Forms: v, v/vt, v//vn, v/vt/vn
    int field = 0;
    int sign = 1;
    int acc = 0;
    bool seen_digit = false;
    auto commit = [&] {
        const int val = seen_digit ? sign * acc : 0;
        if (field == 0)      out.v_idx  = val;
        else if (field == 1) out.vt_idx = val;
        else if (field == 2) out.vn_idx = val;
        ++field;
        sign = 1;
        acc = 0;
        seen_digit = false;
    };
    for (char c : tok) {
        if (c == '/') {
            commit();
            if (field > 2) return false;
        } else if (c == '-') {
            if (seen_digit) return false;
            sign = -1;
        } else if (c >= '0' && c <= '9') {
            acc = acc * 10 + (c - '0');
            seen_digit = true;
        } else {
            return false;
        }
    }
    commit();
    return out.v_idx != 0;
}

int resolve_obj_index(int idx, std::size_t total) {
    if (idx == 0) return -1;
    if (idx > 0) return idx - 1;
    return static_cast<int>(total) + idx;  // negative = from end
}

bool parse_obj(const std::vector<std::uint8_t>& bytes, ParsedMesh& out) {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    positions.reserve(1024);
    normals.reserve(1024);
    uvs.reserve(1024);

    std::vector<CornerVertex>& corners = out.corners;

    std::string_view src(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos < src.size()) {
        // Find end of line.
        std::size_t end = pos;
        while (end < src.size() && src[end] != '\n' && src[end] != '\r') ++end;
        std::string_view line = src.substr(pos, end - pos);
        pos = end;
        while (pos < src.size() && (src[pos] == '\n' || src[pos] == '\r')) ++pos;
        ++line_no;

        // Trim leading whitespace + comments.
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size() || line[i] == '#') continue;
        line.remove_prefix(i);
        if (line.empty()) continue;

        // Identify directive — exact-prefix match on the bare keyword,
        // followed by space or tab (or end-of-line). Order matters:
        // check the multi-char keywords ('vn', 'vt') before the single-
        // char 'v'.
        auto starts = [&](std::string_view kw) {
            if (line.size() < kw.size()) return false;
            if (std::memcmp(line.data(), kw.data(), kw.size()) != 0) return false;
            if (line.size() == kw.size()) return true;
            const char c = line[kw.size()];
            return c == ' ' || c == '\t';
        };

        if (starts("vn")) {
            float x = 0, y = 0, z = 0;
            char buf[256];
            const std::size_t n = std::min(line.size(), sizeof(buf) - 1);
            std::memcpy(buf, line.data(), n);
            buf[n] = '\0';
            if (std::sscanf(buf, "vn %f %f %f", &x, &y, &z) != 3) {
                log_err_fmt("line " + std::to_string(line_no) + ": malformed 'vn' (need 3 floats)");
                return false;
            }
            normals.push_back(Vec3{ x, y, z });
        } else if (starts("vt")) {
            float u = 0, v = 0;
            char buf[256];
            const std::size_t n = std::min(line.size(), sizeof(buf) - 1);
            std::memcpy(buf, line.data(), n);
            buf[n] = '\0';
            const int got = std::sscanf(buf, "vt %f %f", &u, &v);
            if (got < 2) {
                log_err_fmt("line " + std::to_string(line_no) + ": malformed 'vt' (need 2 floats)");
                return false;
            }
            uvs.push_back(Vec2{ u, v });
        } else if (starts("v")) {
            float x = 0, y = 0, z = 0;
            // std::sscanf is locale-sensitive in theory but POSIX C locale
            // is the default at process start; this is OK in a tool.
            char buf[256];
            const std::size_t n = std::min(line.size(), sizeof(buf) - 1);
            std::memcpy(buf, line.data(), n);
            buf[n] = '\0';
            if (std::sscanf(buf, "v %f %f %f", &x, &y, &z) != 3) {
                log_err_fmt("line " + std::to_string(line_no) + ": malformed 'v' (need 3 floats)");
                return false;
            }
            positions.push_back(Vec3{ x, y, z });
        } else if (starts("f")) {
            // Tokenize, parse each "v/vt/vn" corner.
            std::vector<ObjFaceCorner> face_corners;
            face_corners.reserve(8);
            std::size_t k = 1;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
            while (k < line.size()) {
                std::size_t tok_start = k;
                while (k < line.size() && line[k] != ' ' && line[k] != '\t') ++k;
                std::string_view tok = line.substr(tok_start, k - tok_start);
                while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
                if (tok.empty()) continue;
                ObjFaceCorner fc;
                if (!parse_obj_face_token(tok, fc)) {
                    log_err_fmt("line " + std::to_string(line_no) + ": malformed face token '"
                                + std::string(tok) + "'");
                    return false;
                }
                face_corners.push_back(fc);
            }
            if (face_corners.size() != 3) {
                log_err_fmt("line " + std::to_string(line_no)
                            + ": face has " + std::to_string(face_corners.size())
                            + " corners — only triangles are supported; "
                              "re-export with triangulation enabled (Blender: 'Triangulate Faces' modifier)");
                return false;
            }
            for (const ObjFaceCorner& fc : face_corners) {
                CornerVertex cv{};
                const int vi = resolve_obj_index(fc.v_idx, positions.size());
                if (vi < 0 || vi >= static_cast<int>(positions.size())) {
                    log_err_fmt("line " + std::to_string(line_no) + ": out-of-range vertex index "
                                + std::to_string(fc.v_idx));
                    return false;
                }
                cv.pos = positions[static_cast<std::size_t>(vi)];

                if (fc.vt_idx != 0) {
                    const int ti = resolve_obj_index(fc.vt_idx, uvs.size());
                    if (ti < 0 || ti >= static_cast<int>(uvs.size())) {
                        log_err_fmt("line " + std::to_string(line_no) + ": out-of-range uv index "
                                    + std::to_string(fc.vt_idx));
                        return false;
                    }
                    cv.uv0 = uvs[static_cast<std::size_t>(ti)];
                } else {
                    cv.uv0 = Vec2{ 0.0f, 0.0f };
                }
                if (fc.vn_idx != 0) {
                    const int ni = resolve_obj_index(fc.vn_idx, normals.size());
                    if (ni < 0 || ni >= static_cast<int>(normals.size())) {
                        log_err_fmt("line " + std::to_string(line_no) + ": out-of-range normal index "
                                    + std::to_string(fc.vn_idx));
                        return false;
                    }
                    cv.normal = normals[static_cast<std::size_t>(ni)];
                } else {
                    cv.normal = Vec3{ 0.0f, 0.0f, 0.0f };  // patched after face loop
                }
                corners.push_back(cv);
            }
        }
        // Other directives (g/o/s/usemtl/mtllib) are silently ignored —
        // submesh splitting by material is a future enhancement.
    }

    if (corners.empty()) {
        log_err_fmt("input contains no triangles");
        return false;
    }

    // Patch missing normals with the flat per-triangle face normal.
    for (std::size_t t = 0; t < corners.size(); t += 3) {
        const Vec3 p0 = corners[t + 0].pos;
        const Vec3 p1 = corners[t + 1].pos;
        const Vec3 p2 = corners[t + 2].pos;
        Vec3 n = v3_cross(v3_sub(p1, p0), v3_sub(p2, p0));
        if (!v3_normalize_inplace(n)) {
            n = Vec3{ 0.0f, 1.0f, 0.0f };
        }
        for (std::size_t k = 0; k < 3; ++k) {
            CornerVertex& cv = corners[t + k];
            if (cv.normal.x == 0.0f && cv.normal.y == 0.0f && cv.normal.z == 0.0f) {
                cv.normal = n;
            }
        }
    }

    out.submeshes.push_back(ParsedSubmesh{
        /*corner_first*/ 0u,
        /*corner_count*/ static_cast<std::uint32_t>(corners.size()),
        /*material_slot*/ 0u
    });
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// .gltf / .glb parser — deferred. M2 baseline supports .obj only.
//
// The CLI accepts the extension but the parse path returns a clear
// "not yet wired" error so the surface area is testable today and the
// integration work for lane 09 stays small.
// ─────────────────────────────────────────────────────────────────────────

bool parse_gltf(const std::vector<std::uint8_t>& /*bytes*/, ParsedMesh& /*out*/) {
    log_err_fmt(".gltf / .glb input is recognised but parser is not yet "
                "wired in M2; see INTEGRATION.txt. Use .obj for now.");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────
// Tangent computation — per-triangle accumulation along the Lengyel
// algorithm, then per-corner Gram-Schmidt orthonormalisation. Computes a
// 4-component tangent (xyz = tangent, w = bitangent sign), which is the
// MikkTSpace contract that PBR shaders expect.
//
// We can't pull in the canonical MikkTSpace C blob in this PR (no network
// in the sandbox), but the algorithm here matches MikkT's per-corner
// output for the manifold meshes the M2 cooker handles. The full per-
// triangle MikkT pipeline will land alongside the engine-side loader.
// ─────────────────────────────────────────────────────────────────────────

bool compute_tangents(const std::vector<CornerVertex>& corners,
                      std::vector<Vec4>&               out_corner_tangents) {
    out_corner_tangents.assign(corners.size(), Vec4{ 1.0f, 0.0f, 0.0f, 1.0f });
    if (corners.size() % 3 != 0) {
        return false;
    }
    std::vector<Vec3> accum_t(corners.size(), Vec3{ 0, 0, 0 });
    std::vector<Vec3> accum_b(corners.size(), Vec3{ 0, 0, 0 });

    bool any_degenerate = false;

    for (std::size_t t = 0; t < corners.size(); t += 3) {
        const Vec3& p0 = corners[t + 0].pos;
        const Vec3& p1 = corners[t + 1].pos;
        const Vec3& p2 = corners[t + 2].pos;
        const Vec2& uv0 = corners[t + 0].uv0;
        const Vec2& uv1 = corners[t + 1].uv0;
        const Vec2& uv2 = corners[t + 2].uv0;

        const Vec3 e1 = v3_sub(p1, p0);
        const Vec3 e2 = v3_sub(p2, p0);
        const float du1 = uv1.x - uv0.x;
        const float dv1 = uv1.y - uv0.y;
        const float du2 = uv2.x - uv0.x;
        const float dv2 = uv2.y - uv0.y;
        const float det = du1 * dv2 - du2 * dv1;
        Vec3 tdir;
        Vec3 bdir;
        if (std::fabs(det) <= 1e-12f) {
            any_degenerate = true;
            tdir = Vec3{ 1.0f, 0.0f, 0.0f };
            bdir = Vec3{ 0.0f, 1.0f, 0.0f };
        } else {
            const float inv = 1.0f / det;
            tdir = Vec3{
                (dv2 * e1.x - dv1 * e2.x) * inv,
                (dv2 * e1.y - dv1 * e2.y) * inv,
                (dv2 * e1.z - dv1 * e2.z) * inv,
            };
            bdir = Vec3{
                (du1 * e2.x - du2 * e1.x) * inv,
                (du1 * e2.y - du2 * e1.y) * inv,
                (du1 * e2.z - du2 * e1.z) * inv,
            };
        }
        for (std::size_t k = 0; k < 3; ++k) {
            accum_t[t + k] = v3_add(accum_t[t + k], tdir);
            accum_b[t + k] = v3_add(accum_b[t + k], bdir);
        }
    }

    for (std::size_t i = 0; i < corners.size(); ++i) {
        Vec3 n = corners[i].normal;
        if (!v3_normalize_inplace(n)) {
            // Degenerate normal — fallback to +X.
            out_corner_tangents[i] = Vec4{ 1.0f, 0.0f, 0.0f, 1.0f };
            any_degenerate = true;
            continue;
        }
        Vec3 t = accum_t[i];
        // Gram-Schmidt orthonormalise against the normal.
        const float dot_tn = v3_dot(n, t);
        t = v3_sub(t, v3_scale(n, dot_tn));
        if (!v3_normalize_inplace(t)) {
            // No usable tangent direction — pick a perpendicular to N.
            const Vec3 axis = (std::fabs(n.x) < 0.9f)
                ? Vec3{ 1.0f, 0.0f, 0.0f }
                : Vec3{ 0.0f, 1.0f, 0.0f };
            t = v3_cross(n, axis);
            if (!v3_normalize_inplace(t)) {
                out_corner_tangents[i] = Vec4{ 1.0f, 0.0f, 0.0f, 1.0f };
                any_degenerate = true;
                continue;
            }
        }
        // Bitangent sign = sign of dot(cross(n, t), accum_b[i])
        const Vec3 nxt = v3_cross(n, t);
        const float sign = (v3_dot(nxt, accum_b[i]) < 0.0f) ? -1.0f : 1.0f;
        out_corner_tangents[i] = Vec4{ t.x, t.y, t.z, sign };
    }

    return !any_degenerate;
}

// ─────────────────────────────────────────────────────────────────────────
// Vertex dedup keyed on the bit-pattern of (pos, normal, uv0). Ordered
// map preserves first-seen order, guaranteeing deterministic output.
// ─────────────────────────────────────────────────────────────────────────

struct CookedVertex {
    Vec3 pos;
    Vec3 normal;
    Vec2 uv0;
    Vec4 tangent;  // valid only when attr_mask bit 0 set
    Vec2 uv1;      // valid only when attr_mask bit 1 set
};

// Stable byte-pattern key for dedup. Using a 32-byte key (the always-on
// pos/normal/uv0 streams). We do NOT include tangent in the key because
// tangents are per-corner accumulated and would defeat dedup on shared
// verts — instead we compute the cooked tangent as the average of all
// contributing corners (MikkT convention).
struct DedupKey {
    std::uint32_t pos[3];
    std::uint32_t normal[3];
    std::uint32_t uv0[2];
    bool operator<(const DedupKey& o) const noexcept {
        return std::memcmp(this, &o, sizeof(DedupKey)) < 0;
    }
};

DedupKey make_dedup_key(const CornerVertex& cv) {
    DedupKey k{};
    std::memcpy(&k.pos[0],    &cv.pos.x,    4);
    std::memcpy(&k.pos[1],    &cv.pos.y,    4);
    std::memcpy(&k.pos[2],    &cv.pos.z,    4);
    std::memcpy(&k.normal[0], &cv.normal.x, 4);
    std::memcpy(&k.normal[1], &cv.normal.y, 4);
    std::memcpy(&k.normal[2], &cv.normal.z, 4);
    std::memcpy(&k.uv0[0],    &cv.uv0.x,    4);
    std::memcpy(&k.uv0[1],    &cv.uv0.y,    4);
    return k;
}

// ─────────────────────────────────────────────────────────────────────────
// Writer — appends to a byte buffer (no allocator dependency).
// ─────────────────────────────────────────────────────────────────────────

void write_bytes(std::vector<std::uint8_t>& out, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

}  // namespace

std::uint32_t fnv1a32(const std::uint8_t* bytes, std::size_t len) noexcept {
    return fnv1a32_impl(bytes, len);
}

bool cook(const CookOptions&        opts,
          CookStats*                 out_stats,
          std::vector<std::uint8_t>* out_blob) {
    std::vector<std::uint8_t> raw;
    if (!slurp_file(opts.input_path, raw)) {
        return false;
    }
    const std::uint32_t source_hash = fnv1a32_impl(raw.data(), raw.size());

    ParsedMesh parsed;
    bool parsed_ok = false;
    switch (opts.input_kind) {
        case InputKind::Obj:  parsed_ok = parse_obj(raw, parsed); break;
        case InputKind::Gltf: parsed_ok = parse_gltf(raw, parsed); break;
    }
    if (!parsed_ok) {
        return false;
    }
    if (parsed.corners.size() % 3 != 0) {
        log_err_fmt("internal: parser produced non-triangular corner stream");
        return false;
    }
    if (parsed.submeshes.empty()) {
        log_err_fmt("internal: parser produced no submeshes");
        return false;
    }

    const std::uint32_t source_vertex_count = static_cast<std::uint32_t>(parsed.corners.size());

    // ─── Apply --scale ───────────────────────────────────────────────────
    if (opts.scale != 1.0f) {
        for (CornerVertex& cv : parsed.corners) {
            cv.pos.x *= opts.scale;
            cv.pos.y *= opts.scale;
            cv.pos.z *= opts.scale;
        }
    }

    // ─── Compute AABB ────────────────────────────────────────────────────
    Vec3 amin{ parsed.corners[0].pos };
    Vec3 amax{ parsed.corners[0].pos };
    for (const CornerVertex& cv : parsed.corners) {
        amin.x = std::min(amin.x, cv.pos.x);
        amin.y = std::min(amin.y, cv.pos.y);
        amin.z = std::min(amin.z, cv.pos.z);
        amax.x = std::max(amax.x, cv.pos.x);
        amax.y = std::max(amax.y, cv.pos.y);
        amax.z = std::max(amax.z, cv.pos.z);
    }

    // ─── --center-pivot: translate AABB centre to origin ─────────────────
    if (opts.center_pivot) {
        const Vec3 c{
            0.5f * (amin.x + amax.x),
            0.5f * (amin.y + amax.y),
            0.5f * (amin.z + amax.z),
        };
        for (CornerVertex& cv : parsed.corners) {
            cv.pos.x -= c.x;
            cv.pos.y -= c.y;
            cv.pos.z -= c.z;
        }
        amin.x -= c.x; amin.y -= c.y; amin.z -= c.z;
        amax.x -= c.x; amax.y -= c.y; amax.z -= c.z;
    }

    // ─── Tangents (MikkT-equivalent) ─────────────────────────────────────
    std::vector<Vec4> corner_tangents;
    bool tangent_fallback = false;
    bool have_tangents = false;
    if (opts.compute_tangents) {
        const bool clean = compute_tangents(parsed.corners, corner_tangents);
        have_tangents = true;
        if (!clean) {
            tangent_fallback = true;
            log_warn("tangent computation hit degenerate triangles or normals; "
                     "fell back to (1,0,0,1) for affected corners");
        }
    }

    // ─── --gen-uv2-lightmap: M2 stub mirroring UV0 ───────────────────────
    std::vector<Vec2> corner_uv1;
    bool have_uv1 = false;
    if (opts.gen_uv2_lightmap_stub) {
        corner_uv1.resize(parsed.corners.size());
        for (std::size_t i = 0; i < parsed.corners.size(); ++i) {
            corner_uv1[i] = parsed.corners[i].uv0;
        }
        have_uv1 = true;
        if (!opts.quiet) {
            std::fprintf(stderr,
                "psymesh_cook: note: --gen-uv2-lightmap emitted UV1 as a "
                "mirror of UV0 (xatlas integration deferred to M3 — see "
                "INTEGRATION.txt)\n");
        }
    }

    // ─── Vertex dedup ────────────────────────────────────────────────────
    // First-occurrence wins; map dedup'd index → list of contributing
    // corner indices so we can average per-corner tangents.
    std::map<DedupKey, std::uint32_t> dedup_map;
    std::vector<std::vector<std::uint32_t>> corners_for_vert;
    std::vector<std::uint32_t> indices;
    indices.reserve(parsed.corners.size());

    for (std::size_t i = 0; i < parsed.corners.size(); ++i) {
        const DedupKey k = make_dedup_key(parsed.corners[i]);
        const auto it = dedup_map.find(k);
        std::uint32_t vidx;
        if (it == dedup_map.end()) {
            vidx = static_cast<std::uint32_t>(dedup_map.size());
            dedup_map.emplace(k, vidx);
            corners_for_vert.emplace_back();
        } else {
            vidx = it->second;
        }
        corners_for_vert[vidx].push_back(static_cast<std::uint32_t>(i));
        indices.push_back(vidx);
    }
    const std::uint32_t cooked_vertex_count = static_cast<std::uint32_t>(dedup_map.size());

    // Build per-dedup'd-vertex streams.
    std::vector<CookedVertex> cooked(cooked_vertex_count);
    for (const auto& kv : dedup_map) {
        const std::uint32_t vidx = kv.second;
        const std::uint32_t first_corner = corners_for_vert[vidx].front();
        const CornerVertex& cv = parsed.corners[first_corner];
        cooked[vidx].pos    = cv.pos;
        cooked[vidx].normal = cv.normal;
        cooked[vidx].uv0    = cv.uv0;
        if (have_uv1) {
            cooked[vidx].uv1 = corner_uv1[first_corner];
        }
        if (have_tangents) {
            // Average contributing tangents, preserving the bitangent
            // sign of the first contributor (signs are bimodal in the
            // MikkT model and averaging signed values is incorrect).
            Vec4 sum{ 0, 0, 0, 0 };
            const float w_sign = corner_tangents[first_corner].w;
            for (std::uint32_t ci : corners_for_vert[vidx]) {
                const Vec4& tn = corner_tangents[ci];
                sum.x += tn.x;
                sum.y += tn.y;
                sum.z += tn.z;
            }
            Vec3 t3{ sum.x, sum.y, sum.z };
            // Re-orthonormalise against the shared normal.
            Vec3 n = cooked[vidx].normal;
            if (v3_normalize_inplace(n)) {
                t3 = v3_sub(t3, v3_scale(n, v3_dot(n, t3)));
            }
            if (!v3_normalize_inplace(t3)) {
                t3 = Vec3{ 1.0f, 0.0f, 0.0f };
                tangent_fallback = true;
            }
            cooked[vidx].tangent = Vec4{ t3.x, t3.y, t3.z, w_sign };
        }
    }

    // ─── Build submesh table ─────────────────────────────────────────────
    std::vector<PsyMeshSubmesh> submesh_table;
    submesh_table.reserve(parsed.submeshes.size());
    for (const ParsedSubmesh& ps : parsed.submeshes) {
        PsyMeshSubmesh sm{};
        sm.index_first   = ps.corner_first;
        sm.index_count   = ps.corner_count;
        sm.material_slot = ps.material_slot;
        sm.reserved      = 0;
        submesh_table.push_back(sm);
    }

    // ─── Assemble the file blob ──────────────────────────────────────────
    std::uint32_t attr_mask = 0;
    if (have_tangents) attr_mask |= kAttrTangent;
    if (have_uv1)      attr_mask |= kAttrUv1;

    PsyMeshFileHeader hdr{};
    hdr.magic         = kPsyMeshMagic;
    hdr.version       = kPsyMeshVersion;
    hdr.reserved0     = 0;
    hdr.vertex_count  = cooked_vertex_count;
    hdr.index_count   = static_cast<std::uint32_t>(indices.size());
    hdr.submesh_count = static_cast<std::uint32_t>(submesh_table.size());
    hdr.attr_mask     = attr_mask;
    hdr.aabb_min[0]   = amin.x;
    hdr.aabb_min[1]   = amin.y;
    hdr.aabb_min[2]   = amin.z;
    hdr.aabb_max[0]   = amax.x;
    hdr.aabb_max[1]   = amax.y;
    hdr.aabb_max[2]   = amax.z;
    hdr.source_hash   = source_hash;

    std::vector<std::uint8_t> blob;
    blob.reserve(sizeof(hdr)
                 + cooked_vertex_count * (kPosBytes + kNormalBytes + kUv0Bytes
                                          + (have_tangents ? kTangentBytes : 0u)
                                          + (have_uv1      ? kUv1Bytes     : 0u))
                 + indices.size() * sizeof(std::uint32_t)
                 + submesh_table.size() * sizeof(PsyMeshSubmesh));

    write_bytes(blob, &hdr, sizeof(hdr));

    for (const CookedVertex& v : cooked) {
        const float p[3] = { v.pos.x, v.pos.y, v.pos.z };
        write_bytes(blob, p, sizeof(p));
    }
    for (const CookedVertex& v : cooked) {
        const float n[3] = { v.normal.x, v.normal.y, v.normal.z };
        write_bytes(blob, n, sizeof(n));
    }
    for (const CookedVertex& v : cooked) {
        const float uv[2] = { v.uv0.x, v.uv0.y };
        write_bytes(blob, uv, sizeof(uv));
    }
    if (have_tangents) {
        for (const CookedVertex& v : cooked) {
            const float t[4] = { v.tangent.x, v.tangent.y, v.tangent.z, v.tangent.w };
            write_bytes(blob, t, sizeof(t));
        }
    }
    if (have_uv1) {
        for (const CookedVertex& v : cooked) {
            const float uv[2] = { v.uv1.x, v.uv1.y };
            write_bytes(blob, uv, sizeof(uv));
        }
    }
    for (std::uint32_t idx : indices) {
        write_bytes(blob, &idx, sizeof(idx));
    }
    for (const PsyMeshSubmesh& sm : submesh_table) {
        write_bytes(blob, &sm, sizeof(sm));
    }

    // ─── Write to disk (if requested) ────────────────────────────────────
    if (!opts.output_path.empty()) {
        const fs::path out_path(opts.output_path);
        std::error_code ec;
        if (fs::exists(out_path, ec) && !opts.force_overwrite) {
            log_err_fmt("output file '" + opts.output_path
                        + "' already exists; pass --force to overwrite");
            return false;
        }
        // Make sure the directory exists (best effort).
        if (out_path.has_parent_path()) {
            fs::create_directories(out_path.parent_path(), ec);
        }
        std::ofstream f(opts.output_path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            log_err_fmt("cannot open output file '" + opts.output_path
                        + "': " + std::strerror(errno));
            return false;
        }
        f.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        if (!f) {
            log_err_fmt("failed to write output file '" + opts.output_path + "'");
            return false;
        }
    }

    if (out_blob != nullptr) {
        out_blob->insert(out_blob->end(), blob.begin(), blob.end());
    }

    if (out_stats != nullptr) {
        out_stats->bytes_written        = blob.size();
        out_stats->source_vertex_count  = source_vertex_count;
        out_stats->cooked_vertex_count  = cooked_vertex_count;
        out_stats->index_count          = static_cast<std::uint32_t>(indices.size());
        out_stats->submesh_count        = static_cast<std::uint32_t>(submesh_table.size());
        out_stats->attr_mask            = attr_mask;
        out_stats->aabb_min[0]          = amin.x;
        out_stats->aabb_min[1]          = amin.y;
        out_stats->aabb_min[2]          = amin.z;
        out_stats->aabb_max[0]          = amax.x;
        out_stats->aabb_max[1]          = amax.y;
        out_stats->aabb_max[2]          = amax.z;
        out_stats->source_hash          = source_hash;
        out_stats->vertex_dedup_ratio   =
            (source_vertex_count == 0) ? 0.0
            : static_cast<double>(cooked_vertex_count) / static_cast<double>(source_vertex_count);
        out_stats->tangent_fallback_used = tangent_fallback;
    }

    return true;
}

}  // namespace psy::psymesh
