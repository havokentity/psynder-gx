// SPDX-License-Identifier: MIT
// Psynder-GX — Behavior IR interpreter (engine/script/behavior/BehaviorIR), the
// executable middle of ADR-018: a data-driven behavior runs deterministically
// over SoA component columns. Proves a projectile-integration behavior matches a
// reference, a conditional (threshold + select) behavior, and run-to-run
// bit-determinism.

#include "script/behavior/BehaviorIR.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::script::behavior;

namespace {
// Stream layout for the projectile behavior.
enum : u16 { PX = 0, PY = 1, PZ = 2, VX = 3, VY = 4, VZ = 5, NUM = 6 };

// Build: semi-implicit Euler under gravity on Y. vy += g*dt; p += v*dt.
BehaviorProgram projectile_program(f32 dt, f32 g) {
    BehaviorBuilder b(NUM);
    const u16 u_dt = b.uniform(dt);
    const u16 u_g = b.uniform(g);
    const u16 rdt = b.reg();
    const u16 rg = b.reg();
    b.load_uniform(rdt, u_dt);
    b.load_uniform(rg, u_g);

    // vy += g*dt
    const u16 rv = b.reg();
    b.load_stream(rv, VY);
    b.madd(rv, rv, rg, rdt);  // rv = rv + rg*rdt
    b.store_stream(VY, rv);

    // p_axis += v_axis * dt  (uses the just-updated vy -> semi-implicit)
    const u16 rp = b.reg();
    const u16 rvv = b.reg();
    const u16 pos[3] = {PX, PY, PZ};
    const u16 vel[3] = {VX, VY, VZ};
    for (int axis = 0; axis < 3; ++axis) {
        b.load_stream(rp, pos[axis]);
        b.load_stream(rvv, vel[axis]);
        b.madd(rp, rp, rvv, rdt);
        b.store_stream(pos[axis], rp);
    }
    return b.build();
}
}  // namespace

TEST_CASE("behavior-ir: a projectile behavior integrates like the reference",
          "[behavior][gameplay]") {
    constexpr f32 dt = 1.0f / 120.0f;
    constexpr f32 g = -9.81f;
    const BehaviorProgram prog = projectile_program(dt, g);

    BehaviorChunk chunk;
    chunk.configure(NUM, 3);
    // Three projectiles with distinct launch velocities.
    for (usize i = 0; i < 3; ++i) {
        chunk.stream(VX)[i] = 2.0f + static_cast<f32>(i);
        chunk.stream(VY)[i] = 10.0f;
        chunk.stream(PY)[i] = 1.0f;
    }

    // Reference: the same semi-implicit Euler, computed straight.
    struct Ref { f32 px, py, vy; } ref[3];
    for (usize i = 0; i < 3; ++i) ref[i] = {0.0f, 1.0f, 10.0f};
    const f32 vx[3] = {2.0f, 3.0f, 4.0f};

    for (int tick = 0; tick < 90; ++tick) {
        execute(prog, chunk);
        for (usize i = 0; i < 3; ++i) {
            ref[i].vy = ref[i].vy + g * dt;
            ref[i].px = ref[i].px + vx[i] * dt;
            ref[i].py = ref[i].py + ref[i].vy * dt;
        }
    }
    for (usize i = 0; i < 3; ++i) {
        REQUIRE(chunk.stream(VY)[i] == Catch::Approx(ref[i].vy).margin(1e-3f));
        REQUIRE(chunk.stream(PX)[i] == Catch::Approx(ref[i].px).margin(1e-3f));
        REQUIRE(chunk.stream(PY)[i] == Catch::Approx(ref[i].py).margin(1e-3f));
    }
    // Gravity actually decelerated the upward launch (vy fell from 10 m/s) while
    // the projectile is still climbing toward apex.
    REQUIRE(chunk.stream(VY)[0] < 10.0f);
    REQUIRE(chunk.stream(PY)[0] > 1.0f);
}

TEST_CASE("behavior-ir: a threshold+select behavior flags and heals low health",
          "[behavior][gameplay]") {
    enum : u16 { HP = 0, FLAG = 1, NS = 2 };
    BehaviorBuilder b(NS);
    const u16 u_thresh = b.uniform(25.0f);
    const u16 u_heal = b.uniform(50.0f);
    const u16 rhp = b.reg(), rth = b.reg(), rflag = b.reg(), rheal = b.reg(),
              rnew = b.reg();
    b.load_stream(rhp, HP);
    b.load_uniform(rth, u_thresh);
    b.cmp_le(rflag, rhp, rth);             // flag = hp <= 25
    b.store_stream(FLAG, rflag);
    b.load_uniform(rheal, u_heal);
    b.add(rheal, rhp, rheal);              // hp + 50
    b.select(rnew, rflag, rheal, rhp);     // low ? healed : unchanged
    b.store_stream(HP, rnew);
    const BehaviorProgram prog = b.build();

    BehaviorChunk chunk;
    chunk.configure(NS, 4);
    const f32 hp_in[4] = {10.0f, 25.0f, 26.0f, 80.0f};
    for (usize i = 0; i < 4; ++i) chunk.stream(HP)[i] = hp_in[i];

    execute(prog, chunk);

    // hp <= 25 -> flagged + healed by 50; otherwise untouched.
    REQUIRE(chunk.stream(FLAG)[0] == 1.0f);
    REQUIRE(chunk.stream(FLAG)[1] == 1.0f);
    REQUIRE(chunk.stream(FLAG)[2] == 0.0f);
    REQUIRE(chunk.stream(FLAG)[3] == 0.0f);
    REQUIRE(chunk.stream(HP)[0] == Catch::Approx(60.0f));
    REQUIRE(chunk.stream(HP)[1] == Catch::Approx(75.0f));
    REQUIRE(chunk.stream(HP)[2] == Catch::Approx(26.0f));
    REQUIRE(chunk.stream(HP)[3] == Catch::Approx(80.0f));
}

TEST_CASE("behavior-ir: execution is bit-deterministic across runs",
          "[behavior][determinism]") {
    constexpr f32 dt = 1.0f / 144.0f;
    const BehaviorProgram prog = projectile_program(dt, -9.81f);
    const auto run = [&]() {
        BehaviorChunk c;
        c.configure(NUM, 64);
        for (usize i = 0; i < 64; ++i) {
            c.stream(VX)[i] = static_cast<f32>(i % 7) * 0.5f;
            c.stream(VY)[i] = 5.0f + static_cast<f32>(i % 3);
            c.stream(PY)[i] = 2.0f;
        }
        for (int t = 0; t < 200; ++t) execute(prog, c);
        std::vector<f32> out;
        for (u16 s = 0; s < NUM; ++s)
            out.insert(out.end(), c.stream(s).begin(), c.stream(s).end());
        return out;
    };
    const std::vector<f32> a = run();
    const std::vector<f32> b = run();
    REQUIRE(a.size() == b.size());
    REQUIRE(std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0);
}
