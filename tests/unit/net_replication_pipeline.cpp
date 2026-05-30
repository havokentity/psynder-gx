// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/net_replication_pipeline.cpp — the end-to-end replication pipeline
// run headless: a ReplicationServer streams bandwidth-managed snapshots of
// MOVING entities, and a ReplicationClient reassembles + unpacks + interpolates
// them. This composes SnapshotScheduler + BandwidthBudget + Fragment +
// SnapshotPack on the send side and FragmentReassembler + unpack_quantized +
// SnapshotInterpBuffer + PlayoutClock on the receive side into one loop, and
// asserts the client reconstructs the server's interpolated motion, that a
// starved tick drops the low-priority entities while the high-priority ones keep
// tracking, and that the whole pipeline is bit-deterministic across two runs.

#include "net/ReplicationPipeline.h"

#include "net/SnapshotReplication.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

constexpr f32 kRes      = 0.01f;   // 1 cm position quantization
constexpr f32 kTickDt   = 0.05f;   // 20 Hz server tick
constexpr f32 kDelay    = 0.1f;    // interpolation delay (two ticks)
constexpr u32 kEntities = 4u;

// `kEntities` entities whose X position advances linearly with the tick index,
// so a time-bracketed LERP on the client reproduces the exact server position.
// Entity i starts at x = i and moves +0.5 m per tick; y/z are constant; yaw is
// a constant per-entity facing (kept fixed so the shortest-arc yaw LERP is a
// trivial identity and does not muddy the position assertions). Built the same
// way as net_snapshot_scheduler.cpp's make_world().
std::vector<EntityState> world_at_tick(u32 tick) {
    std::vector<EntityState> s(kEntities);
    for (u32 i = 0; i < kEntities; ++i) {
        s[i].id      = 100u + i;
        s[i].pos[0]  = static_cast<f32>(i) + static_cast<f32>(tick) * 0.5f;
        s[i].pos[1]  = 0.5f;
        s[i].pos[2]  = static_cast<f32>(i) * 2.0f;
        s[i].yaw_deg = static_cast<f32>(i) * 10.0f;
    }
    return s;
}

// Base priorities: entity 0 wants sending most, decreasing with index, all
// positive so none is permanently starved (mirrors make_priorities()).
std::vector<f32> make_priorities() {
    std::vector<f32> p(kEntities);
    for (u32 i = 0; i < kEntities; ++i) {
        p[i] = static_cast<f32>(kEntities - i);  // 4..1
    }
    return p;
}

// The true (server-authoritative) X of entity i at a fractional tick `t_tick`.
f32 true_x(u32 i, f64 t_tick) {
    return static_cast<f32>(i) + static_cast<f32>(t_tick) * 0.5f;
}

