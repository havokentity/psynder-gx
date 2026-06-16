// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_shake.cpp
//
// Lane 16 — deterministic trauma-based camera shake. Validates the contract
// documented in engine/camera/Shake.h:
//   - shake_add_trauma clamps the accumulated trauma into [0, 1] (impacts stack
//     but saturate; a stray negative can't drive it below zero).
//   - shake_tick decays trauma by decay_per_s*dt toward 0 (never negative) and
//     accumulates the noise phase clock; non-positive dt is a no-op.
//   - At trauma == 0 shake_sample returns an all-zero offset.
//   - With trauma > 0 each axis offset is bounded by trauma^2 * max_<axis>.
//   - The offset CHANGES as time_s advances (the value noise wiggles).
//   - A larger trauma yields a larger offset envelope (trauma^2 scaling).
//   - A different seed yields a different noise pattern.
//   - The noise is continuous-ish: a tiny dt step moves the sample only a
//     little (a value-noise lerp, not a per-frame jump).
//   - Determinism: two identical (state, params) give bit-identical offsets
//     (== on the raw f32 fields — the replay-safety guarantee).

#include "camera/Shake.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <type_traits>

using namespace psynder;
using namespace psynder::camera;

namespace {

// A representative shake envelope shared by most cases.
ShakeParams make_params() noexcept {
    ShakeParams p{};
    p.max_pitch_deg = 4.0f;
    p.max_yaw_deg   = 4.0f;
    p.max_roll_deg  = 6.0f;
    p.max_pos_m     = 0.05f;
    p.frequency_hz  = 20.0f;
    p.decay_per_s   = 1.0f;
    return p;
}

// Largest absolute component across every offset axis — the "envelope" size.
f32 max_abs_component(const ShakeOffset& o) noexcept {
    f32 m = std::fabs(o.pitch_deg);
    m = std::fmax(m, std::fabs(o.yaw_deg));
    m = std::fmax(m, std::fabs(o.roll_deg));
    m = std::fmax(m, std::fabs(o.pos[0]));
    m = std::fmax(m, std::fabs(o.pos[1]));
    m = std::fmax(m, std::fabs(o.pos[2]));
    return m;
}

}  // namespace

TEST_CASE("shake: state and params are trivially copyable PODs", "[camera]") {
    REQUIRE(std::is_trivially_copyable_v<ShakeState>);
    REQUIRE(std::is_trivially_copyable_v<ShakeParams>);
    REQUIRE(std::is_trivially_copyable_v<ShakeOffset>);
    REQUIRE(sizeof(ShakeState) == 12);
}

TEST_CASE("shake: add_trauma clamps into the [0, 1] range", "[camera]") {
    ShakeState s{};

    // A plain add accumulates.
    shake_add_trauma(s, 0.3f);
    REQUIRE(s.trauma == Catch::Approx(0.3f));

    // Stacking saturates at 1.0 — never above.
    shake_add_trauma(s, 0.5f);
    REQUIRE(s.trauma == Catch::Approx(0.8f));
    shake_add_trauma(s, 0.9f);
    REQUIRE(s.trauma == Catch::Approx(1.0f));
    REQUIRE(s.trauma <= 1.0f);

    // A stray negative cannot drive trauma below zero.
    ShakeState n{};
    shake_add_trauma(n, 0.2f);
    shake_add_trauma(n, -5.0f);
    REQUIRE(n.trauma == 0.0f);
    REQUIRE(n.trauma >= 0.0f);
}

TEST_CASE("shake: tick decays trauma toward zero and never goes negative",
          "[camera]") {
    const ShakeParams p = make_params();
    ShakeState s{};
    shake_add_trauma(s, 1.0f);

    // One step: trauma drops by exactly decay_per_s * dt, time accumulates.
    const f32 dt = 0.25f;
    shake_tick(s, dt, p);
    REQUIRE(s.trauma == Catch::Approx(1.0f - p.decay_per_s * dt));
    REQUIRE(s.time_s == Catch::Approx(dt));

    // Several more steps keep draining it.
    f32 prev = s.trauma;
    for (int i = 0; i < 3; ++i) {
        shake_tick(s, dt, p);
        REQUIRE(s.trauma <= prev);
        REQUIRE(s.trauma >= 0.0f);
        prev = s.trauma;
    }

    // A huge dt clamps trauma AT zero (no negative overshoot) but time still
    // advances.
    const f32 t_before = s.time_s;
    shake_tick(s, 100.0f, p);
    REQUIRE(s.trauma == 0.0f);
    REQUIRE(s.time_s == Catch::Approx(t_before + 100.0f));

    // Non-positive dt is a no-op on BOTH fields.
    ShakeState z{};
    shake_add_trauma(z, 0.5f);
    const ShakeState snap = z;
    shake_tick(z, 0.0f, p);
    REQUIRE(z.trauma == snap.trauma);
    REQUIRE(z.time_s == snap.time_s);
    shake_tick(z, -1.0f, p);
    REQUIRE(z.trauma == snap.trauma);
    REQUIRE(z.time_s == snap.time_s);
}

