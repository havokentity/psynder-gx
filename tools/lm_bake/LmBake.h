// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 24 tools: lm_bake offline path-traced lightmap baker.
//
// Bakes a static lightmap atlas for a set of planar surfaces lit by a set of
// lights, and writes it in the engine cooked-texture format ".lmt"
// (engine/asset/Formats.h LmtHeader, RGBA8). A baked lightmap *is* a
// texture, so the .lmt cooked-texture container is the natural seam — see
// the SEAM note in INTEGRATION.txt.
//
// Header-only (every free function `inline`) so the unit tests drive the
// baker through a relative-path include and run in the default
// (PSYNDER_GX_BUILD_TOOLS=OFF) test build.
//
// ──────────────────────────────────────────────────────────────────────────
//  The tracer
// ──────────────────────────────────────────────────────────────────────────
//
//   * Geometry: each surface is a planar convex polygon (fan-triangulated
//     into the occluder triangle pool). A median-split BVH accelerates
//     ray queries (closest-hit for indirect bounces, any-hit for shadows).
//   * Direct lighting: point lights (inverse-square falloff) and directional
//     lights, each shadow-tested with a ray to the source.
//   * Indirect lighting: cosine-weighted hemisphere sampling with a fixed
//     per-texel deterministic PCG32 stream, gathering N samples; a ray that
//     hits a surface contributes that surface's diffuse-reflected radiance
//     (recursively, up to `bounces`); a ray that escapes contributes the
//     ambient sky radiance.
//   * Output is stored irradiance (the light arriving at the surface), so
//     the runtime shader multiplies it by the base texture — the classic
//     lightmap convention.
//
//  Units: metric. Light positions are metres; point-light intensity uses
//  inverse-square (1/d^2) falloff in metres. 1 world unit = 1 metre.
//
//  Determinism: the only randomness is the PCG32 stream, seeded purely from
//  (global seed, surface index, texel x, texel y). No time / pid / RNG
//  device. Identical inputs ─► byte-identical .lmt (verified by tests).

#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "asset/Formats.h"

