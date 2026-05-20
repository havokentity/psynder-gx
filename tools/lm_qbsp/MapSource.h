// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 24 tools: shared Quake-style ".map" source parser +
// brush geometry helpers.
//
// This header is the common front-end for the two offline tools that
// consume Quake / TrenchBroom ".map" brush files:
//
//   * tools/lm_qbsp     — compiles brushes into the engine .psybsp format.
//   * tools/lm_mapimport — imports brushes + entities into a scene file.
//
// It is header-only (every free function is `inline`) on purpose: the
// tools build only when PSYNDER_GX_BUILD_TOOLS=ON, but the unit tests in
// tests/unit/ exercise the parser directly (TOOLS-independent) by
// including this header through a relative path. Keeping the logic inline
// lets the same source compile into the CLI binary AND the test
// translation unit without a separate static library (which tests cannot
// link, since tests/unit/CMakeLists.txt is orchestrator-owned).
//
// The parser is intentionally self-contained — it links only psynder_common
// for the engine include root + warning flags, and rolls its own small
// vector/plane math so the determinism story stays simple (no dependency on
// engine/math's evolving implementation).
//
// ──────────────────────────────────────────────────────────────────────────
//  Supported ".map" dialects
// ──────────────────────────────────────────────────────────────────────────
//
//   Standard (Quake / Half-Life / Quake II):
//     ( x y z ) ( x y z ) ( x y z ) TEXNAME xoff yoff rot xscale yscale
//
//   Valve 220 (TrenchBroom default, Half-Life):
//     ( x y z ) ( x y z ) ( x y z ) TEXNAME [ ax ay az aoff ] [ bx by bz boff ] rot xscale yscale
//
//   Trailing per-face fields (Quake II/III surface + content flags) are
//   tolerated and ignored. Comments start with `//` and run to end of line.
//
//  Units: ".map" coordinates are read verbatim as metres (1 unit = 1 m,
//  per the engine-wide metric contract). If a source map authored in
//  Quake units (where 1 unit ~= 1 inch) needs rescaling, the importing
//  tool's --scale option handles it; the parser itself does not rescale.
//
//  Coordinate system: the parser keeps the source coordinates unchanged
//  (right-handed, +Z up — the Quake convention). Any axis remap is left to
//  the consuming tool so this layer stays a faithful reader.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psy::lmtools {

