// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 24 tools: lm_qbsp BSP compiler.
//
// Compiles a Quake-style ".map" brush set into the engine .psybsp on-disk
// format (engine/world/bsp/BspFormat.h), including a cell-visibility PVS.
//
// Header-only (every free function `inline`) for the same reason as
// MapSource.h: the tool builds only when PSYNDER_GX_BUILD_TOOLS=ON, but the
// unit tests drive the compiler directly through a relative-path include so
// they run under the default (TOOLS=OFF) test build too.
//
// ──────────────────────────────────────────────────────────────────────────
//  Pipeline
// ──────────────────────────────────────────────────────────────────────────
//
//   .map text ─► MapSource parse ─► brushes (convex, outward planes)
//             ─► SolidBSP (recursive axis-aligned partition)
//             ─► leaf solid/empty classification (point-in-brush)
//             ─► exterior cull via boundary flood-fill (leak-aware)
//             ─► face geometry (brush plane clipping; cull internal faces)
//             ─► cluster assignment + connectivity PVS
//             ─► .psybsp blob (header + nodes/leaves/faces/verts/indices/pvs)
//
//  The axis-aligned SolidBSP is *exact* for axis-aligned brush geometry —
//  the indoor "Quake-style room" the Wave-B demo targets (sample 03). Brush
//  faces at arbitrary angles are still emitted as render geometry, but the
//  space partition only splits on axis-aligned planes; full general-plane
//  SolidBSP + occlusion-aware portal VIS are documented deferrals (see
//  INTEGRATION.txt).
//
//  Determinism: no RNG / time / pid. All passes iterate deterministically-
//  ordered vectors (or sorted keys), so identical input ─► byte-identical
//  output (verified by the tools_lm_qbsp test suite).
//
//  Units: world units are metres throughout (engine-wide contract). The
//  --scale option rescales source coordinates before compilation.

#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "MapSource.h"
#include "world/bsp/BspFormat.h"

