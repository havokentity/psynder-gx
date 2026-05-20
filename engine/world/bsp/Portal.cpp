// SPDX-License-Identifier: MIT
// Psynder — portal-based visibility culling (Wave B). Lane 12 owns.
//
// Two pieces live here:
//
//   1. build_portal_set_from_tree() — the classic Quake qbsp "MakeTreePortals"
//      flood, adapted to the runtime BspMap. We seed 6 portals on a box around
//      the world, then recursively push each node's split plane down the tree,
//      clipping it against the cell it sits in and splitting the inherited
//      portals across the plane. What falls out is a portal polygon for every
//      pair of adjacent leaf cells.
//
//   2. walk_portal_visible_leaves() — from the camera's leaf we flood the
//      portal graph, clipping the view frustum through each portal so that a
//      leaf reached via a portal that fell outside the *clipped* frustum is
//      rejected. The result is intersected with the PVS (a leaf is only visited
//      if it is PVS-visible from the eye's cluster), so portal culling is by
//      construction a subset of PVS — it tightens PVS, never loosens it.
//
// build_portal_set(const BspMap&) (declared in Portal.h) stays a no-op that
// returns an empty set: the frozen .psybsp format has no portals chunk to read
// and the frozen BspMap ABI has no portal field, so there is nothing to derive
// portals from at that entry point. Callers that want real portals call the
// runtime portalizer in PortalGen.h. With an empty portal set
// walk_portal_visible_leaves() degrades to PVS-only, matching Wave A behaviour.

// NOTE: Bsp.h / Portal.h use std::vector without including <vector> (frozen
// public headers — see AGENTS.md "Public-header contracts"); pre-include here.
#include <vector>

#include "Bsp.h"
#include "Portal.h"
#include "PortalGen.h"
#include "BspFormat.h"

#include "math/Math.h"
#include "math/Bounds.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace psynder::world::bsp {

