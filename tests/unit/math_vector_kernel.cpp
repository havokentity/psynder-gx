// SPDX-License-Identifier: MIT
// Psynder-GX SoA vector stack and math-kernel tests.

#include "math/MathLogicKernel.h"
#include "math/VectorStack.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace psynder;
using namespace psynder::math;

namespace {

bool approx_eq(f32 a, f32 b, f32 tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

void require_vec3_close(Vec3 a, Vec3 b) {
    REQUIRE(approx_eq(a.x, b.x));
    REQUIRE(approx_eq(a.y, b.y));
    REQUIRE(approx_eq(a.z, b.z));
}

}  // namespace

TEST_CASE("math: VectorStack keeps SoA streams cache aligned",
          "[math][vector_stack]") {
    Vec3SoaBuffer buffer(32);
    REQUIRE(reinterpret_cast<std::uintptr_t>(buffer.x_data()) % kCacheLine == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(buffer.y_data()) % kCacheLine == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(buffer.z_data()) % kCacheLine == 0);
    REQUIRE(buffer.count() == 32);
}

TEST_CASE("math: VectorStack integrates SoA positions in bulk",
          "[math][vector_stack]") {
    std::array<f32, 4> px{{0.0f, 1.0f, 2.0f, 3.0f}};
    std::array<f32, 4> py{{0.5f, 1.5f, 2.5f, 3.5f}};
    std::array<f32, 4> pz{{1.0f, 2.0f, 3.0f, 4.0f}};
    std::array<f32, 4> vx{{1.0f, 0.5f, -1.0f, 2.0f}};
    std::array<f32, 4> vy{{-1.0f, 1.0f, 0.5f, -0.5f}};
    std::array<f32, 4> vz{{0.0f, 2.0f, -2.0f, 1.0f}};
    const auto ref_x = px;
    const auto ref_y = py;
    const auto ref_z = pz;

    VectorStack stack;
    stack.integrate_positions(MutableVec3SoaView{px.data(), py.data(), pz.data(), px.size()},
                              Vec3SoaView{vx.data(), vy.data(), vz.data(), vx.size()},
                              0.25f);
    stack.flush();

    for (usize i = 0; i < px.size(); ++i) {
        REQUIRE(approx_eq(px[i], ref_x[i] + vx[i] * 0.25f));
        REQUIRE(approx_eq(py[i], ref_y[i] + vy[i] * 0.25f));
        REQUIRE(approx_eq(pz[i], ref_z[i] + vz[i] * 0.25f));
    }
    REQUIRE(stack.stats().soa_elements == px.size());
}

TEST_CASE("math: MathLogicKernel records once and executes SoA streams",
          "[math][logic_kernel]") {
    MathLogicKernelBuilder builder;
    builder.begin_record();

    auto position = builder.vec3_stream(0);
    auto movement = builder.vec3_stream(1);
    auto accel = builder.vec3_stream(2);
    const LogicScalar dt = builder.f32_uniform(0, 0.5f);

    movement = movement + accel * dt;
    position = position + movement * dt;

    MathLogicKernel kernel = builder.end_record();

    Vec3SoaBuffer positions(3);
    Vec3SoaBuffer movements(3);
    Vec3SoaBuffer accels(3);
    for (usize i = 0; i < positions.count(); ++i) {
        positions.x_data()[i] = static_cast<f32>(i);
        positions.y_data()[i] = static_cast<f32>(i) + 10.0f;
        positions.z_data()[i] = static_cast<f32>(i) + 20.0f;
        movements.x_data()[i] = 1.0f;
        movements.y_data()[i] = 2.0f;
        movements.z_data()[i] = 3.0f;
        accels.x_data()[i] = 0.5f;
        accels.y_data()[i] = 1.0f;
        accels.z_data()[i] = -0.25f;
    }

    std::array streams{
        positions.mutable_view(),
        movements.mutable_view(),
        accels.mutable_view(),
    };
    REQUIRE(kernel.execute(streams) == positions.count());

    const Vec3 expected_movement{1.25f, 2.5f, 2.875f};
    for (usize i = 0; i < positions.count(); ++i) {
        require_vec3_close(Vec3{movements.x_data()[i],
                                movements.y_data()[i],
                                movements.z_data()[i]},
                           expected_movement);
        require_vec3_close(Vec3{positions.x_data()[i],
                                positions.y_data()[i],
                                positions.z_data()[i]},
                           Vec3{static_cast<f32>(i) + expected_movement.x * 0.5f,
                                static_cast<f32>(i) + 10.0f +
                                    expected_movement.y * 0.5f,
                                static_cast<f32>(i) + 20.0f +
                                    expected_movement.z * 0.5f});
    }

    REQUIRE(kernel.stats().vec3_streams == 3);
    REQUIRE(kernel.stats().stores == 2);
}

TEST_CASE("math: MathLogicKernel SIMD fused path handles chunks and tails",
          "[math][logic_kernel]") {
    MathLogicKernelBuilder builder;
    builder.begin_record();

    auto position = builder.vec3_stream(0);
    auto movement = builder.vec3_stream(1);
    auto accel = builder.vec3_stream(2);
    const LogicScalar accel_dt = builder.f32_uniform(0, 0.25f);
    const LogicScalar pos_dt = builder.f32_uniform(1, 0.5f);

    movement = movement + accel * accel_dt;
    position = position + movement * pos_dt;

    MathLogicKernel kernel = builder.end_record();
    REQUIRE(kernel.stats().fused_madd == 2);
    REQUIRE(kernel.stats().instructions == 1);

    constexpr usize kCount = 1031;
    Vec3SoaBuffer positions(kCount);
    Vec3SoaBuffer movements(kCount);
    Vec3SoaBuffer accels(kCount);
    std::vector<Vec3> original_positions(kCount);
    std::vector<Vec3> original_movements(kCount);
    std::vector<Vec3> original_accels(kCount);

    for (usize i = 0; i < kCount; ++i) {
        const f32 f = static_cast<f32>(i);
        original_positions[i] = Vec3{f * 0.25f, f * -0.5f, f * 0.125f};
        original_movements[i] = Vec3{1.0f + f * 0.01f, -2.0f + f * 0.02f, 0.5f - f * 0.005f};
        original_accels[i] = Vec3{0.25f, -0.5f, 1.5f};

        positions.x_data()[i] = original_positions[i].x;
        positions.y_data()[i] = original_positions[i].y;
        positions.z_data()[i] = original_positions[i].z;
        movements.x_data()[i] = original_movements[i].x;
        movements.y_data()[i] = original_movements[i].y;
        movements.z_data()[i] = original_movements[i].z;
        accels.x_data()[i] = original_accels[i].x;
        accels.y_data()[i] = original_accels[i].y;
        accels.z_data()[i] = original_accels[i].z;
    }

    std::array streams{
        positions.mutable_view(),
        movements.mutable_view(),
        accels.mutable_view(),
    };
    REQUIRE(kernel.execute(streams) == kCount);

    for (usize i = 0; i < kCount; ++i) {
        const Vec3 expected_movement{
            original_movements[i].x + original_accels[i].x * 0.25f,
            original_movements[i].y + original_accels[i].y * 0.25f,
            original_movements[i].z + original_accels[i].z * 0.25f,
        };
        const Vec3 expected_position{
            original_positions[i].x + expected_movement.x * 0.5f,
            original_positions[i].y + expected_movement.y * 0.5f,
            original_positions[i].z + expected_movement.z * 0.5f,
        };
        require_vec3_close(Vec3{movements.x_data()[i],
                                movements.y_data()[i],
                                movements.z_data()[i]},
                           expected_movement);
        require_vec3_close(Vec3{positions.x_data()[i],
                                positions.y_data()[i],
                                positions.z_data()[i]},
                           expected_position);
    }
}