// Find entity id in a view; returns nullptr if absent.
const EntityState* find_id(const std::vector<EntityState>& v, u32 id) {
    for (const EntityState& e : v) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("replication pipeline: client interpolates the server's moving entities",
          "[net]") {
    ReplicationServer server;
    // A generous budget (all four 20-byte records fit each tick) and a large MTU
    // so the whole snapshot lands in a single fragment.
    server.configure(kEntities, /*bytes_per_s=*/40000.0f, /*burst=*/4000,
                     kRes, /*mtu=*/1200);

    ReplicationClient client;
    client.configure(kRes, kDelay);

    const std::vector<f32> prio = make_priorities();

    // Stream 10 ticks. The client receives each tick at server time tick*dt and
    // advances its playout clock by the same dt, so the render clock stays locked
    // one interpolation delay behind the freshest snapshot.
    std::vector<std::vector<u8>> frags;
    std::vector<EntityState>     view;

    for (u32 tick = 0; tick < 10u; ++tick) {
        const std::vector<EntityState> w = world_at_tick(tick);
        server.tick(w, prio, kTickDt, static_cast<u16>(tick), frags);
        REQUIRE(server.last_plan().record_count == kEntities);

        const f64 server_time = static_cast<f64>(tick) * static_cast<f64>(kTickDt);
        REQUIRE(client.receive(frags, server_time));
        client.advance(kTickDt);
    }

    // After the stream the playout clock trails the newest snapshot (t = 9*dt)
    // by the interpolation delay, so it samples in the past where two snapshots
    // bracket the render time. Reconstruct the interpolated positions.
    REQUIRE(client.view(view));
    REQUIRE(view.size() == kEntities);

    const f64 render_time   = client.render_time_s();
    const f64 rendered_tick = render_time / static_cast<f64>(kTickDt);

    // The render time should sit one delay (two ticks) behind the newest server
    // time (9*dt) and well inside the buffered span, so it brackets real data.
    CHECK(render_time == Catch::Approx(9.0 * kTickDt - kDelay).margin(1.0e-6));

    // Each entity's interpolated X matches the true linear position at the
    // render time, within the quantization step plus a small interp tolerance.
    for (u32 i = 0; i < kEntities; ++i) {
        const EntityState* e = find_id(view, 100u + i);
        REQUIRE(e != nullptr);
        const f32 expect_x = true_x(i, rendered_tick);
        CHECK(e->pos[0] == Catch::Approx(expect_x).margin(kRes + 1.0e-3f));
        CHECK(e->pos[1] == Catch::Approx(0.5f).margin(kRes + 1.0e-3f));
        CHECK(e->pos[2] == Catch::Approx(static_cast<f32>(i) * 2.0f)
                               .margin(kRes + 1.0e-3f));
    }
}

TEST_CASE("replication pipeline: a starved tick drops low-priority entities",
          "[net]") {
    ReplicationServer server;
    ReplicationClient client;
    client.configure(kRes, kDelay);

    const std::vector<f32> prio = make_priorities();

    // First two ticks with a generous budget: the client buffers the full set so
    // interpolation has bracketing snapshots for every entity.
    server.configure(kEntities, /*bytes_per_s=*/40000.0f, /*burst=*/4000,
                     kRes, /*mtu=*/1200);
    std::vector<std::vector<u8>> frags;
    for (u32 tick = 0; tick < 2u; ++tick) {
        server.tick(world_at_tick(tick), prio, kTickDt,
                    static_cast<u16>(tick), frags);
        REQUIRE(server.last_plan().record_count == kEntities);
        REQUIRE(client.receive(frags, static_cast<f64>(tick) * kTickDt));
        client.advance(kTickDt);
    }

    // A starved tick: a tiny budget that affords only TWO 20-byte records. The
    // scheduler keeps the two HIGHEST-priority entities (indices 0 and 1) and
    // drops the rest. We reconfigure the budget but keep the same scheduler
    // accumulators is not possible across configure(); instead build a dedicated
    // tight-budget server seeded to the same priorities so index 0/1 win.
    ReplicationServer tight;
    tight.configure(kEntities, /*bytes_per_s=*/800.0f, /*burst=*/40,
                    kRes, /*mtu=*/1200);
    // refill 0.05s * 800 B/s = 40 bytes => exactly 2 records.
    std::vector<std::vector<u8>> tight_frags;
    tight.tick(world_at_tick(2u), prio, kTickDt, /*msg_id=*/2u, tight_frags);

    const SnapshotScheduler::SendPlan& tp = tight.last_plan();
    REQUIRE(tp.record_count == 2u);
    // The two highest-priority entities (slots 0 and 1) survived the squeeze.
    CHECK(tp.sent_indices[0] == 0u);
    CHECK(tp.sent_indices[1] == 1u);

    // The client still tracks the high-priority entities through the starved
    // tick: it receives the partial snapshot and can interpolate id 100 and 101.
    REQUIRE(client.receive(tight_frags, static_cast<f64>(2u) * kTickDt));
    client.advance(kTickDt);

    std::vector<EntityState> view;
    REQUIRE(client.view(view));
    // The high-priority ids are present in the interpolated view.
    CHECK(find_id(view, 100u) != nullptr);
    CHECK(find_id(view, 101u) != nullptr);
}

TEST_CASE("replication pipeline: out-of-order fragments still interpolate",
          "[net]") {
    ReplicationServer server;
    // A small MTU so a tick's snapshot spans multiple fragments, then deliver
    // them reversed — reassembly must not care about arrival order.
    server.configure(kEntities, /*bytes_per_s=*/40000.0f, /*burst=*/4000,
                     kRes, /*mtu=*/24);
    ReplicationClient client;
    client.configure(kRes, kDelay);

    const std::vector<f32> prio = make_priorities();
    std::vector<std::vector<u8>> frags;

    for (u32 tick = 0; tick < 6u; ++tick) {
        server.tick(world_at_tick(tick), prio, kTickDt,
                    static_cast<u16>(tick), frags);
        REQUIRE(frags.size() >= 2u);  // multi-fragment snapshot
        std::vector<std::vector<u8>> reversed(frags.rbegin(), frags.rend());
        REQUIRE(client.receive(reversed, static_cast<f64>(tick) * kTickDt));
        client.advance(kTickDt);
    }

    std::vector<EntityState> view;
    REQUIRE(client.view(view));
    REQUIRE(view.size() == kEntities);
    const f64 rendered_tick = client.render_time_s() / static_cast<f64>(kTickDt);
    for (u32 i = 0; i < kEntities; ++i) {
        const EntityState* e = find_id(view, 100u + i);
        REQUIRE(e != nullptr);
        CHECK(e->pos[0] ==
              Catch::Approx(true_x(i, rendered_tick)).margin(kRes + 1.0e-3f));
    }
}

TEST_CASE("replication pipeline: the whole pipeline is bit deterministic",
          "[net][determinism]") {
    const std::vector<f32> prio = make_priorities();

    // One run of the full server->client loop, capturing the last tick's
    // fragment bytes and the final interpolated view bytes.
    auto run = [&](std::vector<u8>& last_frag, std::vector<EntityState>& last_view) {
        ReplicationServer server;
        server.configure(kEntities, 40000.0f, 4000, kRes, /*mtu=*/24);
        ReplicationClient client;
        client.configure(kRes, kDelay);

        std::vector<std::vector<u8>> frags;
        for (u32 tick = 0; tick < 12u; ++tick) {
            server.tick(world_at_tick(tick), prio, kTickDt,
                        static_cast<u16>(tick), frags);
            client.receive(frags, static_cast<f64>(tick) * kTickDt);
            client.advance(kTickDt);
            if (tick == 11u && !frags.empty()) {
                last_frag = frags.back();
            }
        }
        client.view(last_view);
    };

    std::vector<u8>          a_frag, b_frag;
    std::vector<EntityState> a_view, b_view;
    run(a_frag, a_view);
    run(b_frag, b_view);

    // Identical fragment bytes on the wire ...
    CHECK(a_frag == b_frag);

    // ... and an identical interpolated view (compare bit-exact via memcmp of
    // each POD record; lockstep determinism forbids epsilon fuzz here).
    REQUIRE(a_view.size() == b_view.size());
    for (usize k = 0; k < a_view.size(); ++k) {
        CHECK(a_view[k].id == b_view[k].id);
        CHECK(a_view[k].pos[0] == b_view[k].pos[0]);
        CHECK(a_view[k].pos[1] == b_view[k].pos[1]);
        CHECK(a_view[k].pos[2] == b_view[k].pos[2]);
        CHECK(a_view[k].yaw_deg == b_view[k].yaw_deg);
    }
}