namespace {

// ─── Plane + winding primitives ──────────────────────────────────────────
// A plane is { n, d } with the surface at dot(n, p) == d; the "front" (positive)
// side is dot(n, p) - d > 0. This matches the BspNode convention used by
// locate(): front child when dot(normal, point) - plane_d >= 0.
struct Plane {
    math::Vec3 n;
    f32        d;
};

using Winding = std::vector<math::Vec3>;

// Clipping epsilon. World units are metres; 1 mm is far below any real BSP
// feature size yet comfortably above f32 noise for the winding sizes we build.
constexpr f32 kEps = 1.0e-3f;

inline f32 plane_dist(const Plane& pl, math::Vec3 v) noexcept {
    return math::dot(pl.n, v) - pl.d;
}

inline Plane plane_flip(const Plane& pl) noexcept {
    return Plane{ math::mul(pl.n, -1.0f), -pl.d };
}

// Normalize a (normal, d) pair so the normal is unit length, preserving the
// zero set and the front/back sign. BSP planes ship unit-normal, but a defensive
// renormalize keeps the geometry robust against authored or cooked drift.
inline Plane plane_normalized(math::Vec3 n, f32 d) noexcept {
    const f32 len = math::length(n);
    if (len > 0.0f) {
        const f32 inv = 1.0f / len;
        return Plane{ math::mul(n, inv), d * inv };
    }
    return Plane{ n, d };
}

// A large quad lying on `pl`, centred on the plane's foot point, big enough to
// span the world before it gets clipped down to a cell. Mirrors Quake's
// BaseWindingForPlane.
Winding base_winding_for_plane(const Plane& pl, f32 size) {
    const f32 ax = std::fabs(pl.n.x);
    const f32 ay = std::fabs(pl.n.y);
    const f32 az = std::fabs(pl.n.z);
    // "up" is any axis not (near-)parallel to the normal.
    math::Vec3 up = (az >= ax && az >= ay) ? math::Vec3{1, 0, 0}
                                           : math::Vec3{0, 0, 1};
    const f32 proj = math::dot(up, pl.n);
    up = math::normalize(math::sub(up, math::mul(pl.n, proj)));
    const math::Vec3 org   = math::mul(pl.n, pl.d);   // foot point (unit n)
    const math::Vec3 right = math::cross(up, pl.n);
    const math::Vec3 us    = math::mul(up, size);
    const math::Vec3 rs    = math::mul(right, size);
    Winding w;
    w.reserve(4);
    w.push_back(math::add(math::sub(org, rs), us));
    w.push_back(math::add(math::add(org, rs), us));
    w.push_back(math::sub(math::add(org, rs), us));
    w.push_back(math::sub(math::sub(org, rs), us));
    return w;
}

// Split `in` into the half on the front side of `s` and the half on the back
// side, inserting the intersection vertices on crossing edges. Either output
// may come back empty.
void split_winding(const Winding& in, const Plane& s, f32 eps,
                   Winding& front, Winding& back) {
    front.clear();
    back.clear();
    const usize n = in.size();
    if (n == 0) {
        return;
    }
    std::vector<f32> dists(n);
    std::vector<int> sides(n);
    for (usize i = 0; i < n; ++i) {
        const f32 dd = plane_dist(s, in[i]);
        dists[i] = dd;
        sides[i] = dd > eps ? 1 : (dd < -eps ? -1 : 0);
    }
    for (usize i = 0; i < n; ++i) {
        const math::Vec3& p1 = in[i];
        const int s1 = sides[i];
        if (s1 >= 0) {
            front.push_back(p1);
        }
        if (s1 <= 0) {
            back.push_back(p1);
        }
        const usize j  = (i + 1) % n;
        const int   s2 = sides[j];
        // Only a strict front<->back crossing produces a new split vertex; an
        // on-plane endpoint is already shared into both sides above.
        if (s1 == 0 || s2 == 0 || s1 == s2) {
            continue;
        }
        const f32 t = dists[i] / (dists[i] - dists[j]);
        const math::Vec3 mid =
            math::add(p1, math::mul(math::sub(in[j], p1), t));
        front.push_back(mid);
        back.push_back(mid);
    }
}

// Keep only the front half of `w` (clip away everything behind `s`).
Winding chop_front(const Winding& w, const Plane& s, f32 eps) {
    Winding f;
    Winding b;
    split_winding(w, s, eps, f, b);
    return f;
}

f32 winding_area(const Winding& w) {
    if (w.size() < 3) {
        return 0.0f;
    }
    math::Vec3 acc{0, 0, 0};
    for (usize i = 1; i + 1 < w.size(); ++i) {
        acc = math::add(acc, math::cross(math::sub(w[i], w[0]),
                                         math::sub(w[i + 1], w[0])));
    }
    return 0.5f * math::length(acc);
}

inline bool winding_valid(const Winding& w) {
    return w.size() >= 3 && winding_area(w) > kEps;
}

math::Vec3 winding_centroid(const Winding& w) {
    math::Vec3 c{0, 0, 0};
    for (const math::Vec3& v : w) {
        c = math::add(c, v);
    }
    const f32 inv = w.size() ? (1.0f / static_cast<f32>(w.size())) : 0.0f;
    return math::mul(c, inv);
}

// ─── Tree portalizer (build_portal_set_from_tree) ────────────────────────
// Child refs reuse the BspNode encoding: >= 0 is a node index, < 0 is a leaf
// (~leaf_index). kOutsideRef is a sentinel for the exterior void created by the
// bounding box; it is never a node or a real leaf.
constexpr i32 kOutsideRef = std::numeric_limits<i32>::min();

inline bool ref_is_node(i32 r) noexcept { return r >= 0; }
inline bool ref_is_leaf(i32 r) noexcept { return r < 0 && r != kOutsideRef; }

struct WorkPortal {
    Winding w;
    Plane   plane;     // node plane orientation: normal toward side[0]
    i32     side[2];   // side[0] = front (+normal), side[1] = back (-normal)
    bool    dead = false;
};

}  // namespace