TEST_CASE("shake: at zero trauma the sampled offset is all zeros", "[camera]") {
    const ShakeParams p = make_params();
    ShakeState s{};
    s.time_s = 3.14159f;   // even with a non-zero phase clock...
    s.seed   = 0xABCDu;

    const ShakeOffset o = shake_sample(s, p);   // ...trauma is 0
    REQUIRE(o.pitch_deg == 0.0f);
    REQUIRE(o.yaw_deg   == 0.0f);
    REQUIRE(o.roll_deg  == 0.0f);
    REQUIRE(o.pos[0]    == 0.0f);
    REQUIRE(o.pos[1]    == 0.0f);
    REQUIRE(o.pos[2]    == 0.0f);

    // shake_intensity agrees (trauma^2 == 0).
    REQUIRE(shake_intensity(s) == 0.0f);
}

TEST_CASE("shake: each axis offset is bounded by trauma^2 * max per axis",
          "[camera]") {
    const ShakeParams p = make_params();

    ShakeState s{};
    s.seed   = 0x5151u;
    shake_add_trauma(s, 0.7f);

    // Walk the noise across many phases; the per-axis bound must hold at EVERY
    // time, since value_noise is in [-1, 1] and the offset is
    // trauma^2 * max_<axis> * noise.
    const f32 mag = s.trauma * s.trauma;
    const f32 eps = 1e-5f;
    for (int i = 0; i < 200; ++i) {
        s.time_s = static_cast<f32>(i) * 0.013f;
        const ShakeOffset o = shake_sample(s, p);
        REQUIRE(std::fabs(o.pitch_deg) <= mag * p.max_pitch_deg + eps);
        REQUIRE(std::fabs(o.yaw_deg)   <= mag * p.max_yaw_deg   + eps);
        REQUIRE(std::fabs(o.roll_deg)  <= mag * p.max_roll_deg  + eps);
        REQUIRE(std::fabs(o.pos[0])    <= mag * p.max_pos_m     + eps);
        REQUIRE(std::fabs(o.pos[1])    <= mag * p.max_pos_m     + eps);
        REQUIRE(std::fabs(o.pos[2])    <= mag * p.max_pos_m     + eps);
    }
}

TEST_CASE("shake: the offset changes as time advances (the noise wiggles)",
          "[camera]") {
    const ShakeParams p = make_params();

    ShakeState a{};
    a.seed = 0x1234u;
    shake_add_trauma(a, 1.0f);

    // Sample at two well-separated phases; the value noise must differ.
    a.time_s = 0.10f;
    const ShakeOffset o0 = shake_sample(a, p);
    a.time_s = 0.55f;   // ~9 lattice cells later at 20 Hz -> different values
    const ShakeOffset o1 = shake_sample(a, p);

    const bool differs =
        (o0.pitch_deg != o1.pitch_deg) || (o0.yaw_deg != o1.yaw_deg) ||
        (o0.roll_deg != o1.roll_deg)   || (o0.pos[0] != o1.pos[0]) ||
        (o0.pos[1] != o1.pos[1])       || (o0.pos[2] != o1.pos[2]);
    REQUIRE(differs);
}

TEST_CASE("shake: larger trauma yields a larger offset envelope (trauma^2)",
          "[camera]") {
    const ShakeParams p = make_params();

    // Two states with the SAME seed + time but different trauma. Because the
    // noise sample is identical (same seed/phase) and only the trauma^2 scale
    // differs, the full-trauma envelope is exactly 4x the half-trauma one
    // (1.0^2 / 0.5^2 == 4), and strictly larger on every non-zero axis.
    ShakeState full{};
    full.seed   = 0x7777u;
    full.time_s = 0.37f;
    full.trauma = 1.0f;

    ShakeState half = full;
    half.trauma = 0.5f;

    const ShakeOffset of = shake_sample(full, p);
    const ShakeOffset oh = shake_sample(half, p);

    const f32 env_full = max_abs_component(of);
    const f32 env_half = max_abs_component(oh);
    REQUIRE(env_full > env_half);

    // trauma^2 scaling: 1.0^2 vs 0.5^2 -> ratio 4. Check on the pitch axis
    // (the noise factor cancels since seed + phase match).
    REQUIRE(of.pitch_deg == Catch::Approx(4.0f * oh.pitch_deg));
    REQUIRE(env_full == Catch::Approx(4.0f * env_half));
}

