// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/net_snapshot_scheduler.cpp — the bandwidth-managed send pipeline
// integrated end to end: priority selection + a byte budget + quantized packing
// + fragmentation, and a peer reassembling + unpacking exactly what was sent.
// This composes PriorityAccumulator + BandwidthBudget + pack/unpack_quantized +
// Fragment/FragmentReassembler into the real "what does the server send" loop.

#include "net/SnapshotScheduler.h"

#include "net/Fragment.h"
#include "net/SnapshotPack.h"
#include "net/SnapshotReplication.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

constexpr f32 kRes = 0.01f;  // 1 cm position quantization

// 8 entities with distinct positions; base priority decreases with index
// (entity 0 wants sending most), but every entity has a POSITIVE base so all of
// them climb the queue and none is permanently starved.
std::vector<EntityState> make_world() {
    std::vector<EntityState> s(8);
    for (u32 i = 0; i < 8; ++i) {
        s[i].id = 100u + i;
        s[i].pos[0] = static_cast<f32>(i) * 1.0f;
        s[i].pos[1] = 0.5f;
        s[i].pos[2] = static_cast<f32>(i) * 2.0f;
        s[i].yaw_deg = static_cast<f32>(i) * 10.0f;
    }
    return s;
}

std::vector<f32> make_priorities() {
    std::vector<f32> p(8);
    for (u32 i = 0; i < 8; ++i) p[i] = static_cast<f32>(8u - i);  // 8..1
    return p;
}

// Reassemble a peer's received fragments (added in the given order) and unpack
// them back to EntityStates. Returns true on a clean reassemble + unpack.
bool receive(const std::vector<std::vector<u8>>& fragments, f32 res,
             std::vector<EntityState>& out) {
    FragmentReassembler ra;
    for (const auto& f : fragments) {
        ra.add_fragment(f);
    }
    if (!ra.complete()) return false;
    std::vector<u8> payload;
    if (!ra.assemble(payload)) return false;
    return unpack_quantized(payload, res, out);
}

}  // namespace

TEST_CASE("scheduler: sends the highest-priority entities within the budget",
          "[net]") {
    const std::vector<EntityState> world = make_world();
    const std::vector<f32> prio = make_priorities();

    SnapshotScheduler sched;
    sched.configure(world.size());

    BandwidthBudget budget;
    budget.configure(/*bytes_per_s=*/6000.0f, /*burst_bytes=*/200);
    budget.refill(0.01f);  // 60 bytes => 3 records of 20 bytes

    std::vector<std::vector<u8>> frags;
    SnapshotScheduler::SendPlan plan;
    sched.tick(world, prio, budget, kRes, /*message_id=*/1u, /*mtu=*/32u,
               frags, plan);

    // The budget afforded exactly 3 records, and the top-3 priorities are
    // entities 0,1,2 (descending priority order).
    REQUIRE(plan.record_count == 3u);
    REQUIRE(plan.sent_indices.size() == 3u);
    CHECK(plan.sent_indices[0] == 0u);
    CHECK(plan.sent_indices[1] == 1u);
    CHECK(plan.sent_indices[2] == 2u);
    CHECK(plan.budget_spent == 60u);
    CHECK(plan.fragment_count >= 1u);
}

TEST_CASE("scheduler: the peer reassembles and unpacks exactly what was sent",
          "[net]") {
    const std::vector<EntityState> world = make_world();
    const std::vector<f32> prio = make_priorities();

    SnapshotScheduler sched;
    sched.configure(world.size());
    BandwidthBudget budget;
    budget.configure(6000.0f, 200);
    budget.refill(0.01f);  // 3 records

    std::vector<std::vector<u8>> frags;
    SnapshotScheduler::SendPlan plan;
    sched.tick(world, prio, budget, kRes, 7u, /*mtu=*/32u, frags, plan);
    REQUIRE(plan.fragment_count >= 2u);  // 64-byte payload over a 32-byte MTU

    std::vector<EntityState> received;
    REQUIRE(receive(frags, kRes, received));
    REQUIRE(received.size() == plan.sent_indices.size());

    // Each received record matches the scheduled entity (id exact, position
    // within half a quantization step).
    for (usize k = 0; k < received.size(); ++k) {
        const EntityState& src = world[plan.sent_indices[k]];
        CHECK(received[k].id == src.id);
        CHECK(received[k].pos[0] == Catch::Approx(src.pos[0]).margin(kRes));
        CHECK(received[k].pos[1] == Catch::Approx(src.pos[1]).margin(kRes));
        CHECK(received[k].pos[2] == Catch::Approx(src.pos[2]).margin(kRes));
    }
}