BspPortalSet build_portal_set_from_tree(const BspMap& map) {
    BspPortalSet out;
    const i32 node_count = static_cast<i32>(map.nodes.size());
    const i32 leaf_count = static_cast<i32>(map.leaves.size());
    if (leaf_count == 0 || node_count == 0) {
        return out;  // single-leaf or empty tree: no interior splits, no portals
    }

    // World bounds → a generous bounding box we seed portals on.
    math::Aabb world = math::aabb_empty();
    for (const BspLeaf& l : map.leaves) {
        world = math::aabb_union(world, l.bounds);
    }
    if (math::is_empty(world)) {
        return out;
    }
    world = math::expand(world, 1.0f);  // 1 m skin so box faces clear geometry
    const math::Vec3 ext     = math::size(world);
    const f32        base_sz = math::length(ext) * 2.0f + 64.0f;

    std::vector<WorkPortal>        portals;
    std::vector<std::vector<i32>>  node_portals(static_cast<usize>(node_count));
    portals.reserve(static_cast<usize>(node_count) * 3 + 6);

    auto plane_of = [&](i32 nidx) -> Plane {
        const BspNode& n = map.nodes[static_cast<usize>(nidx)];
        return plane_normalized(n.plane_normal, n.plane_d);
    };
    auto attach = [&](i32 pidx, i32 ref) {
        if (ref_is_node(ref) && ref < node_count) {
            node_portals[static_cast<usize>(ref)].push_back(pidx);
        }
    };

    // ── Seed: 6 portals on the world box, each connecting headnode <-> void. ──
    const math::Vec3 wmin = world.min;
    const math::Vec3 wmax = world.max;
    const math::Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int a = 0; a < 3; ++a) {
        for (int s = 0; s < 2; ++s) {
            // Inward-facing plane so the box interior (headnode) is the front.
            const f32        sign = (s == 0) ? -1.0f : 1.0f;     // -max face / +min face
            const math::Vec3 n    = math::mul(axes[a], sign);
            const f32 face = (s == 0) ? ((a == 0) ? wmax.x : (a == 1) ? wmax.y : wmax.z)
                                      : ((a == 0) ? wmin.x : (a == 1) ? wmin.y : wmin.z);
            const Plane pl{ n, math::dot(n, math::mul(axes[a], face)) };
            WorkPortal wp;
            wp.w       = base_winding_for_plane(pl, base_sz);
            wp.plane   = pl;
            wp.side[0] = 0;            // headnode (interior, front of inward face)
            wp.side[1] = kOutsideRef;  // exterior void
            const i32 pidx = static_cast<i32>(portals.size());
            portals.push_back(std::move(wp));
            attach(pidx, 0);
        }
    }

    // ── MakeTreePortals: pre-order DFS, parent processed before its children. ──
    std::vector<i32> stack;
    stack.push_back(0);
    while (!stack.empty()) {
        const i32 nidx = stack.back();
        stack.pop_back();
        const BspNode& node = map.nodes[static_cast<usize>(nidx)];
        const Plane    npl  = plane_of(nidx);

        // MakeNodePortal: the node plane clipped to this node's convex cell.
        {
            Winding w = base_winding_for_plane(npl, base_sz);
            for (const i32 pidx : node_portals[static_cast<usize>(nidx)]) {
                const WorkPortal& p = portals[static_cast<usize>(pidx)];
                const Plane clip = (p.side[0] == nidx) ? p.plane : plane_flip(p.plane);
                w = chop_front(w, clip, kEps);
                if (w.size() < 3) {
                    break;
                }
            }
            if (winding_valid(w)) {
                WorkPortal np;
                np.plane   = npl;
                np.side[0] = node.front_child;  // +normal side
                np.side[1] = node.back_child;   // -normal side
                np.w       = std::move(w);
                const i32 pidx = static_cast<i32>(portals.size());
                portals.push_back(std::move(np));
                attach(pidx, node.front_child);
                attach(pidx, node.back_child);
            }
        }

        // SplitNodePortals: hand this node's portals down to its two children.
        {
            const i32 fchild = node.front_child;
            const i32 bchild = node.back_child;
            const std::vector<i32> list = node_portals[static_cast<usize>(nidx)];
            node_portals[static_cast<usize>(nidx)].clear();
            for (const i32 pidx : list) {
                const i32   s0   = portals[static_cast<usize>(pidx)].side[0];
                const i32   s1   = portals[static_cast<usize>(pidx)].side[1];
                const int   side = (s0 == nidx) ? 0 : 1;
                const Plane pplane = portals[static_cast<usize>(pidx)].plane;
                Winding fw;
                Winding bw;
                split_winding(portals[static_cast<usize>(pidx)].w, npl, kEps, fw, bw);
                const bool has_f = winding_valid(fw);
                const bool has_b = winding_valid(bw);
                if (!has_f && !has_b) {
                    portals[static_cast<usize>(pidx)].dead = true;
                    continue;
                }
                if (has_f && has_b) {
                    // Front piece stays as `pidx`; back piece becomes a new portal.
                    portals[static_cast<usize>(pidx)].w       = std::move(fw);
                    portals[static_cast<usize>(pidx)].side[side] = fchild;
                    attach(pidx, fchild);

                    WorkPortal q;
                    q.plane   = pplane;
                    q.side[0] = s0;
                    q.side[1] = s1;
                    q.side[side] = bchild;
                    q.w       = std::move(bw);
                    const i32 qidx = static_cast<i32>(portals.size());
                    portals.push_back(std::move(q));
                    attach(qidx, bchild);
                    attach(qidx, (side == 0) ? s1 : s0);  // the unchanged other side
                } else if (has_f) {
                    portals[static_cast<usize>(pidx)].w       = std::move(fw);
                    portals[static_cast<usize>(pidx)].side[side] = fchild;
                    attach(pidx, fchild);
                } else {
                    portals[static_cast<usize>(pidx)].w       = std::move(bw);
                    portals[static_cast<usize>(pidx)].side[side] = bchild;
                    attach(pidx, bchild);
                }
            }
        }

        if (ref_is_node(node.front_child)) {
            stack.push_back(node.front_child);
        }
        if (ref_is_node(node.back_child)) {
            stack.push_back(node.back_child);
        }
    }

    // ── Collect interior leaf<->leaf portals (drop void / solid walls). ──
    for (const WorkPortal& p : portals) {
        if (p.dead || !ref_is_leaf(p.side[0]) || !ref_is_leaf(p.side[1])) {
            continue;
        }
        const i32 fl = bsp_leaf_index(p.side[0]);  // +normal side
        const i32 bl = bsp_leaf_index(p.side[1]);  // -normal side
        if (fl < 0 || fl >= leaf_count || bl < 0 || bl >= leaf_count) {
            continue;
        }
        if (map.leaves[static_cast<usize>(fl)].cluster < 0 ||
            map.leaves[static_cast<usize>(bl)].cluster < 0) {
            continue;  // a face of solid geometry, not a see-through portal
        }
        if (p.w.size() < 3) {
            continue;
        }
        BspPortal bp;
        bp.front_leaf   = fl;
        bp.back_leaf    = bl;
        bp.first_vertex = static_cast<u32>(out.vertices.size());
        bp.vertex_count = static_cast<u32>(p.w.size());
        // BspPortal's normal points front_leaf -> back_leaf; the node plane
        // normal points toward the front (+normal) side, so flip it.
        bp.plane_normal = math::mul(p.plane.n, -1.0f);
        bp.plane_d      = -p.plane.d;
        for (const math::Vec3& v : p.w) {
            out.vertices.push_back(v);
        }
        out.portals.push_back(bp);
    }
    return out;
}

