// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/net_replication_session.cpp — the server-authoritative replication
// loop (netcode keystone). Proves the end-to-end snapshot + prediction +
// reconciliation path over the deterministic latency channel:
//   (a) each client's local prediction CONVERGES bit-exactly to the server's
//       authoritative state (and to a reference integration of the same inputs);
//   (b) the loop is deterministic across runs (same inputs -> identical world);
//   (c) the snapshot delta is smaller when the world is idle than when it moves.

#include "net/ReplicationSession.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <span>

using namespace psynder;
using namespace psynder::net;

namespace {

constexpr u32 kClients = 2;
constexpr u32 kLatency = 3;        // ticks each way
constexpr u32 kMotionTicks = 20;   // then idle to flush the pipeline
constexpr u32 kTotalTicks = 48;    // > kMotionTicks + 2*latency, all idle tail

// Deterministic per-(client,tick) input schedule. Client 0 jogs +X, client 1
// jogs +Z during the motion window, then both idle (move 0) holding their yaw.
Input input_for(u32 client, u32 tick) {
    Input in{};
    const bool moving = tick < kMotionTicks;
    if (client == 0) {
        in.move[0] = moving ? 2.0f : 0.0f;
        in.yaw_deg = 10.0f;
    } else {
        in.move[2] = moving ? 3.0f : 0.0f;
        in.yaw_deg = -20.0f;
    }
    return in;
}

bool eq(const EntityState& a, const EntityState& b) {
    return a.id == b.id && a.pos[0] == b.pos[0] && a.pos[1] == b.pos[1] &&
           a.pos[2] == b.pos[2] && a.yaw_deg == b.yaw_deg;
}

}  // namespace

TEST_CASE("net: replication session converges client prediction to server",
          "[net][determinism]") {
    const TickConfig cfg = tick_config_128();
    const f32 dt = static_cast<f32>(cfg.frame_sec);
    ReplicationSession sess(cfg, kClients, kLatency);

    // Reference: integrate each client's schedule with the SAME shared step.
    std::array<EntityState, kClients> ref{};
    for (u32 c = 0; c < kClients; ++c) {
        ref[c] = EntityState{};
        ref[c].id = c + 1u;
    }

    usize motion_delta = 0;
    usize idle_delta = 0;
    for (u32 t = 0; t < kTotalTicks; ++t) {
        std::array<Input, kClients> ins;
        for (u32 c = 0; c < kClients; ++c) {
            ins[c] = input_for(c, t);
            step_entity(ref[c], ins[c], dt);
        }
        sess.advance(std::span<const Input>(ins.data(), kClients));
        if (t == 5) motion_delta = sess.last_delta_bytes();
        if (t == 40) idle_delta = sess.last_delta_bytes();
    }

    // The unapplied tail inputs are all idle (no-ops past kMotionTicks), so the
    // server world and every client's prediction settle on the reference.
    for (u32 c = 0; c < kClients; ++c) {
        REQUIRE(eq(sess.authoritative()[c], ref[c]));
        REQUIRE(eq(sess.predicted(c), ref[c]));
    }
    // Client 0 actually moved on +X; sanity-check it's not still at the origin.
    REQUIRE(sess.authoritative()[0].pos[0] > 0.1f);
    REQUIRE(sess.authoritative()[1].pos[2] > 0.1f);

    // A moving world delta carries the changed entities; an idle one is ~header.
    REQUIRE(idle_delta < motion_delta);
}

TEST_CASE("net: replication session is deterministic across runs",
          "[net][determinism]") {
    const auto run = []() {
        const TickConfig cfg = tick_config_128();
        ReplicationSession sess(cfg, kClients, kLatency);
        for (u32 t = 0; t < kTotalTicks; ++t) {
            std::array<Input, kClients> ins;
            for (u32 c = 0; c < kClients; ++c) ins[c] = input_for(c, t);
            sess.advance(std::span<const Input>(ins.data(), kClients));
        }
        return sess.authoritative();  // std::vector<EntityState>
    };
    const std::vector<EntityState> a = run();
    const std::vector<EntityState> b = run();
    REQUIRE(a.size() == b.size());
    REQUIRE(std::memcmp(a.data(), b.data(), a.size() * sizeof(EntityState)) == 0);
}

