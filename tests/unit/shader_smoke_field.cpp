// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/shader_smoke_field.cpp
//
// Lane 08 — VolumetricSmokeField unit tests (ADR-022 / issue #44).
//
// The smoke density grid is the authoritative coarse line-of-sight field, so
// it must be deterministic (bit-fair across lockstep clients). These tests
// pin the gameplay contract:
//   * inject() raises density at the cloud centre above 0.
//   * step() decays density toward 0 (dissipation).
//   * carve() returns a sampled region to ~0 (a bullet hole through smoke).
//   * the same inject+step+carve sequence run twice yields BIT-IDENTICAL
//     sampled values (determinism — the load-bearing property).
//
// Header-only field, no GPU touched. ASCII-only TEST_CASE names (AGENTS.md).

#include "shader/VolumetricSmokeField.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace psynder;
using psynder::shader::VolumetricSmokeField;
using Vec3 = psynder::math::Vec3;

namespace {

// A modest grid centred on the origin: 16^3 voxels at 0.5 m each → an 8 m
// cube spanning [-4, +4] on every axis.
VolumetricSmokeField make_field() {
    return VolumetricSmokeField(/*dim*/ 16, 16, 16,
                                /*cell_m*/ 0.5f,
                                /*origin_m*/ Vec3{-4.0f, -4.0f, -4.0f});
}

// Bit-exact float compare (NOT approximate) — determinism means identical bits.
bool bit_equal(f32 a, f32 b) {
    return std::memcmp(&a, &b, sizeof(f32)) == 0;
}

}  // namespace

TEST_CASE("smoke field starts empty", "[shader][smoke]") {
    VolumetricSmokeField f = make_field();
    REQUIRE(f.voxel_count() == 16u * 16u * 16u);
    REQUIRE(f.sample(Vec3{0, 0, 0}) == 0.0f);
    for (f32 d : f.data()) {
        REQUIRE(d == 0.0f);
    }
}

TEST_CASE("inject raises density at the cloud centre", "[shader][smoke]") {
    VolumetricSmokeField f = make_field();
    const Vec3 c{0.0f, 0.0f, 0.0f};
    f.inject(c, /*radius_m*/ 1.5f, /*amount*/ 0.8f);

    REQUIRE(f.sample(c) > 0.0f);
    REQUIRE(f.sample_nearest(c) > 0.0f);

    // A point well outside the injected sphere stays clear.
    REQUIRE(f.sample(Vec3{3.5f, 3.5f, 3.5f}) == 0.0f);
}

TEST_CASE("inject clamps density to one", "[shader][smoke]") {
    VolumetricSmokeField f = make_field();
    const Vec3 c{0.0f, 0.0f, 0.0f};
    // Over-saturate: amount > 1 plus repeated injects must never exceed 1.
    f.inject(c, 2.0f, 5.0f);
    f.inject(c, 2.0f, 5.0f);
    REQUIRE(f.sample_nearest(c) <= 1.0f);
    REQUIRE(f.sample_nearest(c) > 0.0f);
}

TEST_CASE("step dissipates the field toward zero", "[shader][smoke]") {
    VolumetricSmokeField f = make_field();
    const Vec3 c{0.0f, 0.0f, 0.0f};
    f.inject(c, 1.5f, 0.9f);

    const f32 before = f.sample_nearest(c);
    REQUIRE(before > 0.0f);

    f.step(/*dissipation*/ 0.25f);
    const f32 after = f.sample_nearest(c);

    REQUIRE(after < before);
    REQUIRE(after > 0.0f);  // a single quarter-decay does not fully clear it

    // Decaying hard repeatedly drives it arbitrarily close to 0.
    for (int i = 0; i < 64; ++i) f.step(0.5f);
    REQUIRE(f.sample_nearest(c) < 1e-3f);
}

TEST_CASE("carve clears a sphere back to zero", "[shader][smoke]") {
    VolumetricSmokeField f = make_field();
    const Vec3 c{0.0f, 0.0f, 0.0f};
    f.inject(c, 2.5f, 1.0f);
    REQUIRE(f.sample_nearest(c) > 0.0f);

    // Punch a bullet hole through the centre.
    f.carve(c, 1.0f);
    REQUIRE(f.sample_nearest(c) == 0.0f);

    // Density just outside the carved sphere should survive (still smoky).
    REQUIRE(f.sample_nearest(Vec3{2.0f, 0.0f, 0.0f}) > 0.0f);
}

TEST_CASE("inject step carve sequence is bit identical across runs",
          "[shader][smoke][determinism]") {
    auto run = []() {
        VolumetricSmokeField f = make_field();
        // A non-trivial, off-axis sequence so any reordering would diverge.
        f.inject(Vec3{0.3f, -0.7f, 1.1f}, 2.0f, 0.6f);
        f.inject(Vec3{-1.2f, 0.4f, -0.5f}, 1.3f, 0.9f);
        f.step(0.137f);
        f.carve(Vec3{0.0f, 0.0f, 0.0f}, 0.8f);
        f.inject(Vec3{1.5f, 1.5f, -1.5f}, 1.7f, 0.4f);
        f.step(0.05f);
        return f.data();  // full flat SoA snapshot
    };

    const std::vector<f32> a = run();
    const std::vector<f32> b = run();

    REQUIRE(a.size() == b.size());
    bool all_bit_equal = true;
    for (usize i = 0; i < a.size(); ++i) {
        if (!bit_equal(a[i], b[i])) { all_bit_equal = false; break; }
    }
    REQUIRE(all_bit_equal);

    // The two snapshots must also be byte-for-byte identical in bulk.
    REQUIRE(std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0);
}