// ─── PVS gate + leaf locate (mirror Bsp.cpp so results stay a PVS subset) ──
namespace {

// Mirror of walk_visible_leaves' PVS row resolution, so the portal walk culls
// against exactly the same PVS bits. `all` means "no usable PVS data → treat
// every cluster as visible" (conservative, matches the loader fallback).
struct PvsRow {
    const u8* row           = nullptr;
    u32       cluster_count = 0;
    u32       row_bytes     = 0;
    bool      all           = true;
};

PvsRow resolve_pvs(const BspMap& map, i32 eye_cluster) {
    PvsRow r;
    i32 max_cluster = 0;
    for (const BspLeaf& l : map.leaves) {
        if (l.cluster > max_cluster) {
            max_cluster = l.cluster;
        }
    }
    const u32 cc = static_cast<u32>(max_cluster + 1);
    if (cc == 0 || map.pvs.empty()) {
        return r;  // all = true
    }
    const u32 rb = static_cast<u32>(map.pvs.size()) / cc;
    if (rb == 0 || eye_cluster < 0 || static_cast<u32>(eye_cluster) >= cc) {
        return r;  // all = true
    }
    r.row           = map.pvs.data() + static_cast<usize>(eye_cluster) * rb;
    r.cluster_count = cc;
    r.row_bytes     = rb;
    r.all           = false;
    return r;
}

bool pvs_visible(const PvsRow& r, i32 cluster) {
    if (cluster < 0) {
        return false;
    }
    if (r.all) {
        return true;
    }
    const u32 ci = static_cast<u32>(cluster);
    if (ci >= r.cluster_count) {
        return false;
    }
    return (r.row[ci >> 3] & static_cast<u8>(1u << (ci & 7u))) != 0;
}

// Index-returning variant of locate() (the public locate returns a leaf copy,
// but the portal graph is addressed by leaf index).
i32 locate_index(const BspMap& map, math::Vec3 point) {
    if (map.leaves.empty()) {
        return -1;
    }
    if (map.nodes.empty()) {
        return 0;
    }
    i32       node_index = 0;
    const i32 max_depth  = static_cast<i32>(map.leaves.size()) * 2 + 64;
    for (i32 step = 0; step < max_depth; ++step) {
        const BspNode& n = map.nodes[static_cast<usize>(node_index)];
        const f32 d = math::dot(n.plane_normal, point) - n.plane_d;
        const i32 child = (d >= 0.0f) ? n.front_child : n.back_child;
        if (bsp_is_leaf(child)) {
            return bsp_leaf_index(child);
        }
        node_index = child;
    }
    return 0;
}

// Internal frustum: inward planes, point inside iff plane_dist >= 0 for all.
using ClipFrustum = std::vector<Plane>;

ClipFrustum frustum_from_public(const PortalFrustum& pf) {
    ClipFrustum f;
    const u32 n = std::min<u32>(pf.plane_count, 6u);
    f.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        f.push_back(Plane{ pf.normals[i], pf.d[i] });
    }
    return f;
}