TEST_CASE("shake: a different seed yields a different noise pattern", "[camera]") {
    const ShakeParams p = make_params();

    ShakeState a{};
    ShakeState b{};
    a.seed = 0x11111111u;
    b.seed = 0x22222222u;
    shake_add_trauma(a, 1.0f);
    shake_add_trauma(b, 1.0f);

    // Same trauma + same phase, different seed -> the patterns must diverge at
    // some sampled time.
    bool differs = false;
    for (int i = 0; i < 64 && !differs; ++i) {
        const f32 t = static_cast<f32>(i) * 0.05f;
        a.time_s = t;
        b.time_s = t;
        const ShakeOffset oa = shake_sample(a, p);
        const ShakeOffset ob = shake_sample(b, p);
        if (oa.pitch_deg != ob.pitch_deg || oa.yaw_deg != ob.yaw_deg ||
            oa.roll_deg != ob.roll_deg) {
            differs = true;
        }
    }
    REQUIRE(differs);
}

TEST_CASE("shake: the noise is continuous-ish across a tiny time step",
          "[camera]") {
    const ShakeParams p = make_params();

    ShakeState s{};
    s.seed   = 0x9A9Au;
    s.trauma = 1.0f;
    s.time_s = 0.20f;
    const ShakeOffset o0 = shake_sample(s, p);

    // A tiny dt step (well inside one lattice cell at 20 Hz: a cell is 0.05 s,
    // this advances the phase by only 0.00002 of a cell) must move the sample
    // by only a little — a value-noise lerp, not a discontinuous re-roll.
    s.time_s = 0.20f + 1e-6f;
    const ShakeOffset o1 = shake_sample(s, p);

    // The peak per-axis change here is tiny relative to the full envelope
    // (max_roll = 6 deg). A coarse bound of 0.01 deg comfortably proves there
    // is no jump while still being far below the per-frame wiggle amplitude.
    REQUIRE(std::fabs(o1.pitch_deg - o0.pitch_deg) < 0.01f);
    REQUIRE(std::fabs(o1.yaw_deg   - o0.yaw_deg)   < 0.01f);
    REQUIRE(std::fabs(o1.roll_deg  - o0.roll_deg)  < 0.01f);
}

TEST_CASE("shake: reset zeroes trauma and time but preserves the seed",
          "[camera]") {
    ShakeState s{};
    s.seed = 0xDEADBEEFu;
    shake_add_trauma(s, 0.8f);
    s.time_s = 1.5f;

    shake_reset(s);
    REQUIRE(s.trauma == 0.0f);
    REQUIRE(s.time_s == 0.0f);
    REQUIRE(s.seed   == 0xDEADBEEFu);   // pattern preserved across reset
}

TEST_CASE("shake: identical (state, params) give bit-identical offsets "
          "(determinism)",
          "[camera]") {
    const ShakeParams p = make_params();

    ShakeState a{};
    a.seed   = 0xC0FFEEu;
    a.trauma = 0.83f;
    a.time_s = 2.7183f;
    ShakeState b = a;   // exact copy

    const ShakeOffset oa = shake_sample(a, p);
    const ShakeOffset ob = shake_sample(b, p);

    // Bit-identical on every field (== on the raw f32s, not Approx).
    REQUIRE(oa.pitch_deg == ob.pitch_deg);
    REQUIRE(oa.yaw_deg   == ob.yaw_deg);
    REQUIRE(oa.roll_deg  == ob.roll_deg);
    REQUIRE(oa.pos[0]    == ob.pos[0]);
    REQUIRE(oa.pos[1]    == ob.pos[1]);
    REQUIRE(oa.pos[2]    == ob.pos[2]);

    // And a re-run of the same fire/tick sequence reproduces the same trauma
    // and phase exactly.
    ShakeState r1{}, r2{};
    auto run = [&](ShakeState& s) {
        shake_add_trauma(s, 0.6f);
        for (int i = 0; i < 10; ++i) shake_tick(s, 0.016f, p);
    };
    run(r1);
    run(r2);
    REQUIRE(r1.trauma == r2.trauma);
    REQUIRE(r1.time_s == r2.time_s);
}
