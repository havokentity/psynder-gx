// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_threat_memory.cpp — a bot's decaying last-seen-enemy memory:
// recording a sighting, refreshing in place (no duplicate slot), ageing +
// forgetting after a timeout, oldest-first eviction when full, freshest-pick
// with the id tie-break, and bit-reproducible state.

#include "ai/ThreatMemory.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::ai;

TEST_CASE("ai: record then find returns the last-seen position", "[ai][threat]") {
    ThreatMemory mem(4);
    mem.record(7u, {3.0f, 0.0f, -2.0f});

    const ThreatEntry* e = mem.find(7u);
    REQUIRE(e != nullptr);
    REQUIRE(e->enemy_id == 7u);
    REQUIRE(e->active);
    REQUIRE(e->last_pos.x == Catch::Approx(3.0f));
    REQUIRE(e->last_pos.y == Catch::Approx(0.0f));
    REQUIRE(e->last_pos.z == Catch::Approx(-2.0f));
    REQUIRE(e->age_s == Catch::Approx(0.0f));

    // An enemy never seen is not remembered.
    REQUIRE(mem.find(99u) == nullptr);
    REQUIRE(mem.active_count() == 1u);
}

TEST_CASE("ai: re-recording an enemy refreshes pos and resets age, no duplicate",
          "[ai][threat]") {
    ThreatMemory mem(4);
    mem.record(7u, {1.0f, 0.0f, 0.0f});
    mem.tick(2.0f, 10.0f);  // age the entry to 2 s

    const ThreatEntry* before = mem.find(7u);
    REQUIRE(before != nullptr);
    REQUIRE(before->age_s == Catch::Approx(2.0f));

    // Seen again somewhere else: position updates, age snaps back to 0...
    mem.record(7u, {5.0f, 0.0f, 6.0f});
    const ThreatEntry* after = mem.find(7u);
    REQUIRE(after != nullptr);
    REQUIRE(after->age_s == Catch::Approx(0.0f));
    REQUIRE(after->last_pos.x == Catch::Approx(5.0f));
    REQUIRE(after->last_pos.z == Catch::Approx(6.0f));

    // ...and no second slot was consumed for the same enemy.
    REQUIRE(mem.active_count() == 1u);
}

TEST_CASE("ai: tick ages entries and forgets them after the timeout",
          "[ai][threat]") {
    ThreatMemory mem(4);
    mem.record(3u, {0.0f, 0.0f, 0.0f});

    // Below the timeout: still remembered, age accumulates.
    mem.tick(1.0f, 5.0f);
    mem.tick(1.5f, 5.0f);
    const ThreatEntry* mid = mem.find(3u);
    REQUIRE(mid != nullptr);
    REQUIRE(mid->age_s == Catch::Approx(2.5f));

    // Exactly at the timeout the trail is still warm (forget is strictly past it).
    mem.tick(2.5f, 5.0f);  // age now 5.0
    REQUIRE(mem.find(3u) != nullptr);

    // One more tick pushes age past the timeout: forgotten.
    mem.tick(0.1f, 5.0f);  // age now 5.1 > 5.0
    REQUIRE(mem.find(3u) == nullptr);
    REQUIRE(mem.active_count() == 0u);
}

TEST_CASE("ai: a full memory evicts the oldest entry for a new enemy",
          "[ai][threat]") {
    ThreatMemory mem(2);
    mem.record(1u, {0.0f, 0.0f, 0.0f});
    mem.tick(3.0f, 100.0f);   // enemy 1 ages to 3 s (older)
    mem.record(2u, {1.0f, 0.0f, 0.0f});
    mem.tick(1.0f, 100.0f);   // enemy 1 -> 4 s, enemy 2 -> 1 s; memory now full

    // New sighting must evict the OLDEST (enemy 1), keeping enemy 2.
    mem.record(3u, {2.0f, 0.0f, 2.0f});
    REQUIRE(mem.find(1u) == nullptr);  // evicted
    REQUIRE(mem.find(2u) != nullptr);  // kept
    const ThreatEntry* fresh = mem.find(3u);
    REQUIRE(fresh != nullptr);
    REQUIRE(fresh->last_pos.x == Catch::Approx(2.0f));
    REQUIRE(mem.active_count() == 2u);
}

TEST_CASE("ai: full-memory eviction tie-breaks to the lowest slot index",
          "[ai][threat]") {
    // Two entries recorded together share the same age; on eviction the
    // lowest-index (first-recorded) slot must lose, mirroring the lane's
    // lowest-index tie-break.
    ThreatMemory mem(2);
    mem.record(10u, {0.0f, 0.0f, 0.0f});  // slot 0
    mem.record(20u, {0.0f, 0.0f, 0.0f});  // slot 1
    mem.tick(2.0f, 100.0f);               // both age to 2 s (tie)

    mem.record(30u, {0.0f, 0.0f, 0.0f});  // evict the older-or-tied lowest slot
    REQUIRE(mem.find(10u) == nullptr);    // slot 0 lost the tie
    REQUIRE(mem.find(20u) != nullptr);
    REQUIRE(mem.find(30u) != nullptr);
}