// ─────────────────────────────────────────────────────────────────────────
// Minimal self-contained vector / plane math (f32, deterministic).
// ─────────────────────────────────────────────────────────────────────────

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 v3_add(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 v3_sub(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 v3_scale(Vec3 a, float s) noexcept {
    return Vec3{a.x * s, a.y * s, a.z * s};
}
inline float v3_dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 v3_cross(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float v3_length(Vec3 a) noexcept {
    return std::sqrt(v3_dot(a, a));
}
inline Vec3 v3_normalize(Vec3 a) noexcept {
    const float len = v3_length(a);
    if (len <= 1e-20f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    const float inv = 1.0f / len;
    return Vec3{a.x * inv, a.y * inv, a.z * inv};
}
inline Vec3 v3_min(Vec3 a, Vec3 b) noexcept {
    return Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline Vec3 v3_max(Vec3 a, Vec3 b) noexcept {
    return Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

// Plane in Hessian normal form: a point p lies on the plane iff
// dot(n, p) == d. This matches the engine's BspFileNode convention
// (see engine/world/bsp/BspFormat.h: "plane distance (n.dot(p) = d)") and
// editor::brush::make_plane (engine/editor/core/Brush.h).
struct Plane {
    Vec3 n{0.0f, 0.0f, 1.0f};
    float d = 0.0f;
};

// Signed distance of p from the plane: > 0 in front (the +normal side).
inline float plane_distance(const Plane& pl, Vec3 p) noexcept {
    return v3_dot(pl.n, p) - pl.d;
}

// ─────────────────────────────────────────────────────────────────────────
// Parsed ".map" data model.
// ─────────────────────────────────────────────────────────────────────────

// A single brush face: three source points defining the plane, the
// texture name, and the texture-projection parameters (kept so importers
// can round-trip UVs; lm_qbsp derives lightmap UVs from the plane basis).
struct MapFace {
    Vec3 points[3];         // the three source points, file order
    Plane plane;            // outward-oriented (see brush_orient_planes)
    std::string texture;    // texture / material name
    bool valve220 = false;  // true if axes came from Valve-220 brackets
    // Valve-220 texture axes ([ax ay az aoff], [bx by bz boff]). For standard
    // brushes these are synthesised from the dominant plane axis at parse time.
    Vec3 u_axis{1.0f, 0.0f, 0.0f};
    float u_offset = 0.0f;
    Vec3 v_axis{0.0f, 1.0f, 0.0f};
    float v_offset = 0.0f;
    float rotation = 0.0f;  // standard-format rotation (degrees)
    float scale_x = 1.0f;
    float scale_y = 1.0f;
};

struct MapBrush {
    std::vector<MapFace> faces;
    Vec3 bounds_min{0.0f, 0.0f, 0.0f};
    Vec3 bounds_max{0.0f, 0.0f, 0.0f};
    bool bounds_valid = false;
};

struct MapEntity {
    // Key/value properties in source order (worldspawn, light, info_*, etc.).
    std::vector<std::pair<std::string, std::string>> props;
    std::vector<MapBrush> brushes;

    // Returns the value for `key`, or `fallback` if absent. First match wins.
    std::string_view get(std::string_view key, std::string_view fallback = std::string_view{}) const {
        for (const auto& kv : props) {
            if (kv.first == key) {
                return kv.second;
            }
        }
        return fallback;
    }
    std::string_view classname() const { return get("classname"); }
};

struct MapFile {
    std::vector<MapEntity> entities;
};

struct MapParseError {
    bool ok = true;
    std::size_t line = 0;
    std::string message;
};

// ─────────────────────────────────────────────────────────────────────────
// Tokeniser — whitespace + `//` comment aware. Operates over a string_view
// of the whole file; tracks a 1-based line number for diagnostics.
// ─────────────────────────────────────────────────────────────────────────

class MapLexer {
   public:
    explicit MapLexer(std::string_view src) noexcept : src_(src) {}

    std::size_t line() const noexcept { return line_; }

    // Skip whitespace and `//` line comments. Returns false at end of input.
    bool skip_trivia() noexcept {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '\n') {
                ++line_;
                ++pos_;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < src_.size() && src_[pos_] != '\n') {
                    ++pos_;
                }
            } else {
                return true;
            }
        }
        return false;
    }

    // Peek the next non-trivia character without consuming it (0 at EOF).
    char peek() noexcept {
        if (!skip_trivia()) {
            return '\0';
        }
        return src_[pos_];
    }

    // Consume a single punctuation character (one of {}()[]).
    bool consume_char(char expected) noexcept {
        if (peek() == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    // Read a double-quoted string (the opening quote must be next). Escapes
    // are not part of the .map grammar; a literal backslash is preserved.
    bool read_quoted(std::string& out) noexcept {
        if (!skip_trivia() || src_[pos_] != '"') {
            return false;
        }
        ++pos_;  // opening quote
        out.clear();
        while (pos_ < src_.size() && src_[pos_] != '"') {
            if (src_[pos_] == '\n') {
                ++line_;
            }
            out.push_back(src_[pos_]);
            ++pos_;
        }
        if (pos_ >= src_.size()) {
            return false;  // unterminated
        }
        ++pos_;  // closing quote
        return true;
    }

    // Read a bare token (anything up to whitespace or punctuation). Returns
    // false at end of input.
    bool read_bare(std::string_view& out) noexcept {
        if (!skip_trivia()) {
            return false;
        }
        const std::size_t start = pos_;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' ||
                c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' || c == '"') {
                break;
            }
            ++pos_;
        }
        if (pos_ == start) {
            return false;
        }
        out = src_.substr(start, pos_ - start);
        return true;
    }

   private:
    std::string_view src_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
};

// ─────────────────────────────────────────────────────────────────────────
// Numeric parsing helpers (locale-independent strtod over a bounded copy).
// ─────────────────────────────────────────────────────────────────────────

inline bool parse_float(std::string_view tok, float& out) noexcept {
    if (tok.empty() || tok.size() >= 64) {
        return false;
    }
    char buf[64];
    std::memcpy(buf, tok.data(), tok.size());
    buf[tok.size()] = '\0';
    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end != buf + tok.size()) {
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

inline bool read_float(MapLexer& lex, float& out) noexcept {
    std::string_view tok;
    return lex.read_bare(tok) && parse_float(tok, out);
}

inline bool read_vec3(MapLexer& lex, Vec3& out) noexcept {
    return lex.consume_char('(') && read_float(lex, out.x) && read_float(lex, out.y) &&
           read_float(lex, out.z) && lex.consume_char(')');
}

// Read the Valve-220 axis block: `[ ax ay az aoff ]`.
inline bool read_axis_block(MapLexer& lex, Vec3& axis, float& offset) noexcept {
    return lex.consume_char('[') && read_float(lex, axis.x) && read_float(lex, axis.y) &&
           read_float(lex, axis.z) && read_float(lex, offset) && lex.consume_char(']');
}

// ─────────────────────────────────────────────────────────────────────────
// Plane extraction.
// ─────────────────────────────────────────────────────────────────────────

// Build a plane from the three source points (orientation resolved later by
// brush_orient_planes once the whole brush is known).
inline Plane plane_from_points(Vec3 a, Vec3 b, Vec3 c) noexcept {
    const Vec3 n = v3_normalize(v3_cross(v3_sub(b, a), v3_sub(c, a)));
    return Plane{n, v3_dot(n, a)};
}

// Synthesise a standard-format texture basis from the plane's dominant axis
// (the classic Quake "QuArK"/idTech axial projection). Only used when the
// source brush is not Valve-220.
inline void axial_texture_basis(Vec3 normal, Vec3& u_axis, Vec3& v_axis) noexcept {
    const float ax = std::fabs(normal.x);
    const float ay = std::fabs(normal.y);
    const float az = std::fabs(normal.z);
    if (az >= ax && az >= ay) {
        u_axis = Vec3{1.0f, 0.0f, 0.0f};
        v_axis = Vec3{0.0f, -1.0f, 0.0f};
    } else if (ax >= ay) {
        u_axis = Vec3{0.0f, 1.0f, 0.0f};
        v_axis = Vec3{0.0f, 0.0f, -1.0f};
    } else {
        u_axis = Vec3{1.0f, 0.0f, 0.0f};
        v_axis = Vec3{0.0f, 0.0f, -1.0f};
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Face parsing.
// ─────────────────────────────────────────────────────────────────────────

inline bool parse_face(MapLexer& lex, MapFace& out, MapParseError& err) {
    if (!read_vec3(lex, out.points[0]) || !read_vec3(lex, out.points[1]) ||
        !read_vec3(lex, out.points[2])) {
        err = MapParseError{false, lex.line(), "malformed face plane points"};
        return false;
    }
    std::string_view tex;
    if (!lex.read_bare(tex)) {
        err = MapParseError{false, lex.line(), "missing texture name on face"};
        return false;
    }
    out.texture.assign(tex);
    out.plane = plane_from_points(out.points[0], out.points[1], out.points[2]);

    if (lex.peek() == '[') {
        out.valve220 = true;
        if (!read_axis_block(lex, out.u_axis, out.u_offset) ||
            !read_axis_block(lex, out.v_axis, out.v_offset)) {
            err = MapParseError{false, lex.line(), "malformed Valve-220 texture axes"};
            return false;
        }
        if (!read_float(lex, out.rotation) || !read_float(lex, out.scale_x) ||
            !read_float(lex, out.scale_y)) {
            err = MapParseError{false, lex.line(), "malformed Valve-220 texture parameters"};
            return false;
        }
    } else {
        out.valve220 = false;
        if (!read_float(lex, out.u_offset) || !read_float(lex, out.v_offset) ||
            !read_float(lex, out.rotation) || !read_float(lex, out.scale_x) ||
            !read_float(lex, out.scale_y)) {
            err = MapParseError{false, lex.line(), "malformed standard texture parameters"};
            return false;
        }
        axial_texture_basis(out.plane.n, out.u_axis, out.v_axis);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Brush plane orientation + bounds.
// ─────────────────────────────────────────────────────────────────────────

// Orient every face plane so its normal points OUTWARD (away from the brush
// interior). For a convex brush, the mean of the face-defining points is a
// strict interior point, so we flip any plane that puts that point in front.
// This removes all winding-order ambiguity between .map dialects.
inline void brush_orient_planes(MapBrush& brush) {
    if (brush.faces.empty()) {
        return;
    }
    Vec3 interior{0.0f, 0.0f, 0.0f};
    for (const MapFace& f : brush.faces) {
        interior = v3_add(interior, v3_add(f.points[0], v3_add(f.points[1], f.points[2])));
    }
    const float inv = 1.0f / static_cast<float>(brush.faces.size() * 3);
    interior = v3_scale(interior, inv);

    for (MapFace& f : brush.faces) {
        if (plane_distance(f.plane, interior) > 0.0f) {
            f.plane.n = v3_scale(f.plane.n, -1.0f);
            f.plane.d = -f.plane.d;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Brush → convex face polygons (the standard QBSP plane-clipping build).
// ─────────────────────────────────────────────────────────────────────────

// A finite polygon on one brush face, in CCW order around the outward
// normal. Empty when the face plane is redundant (entirely clipped away).
struct FacePolygon {
    Plane plane;
    std::string texture;
    std::vector<Vec3> vertices;
};

// Clip a convex polygon by a half-space (keep the part on/behind `plane`,
// i.e. plane_distance <= epsilon). Sutherland-Hodgman with edge splitting.
inline std::vector<Vec3> clip_polygon(const std::vector<Vec3>& poly, const Plane& plane, float epsilon) {
    std::vector<Vec3> out;
    const std::size_t n = poly.size();
    if (n == 0) {
        return out;
    }
    out.reserve(n + 4);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3 cur = poly[i];
        const Vec3 nxt = poly[(i + 1) % n];
        const float dc = plane_distance(plane, cur);
        const float dn = plane_distance(plane, nxt);
        const bool cur_in = dc <= epsilon;
        const bool nxt_in = dn <= epsilon;
        if (cur_in) {
            out.push_back(cur);
        }
        if (cur_in != nxt_in) {
            const float denom = dc - dn;
            // Guard against a near-parallel edge producing a wild split point.
            const float t = (std::fabs(denom) > 1e-20f) ? (dc / denom) : 0.0f;
            out.push_back(v3_add(cur, v3_scale(v3_sub(nxt, cur), t)));
        }
    }
    return out;
}

// Build the maximal quad lying on `plane`, sized to comfortably enclose
// `world_extent` (a generous half-size of the whole map). CCW around n.
inline std::vector<Vec3> base_polygon_for_plane(const Plane& plane, float world_extent) {
    // Pick a tangent that is not parallel to the normal.
    Vec3 up = (std::fabs(plane.n.z) < 0.9f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 t = v3_normalize(v3_cross(up, plane.n));
    const Vec3 b = v3_normalize(v3_cross(plane.n, t));
    const Vec3 center = v3_scale(plane.n, plane.d);  // dot(n, n*d) = d when |n|=1
    const Vec3 tt = v3_scale(t, world_extent);
    const Vec3 bb = v3_scale(b, world_extent);
    std::vector<Vec3> quad;
    quad.reserve(4);
    quad.push_back(v3_add(center, v3_sub(v3_scale(tt, -1.0f), bb)));
    quad.push_back(v3_add(center, v3_sub(tt, bb)));
    quad.push_back(v3_add(center, v3_add(tt, bb)));
    quad.push_back(v3_add(center, v3_add(v3_scale(tt, -1.0f), bb)));
    return quad;
}

// Compute the convex polygon for every face of `brush`. `world_extent` is a
// half-size large enough to enclose the whole map (callers pass the overall
// map extent). Faces whose polygon collapses (< 3 verts) are dropped.
inline std::vector<FacePolygon> brush_build_polygons(const MapBrush& brush, float world_extent) {
    std::vector<FacePolygon> out;
    out.reserve(brush.faces.size());
    const float eps = 1e-3f;
    for (const MapFace& face : brush.faces) {
        std::vector<Vec3> poly = base_polygon_for_plane(face.plane, world_extent);
        for (const MapFace& other : brush.faces) {
            if (&other == &face) {
                continue;
            }
            poly = clip_polygon(poly, other.plane, eps);
            if (poly.size() < 3) {
                break;
            }
        }
        if (poly.size() >= 3) {
            out.push_back(FacePolygon{face.plane, face.texture, std::move(poly)});
        }
    }
    return out;
}

// Recompute a brush's axis-aligned bounds from its face polygons.
inline void brush_compute_bounds(MapBrush& brush, float world_extent) {
    const std::vector<FacePolygon> polys = brush_build_polygons(brush, world_extent);
    bool any = false;
    Vec3 lo{0.0f, 0.0f, 0.0f};
    Vec3 hi{0.0f, 0.0f, 0.0f};
    for (const FacePolygon& fp : polys) {
        for (Vec3 v : fp.vertices) {
            if (!any) {
                lo = v;
                hi = v;
                any = true;
            } else {
                lo = v3_min(lo, v);
                hi = v3_max(hi, v);
            }
        }
    }
    brush.bounds_min = lo;
    brush.bounds_max = hi;
    brush.bounds_valid = any;
}

// ─────────────────────────────────────────────────────────────────────────
// Top-level parse entry.
// ─────────────────────────────────────────────────────────────────────────

inline bool parse_brush(MapLexer& lex, MapBrush& out, MapParseError& err) {
    // The opening '{' was already consumed by the entity loop.
    while (true) {
        const char c = lex.peek();
        if (c == '\0') {
            err = MapParseError{false, lex.line(), "unterminated brush (missing '}')"};
            return false;
        }
        if (c == '}') {
            lex.consume_char('}');
            break;
        }
        MapFace face;
        if (!parse_face(lex, face, err)) {
            return false;
        }
        out.faces.push_back(std::move(face));
    }
    if (out.faces.size() < 4) {
        err = MapParseError{false, lex.line(), "brush has fewer than 4 faces (not a closed volume)"};
        return false;
    }
    brush_orient_planes(out);
    return true;
}

inline bool parse_entity(MapLexer& lex, MapEntity& out, MapParseError& err) {
    // The opening '{' was already consumed by the caller.
    while (true) {
        const char c = lex.peek();
        if (c == '\0') {
            err = MapParseError{false, lex.line(), "unterminated entity (missing '}')"};
            return false;
        }
        if (c == '}') {
            lex.consume_char('}');
            return true;
        }
        if (c == '{') {
            lex.consume_char('{');
            MapBrush brush;
            if (!parse_brush(lex, brush, err)) {
                return false;
            }
            out.brushes.push_back(std::move(brush));
            continue;
        }
        if (c == '"') {
            std::string key;
            std::string value;
            if (!lex.read_quoted(key) || !lex.read_quoted(value)) {
                err = MapParseError{false, lex.line(), "malformed key/value pair"};
                return false;
            }
            out.props.emplace_back(std::move(key), std::move(value));
            continue;
        }
        err = MapParseError{false,
                            lex.line(),
                            std::string("unexpected character '") + c + "' in entity body"};
        return false;
    }
}

// Parse a whole ".map" file. On failure, returns false and fills `err`.
inline bool parse_map(std::string_view src, MapFile& out, MapParseError& err) {
    out.entities.clear();
    err = MapParseError{};
    MapLexer lex(src);
    while (true) {
        const char c = lex.peek();
        if (c == '\0') {
            break;  // clean end of input
        }
        if (c != '{') {
            err = MapParseError{false,
                                lex.line(),
                                std::string("expected '{' to open an entity, found '") + c + "'"};
            return false;
        }
        lex.consume_char('{');
        MapEntity ent;
        if (!parse_entity(lex, ent, err)) {
            return false;
        }
        out.entities.push_back(std::move(ent));
    }
    if (out.entities.empty()) {
        err = MapParseError{false, lex.line(), "map contains no entities"};
        return false;
    }
    return true;
}

// Overall half-extent of every brush point in the map — used to size the
// base polygons for clipping. Always returns a strictly positive value.
inline float map_world_extent(const MapFile& map) {
    float extent = 0.0f;
    for (const MapEntity& ent : map.entities) {
        for (const MapBrush& brush : ent.brushes) {
            for (const MapFace& face : brush.faces) {
                for (const Vec3& p : face.points) {
                    extent = std::max(extent, std::fabs(p.x));
                    extent = std::max(extent, std::fabs(p.y));
                    extent = std::max(extent, std::fabs(p.z));
                }
            }
        }
    }
    // Pad generously so base quads enclose the geometry even after rotation.
    return std::max(extent * 8.0f, 1024.0f);
}

}  // namespace psy::lmtools
