// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_killfeed.cpp — the fixed-capacity, newest-first kill feed.
// Covers: push appends a fresh event readable newest-first; a full feed evicts
// the oldest; tick ages events and drops those past max_age; newest()/clear();
// at() recency ordering; and bit-reproducible determinism over an op sequence.

#include "gameplay/Killfeed.h"
#include "gameplay/GameplayComponents.h"

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::World;

TEST_CASE("gameplay: killfeed push adds an event readable newest-first", "[gameplay]") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();

    Killfeed feed(8);
    REQUIRE(feed.count() == 0u);

    feed.push(a, b, 3u);
    REQUIRE(feed.count() == 1u);

    const KillEvent& e = feed.at(0);
    REQUIRE(e.killer == a);
    REQUIRE(e.victim == b);
    REQUIRE(e.weapon_class == 3u);
    REQUIRE(e.age_s == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: killfeed orders multiple pushes newest-first", "[gameplay]") {
    World w;
    const Entity k0 = w.create();
    const Entity k1 = w.create();
    const Entity k2 = w.create();
    const Entity v = w.create();

    Killfeed feed(8);
    feed.push(k0, v, 0u);
    feed.push(k1, v, 1u);
    feed.push(k2, v, 2u);

    REQUIRE(feed.count() == 3u);
    // Newest first: last pushed is at index 0.
    REQUIRE(feed.at(0).killer == k2);
    REQUIRE(feed.at(0).weapon_class == 2u);
    REQUIRE(feed.at(1).killer == k1);
    REQUIRE(feed.at(2).killer == k0);
}

TEST_CASE("gameplay: a full killfeed evicts the oldest", "[gameplay]") {
    World w;
    std::vector<Entity> ks;
    const Entity v = w.create();
    for (int i = 0; i < 4; ++i) ks.push_back(w.create());

    Killfeed feed(3);
    feed.push(ks[0], v, 0u);
    feed.push(ks[1], v, 1u);
    feed.push(ks[2], v, 2u);
    REQUIRE(feed.count() == 3u);

    // Overflow: ks[0] (oldest) is evicted, ks[3] becomes newest.
    feed.push(ks[3], v, 3u);
    REQUIRE(feed.count() == 3u);
    REQUIRE(feed.at(0).killer == ks[3]);
    REQUIRE(feed.at(1).killer == ks[2]);
    REQUIRE(feed.at(2).killer == ks[1]);
    // ks[0] is gone.
    for (usize i = 0; i < feed.count(); ++i) {
        REQUIRE_FALSE(feed.at(i).killer == ks[0]);
    }
}

TEST_CASE("gameplay: killfeed tick ages events and drops those past max_age",
          "[gameplay]") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();

    Killfeed feed(8);
    feed.push(a, b, 0u);  // event 0

    // Age it 1 s in 0.25 s steps; age accumulates, nothing dropped (max 5 s).
    for (int i = 0; i < 4; ++i) feed.tick(0.25f, 5.0f);
    REQUIRE(feed.count() == 1u);
    REQUIRE(feed.at(0).age_s == Catch::Approx(1.0f));

    // Push a fresher event; the old one is older.
    feed.push(a, b, 1u);  // event 1, age 0
    REQUIRE(feed.count() == 2u);
    REQUIRE(feed.at(0).weapon_class == 1u);
    REQUIRE(feed.at(0).age_s == Catch::Approx(0.0f));
    REQUIRE(feed.at(1).age_s == Catch::Approx(1.0f));

    // Advance 4.5 s: event 1 -> 4.5 s (survives), event 0 -> 5.5 s (> 5, dropped).
    feed.tick(4.5f, 5.0f);
    REQUIRE(feed.count() == 1u);
    REQUIRE(feed.at(0).weapon_class == 1u);
    REQUIRE(feed.at(0).age_s == Catch::Approx(4.5f));

    // Push it past the limit too: now empty.
    feed.tick(1.0f, 5.0f);  // event 1 -> 5.5 s
    REQUIRE(feed.count() == 0u);
}

TEST_CASE("gameplay: killfeed newest returns the latest or false when empty",
          "[gameplay]") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();

    Killfeed feed(4);
    KillEvent out{};
    REQUIRE_FALSE(feed.newest(out));  // empty

    feed.push(a, b, 7u);
    REQUIRE(feed.newest(out));
    REQUIRE(out.killer == a);
    REQUIRE(out.weapon_class == 7u);

    feed.push(a, c, 9u);
    REQUIRE(feed.newest(out));
    REQUIRE(out.victim == c);
    REQUIRE(out.weapon_class == 9u);
}

TEST_CASE("gameplay: killfeed clear empties the feed", "[gameplay]") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();

    Killfeed feed(4);
    feed.push(a, b, 0u);
    feed.push(a, b, 1u);
    REQUIRE(feed.count() == 2u);

    feed.clear();
    REQUIRE(feed.count() == 0u);
    KillEvent out{};
    REQUIRE_FALSE(feed.newest(out));

    // Still usable (capacity preserved) after clear.
    feed.push(a, b, 5u);
    REQUIRE(feed.count() == 1u);
    REQUIRE(feed.at(0).weapon_class == 5u);
}

TEST_CASE("gameplay: killfeed at returns the right recency order", "[gameplay]") {
    World w;
    std::vector<Entity> ks;
    const Entity v = w.create();
    for (int i = 0; i < 5; ++i) ks.push_back(w.create());

    Killfeed feed(5);
    for (int i = 0; i < 5; ++i) feed.push(ks[i], v, static_cast<u32>(i));

    // at(0) newest (last pushed) .. at(4) oldest (first pushed).
    REQUIRE(feed.count() == 5u);
    for (usize i = 0; i < feed.count(); ++i) {
        REQUIRE(feed.at(i).killer == ks[4 - i]);
        REQUIRE(feed.at(i).weapon_class == static_cast<u32>(4 - i));
    }
}

TEST_CASE("gameplay: killfeed clamps zero capacity to one", "[gameplay]") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();

    Killfeed feed(0);  // clamped to 1
    feed.push(a, b, 0u);
    feed.push(a, c, 1u);  // evicts the first, only the newest survives
    REQUIRE(feed.count() == 1u);
    REQUIRE(feed.at(0).victim == c);
    REQUIRE(feed.at(0).weapon_class == 1u);
}

TEST_CASE("gameplay: killfeed is deterministic across runs", "[gameplay][determinism]") {
    const auto run = []() {
        World w;
        std::vector<Entity> es;
        for (int i = 0; i < 16; ++i) es.push_back(w.create());

        Killfeed feed(6);
        std::vector<u32> trace;
        for (int step = 0; step < 64; ++step) {
            // Deterministic op pattern (no RNG).
            const Entity killer = es[static_cast<usize>((step * 3) % 16)];
            const Entity victim = es[static_cast<usize>((step * 7 + 1) % 16)];
            feed.push(killer, victim, static_cast<u32>(step % 5));
            feed.tick(0.1f, 1.5f);

            trace.push_back(static_cast<u32>(feed.count()));
            for (usize i = 0; i < feed.count(); ++i) {
                trace.push_back(feed.at(i).killer.index());
                trace.push_back(feed.at(i).victim.index());
                trace.push_back(feed.at(i).weapon_class);
            }
        }
        return trace;
    };
    const std::vector<u32> a = run();
    const std::vector<u32> b = run();
    REQUIRE(a == b);
}
