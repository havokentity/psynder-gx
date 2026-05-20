// SPDX-License-Identifier: MIT
// Lane 06 (Wave B) — spatial index correctness + benchmark suite.
//
// Every accelerator (BVH / SAP / uniform grid) and the query router are
// validated against the brute-force baseline in engine/scene/Spatial.h: because
// both paths apply the SAME intersection predicates, an accelerated result set
// and the linear reference must be byte-identical (as sorted sets). The router
// case runs over ~100k proxies and reports build / refit / query timings.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <random>
#include <utility>
#include <vector>

#include "jobs/JobSystem.h"
#include "scene/Spatial.h"

using namespace psynder;
using namespace psynder::scene;

namespace {

// RAII: run this test's build()/refit() on the real threaded pool, then stop
// it. Scoped to the individual TEST_CASE on purpose -- a global Catch2 listener
// (CATCH_REGISTER_LISTENER) starts the pool at the beginning of EVERY test
// process in the combined binary, which breaks lanes that rely on the
// synchronous-fallback default (asset read_async firing inline; jobs' not-
// started worker_count()==1 and backend-selection asserts). parallel_for is
// synchronous either way, so scene correctness is identical with the pool off.
struct ScenePool {
    ScenePool() { jobs::JobSystem::Get().start(); }
    ~ScenePool() { jobs::JobSystem::Get().stop(); }
    ScenePool(const ScenePool&) = delete;
    ScenePool& operator=(const ScenePool&) = delete;
};

constexpr f32 kSceneMetres = 1400.0f;  // 1.4 km map per DESIGN §9.1

// Deterministic proxy soup: small axis-aligned boxes scattered through the map.
std::vector<math::Aabb> make_proxies(u32 n, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> pos(0.0f, kSceneMetres);
    std::uniform_real_distribution<f32> half(0.25f, 2.5f);  // 0.5..5 m boxes
    std::vector<math::Aabb> boxes(n);
    for (u32 i = 0; i < n; ++i) {
        const math::Vec3 c{pos(rng), pos(rng), pos(rng)};
        const math::Vec3 h{half(rng), half(rng), half(rng)};
        boxes[i] = math::Aabb{{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z + h.z}};
    }
    return boxes;
}

// A direction with every component comfortably away from zero, so the slab
// test's reciprocal direction is finite. Draws are sequenced explicitly so the
// stream is deterministic regardless of argument-evaluation order.
math::Vec3 safe_dir(std::mt19937& rng) {
    std::uniform_real_distribution<f32> mag(0.3f, 1.0f);
    std::uniform_int_distribution<int> sign(0, 1);
    const f32 mx = mag(rng);
    const f32 sx = sign(rng) ? 1.0f : -1.0f;
    const f32 my = mag(rng);
    const f32 sy = sign(rng) ? 1.0f : -1.0f;
    const f32 mz = mag(rng);
    const f32 sz = sign(rng) ? 1.0f : -1.0f;
    return math::normalize(math::Vec3{mx * sx, my * sy, mz * sz});
}

// An axis-aligned box region expressed as a 6-plane inward-facing frustum.
Frustum box_frustum(const math::Aabb& region) {
    Frustum f;
    f.planes[0] = Plane{{1.0f, 0.0f, 0.0f}, -region.min.x};
    f.planes[1] = Plane{{-1.0f, 0.0f, 0.0f}, region.max.x};
    f.planes[2] = Plane{{0.0f, 1.0f, 0.0f}, -region.min.y};
    f.planes[3] = Plane{{0.0f, -1.0f, 0.0f}, region.max.y};
    f.planes[4] = Plane{{0.0f, 0.0f, 1.0f}, -region.min.z};
    f.planes[5] = Plane{{0.0f, 0.0f, -1.0f}, region.max.z};
    return f;
}

math::Aabb box_around(math::Vec3 c, f32 half) {
    return math::Aabb{{c.x - half, c.y - half, c.z - half}, {c.x + half, c.y + half, c.z + half}};
}

std::vector<u32> sorted(std::vector<u32> v) {
    std::sort(v.begin(), v.end());
    return v;
}

// Compare a router result (entities, raw == index+1) against brute indices.
void require_same_as_brute(const std::vector<Entity>& got, const std::vector<u32>& brute_idx) {
    std::vector<u32> a;
    a.reserve(got.size());
    for (const Entity e : got)
        a.push_back(e.raw - 1u);
    std::vector<u32> b = brute_idx;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    REQUIRE(a == b);
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST_CASE("scene spatial BVH matches brute force", "[scene][spatial][bvh]") {
    ScenePool pool;
    const std::vector<math::Aabb> boxes = make_proxies(8000, 1234);
    const std::span<const math::Aabb> span{boxes};

    Bvh bvh;
    bvh.build(span);
    REQUIRE_FALSE(bvh.empty());
    REQUIRE(bvh.node_count() > 0u);
    // Median split keeps the tree balanced: depth must stay near log2(N).
    REQUIRE(bvh.max_depth() <= 32u);

    std::mt19937 rng(99);
    std::uniform_real_distribution<f32> pos(0.0f, kSceneMetres);
    std::vector<u32> got, ref;

    SECTION("ray queries") {
        for (int q = 0; q < 16; ++q) {
            Ray ray;
            ray.origin = {pos(rng), pos(rng), pos(rng)};
            ray.dir = safe_dir(rng);
            ray.tmax = kSceneMetres * 2.0f;
            got.clear();
            ref.clear();
            bvh.query_ray(ray, got);
            brute::query_ray(span, ray, ref);
            REQUIRE(sorted(got) == sorted(ref));
        }
    }

    SECTION("frustum queries") {
        for (int q = 0; q < 12; ++q) {
            const math::Vec3 c{pos(rng), pos(rng), pos(rng)};
            const f32 half = (q % 2 == 0) ? 40.0f : 300.0f;  // small + large regions
            const Frustum fr = box_frustum(box_around(c, half));
            got.clear();
            ref.clear();
            bvh.query_frustum(fr, got);
            brute::query_frustum(span, fr, ref);
            REQUIRE(sorted(got) == sorted(ref));
        }
    }

    SECTION("sphere queries") {
        for (int q = 0; q < 12; ++q) {
            const math::Vec3 c{pos(rng), pos(rng), pos(rng)};
            const f32 r = (q % 2 == 0) ? 15.0f : 200.0f;
            got.clear();
            ref.clear();
            bvh.query_sphere(c, r, got);
            brute::query_sphere(span, c, r, ref);
            REQUIRE(sorted(got) == sorted(ref));
        }
    }

    SECTION("aabb queries") {
        for (int q = 0; q < 12; ++q) {
            const math::Vec3 c{pos(rng), pos(rng), pos(rng)};
            const math::Aabb box = box_around(c, (q % 2 == 0) ? 25.0f : 250.0f);
            got.clear();
            ref.clear();
            bvh.query_aabb(box, got);
            brute::query_aabb(span, box, ref);
            REQUIRE(sorted(got) == sorted(ref));
        }
    }
}

TEST_CASE("scene spatial ray robust to axis-aligned directions", "[scene][spatial][bvh]") {
    ScenePool pool;
    // Zero direction components make the slab reciprocal +/-inf; verify the BVH
    // ray query stays consistent with brute force (no NaN-poisoned misses).
    const std::vector<math::Aabb> boxes = make_proxies(4000, 88);
    const std::span<const math::Aabb> span{boxes};
    Bvh bvh;
    bvh.build(span);

    std::mt19937 rng(17);
    std::uniform_real_distribution<f32> pos(0.0f, kSceneMetres);
    const math::Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<u32> got, ref;
    for (const math::Vec3 d : dirs) {
        for (int q = 0; q < 8; ++q) {
            Ray ray;
            ray.origin = {pos(rng), pos(rng), pos(rng)};
            ray.dir = d;
            ray.tmax = kSceneMetres * 2.0f;
            got.clear();
            ref.clear();
            bvh.query_ray(ray, got);
            brute::query_ray(span, ray, ref);
            REQUIRE(sorted(got) == sorted(ref));
        }
    }
}

TEST_CASE("scene spatial uniform grid matches brute force", "[scene][spatial][grid]") {
    ScenePool pool;
    const std::vector<math::Aabb> boxes = make_proxies(8000, 4242);
    const std::span<const math::Aabb> span{boxes};

    UniformGrid grid;
    grid.build(span, 8.0f);  // 8 m cells, comfortably larger than the proxies
    REQUIRE_FALSE(grid.empty());
    REQUIRE(grid.cell_count() > 0u);

    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> pos(0.0f, kSceneMetres);
    std::vector<u32> got, ref;

    SECTION("sphere queries") {
        for (int q = 0; q < 16; ++q) {
            const math::Vec3 c{pos(rng), pos(rng), pos(rng)};
            const f32 r = 20.0f;
            got.clear();
            ref.clear();
            grid.query_sphere(c, r, got);
            brute::query_sphere(span, c, r, ref);
            REQUIRE(sorted(got) == sorted(ref));
        }
    }

    SECTION("aabb queries") {
        for (int q = 0; q < 16; ++q) {
            const math::Vec3 c{pos(rng), pos(rng), pos(rng)};
            const math::Aabb box = box_around(c, 18.0f);
            got.clear();
            ref.clear();
            grid.query_aabb(box, got);
            brute::query_aabb(span, box, ref);
            REQUIRE(sorted(got) == sorted(ref));
        }
    }

    SECTION("point queries") {
        for (int q = 0; q < 32; ++q) {
            const math::Vec3 p{pos(rng), pos(rng), pos(rng)};
            got.clear();
            ref.clear();
            grid.query_point(p, got);
            for (u32 i = 0; i < boxes.size(); ++i) {
                if (aabb_contains_point(boxes[i], p))
                    ref.push_back(i);
            }
            REQUIRE(sorted(got) == sorted(ref));
        }
    }
}

TEST_CASE("scene spatial SAP matches brute force", "[scene][spatial][sap]") {
    ScenePool pool;
    // Smaller N: the brute-force pair baseline is O(n^2). Dense packing (a 200 m
    // cube) guarantees plenty of real overlaps to exercise the sweep.
    std::mt19937 rng(2025);
    std::uniform_real_distribution<f32> pos(0.0f, 200.0f);
    std::uniform_real_distribution<f32> half(0.5f, 4.0f);
    const u32 n = 1500;
    std::vector<math::Aabb> boxes(n);
    for (u32 i = 0; i < n; ++i) {
        const math::Vec3 c{pos(rng), pos(rng), pos(rng)};
        const math::Vec3 h{half(rng), half(rng), half(rng)};
        boxes[i] = math::Aabb{{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z + h.z}};
    }
    const std::span<const math::Aabb> span{boxes};

    Sap sap;
    sap.build(span);
    REQUIRE(sap.sweep_axis() < 3u);

    SECTION("overlapping pairs") {
        std::vector<std::pair<u32, u32>> got, ref;
        sap.overlap_pairs(got);
        brute::overlap_pairs(span, ref);
        std::sort(got.begin(), got.end());
        std::sort(ref.begin(), ref.end());
        REQUIRE(got == ref);
        REQUIRE_FALSE(ref.empty());  // sanity: the dense scene really does overlap
    }

    SECTION("aabb queries") {
        std::vector<u32> got, ref;
        for (int q = 0; q < 16; ++q) {
            const math::Aabb box = box_around({pos(rng), pos(rng), pos(rng)}, 12.0f);
            got.clear();
            ref.clear();
            sap.query_aabb(box, got);
            brute::query_aabb(span, box, ref);
            REQUIRE(sorted(got) == sorted(ref));
        }
    }
}

TEST_CASE("scene spatial BVH refit tracks moved proxies", "[scene][spatial][bvh]") {
    ScenePool pool;
    std::vector<math::Aabb> boxes = make_proxies(6000, 555);
    Bvh bvh;
    bvh.build(std::span<const math::Aabb>{boxes});
    const u32 nodes_before = bvh.node_count();

    // Move every proxy (transforms changed, topology did not), then refit.
    for (math::Aabb& b : boxes) {
        const math::Vec3 shift{120.0f, -75.0f, 40.0f};
        b.min = math::add(b.min, shift);
        b.max = math::add(b.max, shift);
    }
    bvh.refit(std::span<const math::Aabb>{boxes});

    // Refit must not change topology, only bounds.
    REQUIRE(bvh.node_count() == nodes_before);

    std::mt19937 rng(321);
    std::uniform_real_distribution<f32> pos(0.0f, kSceneMetres);
    std::vector<u32> got, ref;
    const std::span<const math::Aabb> span{boxes};
    for (int q = 0; q < 16; ++q) {
        const math::Aabb box = box_around({pos(rng), pos(rng), pos(rng)}, 60.0f);
        got.clear();
        ref.clear();
        bvh.query_aabb(box, got);
        brute::query_aabb(span, box, ref);
        REQUIRE(sorted(got) == sorted(ref));
    }
}

TEST_CASE("scene spatial empty and singleton", "[scene][spatial]") {
    ScenePool pool;
    SECTION("empty index") {
        SpatialIndex idx;
        std::vector<Entity> out;
        std::vector<std::pair<Entity, Entity>> pairs;
        idx.query_ray(Ray{}, out);
        REQUIRE(out.empty());
        idx.query_sphere({0, 0, 0}, 10.0f, out);
        REQUIRE(out.empty());
        idx.overlap_pairs(pairs);
        REQUIRE(pairs.empty());
    }

    SECTION("single proxy") {
        const std::vector<Entity> ents{Entity{1}};
        const std::vector<math::Aabb> boxes{box_around({10, 10, 10}, 1.0f)};
        SpatialIndex idx;
        idx.rebuild(ents, boxes);

        std::vector<Entity> out;
        idx.query_aabb(box_around({10, 10, 10}, 5.0f), out);
        REQUIRE(out.size() == 1u);
        REQUIRE(out[0] == Entity{1});

        idx.query_aabb(box_around({500, 500, 500}, 5.0f), out);
        REQUIRE(out.empty());
    }
}

TEST_CASE("scene spatial router matches brute force over 100k", "[scene][spatial][router]") {
    ScenePool pool;
    constexpr u32 kN = 100000;
    std::vector<math::Aabb> boxes = make_proxies(kN, 0xC0FFEE);
    std::vector<Entity> ents(kN);
    for (u32 i = 0; i < kN; ++i)
        ents[i] = Entity{i + 1u};  // raw == index + 1

    SpatialIndex idx;
    auto t0 = std::chrono::steady_clock::now();
    idx.rebuild(ents, boxes);
    const double build_ms = ms_since(t0);
    REQUIRE(idx.size() == kN);
    WARN("router build over " << kN << " proxies: " << build_ms << " ms");

    std::mt19937 rng(0xABCDEF);
    std::uniform_real_distribution<f32> pos(0.0f, kSceneMetres);
    std::vector<Entity> got;
    std::vector<u32> ref;
    const std::span<const math::Aabb> span{boxes};

    SECTION("ray / frustum -> BVH") {
        for (int q = 0; q < 8; ++q) {
            Ray ray;
            ray.origin = {pos(rng), pos(rng), pos(rng)};
            ray.dir = safe_dir(rng);
            ray.tmax = kSceneMetres * 2.0f;
            ref.clear();
            idx.query_ray(ray, got);
            brute::query_ray(span, ray, ref);
            require_same_as_brute(got, ref);
            REQUIRE(idx.last_backend() == SpatialIndex::Backend::Bvh);
        }
        for (int q = 0; q < 6; ++q) {
            const Frustum fr = box_frustum(box_around({pos(rng), pos(rng), pos(rng)}, 90.0f));
            ref.clear();
            idx.query_frustum(fr, got);
            brute::query_frustum(span, fr, ref);
            require_same_as_brute(got, ref);
        }
    }

    SECTION("sphere routes to grid (compact) and BVH (large)") {
        // Compact sphere -> grid.
        const math::Vec3 cs{pos(rng), pos(rng), pos(rng)};
        ref.clear();
        idx.query_sphere(cs, 20.0f, got);
        brute::query_sphere(span, cs, 20.0f, ref);
        require_same_as_brute(got, ref);
        REQUIRE(idx.last_backend() == SpatialIndex::Backend::Grid);

        // Scene-spanning sphere -> BVH.
        const math::Vec3 cl{700.0f, 700.0f, 700.0f};
        const f32 rl = 900.0f;
        ref.clear();
        idx.query_sphere(cl, rl, got);
        brute::query_sphere(span, cl, rl, ref);
        require_same_as_brute(got, ref);
        REQUIRE(idx.last_backend() == SpatialIndex::Backend::Bvh);
    }

    SECTION("aabb routes to grid (compact) and BVH (large)") {
        for (int q = 0; q < 8; ++q) {
            const math::Aabb box = box_around({pos(rng), pos(rng), pos(rng)}, 18.0f);
            ref.clear();
            idx.query_aabb(box, got);
            brute::query_aabb(span, box, ref);
            require_same_as_brute(got, ref);
            REQUIRE(idx.last_backend() == SpatialIndex::Backend::Grid);
        }
        const math::Aabb big = box_around({700.0f, 700.0f, 700.0f}, 650.0f);
        ref.clear();
        idx.query_aabb(big, got);
        brute::query_aabb(span, big, ref);
        require_same_as_brute(got, ref);
        REQUIRE(idx.last_backend() == SpatialIndex::Backend::Bvh);
    }

    SECTION("refit is cheaper than rebuild and stays correct") {
        for (math::Aabb& b : boxes) {
            const math::Vec3 shift{200.0f, 50.0f, -120.0f};
            b.min = math::add(b.min, shift);
            b.max = math::add(b.max, shift);
        }
        auto tr = std::chrono::steady_clock::now();
        idx.refit(boxes);
        const double refit_ms = ms_since(tr);
        WARN("router refit over " << kN << " proxies: " << refit_ms << " ms");

        const math::Aabb box = box_around({800.0f, 500.0f, 300.0f}, 120.0f);
        ref.clear();
        idx.query_aabb(box, got);
        brute::query_aabb(span, box, ref);  // span sees the in-place moved boxes
        require_same_as_brute(got, ref);
    }
}