TEST_CASE("ai: freshest returns the most-recently-seen entry with the id tie-break",
          "[ai][threat]") {
    ThreatMemory mem(4);
    mem.record(1u, {0.0f, 0.0f, 0.0f});
    mem.tick(2.0f, 100.0f);                  // enemy 1 -> 2 s
    mem.record(2u, {9.0f, 0.0f, 9.0f});      // enemy 2 just seen (0 s)

    ThreatEntry out{};
    REQUIRE(mem.freshest(out));
    REQUIRE(out.enemy_id == 2u);             // smallest age wins
    REQUIRE(out.last_pos.x == Catch::Approx(9.0f));
    REQUIRE(out.age_s == Catch::Approx(0.0f));

    // Age the existing entries first (so enemy 2 is no longer at age 0), then
    // give enemies 5 and 3 the SAME (freshest, age 0) timestamp: among the
    // tied-freshest pair, the lower id (3) must win.
    mem.tick(1.0f, 100.0f);
    mem.record(5u, {0.0f, 0.0f, 0.0f});
    mem.record(3u, {0.0f, 0.0f, 0.0f});
    REQUIRE(mem.freshest(out));
    REQUIRE(out.enemy_id == 3u);

    // Empty memory: freshest reports nothing and leaves out untouched.
    ThreatMemory empty(2);
    ThreatEntry sentinel{};
    sentinel.enemy_id = 12345u;
    REQUIRE_FALSE(empty.freshest(sentinel));
    REQUIRE(sentinel.enemy_id == 12345u);
}

TEST_CASE("ai: active_count tracks and clear empties the memory", "[ai][threat]") {
    ThreatMemory mem(4);
    REQUIRE(mem.active_count() == 0u);

    mem.record(1u, {0.0f, 0.0f, 0.0f});
    mem.record(2u, {0.0f, 0.0f, 0.0f});
    mem.record(3u, {0.0f, 0.0f, 0.0f});
    REQUIRE(mem.active_count() == 3u);

    mem.record(2u, {1.0f, 0.0f, 0.0f});  // refresh, not a new slot
    REQUIRE(mem.active_count() == 3u);

    mem.clear();
    REQUIRE(mem.active_count() == 0u);
    REQUIRE(mem.find(1u) == nullptr);
    ThreatEntry out{};
    REQUIRE_FALSE(mem.freshest(out));
}

TEST_CASE("ai: capacity-zero memory drops records gracefully", "[ai][threat]") {
    ThreatMemory mem(0);
    mem.record(1u, {1.0f, 2.0f, 3.0f});  // no slot: silently dropped
    REQUIRE(mem.active_count() == 0u);
    REQUIRE(mem.find(1u) == nullptr);
    mem.tick(1.0f, 5.0f);                // no-op, must not crash
    ThreatEntry out{};
    REQUIRE_FALSE(mem.freshest(out));
}

TEST_CASE("ai: threat memory is deterministic for the same op sequence",
          "[ai][threat][determinism]") {
    const auto run = []() {
        ThreatMemory mem(3);
        std::vector<f32> trace;
        const auto snapshot = [&]() {
            trace.push_back(static_cast<f32>(mem.active_count()));
            ThreatEntry f{};
            const bool any = mem.freshest(f);
            trace.push_back(any ? 1.0f : 0.0f);
            trace.push_back(any ? static_cast<f32>(f.enemy_id) : -1.0f);
            trace.push_back(any ? f.age_s : -1.0f);
            for (u32 id = 0; id < 6; ++id) {
                const ThreatEntry* e = mem.find(id);
                trace.push_back(e ? e->last_pos.x : -1.0f);
                trace.push_back(e ? e->last_pos.z : -1.0f);
                trace.push_back(e ? e->age_s : -1.0f);
            }
        };

        mem.record(1u, {1.0f, 0.0f, 1.0f});
        mem.record(2u, {2.0f, 0.0f, 2.0f});
        mem.tick(0.5f, 10.0f);
        snapshot();
        mem.record(3u, {3.0f, 0.0f, 3.0f});
        mem.record(4u, {4.0f, 0.0f, 4.0f});  // forces an eviction (cap 3)
        mem.tick(0.5f, 10.0f);
        snapshot();
        mem.record(2u, {7.0f, 0.0f, 7.0f});  // refresh
        mem.tick(20.0f, 10.0f);              // forget everything
        snapshot();
        return trace;
    };
    REQUIRE(run() == run());
}