// PortalFrustum has 6 plane slots; a clipped portal cone can have more edges.
// Keeping only the first 6 yields a looser (superset) frustum, which is safe —
// the renderer's in-leaf cull stays conservative. The walk itself clips against
// the full-precision ClipFrustum, so visibility is unaffected by the truncation.
PortalFrustum frustum_to_public(const ClipFrustum& f) {
    PortalFrustum pf{};
    const u32 n = static_cast<u32>(std::min<usize>(f.size(), 6));
    for (u32 i = 0; i < n; ++i) {
        pf.normals[i] = f[i].n;
        pf.d[i]       = f[i].d;
    }
    pf.plane_count = n;
    return pf;
}

Winding clip_winding_to_frustum(Winding w, const ClipFrustum& f) {
    for (const Plane& pl : f) {
        if (w.size() < 3) {
            return Winding{};
        }
        w = chop_front(w, pl, kEps);
    }
    return w;
}

// Build the lateral planes of the cone from `eye` through polygon `w`. A point
// is inside iff it is in front of every edge plane.
ClipFrustum cone_from_winding(math::Vec3 eye, const Winding& w) {
    ClipFrustum out;
    const usize n = w.size();
    if (n < 3) {
        return out;
    }
    const math::Vec3 centroid = winding_centroid(w);
    out.reserve(n);
    for (usize i = 0; i < n; ++i) {
        const math::Vec3& a = w[i];
        const math::Vec3& b = w[(i + 1) % n];
        math::Vec3 nrm = math::cross(math::sub(a, eye), math::sub(b, eye));
        const f32 len = math::length(nrm);
        if (len < 1.0e-6f) {
            continue;  // eye colinear with this edge → degenerate plane
        }
        nrm = math::mul(nrm, 1.0f / len);
        f32 d = math::dot(nrm, eye);
        if (math::dot(nrm, centroid) - d < 0.0f) {  // orient inward
            nrm = math::mul(nrm, -1.0f);
            d   = -d;
        }
        out.push_back(Plane{ nrm, d });
    }
    return out;
}

// Recursive portal flood. All cones emanate from the fixed eye, so frustums
// only ever tighten; that plus the came-from skip and the depth cap bound the
// walk on cyclic portal graphs.
struct PortalWalk {
    const BspMap&                            map;
    const BspPortalSet&                      portals;
    math::Vec3                               eye;
    const std::vector<std::vector<u32>>&     leaf_portals;
    const PvsRow&                            pvs;
    void (*emit)(const BspLeaf&, const PortalFrustum&, void*);
    void*                                    user;
    i32                                      leaf_count;
    i32                                      max_depth;