TEST_CASE("scheduler: out-of-order fragments still reassemble", "[net]") {
    const std::vector<EntityState> world = make_world();
    const std::vector<f32> prio = make_priorities();
    SnapshotScheduler sched;
    sched.configure(world.size());
    BandwidthBudget budget;
    budget.configure(6000.0f, 200);
    budget.refill(0.01f);

    std::vector<std::vector<u8>> frags;
    SnapshotScheduler::SendPlan plan;
    sched.tick(world, prio, budget, kRes, 9u, 32u, frags, plan);
    REQUIRE(frags.size() >= 2u);

    // Reverse the fragment delivery order — reassembly must not care.
    std::vector<std::vector<u8>> shuffled(frags.rbegin(), frags.rend());
    std::vector<EntityState> received;
    REQUIRE(receive(shuffled, kRes, received));
    CHECK(received.size() == plan.record_count);
    CHECK(received[0].id == world[plan.sent_indices[0]].id);
}

TEST_CASE("scheduler: a tiny budget throttles to one record per tick", "[net]") {
    const std::vector<EntityState> world = make_world();
    const std::vector<f32> prio = make_priorities();
    SnapshotScheduler sched;
    sched.configure(world.size());

    BandwidthBudget budget;
    budget.configure(/*bytes_per_s=*/2000.0f, /*burst_bytes=*/20);
    budget.refill(0.01f);  // 20 bytes => exactly 1 record

    std::vector<std::vector<u8>> frags;
    SnapshotScheduler::SendPlan plan;
    sched.tick(world, prio, budget, kRes, 1u, 64u, frags, plan);
    CHECK(plan.record_count == 1u);
    CHECK(plan.sent_indices[0] == 0u);  // the single highest priority
}

TEST_CASE("scheduler: no entity is starved over many ticks", "[net]") {
    const std::vector<EntityState> world = make_world();
    const std::vector<f32> prio = make_priorities();
    SnapshotScheduler sched;
    sched.configure(world.size());
    BandwidthBudget budget;
    budget.configure(6000.0f, 200);

    std::vector<bool> ever_sent(world.size(), false);
    std::vector<std::vector<u8>> frags;
    SnapshotScheduler::SendPlan plan;
    for (int t = 0; t < 60; ++t) {
        budget.refill(0.01f);  // 3 records/tick
        sched.tick(world, prio, budget, kRes, static_cast<u16>(t), 64u, frags,
                   plan);
        for (const u32 idx : plan.sent_indices) ever_sent[idx] = true;
    }
    // Even the lowest-priority entities climbed the queue and were sent.
    for (usize i = 0; i < ever_sent.size(); ++i) {
        CHECK(ever_sent[i]);
    }
}

TEST_CASE("scheduler: an empty budget sends nothing", "[net]") {
    const std::vector<EntityState> world = make_world();
    const std::vector<f32> prio = make_priorities();
    SnapshotScheduler sched;
    sched.configure(world.size());
    BandwidthBudget budget;  // unconfigured: zero budget

    std::vector<std::vector<u8>> frags;
    SnapshotScheduler::SendPlan plan;
    sched.tick(world, prio, budget, kRes, 1u, 64u, frags, plan);
    CHECK(plan.record_count == 0u);
    CHECK(frags.empty());
    CHECK(plan.fragment_count == 0u);
}

TEST_CASE("scheduler: the whole pipeline is deterministic", "[net][determinism]") {
    const std::vector<EntityState> world = make_world();
    const std::vector<f32> prio = make_priorities();

    auto run = [&](std::vector<u32>& trace, std::vector<u8>& last_frag) {
        SnapshotScheduler sched;
        sched.configure(world.size());
        BandwidthBudget budget;
        budget.configure(6000.0f, 200);
        std::vector<std::vector<u8>> frags;
        SnapshotScheduler::SendPlan plan;
        for (int t = 0; t < 20; ++t) {
            budget.refill(0.01f);
            sched.tick(world, prio, budget, kRes, static_cast<u16>(t), 32u, frags,
                       plan);
            for (const u32 idx : plan.sent_indices) trace.push_back(idx);
            if (t == 19 && !frags.empty()) last_frag = frags.back();
        }
    };

    std::vector<u32> a_trace, b_trace;
    std::vector<u8> a_frag, b_frag;
    run(a_trace, a_frag);
    run(b_trace, b_frag);
    CHECK(a_trace == b_trace);
    CHECK(a_frag == b_frag);
}