namespace psy::lm_qbsp {

namespace bspfmt = psynder::world::bsp;

// ─────────────────────────────────────────────────────────────────────────
// Public options / stats.
// ─────────────────────────────────────────────────────────────────────────

struct CompileOptions {
    std::string input_path;   // source .map; read by compile()
    std::string output_path;  // .psybsp path; "" => no file write
    float scale = 1.0f;       // uniform scale applied to source coordinates
    bool force_overwrite = false;
    bool print_stats = false;
    bool quiet = false;
    int max_depth = 128;  // BSP recursion cap (safety)
};

struct CompileStats {
    std::uint64_t bytes_written = 0;
    std::uint32_t brush_count = 0;
    std::uint32_t node_count = 0;
    std::uint32_t leaf_count = 0;
    std::uint32_t solid_leaf_count = 0;
    std::uint32_t empty_leaf_count = 0;  // == cluster_count
    std::uint32_t face_count = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t cluster_count = 0;
    std::uint32_t pvs_row_bytes = 0;
    bool leaked = false;  // interior opened to the void (flood escaped)
    float bounds_min[3] = {0.0f, 0.0f, 0.0f};
    float bounds_max[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t source_hash = 0;
};

// ─────────────────────────────────────────────────────────────────────────
// FNV-1a 32-bit (texture-name material ids + source-blob staleness hash).
// ─────────────────────────────────────────────────────────────────────────

inline std::uint32_t fnv1a32(const std::uint8_t* bytes, std::size_t len) noexcept {
    std::uint32_t h = 0x811C9DC5u;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint32_t>(bytes[i]);
        h *= 0x01000193u;
    }
    return h;
}
inline std::uint32_t fnv1a32(std::string_view s) noexcept {
    return fnv1a32(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// ─────────────────────────────────────────────────────────────────────────
// Internal build model.
// ─────────────────────────────────────────────────────────────────────────

namespace detail {

using lmtools::Plane;
using lmtools::Vec3;

struct Aabb {
    Vec3 lo{0.0f, 0.0f, 0.0f};
    Vec3 hi{0.0f, 0.0f, 0.0f};
};

inline Vec3 aabb_center(const Aabb& a) noexcept {
    return Vec3{0.5f * (a.lo.x + a.hi.x), 0.5f * (a.lo.y + a.hi.y), 0.5f * (a.lo.z + a.hi.z)};
}

// A solid convex brush: its outward face planes + bounds.
struct SolidBrush {
    std::vector<Plane> planes;
    Aabb bounds;
};

struct BuildLeaf {
    bool solid = false;
    bool reachable_void = false;  // connected to the exterior (flood-fill)
    Aabb bounds;
    int cluster = bspfmt::kBspSolidCluster;   // -1 until assigned
    std::vector<std::uint32_t> face_indices;  // faces whose front lies here
};

struct BuildNode {
    Plane plane;
    int front_child = 0;  // child code: >=0 node index, <0 ~leaf index
    int back_child = 0;
};

// A finished render face: an n-gon plus its texture/material binding.
struct BuildFace {
    std::vector<Vec3> verts;  // CCW around plane.n
    Vec3 normal{0.0f, 0.0f, 1.0f};
    std::string texture;
    int leaf = -1;  // owning empty leaf (set during assignment)
};

inline float axis_get(const Vec3& v, int axis) noexcept {
    return (axis == 0) ? v.x : (axis == 1) ? v.y : v.z;
}
inline void axis_set(Vec3& v, int axis, float value) noexcept {
    if (axis == 0) {
        v.x = value;
    } else if (axis == 1) {
        v.y = value;
    } else {
        v.z = value;
    }
}

// True when a brush plane is axis-aligned; reports the axis (0/1/2) and the
// coordinate of the plane on that axis (in +axis orientation: coord = d/n).
inline bool plane_axis_aligned(const Plane& p, int& axis, float& coord) noexcept {
    const float ax = std::fabs(p.n.x);
    const float ay = std::fabs(p.n.y);
    const float az = std::fabs(p.n.z);
    constexpr float kAxisEps = 1e-4f;
    if (ax > 0.999f && ay < kAxisEps && az < kAxisEps) {
        axis = 0;
        coord = p.d / p.n.x;
        return true;
    }
    if (ay > 0.999f && ax < kAxisEps && az < kAxisEps) {
        axis = 1;
        coord = p.d / p.n.y;
        return true;
    }
    if (az > 0.999f && ax < kAxisEps && ay < kAxisEps) {
        axis = 2;
        coord = p.d / p.n.z;
        return true;
    }
    return false;
}

inline bool point_in_brush(const SolidBrush& b, Vec3 p, float eps) noexcept {
    for (const Plane& pl : b.planes) {
        if (lmtools::plane_distance(pl, p) > eps) {
            return false;
        }
    }
    return true;
}

inline bool point_in_any_brush(const std::vector<SolidBrush>& brushes, Vec3 p, float eps) noexcept {
    for (const SolidBrush& b : brushes) {
        if (point_in_brush(b, p, eps)) {
            return true;
        }
    }
    return false;
}

// Two axis-aligned boxes share a portal (touch on a face with positive area).
inline bool aabb_face_adjacent(const Aabb& a, const Aabb& b, float eps) noexcept {
    int touching_axis = -1;
    for (int axis = 0; axis < 3; ++axis) {
        const float a_lo = axis_get(a.lo, axis);
        const float a_hi = axis_get(a.hi, axis);
        const float b_lo = axis_get(b.lo, axis);
        const float b_hi = axis_get(b.hi, axis);
        if (std::fabs(a_hi - b_lo) < eps || std::fabs(b_hi - a_lo) < eps) {
            if (touching_axis != -1) {
                return false;  // touch on >1 axis => edge/corner, not a face
            }
            touching_axis = axis;
        } else if (a_hi <= b_lo - eps || b_hi <= a_lo - eps) {
            return false;  // separated on this axis
        }
    }
    if (touching_axis == -1) {
        return false;  // overlapping volumes (shouldn't happen for BSP cells)
    }
    // Require positive overlap area on the two non-touching axes.
    for (int axis = 0; axis < 3; ++axis) {
        if (axis == touching_axis) {
            continue;
        }
        const float lo = std::max(axis_get(a.lo, axis), axis_get(b.lo, axis));
        const float hi = std::min(axis_get(a.hi, axis), axis_get(b.hi, axis));
        if (hi - lo <= eps) {
            return false;
        }
    }
    return true;
}

// Face-adjacency lists for every leaf, bucketed by the touching coordinate so
// neighbour search is near-linear instead of O(N^2) all-pairs. Two cells are
// adjacent only if one's hi[axis] face meets the other's lo[axis] face at the
// same coordinate, so we only ever test pairs that share a quantised plane.
inline std::vector<std::vector<std::uint32_t>> build_leaf_adjacency(const std::vector<BuildLeaf>& leaves,
                                                                    float eps) {
    std::vector<std::vector<std::uint32_t>> adj(leaves.size());
    const float quant = std::max(eps, 1e-4f);
    for (int axis = 0; axis < 3; ++axis) {
        std::map<long long, std::vector<std::uint32_t>> lo_at;
        std::map<long long, std::vector<std::uint32_t>> hi_at;
        for (std::uint32_t i = 0; i < leaves.size(); ++i) {
            const long long lo_key =
                static_cast<long long>(std::llround(axis_get(leaves[i].bounds.lo, axis) / quant));
            const long long hi_key =
                static_cast<long long>(std::llround(axis_get(leaves[i].bounds.hi, axis) / quant));
            lo_at[lo_key].push_back(i);
            hi_at[hi_key].push_back(i);
        }
        for (const auto& bucket : hi_at) {
            const auto it = lo_at.find(bucket.first);
            if (it == lo_at.end()) {
                continue;
            }
            for (std::uint32_t a : bucket.second) {
                for (std::uint32_t b : it->second) {
                    if (a != b && aabb_face_adjacent(leaves[a].bounds, leaves[b].bounds, eps)) {
                        adj[a].push_back(b);
                        adj[b].push_back(a);
                    }
                }
            }
        }
    }
    for (auto& nbrs : adj) {
        std::sort(nbrs.begin(), nbrs.end());
        nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
    }
    return adj;
}

// ─── The recursive SolidBSP builder ──────────────────────────────────────
class Builder {
   public:
    Builder(const std::vector<SolidBrush>& brushes,
            const std::vector<std::array<float, 3>>& planes,
            int max_depth)
        : brushes_(brushes), max_depth_(max_depth) {
        // Split candidates: distinct axis-aligned plane coordinates per axis.
        for (const auto& key : planes) {
            const int axis = static_cast<int>(key[0]);
            axis_coords_[static_cast<std::size_t>(axis)].push_back(key[1]);
        }
        for (auto& coords : axis_coords_) {
            std::sort(coords.begin(), coords.end());
            coords.erase(std::unique(coords.begin(),
                                     coords.end(),
                                     [](float a, float b) { return std::fabs(a - b) < 1e-4f; }),
                         coords.end());
        }
    }

    int build(const Aabb& region, int depth) {
        int best_axis = -1;
        float best_coord = 0.0f;
        if (depth < max_depth_ && pick_split(region, best_axis, best_coord)) {
            Aabb front = region;
            Aabb back = region;
            axis_set(back.hi, best_axis, best_coord);
            axis_set(front.lo, best_axis, best_coord);
            const int node_index = static_cast<int>(nodes_.size());
            nodes_.push_back(BuildNode{});
            const int f = build(front, depth + 1);
            const int b = build(back, depth + 1);
            Plane pl;
            pl.n = Vec3{0.0f, 0.0f, 0.0f};
            axis_set(pl.n, best_axis, 1.0f);
            pl.d = best_coord;
            nodes_[static_cast<std::size_t>(node_index)] = BuildNode{pl, f, b};
            return node_index;
        }
        // Atomic cell: classify by its centre.
        const bool solid = point_in_any_brush(brushes_, aabb_center(region), 1e-3f);
        const int leaf_index = static_cast<int>(leaves_.size());
        BuildLeaf leaf;
        leaf.solid = solid;
        leaf.bounds = region;
        leaves_.push_back(std::move(leaf));
        return ~leaf_index;
    }

    std::vector<BuildNode>& nodes() noexcept { return nodes_; }
    std::vector<BuildLeaf>& leaves() noexcept { return leaves_; }

   private:
    // Choose the straddling candidate plane closest to the region centre
    // (balance), preferring axis order X,Y,Z with deterministic tie-breaks.
    bool pick_split(const Aabb& region, int& out_axis, float& out_coord) const {
        constexpr float kEps = 1e-3f;
        bool found = false;
        float best_score = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float lo = axis_get(region.lo, axis);
            const float hi = axis_get(region.hi, axis);
            const float mid = 0.5f * (lo + hi);
            for (float coord : axis_coords_[static_cast<std::size_t>(axis)]) {
                if (coord <= lo + kEps || coord >= hi - kEps) {
                    continue;  // does not straddle this region
                }
                const float score = std::fabs(coord - mid);
                if (!found || score < best_score - 1e-6f) {
                    found = true;
                    best_score = score;
                    out_axis = axis;
                    out_coord = coord;
                }
            }
        }
        return found;
    }

    const std::vector<SolidBrush>& brushes_;
    std::array<std::vector<float>, 3> axis_coords_;
    std::vector<BuildNode> nodes_;
    std::vector<BuildLeaf> leaves_;
    int max_depth_;
};

// Point-in-tree descent mirroring engine Bsp::locate (d >= 0 -> front).
inline int locate_leaf(const std::vector<BuildNode>& nodes, int root_code, Vec3 p) {
    if (root_code < 0) {
        return ~root_code;
    }
    int node = root_code;
    for (int guard = 0; guard < 1 << 20; ++guard) {
        const BuildNode& n = nodes[static_cast<std::size_t>(node)];
        const float d = lmtools::plane_distance(n.plane, p);
        const int child = (d >= 0.0f) ? n.front_child : n.back_child;
        if (child < 0) {
            return ~child;
        }
        node = child;
    }
    return 0;
}

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// On-disk vertex (matches engine BspVertex field order; padded to the
// format's documented stride). See INTEGRATION.txt for the 44-vs-48 note.
// ─────────────────────────────────────────────────────────────────────────
inline void write_le_bytes(std::vector<std::uint8_t>& out, const void* src, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(src);
    out.insert(out.end(), p, p + n);
}
template <class T>
inline void write_pod(std::vector<std::uint8_t>& out, const T& v) {
    write_le_bytes(out, &v, sizeof(T));
}

// ─────────────────────────────────────────────────────────────────────────
// Core compile: parsed map -> .psybsp blob. No file I/O (for tests).
// ─────────────────────────────────────────────────────────────────────────

inline bool compile_map(const lmtools::MapFile& map,
                        const CompileOptions& opts,
                        std::vector<std::uint8_t>& blob,
                        CompileStats* out_stats,
                        std::string* out_error) {
    using detail::Aabb;
    using detail::BuildFace;
    using detail::BuildLeaf;
    using detail::BuildNode;
    using detail::SolidBrush;
    using lmtools::Vec3;

    auto fail = [&](const std::string& msg) -> bool {
        if (out_error != nullptr) {
            *out_error = msg;
        }
        return false;
    };

    // ── Gather solid brushes (worldspawn + any brush-bearing entity) ──────
    const float world_extent = lmtools::map_world_extent(map) * std::max(opts.scale, 1.0f);
    std::vector<SolidBrush> brushes;
    std::vector<lmtools::FacePolygon> all_polys;
    std::uint32_t brush_count = 0;

    auto scaled = [&](Vec3 v) { return lmtools::v3_scale(v, opts.scale); };

    for (const lmtools::MapEntity& ent : map.entities) {
        for (const lmtools::MapBrush& src_brush : ent.brushes) {
            ++brush_count;
            SolidBrush sb;
            sb.planes.reserve(src_brush.faces.size());
            // Apply --scale by scaling the plane distance (normals unchanged
            // for a uniform scale).
            for (const lmtools::MapFace& f : src_brush.faces) {
                lmtools::Plane pl = f.plane;
                pl.d *= opts.scale;
                sb.planes.push_back(pl);
            }
            // Build polygons (scaled) for geometry + bounds.
            lmtools::MapBrush scaled_brush = src_brush;
            for (lmtools::MapFace& f : scaled_brush.faces) {
                f.points[0] = scaled(f.points[0]);
                f.points[1] = scaled(f.points[1]);
                f.points[2] = scaled(f.points[2]);
                f.plane.d *= opts.scale;
            }
            std::vector<lmtools::FacePolygon> polys =
                lmtools::brush_build_polygons(scaled_brush, world_extent);
            bool any = false;
            Aabb bounds;
            for (const lmtools::FacePolygon& fp : polys) {
                for (Vec3 v : fp.vertices) {
                    if (!any) {
                        bounds.lo = v;
                        bounds.hi = v;
                        any = true;
                    } else {
                        bounds.lo = lmtools::v3_min(bounds.lo, v);
                        bounds.hi = lmtools::v3_max(bounds.hi, v);
                    }
                }
                all_polys.push_back(fp);
            }
            sb.bounds = bounds;
            brushes.push_back(std::move(sb));
        }
    }

    // ── World bounds (union of brush bounds) ──────────────────────────────
    Aabb world;
    bool have_world = false;
    for (const SolidBrush& b : brushes) {
        if (!have_world) {
            world = b.bounds;
            have_world = true;
        } else {
            world.lo = lmtools::v3_min(world.lo, b.bounds.lo);
            world.hi = lmtools::v3_max(world.hi, b.bounds.hi);
        }
    }

    std::vector<BuildNode> nodes;
    std::vector<BuildLeaf> leaves;
    int root_code = ~0;

    if (!have_world) {
        // Empty map (no brushes): single empty leaf spanning nothing.
        BuildLeaf only;
        only.solid = false;
        leaves.push_back(only);
        root_code = ~0;
    } else {
        // Distinct axis-aligned split planes from all brush faces.
        std::vector<std::array<float, 3>> axis_planes;
        for (const SolidBrush& b : brushes) {
            for (const lmtools::Plane& pl : b.planes) {
                int axis = 0;
                float coord = 0.0f;
                if (detail::plane_axis_aligned(pl, axis, coord)) {
                    axis_planes.push_back({static_cast<float>(axis), coord, 0.0f});
                }
            }
        }
        detail::Builder builder(brushes, axis_planes, opts.max_depth);
        root_code = builder.build(world, 0);
        nodes = std::move(builder.nodes());
        leaves = std::move(builder.leaves());
    }

    // Face-adjacency among cells (bucketed; near-linear). Shared by the
    // exterior flood-fill and the PVS connectivity pass.
    const std::vector<std::vector<std::uint32_t>> leaf_adjacency =
        detail::build_leaf_adjacency(leaves, 1e-3f);

    // ── Exterior cull: flood the void inward from boundary-touching empties ─
    bool leaked = false;
    {
        constexpr float kEps = 1e-3f;
        const std::size_t leaf_count = leaves.size();
        std::vector<char> is_void(leaf_count, 0);
        std::vector<std::uint32_t> stack;
        for (std::size_t i = 0; i < leaf_count; ++i) {
            if (leaves[i].solid || !have_world) {
                continue;
            }
            const Aabb& r = leaves[i].bounds;
            const bool touches = detail::axis_get(r.lo, 0) <= detail::axis_get(world.lo, 0) + kEps ||
                                 detail::axis_get(r.hi, 0) >= detail::axis_get(world.hi, 0) - kEps ||
                                 detail::axis_get(r.lo, 1) <= detail::axis_get(world.lo, 1) + kEps ||
                                 detail::axis_get(r.hi, 1) >= detail::axis_get(world.hi, 1) - kEps ||
                                 detail::axis_get(r.lo, 2) <= detail::axis_get(world.lo, 2) + kEps ||
                                 detail::axis_get(r.hi, 2) >= detail::axis_get(world.hi, 2) - kEps;
            if (touches) {
                is_void[i] = 1;
                stack.push_back(static_cast<std::uint32_t>(i));
            }
        }
        while (!stack.empty()) {
            const std::uint32_t cur = stack.back();
            stack.pop_back();
            for (std::uint32_t j : leaf_adjacency[cur]) {
                if (is_void[j] || leaves[j].solid) {
                    continue;
                }
                is_void[j] = 1;
                stack.push_back(j);
            }
        }
        std::size_t inside_empty = 0;
        for (std::size_t i = 0; i < leaf_count; ++i) {
            if (!leaves[i].solid && !is_void[i]) {
                ++inside_empty;
            }
            leaves[i].reachable_void = (is_void[i] != 0);
        }
        // If every empty cell flooded to the boundary there is no sealed
        // interior (a leak, or an open/unbounded map). Keep all empty cells
        // rather than emit a faceless, clusterless blob.
        if (inside_empty == 0 && have_world) {
            leaked = true;
            for (BuildLeaf& l : leaves) {
                l.reachable_void = false;
            }
        }
    }

    // ── Cluster assignment (inside-empty leaves only) ─────────────────────
    std::uint32_t cluster_count = 0;
    for (BuildLeaf& l : leaves) {
        if (!l.solid && !l.reachable_void) {
            l.cluster = static_cast<int>(cluster_count++);
        } else {
            l.cluster = bspfmt::kBspSolidCluster;
        }
    }

    // ── Face geometry: assign each brush face polygon to its front leaf ───
    std::vector<BuildFace> faces;
    faces.reserve(all_polys.size());
    for (const lmtools::FacePolygon& fp : all_polys) {
        if (fp.vertices.size() < 3) {
            continue;
        }
        // Sample point just in front of the polygon centroid.
        Vec3 centroid{0.0f, 0.0f, 0.0f};
        for (Vec3 v : fp.vertices) {
            centroid = lmtools::v3_add(centroid, v);
        }
        centroid = lmtools::v3_scale(centroid, 1.0f / static_cast<float>(fp.vertices.size()));
        const Vec3 sample = lmtools::v3_add(centroid, lmtools::v3_scale(fp.plane.n, 0.25f));
        const int leaf_index = leaves.empty() ? -1 : detail::locate_leaf(nodes, root_code, sample);
        if (leaf_index < 0 || leaf_index >= static_cast<int>(leaves.size())) {
            continue;
        }
        const BuildLeaf& owner = leaves[static_cast<std::size_t>(leaf_index)];
        if (owner.solid || owner.reachable_void) {
            continue;  // face borders solid / exterior -> not a visible surface
        }
        BuildFace bf;
        bf.verts = fp.vertices;
        bf.normal = fp.plane.n;
        bf.texture = fp.texture;
        bf.leaf = leaf_index;
        faces.push_back(std::move(bf));
    }

    // Group faces by owning leaf so each leaf references a contiguous range.
    for (std::size_t fi = 0; fi < faces.size(); ++fi) {
        leaves[static_cast<std::size_t>(faces[fi].leaf)].face_indices.push_back(
            static_cast<std::uint32_t>(fi));
    }

    // ── Connectivity PVS over inside-empty leaves ─────────────────────────
    // Union-find: two clusters share a PVS group when their cells are
    // face-adjacent (a portal). Conservative (no occlusion); see INTEGRATION.
    std::vector<int> uf(cluster_count);
    for (std::uint32_t i = 0; i < cluster_count; ++i) {
        uf[i] = static_cast<int>(i);
    }
    auto find = [&](int x) -> int {
        while (uf[static_cast<std::size_t>(x)] != x) {
            const int parent = uf[static_cast<std::size_t>(x)];
            uf[static_cast<std::size_t>(x)] = uf[static_cast<std::size_t>(parent)];  // path halving
            x = uf[static_cast<std::size_t>(x)];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        const int ra = find(a);
        const int rb = find(b);
        if (ra != rb) {
            uf[static_cast<std::size_t>(std::max(ra, rb))] = std::min(ra, rb);
        }
    };
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        if (leaves[i].cluster < 0) {
            continue;
        }
        for (std::uint32_t j : leaf_adjacency[i]) {
            if (leaves[j].cluster >= 0) {
                unite(leaves[i].cluster, leaves[j].cluster);
            }
        }
    }
    const std::uint32_t pvs_row_bytes = (cluster_count + 7u) / 8u;
    std::vector<std::uint8_t> pvs(static_cast<std::size_t>(pvs_row_bytes) * cluster_count, 0u);
    for (std::uint32_t a = 0; a < cluster_count; ++a) {
        for (std::uint32_t b = 0; b < cluster_count; ++b) {
            if (find(static_cast<int>(a)) == find(static_cast<int>(b))) {
                const std::size_t bit = static_cast<std::size_t>(a) * pvs_row_bytes + (b >> 3);
                pvs[bit] |= static_cast<std::uint8_t>(1u << (b & 7u));
            }
        }
    }

    // ── Assemble on-disk records ──────────────────────────────────────────
    std::vector<bspfmt::BspFileNode> file_nodes;
    file_nodes.reserve(nodes.size());
    for (const BuildNode& n : nodes) {
        bspfmt::BspFileNode fn{};
        fn.nx = n.plane.n.x;
        fn.ny = n.plane.n.y;
        fn.nz = n.plane.n.z;
        fn.d = n.plane.d;
        fn.front_child = n.front_child;
        fn.back_child = n.back_child;
        file_nodes.push_back(fn);
    }

    // Faces are emitted leaf by leaf so each leaf's range is contiguous.
    std::vector<bspfmt::BspFileFace> file_faces;
    std::vector<bspfmt::BspFileLeaf> file_leaves;
    std::vector<std::uint8_t> vertex_blob;
    std::vector<std::uint32_t> index_blob;
    std::uint32_t emitted_faces = 0;
    std::uint32_t emitted_vertices = 0;
    file_leaves.reserve(leaves.size());

    for (const BuildLeaf& leaf : leaves) {
        bspfmt::BspFileLeaf fl{};
        fl.cluster = leaf.cluster;
        fl.first_face = emitted_faces;
        fl.face_count = static_cast<std::uint32_t>(leaf.face_indices.size());
        fl.bbox_min_x = leaf.bounds.lo.x;
        fl.bbox_min_y = leaf.bounds.lo.y;
        fl.bbox_min_z = leaf.bounds.lo.z;
        fl.bbox_max_x = leaf.bounds.hi.x;
        fl.bbox_max_y = leaf.bounds.hi.y;
        fl.bbox_max_z = leaf.bounds.hi.z;
        file_leaves.push_back(fl);

        for (std::uint32_t fidx : leaf.face_indices) {
            const BuildFace& bf = faces[fidx];
            const std::uint32_t first_vertex = emitted_vertices;
            const std::uint32_t vcount = static_cast<std::uint32_t>(bf.verts.size());

            // Planar lightmap UV parameterisation: project onto a face basis
            // and normalise into [0,1] over the face's own extent. lm_bake
            // re-packs these into an atlas (see SEAM note in INTEGRATION.txt).
            Vec3 up =
                (std::fabs(bf.normal.z) < 0.9f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
            const Vec3 t = lmtools::v3_normalize(lmtools::v3_cross(up, bf.normal));
            const Vec3 b = lmtools::v3_normalize(lmtools::v3_cross(bf.normal, t));
            float u_lo = 0.0f, u_hi = 0.0f, v_lo = 0.0f, v_hi = 0.0f;
            for (std::size_t i = 0; i < bf.verts.size(); ++i) {
                const float uu = lmtools::v3_dot(bf.verts[i], t);
                const float vv = lmtools::v3_dot(bf.verts[i], b);
                if (i == 0) {
                    u_lo = u_hi = uu;
                    v_lo = v_hi = vv;
                } else {
                    u_lo = std::min(u_lo, uu);
                    u_hi = std::max(u_hi, uu);
                    v_lo = std::min(v_lo, vv);
                    v_hi = std::max(v_hi, vv);
                }
            }
            const float u_span = (u_hi - u_lo) > 1e-6f ? (u_hi - u_lo) : 1.0f;
            const float v_span = (v_hi - v_lo) > 1e-6f ? (v_hi - v_lo) : 1.0f;

            for (Vec3 vtx : bf.verts) {
                const float pos[3] = {vtx.x, vtx.y, vtx.z};
                const float nrm[3] = {bf.normal.x, bf.normal.y, bf.normal.z};
                const float uu = lmtools::v3_dot(vtx, t);
                const float vv = lmtools::v3_dot(vtx, b);
                const float uv[2] = {uu, vv};
                const float luv[2] = {(uu - u_lo) / u_span, (vv - v_lo) / v_span};
                const std::uint32_t color = 0xFFFFFFFFu;
                const std::uint32_t pad = 0u;
                write_le_bytes(vertex_blob, pos, sizeof(pos));
                write_le_bytes(vertex_blob, nrm, sizeof(nrm));
                write_le_bytes(vertex_blob, uv, sizeof(uv));
                write_le_bytes(vertex_blob, luv, sizeof(luv));
                write_pod(vertex_blob, color);
                write_pod(vertex_blob, pad);  // pad to kBspFileVertexBytes (48)
            }
            // Fan triangulation: (0,1,2),(0,2,3),... global vertex indices.
            for (std::uint32_t i = 1; i + 1 < vcount; ++i) {
                index_blob.push_back(first_vertex);
                index_blob.push_back(first_vertex + i);
                index_blob.push_back(first_vertex + i + 1);
            }

            bspfmt::BspFileFace ff{};
            ff.first_vertex = first_vertex;
            ff.vertex_count = vcount;
            ff.material = fnv1a32(bf.texture);
            ff.lightmap = emitted_faces;  // one lightmap region per face (lm_bake)
            file_faces.push_back(ff);

            emitted_vertices += vcount;
            ++emitted_faces;
        }
    }

    // ── Header + chunk layout (all offsets 4-byte aligned) ────────────────
    const std::uint32_t header_bytes = static_cast<std::uint32_t>(sizeof(bspfmt::BspFileHeader));
    std::uint32_t cursor = header_bytes;
    const std::uint32_t nodes_off = cursor;
    cursor += static_cast<std::uint32_t>(file_nodes.size() * sizeof(bspfmt::BspFileNode));
    const std::uint32_t leaves_off = cursor;
    cursor += static_cast<std::uint32_t>(file_leaves.size() * sizeof(bspfmt::BspFileLeaf));
    const std::uint32_t faces_off = cursor;
    cursor += static_cast<std::uint32_t>(file_faces.size() * sizeof(bspfmt::BspFileFace));
    const std::uint32_t verts_off = cursor;
    cursor += static_cast<std::uint32_t>(vertex_blob.size());
    const std::uint32_t indices_off = cursor;
    cursor += static_cast<std::uint32_t>(index_blob.size() * sizeof(std::uint32_t));
    const std::uint32_t pvs_off = cursor;
    cursor += static_cast<std::uint32_t>(pvs.size());
    const std::uint32_t total_bytes = cursor;

    bspfmt::BspFileHeader header{};
    header.magic = bspfmt::kBspFileMagic;
    header.version = bspfmt::kBspFileVersion;
    header.flags = 0;
    header.total_bytes = total_bytes;
    header.cluster_count = cluster_count;
    header.pvs_row_bytes = pvs_row_bytes;
    header.nodes = {nodes_off, static_cast<std::uint32_t>(file_nodes.size())};
    header.leaves = {leaves_off, static_cast<std::uint32_t>(file_leaves.size())};
    header.faces = {faces_off, static_cast<std::uint32_t>(file_faces.size())};
    header.vertices = {verts_off, emitted_vertices};
    header.indices = {indices_off, static_cast<std::uint32_t>(index_blob.size())};
    header.pvs = {pvs_off, static_cast<std::uint32_t>(pvs.size())};
    for (std::uint32_t& r : header.reserved) {
        r = 0u;
    }

    blob.clear();
    blob.reserve(total_bytes);
    write_pod(blob, header);
    for (const auto& n : file_nodes) {
        write_pod(blob, n);
    }
    for (const auto& l : file_leaves) {
        write_pod(blob, l);
    }
    for (const auto& f : file_faces) {
        write_pod(blob, f);
    }
    write_le_bytes(blob, vertex_blob.data(), vertex_blob.size());
    for (std::uint32_t idx : index_blob) {
        write_pod(blob, idx);
    }
    write_le_bytes(blob, pvs.data(), pvs.size());

    if (blob.size() != total_bytes) {
        return fail("internal: assembled blob size mismatch");
    }

    if (out_stats != nullptr) {
        std::uint32_t solid = 0;
        std::uint32_t empty = 0;
        for (const BuildLeaf& l : leaves) {
            if (l.solid || l.reachable_void) {
                ++solid;
            } else {
                ++empty;
            }
        }
        out_stats->bytes_written = blob.size();
        out_stats->brush_count = brush_count;
        out_stats->node_count = static_cast<std::uint32_t>(file_nodes.size());
        out_stats->leaf_count = static_cast<std::uint32_t>(file_leaves.size());
        out_stats->solid_leaf_count = solid;
        out_stats->empty_leaf_count = empty;
        out_stats->face_count = emitted_faces;
        out_stats->vertex_count = emitted_vertices;
        out_stats->index_count = static_cast<std::uint32_t>(index_blob.size());
        out_stats->cluster_count = cluster_count;
        out_stats->pvs_row_bytes = pvs_row_bytes;
        out_stats->leaked = leaked;
        if (have_world) {
            out_stats->bounds_min[0] = world.lo.x;
            out_stats->bounds_min[1] = world.lo.y;
            out_stats->bounds_min[2] = world.lo.z;
            out_stats->bounds_max[0] = world.hi.x;
            out_stats->bounds_max[1] = world.hi.y;
            out_stats->bounds_max[2] = world.hi.z;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// File-driven compile: read .map, parse, compile, write .psybsp.
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

inline bool compile(const CompileOptions& opts,
                    CompileStats* out_stats = nullptr,
                    std::vector<std::uint8_t>* out_blob = nullptr) {
    auto log_err = [&](const std::string& msg) {
        std::fprintf(stderr, "lm_qbsp: error: %s\n", msg.c_str());
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
    CompileStats stats;
    if (!compile_map(map, opts, blob, &stats, &err)) {
        log_err(err);
        return false;
    }
    stats.source_hash = source_hash;

    if (stats.leaked && !opts.quiet) {
        std::fprintf(stderr,
                     "lm_qbsp: warning: no sealed interior found (map leaks to the void or is "
                     "open); keeping all empty cells as clusters.\n");
    }

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

}  // namespace psy::lm_qbsp