    void go(i32 cur, const ClipFrustum& frustum, u32 came, i32 depth) {
        if (depth > max_depth) {
            return;
        }
        for (const u32 pidx : leaf_portals[static_cast<usize>(cur)]) {
            if (pidx == came) {
                continue;
            }
            const BspPortal& pr = portals.portals[pidx];
            const i32 next = (pr.front_leaf == cur) ? pr.back_leaf : pr.front_leaf;
            if (next < 0 || next >= leaf_count) {
                continue;
            }
            const i32 ncluster = map.leaves[static_cast<usize>(next)].cluster;
            if (ncluster < 0 || !pvs_visible(pvs, ncluster)) {
                continue;  // solid or not PVS-visible → never traversed
            }
            // Orient the portal plane to point into the neighbour, then require
            // the eye on the current side (don't step back through a portal).
            math::Vec3 n;
            f32        d;
            if (pr.front_leaf == cur) {
                n = pr.plane_normal;
                d = pr.plane_d;
            } else {
                n = math::mul(pr.plane_normal, -1.0f);
                d = -pr.plane_d;
            }
            if (math::dot(n, eye) - d > kEps) {
                continue;
            }
            Winding pw;
            pw.reserve(pr.vertex_count);
            for (u32 k = 0; k < pr.vertex_count; ++k) {
                pw.push_back(portals.vertices[pr.first_vertex + k]);
            }
            Winding clipped = clip_winding_to_frustum(std::move(pw), frustum);
            if (clipped.size() < 3) {
                continue;  // portal fully outside the current frustum
            }
            ClipFrustum next_frustum = cone_from_winding(eye, clipped);
            if (next_frustum.empty()) {
                continue;
            }
            // A leaf may be reported once per distinct portal path that reaches
            // it, each with the frustum clipped along that path (useful for
            // per-path in-leaf culling); consumers that submit draws dedup.
            emit(map.leaves[static_cast<usize>(next)], frustum_to_public(next_frustum), user);
            go(next, next_frustum, pidx, depth + 1);
        }
    }
};

}  // namespace

// ─── Public API: Portal.h ─────────────────────────────────────────────────

// build_portal_set keeps the Wave A contract: an empty set (see file header).
BspPortalSet build_portal_set(const BspMap& /*map*/) {
    return BspPortalSet{};
}

void walk_portal_visible_leaves(const BspMap&        map,
                               const BspPortalSet&  portals,
                               math::Vec3           eye,
                               const PortalFrustum& initial,
                               void (*emit)(const BspLeaf&,
                                            const PortalFrustum&,
                                            void* user),
                               void*                user) {
    if (emit == nullptr) {
        return;
    }

    // No portal graph → fall back to PVS-only visibility (Wave A behaviour).
    if (portals.portals.empty()) {
        struct Ctx {
            void (*cb)(const BspLeaf&, const PortalFrustum&, void*);
            void*                user;
            const PortalFrustum* frustum;
        };
        Ctx ctx{ emit, user, &initial };
        auto bridge = +[](const BspLeaf& leaf, void* u) {
            Ctx& c = *static_cast<Ctx*>(u);
            c.cb(leaf, *c.frustum, c.user);
        };
        walk_visible_leaves(map, eye, bridge, &ctx);
        return;
    }

    if (map.leaves.empty()) {
        return;
    }
    const i32 leaf_count = static_cast<i32>(map.leaves.size());
    const i32 eye_idx    = locate_index(map, eye);
    if (eye_idx < 0 || eye_idx >= leaf_count) {
        return;
    }
    const i32 eye_cluster = map.leaves[static_cast<usize>(eye_idx)].cluster;
    if (eye_cluster < 0) {
        return;  // eye in solid → see nothing (matches PVS walk)
    }

    const PvsRow pvs = resolve_pvs(map, eye_cluster);

    // Leaf → incident portal indices.
    std::vector<std::vector<u32>> leaf_portals(static_cast<usize>(leaf_count));
    for (u32 i = 0; i < portals.portals.size(); ++i) {
        const BspPortal& p = portals.portals[i];
        if (p.front_leaf >= 0 && p.front_leaf < leaf_count) {
            leaf_portals[static_cast<usize>(p.front_leaf)].push_back(i);
        }
        if (p.back_leaf >= 0 && p.back_leaf < leaf_count) {
            leaf_portals[static_cast<usize>(p.back_leaf)].push_back(i);
        }
    }

    // The eye's own leaf is always visible.
    emit(map.leaves[static_cast<usize>(eye_idx)], initial, user);

    const ClipFrustum f0 = frustum_from_public(initial);
    PortalWalk walk{ map, portals, eye, leaf_portals, pvs,
                     emit, user, leaf_count, leaf_count + 8 };
    walk.go(eye_idx, f0, static_cast<u32>(-1), 0);
}

}  // namespace psynder::world::bsp