namespace psy::lm_bake {

namespace fmt = psynder::asset::formats;

// ─────────────────────────────────────────────────────────────────────────
// Local f32 vector math (self-contained, deterministic).
// ─────────────────────────────────────────────────────────────────────────

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(Vec3 a, float s) noexcept {
    return Vec3{a.x * s, a.y * s, a.z * s};
}
inline Vec3 mul(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
}
inline float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) noexcept {
    return std::sqrt(dot(a, a));
}
inline Vec3 normalize(Vec3 a) noexcept {
    const float len = length(a);
    if (len <= 1e-20f) {
        return Vec3{0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / len;
    return Vec3{a.x * inv, a.y * inv, a.z * inv};
}
inline Vec3 vmin(Vec3 a, Vec3 b) noexcept {
    return Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline Vec3 vmax(Vec3 a, Vec3 b) noexcept {
    return Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

// ─────────────────────────────────────────────────────────────────────────
// Scene description.
// ─────────────────────────────────────────────────────────────────────────

struct BakeMaterial {
    Vec3 albedo{0.7f, 0.7f, 0.7f};  // diffuse reflectance in [0,1]
};

struct BakeSurface {
    std::vector<Vec3> vertices;  // planar convex polygon, CCW around normal
    Vec3 normal{0.0f, 0.0f, 1.0f};
    std::uint32_t material = 0;  // index into Scene::materials
};

struct BakeLight {
    enum class Kind : std::uint8_t { Point, Directional };
    Kind kind = Kind::Point;
    Vec3 position{0.0f, 0.0f, 0.0f};    // point light location (metres)
    Vec3 direction{0.0f, 0.0f, -1.0f};  // directional: travel direction
    Vec3 color{1.0f, 1.0f, 1.0f};       // linear RGB
    float intensity = 1.0f;             // point: candela-like; directional: irradiance
};

struct Scene {
    std::vector<BakeSurface> surfaces;
    std::vector<BakeMaterial> materials;
    std::vector<BakeLight> lights;
};

struct BakeOptions {
    std::string output_path;        // .lmt path; "" => no file write
    float texels_per_metre = 4.0f;  // lightmap density
    std::uint32_t max_atlas_dim = 1024;
    std::uint32_t samples = 16;      // indirect hemisphere samples / texel
    std::uint32_t bounces = 1;       // indirect bounce depth (0 = direct only)
    Vec3 ambient{0.0f, 0.0f, 0.0f};  // ambient sky radiance
    float exposure = 1.0f;           // linear exposure before tonemap
    std::uint32_t seed = 0x9E3779B9u;
    bool force_overwrite = false;
    bool quiet = false;
    bool print_stats = false;
};

struct BakeStats {
    std::uint64_t bytes_written = 0;
    std::uint32_t surface_count = 0;
    std::uint32_t triangle_count = 0;
    std::uint32_t light_count = 0;
    std::uint32_t atlas_width = 0;
    std::uint32_t atlas_height = 0;
    std::uint32_t texels_lit = 0;
    double max_luminance = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────
// Deterministic PCG32.
// ─────────────────────────────────────────────────────────────────────────

class Pcg32 {
   public:
    void seed(std::uint64_t init_state, std::uint64_t init_seq) noexcept {
        state_ = 0u;
        inc_ = (init_seq << 1u) | 1u;
        next();
        state_ += init_state;
        next();
    }
    std::uint32_t next() noexcept {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const std::uint32_t xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const std::uint32_t rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }
    // Uniform float in [0, 1).
    float nextf() noexcept { return static_cast<float>(next() >> 8u) * (1.0f / 16777216.0f); }

   private:
    std::uint64_t state_ = 0u;
    std::uint64_t inc_ = 1u;
};

inline std::uint64_t mix_seed(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (std::uint32_t v : {a, b, c, d}) {
        h ^= v;
        h *= 0x100000001B3ULL;
    }
    return h;
}

// ─────────────────────────────────────────────────────────────────────────
// Triangles + BVH.
// ─────────────────────────────────────────────────────────────────────────

namespace detail {

struct Tri {
    Vec3 v0, v1, v2;
    Vec3 normal;
    std::uint32_t material = 0;
};

struct Aabb {
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    void grow(Vec3 p) noexcept {
        lo = vmin(lo, p);
        hi = vmax(hi, p);
    }
    void grow(const Aabb& o) noexcept {
        lo = vmin(lo, o.lo);
        hi = vmax(hi, o.hi);
    }
    Vec3 centroid() const noexcept { return (lo + hi) * 0.5f; }
};

struct BvhNode {
    Aabb bounds;
    std::int32_t left = -1;   // inner: left child node index; -1 for a leaf
    std::int32_t right = -1;  // inner: right child node index
    std::uint32_t start = 0;  // leaf: first triangle index
    std::uint32_t count = 0;  // leaf: triangle count
};

// Moller-Trumbore. Returns true and sets `t` (ray param) on a front/back hit
// within (t_min, t_max).
inline bool ray_tri(Vec3 orig, Vec3 dir, const Tri& tri, float t_min, float t_max, float& t_out) noexcept {
    const Vec3 e1 = tri.v1 - tri.v0;
    const Vec3 e2 = tri.v2 - tri.v0;
    const Vec3 p = cross(dir, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    const float inv_det = 1.0f / det;
    const Vec3 tvec = orig - tri.v0;
    const float u = dot(tvec, p) * inv_det;
    if (u < -1e-5f || u > 1.0f + 1e-5f) {
        return false;
    }
    const Vec3 q = cross(tvec, e1);
    const float v = dot(dir, q) * inv_det;
    if (v < -1e-5f || u + v > 1.0f + 1e-5f) {
        return false;
    }
    const float t = dot(e2, q) * inv_det;
    if (t < t_min || t > t_max) {
        return false;
    }
    t_out = t;
    return true;
}

class Bvh {
   public:
    void build(std::vector<Tri> tris) {
        tris_ = std::move(tris);
        const std::uint32_t n = static_cast<std::uint32_t>(tris_.size());
        order_.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            order_[i] = i;
        }
        nodes_.clear();
        if (n == 0) {
            return;
        }
        nodes_.reserve(2u * n);
        build_range(0u, n, 0);
    }

    const std::vector<Tri>& tris() const noexcept { return tris_; }

    // Closest hit. Returns the triangle index (in original order) or -1.
    std::int32_t closest_hit(Vec3 orig, Vec3 dir, float t_min, float t_max, float& t_hit) const noexcept {
        if (nodes_.empty()) {
            return -1;
        }
        std::int32_t best = -1;
        float best_t = t_max;
        std::array<std::int32_t, 64> stack;
        std::int32_t sp = 0;
        stack[static_cast<std::size_t>(sp++)] = 0;
        while (sp > 0) {
            const BvhNode& node =
                nodes_[static_cast<std::size_t>(stack[static_cast<std::size_t>(--sp)])];
            if (!slab_test(node.bounds, orig, dir, t_min, best_t)) {
                continue;
            }
            if (node.left < 0) {
                for (std::uint32_t i = 0; i < node.count; ++i) {
                    const std::uint32_t tri_index = order_[node.start + i];
                    float t = 0.0f;
                    if (ray_tri(orig, dir, tris_[tri_index], t_min, best_t, t)) {
                        best_t = t;
                        best = static_cast<std::int32_t>(tri_index);
                    }
                }
            } else {
                stack[static_cast<std::size_t>(sp++)] = node.left;
                stack[static_cast<std::size_t>(sp++)] = node.right;
            }
        }
        if (best >= 0) {
            t_hit = best_t;
        }
        return best;
    }

    // Any hit in (t_min, t_max) — used for shadow rays.
    bool any_hit(Vec3 orig, Vec3 dir, float t_min, float t_max) const noexcept {
        if (nodes_.empty()) {
            return false;
        }
        std::array<std::int32_t, 64> stack;
        std::int32_t sp = 0;
        stack[static_cast<std::size_t>(sp++)] = 0;
        while (sp > 0) {
            const BvhNode& node =
                nodes_[static_cast<std::size_t>(stack[static_cast<std::size_t>(--sp)])];
            if (!slab_test(node.bounds, orig, dir, t_min, t_max)) {
                continue;
            }
            if (node.left < 0) {
                for (std::uint32_t i = 0; i < node.count; ++i) {
                    const std::uint32_t tri_index = order_[node.start + i];
                    float t = 0.0f;
                    if (ray_tri(orig, dir, tris_[tri_index], t_min, t_max, t)) {
                        return true;
                    }
                }
            } else {
                stack[static_cast<std::size_t>(sp++)] = node.left;
                stack[static_cast<std::size_t>(sp++)] = node.right;
            }
        }
        return false;
    }

   private:
    static bool slab_test(const Aabb& b, Vec3 o, Vec3 d, float t_min, float t_max) noexcept {
        for (int axis = 0; axis < 3; ++axis) {
            const float oc = (axis == 0) ? o.x : (axis == 1) ? o.y : o.z;
            const float dc = (axis == 0) ? d.x : (axis == 1) ? d.y : d.z;
            const float lo = (axis == 0) ? b.lo.x : (axis == 1) ? b.lo.y : b.lo.z;
            const float hi = (axis == 0) ? b.hi.x : (axis == 1) ? b.hi.y : b.hi.z;
            const float inv = (std::fabs(dc) > 1e-20f) ? 1.0f / dc : 1e30f;
            float t0 = (lo - oc) * inv;
            float t1 = (hi - oc) * inv;
            if (t0 > t1) {
                std::swap(t0, t1);
            }
            t_min = std::max(t_min, t0);
            t_max = std::min(t_max, t1);
            if (t_max < t_min) {
                return false;
            }
        }
        return true;
    }

    std::int32_t build_range(std::uint32_t start, std::uint32_t count, int depth) {
        const std::int32_t node_index = static_cast<std::int32_t>(nodes_.size());
        nodes_.push_back(BvhNode{});
        Aabb bounds;
        Aabb centroid_bounds;
        for (std::uint32_t i = 0; i < count; ++i) {
            const Tri& tri = tris_[order_[start + i]];
            Aabb tb;
            tb.grow(tri.v0);
            tb.grow(tri.v1);
            tb.grow(tri.v2);
            bounds.grow(tb);
            centroid_bounds.grow(tb.centroid());
        }
        // Leaf?
        if (count <= 4u || depth >= 48) {
            BvhNode leaf;
            leaf.bounds = bounds;
            leaf.left = -1;
            leaf.start = start;
            leaf.count = count;
            nodes_[static_cast<std::size_t>(node_index)] = leaf;
            return node_index;
        }
        // Split on the widest centroid axis at the median.
        const Vec3 ext = centroid_bounds.hi - centroid_bounds.lo;
        const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
        const std::uint32_t mid = start + count / 2u;
        std::nth_element(order_.begin() + start,
                         order_.begin() + mid,
                         order_.begin() + start + count,
                         [&](std::uint32_t a, std::uint32_t b) {
                             return tri_centroid_axis(a, axis) < tri_centroid_axis(b, axis);
                         });
        const std::int32_t left = build_range(start, mid - start, depth + 1);
        const std::int32_t right = build_range(mid, start + count - mid, depth + 1);
        BvhNode inner;
        inner.bounds = bounds;
        inner.left = left;
        inner.right = right;
        nodes_[static_cast<std::size_t>(node_index)] = inner;
        return node_index;
    }

    float tri_centroid_axis(std::uint32_t tri_index, int axis) const noexcept {
        const Tri& t = tris_[tri_index];
        const Vec3 c = (t.v0 + t.v1 + t.v2) * (1.0f / 3.0f);
        return (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;
    }

    std::vector<Tri> tris_;
    std::vector<std::uint32_t> order_;
    std::vector<BvhNode> nodes_;
};

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// Lighting.
// ─────────────────────────────────────────────────────────────────────────

namespace detail {

// Build an orthonormal basis (t, b) for a plane with unit normal n.
inline void basis_for_normal(Vec3 n, Vec3& t, Vec3& b) noexcept {
    const Vec3 up = (std::fabs(n.z) < 0.9f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
    t = normalize(cross(up, n));
    b = normalize(cross(n, t));
}

// Cosine-weighted hemisphere direction around n from two uniform samples.
inline Vec3 cosine_hemisphere(Vec3 n, float u1, float u2) noexcept {
    const float r = std::sqrt(u1);
    const float phi = 6.2831853071795864769f * u2;
    const float x = r * std::cos(phi);
    const float y = r * std::sin(phi);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    Vec3 t, b;
    basis_for_normal(n, t, b);
    return normalize(t * x + b * y + n * z);
}

struct Tracer {
    const Scene* scene = nullptr;
    const Bvh* bvh = nullptr;
    BakeOptions opts;

    // Direct irradiance arriving at point p with surface normal n.
    Vec3 direct(Vec3 p, Vec3 n) const {
        Vec3 e{0.0f, 0.0f, 0.0f};
        const Vec3 origin = p + n * 1e-3f;
        for (const BakeLight& light : scene->lights) {
            Vec3 to_light;
            float dist = 0.0f;
            float falloff = 1.0f;
            if (light.kind == BakeLight::Kind::Point) {
                to_light = light.position - p;
                dist = length(to_light);
                if (dist < 1e-4f) {
                    continue;
                }
                to_light = to_light * (1.0f / dist);
                falloff = 1.0f / std::max(dist * dist, 1e-4f);
            } else {
                to_light = normalize(light.direction * -1.0f);
                dist = 1e30f;
                falloff = 1.0f;
            }
            const float ndotl = dot(n, to_light);
            if (ndotl <= 0.0f) {
                continue;
            }
            const float max_t = (light.kind == BakeLight::Kind::Point) ? dist - 2e-3f : 1e30f;
            if (bvh->any_hit(origin, to_light, 1e-3f, max_t)) {
                continue;  // shadowed
            }
            e = e + light.color * (light.intensity * ndotl * falloff);
        }
        return e;
    }

    // Total irradiance at (p, n): direct + indirect (gathered to `bounce`).
    Vec3 irradiance(Vec3 p, Vec3 n, std::uint32_t bounce, Pcg32& rng) const {
        Vec3 e = direct(p, n);
        if (opts.samples == 0u || bounce >= opts.bounces) {
            // Flat ambient floor (hemisphere integral of constant radiance).
            e = e + opts.ambient * 3.1415926535897932f;
            return e;
        }
        const Vec3 origin = p + n * 1e-3f;
        Vec3 gathered{0.0f, 0.0f, 0.0f};
        const std::uint32_t n_samples = opts.samples;
        // Stratify over a near-square grid for lower variance.
        const std::uint32_t sx = static_cast<std::uint32_t>(
            std::max(1.0f, std::floor(std::sqrt(static_cast<float>(n_samples)))));
        for (std::uint32_t s = 0; s < n_samples; ++s) {
            const std::uint32_t cell_x = s % sx;
            const std::uint32_t cell_y = (s / sx);
            const float u1 = (static_cast<float>(cell_x) + rng.nextf()) / static_cast<float>(sx);
            const float u2 = (static_cast<float>(cell_y) + rng.nextf()) /
                             static_cast<float>((n_samples + sx - 1u) / sx);
            const Vec3 dir = cosine_hemisphere(n, std::min(u1, 0.999999f), u2);
            float t_hit = 0.0f;
            const std::int32_t tri = bvh->closest_hit(origin, dir, 1e-3f, 1e30f, t_hit);
            if (tri < 0) {
                gathered = gathered + opts.ambient;  // escaped to sky
                continue;
            }
            const detail::Tri& hit = bvh->tris()[static_cast<std::size_t>(tri)];
            Vec3 hit_n = hit.normal;
            if (dot(hit_n, dir) > 0.0f) {
                hit_n = hit_n * -1.0f;  // face the incoming ray
            }
            const Vec3 hit_p = origin + dir * t_hit;
            const Vec3 albedo =
                scene->materials.empty()
                    ? Vec3{0.7f, 0.7f, 0.7f}
                    : scene
                          ->materials[std::min<std::size_t>(hit.material, scene->materials.size() - 1)]
                          .albedo;
            const Vec3 hit_e = irradiance(hit_p, hit_n, bounce + 1u, rng);
            // Diffuse reflected radiance Lo = albedo/pi * E.
            const Vec3 lo = mul(albedo, hit_e) * (1.0f / 3.1415926535897932f);
            gathered = gathered + lo;
        }
        // Cosine-weighted estimator of irradiance: E ~= (pi / N) * sum(Li).
        e = e + gathered * (3.1415926535897932f / static_cast<float>(n_samples));
        return e;
    }
};

// sRGB encode a linear value in [0,1].
inline float linear_to_srgb(float c) noexcept {
    c = std::min(std::max(c, 0.0f), 1.0f);
    return (c <= 0.0031308f) ? (12.92f * c) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
}

inline std::uint8_t to_u8(float srgb) noexcept {
    const float v = std::min(std::max(srgb, 0.0f), 1.0f) * 255.0f + 0.5f;
    return static_cast<std::uint8_t>(v);
}

// One surface's lightmap region inside the atlas.
struct Patch {
    std::uint32_t surface = 0;
    std::uint32_t x = 0, y = 0;  // atlas origin
    std::uint32_t w = 1, h = 1;
    Vec3 t{1, 0, 0}, b{0, 1, 0}, n{0, 0, 1};
    float u_lo = 0, u_hi = 1, v_lo = 0, v_hi = 1;
    float plane_c = 0;  // dot(vertex, n)
};

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// Bake entry: Scene -> .lmt blob.
// ─────────────────────────────────────────────────────────────────────────

inline bool bake(const Scene& scene,
                 const BakeOptions& opts,
                 BakeStats* out_stats,
                 std::vector<std::uint8_t>* out_blob,
                 std::string* out_error) {
    using detail::Patch;
    using detail::Tri;

    auto fail = [&](const std::string& msg) -> bool {
        if (out_error != nullptr) {
            *out_error = msg;
        }
        return false;
    };

    // ── Build the occluder triangle pool + BVH ────────────────────────────
    std::vector<Tri> tris;
    for (const BakeSurface& surf : scene.surfaces) {
        if (surf.vertices.size() < 3) {
            continue;
        }
        for (std::size_t i = 1; i + 1 < surf.vertices.size(); ++i) {
            Tri tri;
            tri.v0 = surf.vertices[0];
            tri.v1 = surf.vertices[i];
            tri.v2 = surf.vertices[i + 1];
            tri.normal = surf.normal;
            tri.material = surf.material;
            tris.push_back(tri);
        }
    }
    detail::Bvh bvh;
    bvh.build(tris);

    // ── Plan the atlas: one rectangular patch per surface, shelf-packed ───
    std::vector<Patch> patches;
    patches.reserve(scene.surfaces.size());
    // Clamp to a sane bound: width/height must fit the LmtHeader u16 fields,
    // and width*height*4 must stay within the 32-bit byte offsets in the file
    // (an 8192x8192 RGBA8 atlas is already 256 MB).
    const std::uint32_t max_dim = std::min(std::max(opts.max_atlas_dim, 8u), 8192u);
    for (std::uint32_t si = 0; si < scene.surfaces.size(); ++si) {
        const BakeSurface& surf = scene.surfaces[si];
        if (surf.vertices.size() < 3) {
            continue;
        }
        Patch patch;
        patch.surface = si;
        patch.n = normalize(surf.normal);
        detail::basis_for_normal(patch.n, patch.t, patch.b);
        patch.plane_c = dot(surf.vertices[0], patch.n);
        bool first = true;
        for (Vec3 v : surf.vertices) {
            const float u = dot(v, patch.t);
            const float w = dot(v, patch.b);
            if (first) {
                patch.u_lo = patch.u_hi = u;
                patch.v_lo = patch.v_hi = w;
                first = false;
            } else {
                patch.u_lo = std::min(patch.u_lo, u);
                patch.u_hi = std::max(patch.u_hi, u);
                patch.v_lo = std::min(patch.v_lo, w);
                patch.v_hi = std::max(patch.v_hi, w);
            }
        }
        patches.push_back(patch);
    }

    if (patches.empty()) {
        return fail("no bakeable surfaces (need at least one polygon with >= 3 vertices)");
    }

    // Size each patch at a given density and shelf-pack into a max_dim-wide
    // atlas (+1px gutter to limit bleed); returns the packed atlas height and
    // reports the packed width via `out_width`.
    const std::uint32_t gutter = 1u;
    auto size_and_pack = [&](float density, std::uint32_t& out_width) -> std::uint32_t {
        std::uint32_t pen_x = gutter;
        std::uint32_t pen_y = gutter;
        std::uint32_t shelf_h = 0u;
        std::uint32_t packed_w = 0u;
        // Bound each patch so a placed patch (origin >= gutter) plus its right
        // gutter never crosses max_dim — otherwise edge texels would fall
        // outside the atlas and be silently dropped.
        const std::uint32_t cell_max = (max_dim > 2u * gutter) ? (max_dim - 2u * gutter) : 1u;
        for (Patch& patch : patches) {
            const float uw = std::max(patch.u_hi - patch.u_lo, 1e-4f);
            const float vh = std::max(patch.v_hi - patch.v_lo, 1e-4f);
            patch.w =
                std::min(cell_max, std::max(1u, static_cast<std::uint32_t>(std::ceil(uw * density))));
            patch.h =
                std::min(cell_max, std::max(1u, static_cast<std::uint32_t>(std::ceil(vh * density))));
            if (pen_x + patch.w + gutter > max_dim && pen_x > gutter) {
                pen_x = gutter;
                pen_y += shelf_h + gutter;
                shelf_h = 0u;
            }
            patch.x = pen_x;
            patch.y = pen_y;
            pen_x += patch.w + gutter;
            packed_w = std::max(packed_w, pen_x);
            shelf_h = std::max(shelf_h, patch.h);
        }
        out_width = packed_w;
        return pen_y + shelf_h + gutter;
    };

    // Fit the atlas inside max_dim x max_dim: if the content overflows the
    // height, reduce the effective texel density and re-pack. This bounds the
    // output to at most max_dim^2 RGBA8 texels regardless of world scale.
    float density = std::max(opts.texels_per_metre, 0.01f);
    std::uint32_t packed_w = 0u;
    std::uint32_t atlas_h = size_and_pack(density, packed_w);
    bool downscaled = false;
    for (int iter = 0; iter < 8 && atlas_h > max_dim; ++iter) {
        const float factor =
            std::max(0.5f, (static_cast<float>(max_dim) / static_cast<float>(atlas_h)) * 0.92f);
        density *= factor;
        atlas_h = size_and_pack(density, packed_w);
        downscaled = true;
    }
    if (downscaled && !opts.quiet) {
        std::fprintf(stderr,
                     "lm_bake: note: lightmap density reduced to %.3f texels/m to fit the atlas "
                     "within %u x %u\n",
                     static_cast<double>(density),
                     max_dim,
                     max_dim);
    }
    const std::uint32_t width = std::min(std::max(packed_w, 1u), max_dim);
    const std::uint32_t height = std::min(std::max(atlas_h, 1u), max_dim);

    // ── Bake every patch texel ────────────────────────────────────────────
    std::vector<float> hdr(static_cast<std::size_t>(width) * height * 3u, 0.0f);
    detail::Tracer tracer;
    tracer.scene = &scene;
    tracer.bvh = &bvh;
    tracer.opts = opts;
    // The indirect gather branches `samples` rays at each bounce, so cost is
    // ~samples^bounces; clamp both so a careless (or overflowing) value can't
    // wedge the bake into an effectively unbounded run.
    tracer.opts.samples = std::min(opts.samples, 4096u);
    tracer.opts.bounces = std::min(opts.bounces, 8u);

    std::uint32_t texels_lit = 0u;
    double max_lum = 0.0;
    for (const Patch& patch : patches) {
        const float uw = (patch.u_hi - patch.u_lo);
        const float vh = (patch.v_hi - patch.v_lo);
        for (std::uint32_t ty = 0; ty < patch.h; ++ty) {
            for (std::uint32_t tx = 0; tx < patch.w; ++tx) {
                const float fu = (static_cast<float>(tx) + 0.5f) / static_cast<float>(patch.w);
                const float fv = (static_cast<float>(ty) + 0.5f) / static_cast<float>(patch.h);
                const float u = patch.u_lo + fu * uw;
                const float v = patch.v_lo + fv * vh;
                const Vec3 world = patch.t * u + patch.b * v + patch.n * patch.plane_c;

                Pcg32 rng;
                rng.seed(mix_seed(opts.seed, patch.surface, tx, ty), 0xDA3E39CB94B95BDBULL);
                const Vec3 e = tracer.irradiance(world, patch.n, 0u, rng);
                const Vec3 lit = e * opts.exposure;

                const std::uint32_t ax = patch.x + tx;
                const std::uint32_t ay = patch.y + ty;
                if (ax >= width || ay >= height) {
                    continue;
                }
                const std::size_t idx = (static_cast<std::size_t>(ay) * width + ax) * 3u;
                hdr[idx + 0] = lit.x;
                hdr[idx + 1] = lit.y;
                hdr[idx + 2] = lit.z;
                const double lum = 0.2126 * lit.x + 0.7152 * lit.y + 0.0722 * lit.z;
                if (lum > 1e-4) {
                    ++texels_lit;
                }
                max_lum = std::max(max_lum, lum);
            }
        }
    }

    // ── Tonemap (Reinhard) + sRGB encode to RGBA8 ─────────────────────────
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u, 0u);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        const float r = hdr[i * 3 + 0];
        const float g = hdr[i * 3 + 1];
        const float b = hdr[i * 3 + 2];
        const float tr = r / (1.0f + r);
        const float tg = g / (1.0f + g);
        const float tb = b / (1.0f + b);
        pixels[i * 4 + 0] = detail::to_u8(detail::linear_to_srgb(tr));
        pixels[i * 4 + 1] = detail::to_u8(detail::linear_to_srgb(tg));
        pixels[i * 4 + 2] = detail::to_u8(detail::linear_to_srgb(tb));
        pixels[i * 4 + 3] = 255u;
    }

    // ── Assemble the .lmt blob ────────────────────────────────────────────
    // 64-bit byte math (width/height are clamped to <= 8192, so this never
    // overflows, but compute in u64 regardless to be robust to the bound).
    const std::uint64_t pixels_bytes = static_cast<std::uint64_t>(width) * height * 4u;
    const std::uint32_t header_bytes = static_cast<std::uint32_t>(sizeof(fmt::LmtHeader));
    const std::uint32_t mip_bytes = static_cast<std::uint32_t>(sizeof(fmt::LmtMip));
    const std::uint32_t pixels_offset = header_bytes + mip_bytes;  // single mip
    const std::uint64_t total = static_cast<std::uint64_t>(pixels_offset) + pixels_bytes;

    fmt::LmtHeader header{};
    header.file.magic = fmt::kLmtMagic;
    header.file.version = fmt::kLmtVersion;
    header.file.flags = fmt::kLmtFlagSRGB;
    header.file.payload_size = total - sizeof(fmt::FileHeader);
    header.width = static_cast<std::uint16_t>(width);
    header.height = static_cast<std::uint16_t>(height);
    header.mip_count = 1u;
    header.reserved0 = 0u;
    header.pixel_fmt = fmt::LmtPixelFmt::RGBA8;
    header.palette_offset = 0u;
    header.pixels_offset = pixels_offset;

    fmt::LmtMip mip0{};
    mip0.width = width;
    mip0.height = height;
    mip0.offset = pixels_offset;
    mip0.byte_size = static_cast<std::uint32_t>(pixels_bytes);

    std::vector<std::uint8_t> blob;
    blob.reserve(static_cast<std::size_t>(total));
    auto append = [&](const void* src, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(src);
        blob.insert(blob.end(), p, p + n);
    };
    append(&header, sizeof(header));
    append(&mip0, sizeof(mip0));
    append(pixels.data(), pixels.size());

    if (blob.size() != total) {
        return fail("internal: assembled .lmt size mismatch");
    }

    // ── Write to disk (if requested) ──────────────────────────────────────
    if (!opts.output_path.empty()) {
        const std::filesystem::path out_path(opts.output_path);
        std::error_code ec;
        if (std::filesystem::exists(out_path, ec) && !opts.force_overwrite) {
            return fail("output file '" + opts.output_path + "' already exists; pass --force");
        }
        if (out_path.has_parent_path()) {
            std::filesystem::create_directories(out_path.parent_path(), ec);
        }
        std::ofstream f(opts.output_path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            return fail("cannot open output file '" + opts.output_path + "': " + std::strerror(errno));
        }
        f.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        if (!f) {
            return fail("failed to write output file '" + opts.output_path + "'");
        }
    }

    if (out_blob != nullptr) {
        out_blob->insert(out_blob->end(), blob.begin(), blob.end());
    }
    if (out_stats != nullptr) {
        out_stats->bytes_written = blob.size();
        out_stats->surface_count = static_cast<std::uint32_t>(patches.size());
        out_stats->triangle_count = static_cast<std::uint32_t>(tris.size());
        out_stats->light_count = static_cast<std::uint32_t>(scene.lights.size());
        out_stats->atlas_width = width;
        out_stats->atlas_height = height;
        out_stats->texels_lit = texels_lit;
        out_stats->max_luminance = max_lum;
    }
    return true;
}

}  // namespace psy::lm_bake