TEST_CASE("net: raycast_nearest_entity hits on-axis, misses off-axis", "[net]") {
    const f32 o[3] = {0.0f, 0.0f, 0.0f};
    const f32 d[3] = {1.0f, 0.0f, 0.0f};  // +X
    std::array<EntityState, 2> w{};
    w[0].id = 2;
    w[0].pos[0] = 5.0f;  // on the +X axis -> hit
    w[1].id = 3;
    w[1].pos[0] = 5.0f;
    w[1].pos[2] = 2.0f;  // off axis (z=2 > radius 0.5)
    REQUIRE(raycast_nearest_entity(std::span<const EntityState>(w), /*exclude=*/0u,
                                   o, d, 0.5f, 100.0f) == 2u);
    // Exclude the on-axis one: the off-axis entity is a clean miss.
    REQUIRE(raycast_nearest_entity(std::span<const EntityState>(w), /*exclude=*/2u,
                                   o, d, 0.5f, 100.0f) == 0u);
}

TEST_CASE("net: lag-compensated hitreg hits where the shooter saw the target",
          "[net][determinism]") {
    const TickConfig cfg = tick_config_128();
    constexpr u32 kLat = 3;
    constexpr u32 kPhase1 = 32;  // client 1 runs out along +X
    constexpr u32 kTotal = 70;   // then strafes +Z while client 0 fires +X

    ReplicationSession sess(cfg, /*clients=*/2, kLat);
    for (u32 t = 0; t < kTotal; ++t) {
        std::array<Input, 2> ins{};
        // Client 0: stationary at origin, aiming +X (yaw 90, pitch 0); fires
        // every tick once client 1 starts strafing.
        ins[0].yaw_deg = 90.0f;
        if (t >= kPhase1) ins[0].buttons = kInputBtnFire;
        // Client 1: run +X onto the shot axis, then strafe +Z off it.
        if (t < kPhase1) {
            ins[1].move[0] = 40.0f;
        } else {
            ins[1].move[2] = 20.0f;
        }
        sess.advance(std::span<const Input>(ins.data(), 2));
    }

    // Lag compensation registered a hit of client 0 (id 1) on client 1 (id 2):
    // the server rewound to when the shooter saw the target still on the axis.
    bool hit = false;
    for (const HitEvent& h : sess.hit_events()) {
        if (h.attacker == 1u && h.victim == 2u) hit = true;
    }
    REQUIRE(hit);

    // Without rewind, a ray against the CURRENT authoritative world misses —
    // client 1 has strafed well off the axis by now. (Proves lag comp did work.)
    const f32 o[3] = {0.0f, 0.0f, 0.0f};
    const f32 d[3] = {1.0f, 0.0f, 0.0f};
    REQUIRE(raycast_nearest_entity(
                std::span<const EntityState>(sess.authoritative()),
                /*exclude=*/1u, o, d, 0.5f, 1000.0f) == 0u);
}

TEST_CASE("net: per-peer AoI culls entities outside the interest radius",
          "[net][determinism]") {
    const TickConfig cfg = tick_config_128();
    // 5 m interest radius; client 1 runs far past it while client 0 sits still.
    ReplicationSession sess(cfg, /*clients=*/2, /*latency=*/0, /*aoi_radius_m=*/5.0f);
    for (u32 t = 0; t < 60; ++t) {
        std::array<Input, 2> ins{};
        ins[1].move[0] = 40.0f;  // ~18.75 m out after 60 ticks at 128 Hz
        sess.advance(std::span<const Input>(ins.data(), 2));
    }
    // Each peer now sees only itself — the other is outside the 5 m sphere.
    REQUIRE(sess.visible_count(0) == 1u);
    REQUIRE(sess.visible_count(1) == 1u);
    // The server still owns the full authoritative world regardless of AoI.
    REQUIRE(sess.authoritative().size() == 2u);
    REQUIRE(sess.authoritative()[1].pos[0] > 5.0f);
}

TEST_CASE("net: infinite AoI keeps every entity visible to every peer", "[net]") {
    const TickConfig cfg = tick_config_128();
    ReplicationSession sess(cfg, 2, 0);  // default radius => no culling
    for (u32 t = 0; t < 60; ++t) {
        std::array<Input, 2> ins{};
        ins[1].move[0] = 40.0f;
        sess.advance(std::span<const Input>(ins.data(), 2));
    }
    REQUIRE(sess.visible_count(0) == 2u);
    REQUIRE(sess.visible_count(1) == 2u);
}
