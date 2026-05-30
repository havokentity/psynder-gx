// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net per-entity send-priority accumulator tests.
//
// Validates the Glenn-Fiedler scheme in PriorityAccumulator: accumulate() grows
// each entity by its base, select() ships the top-K and resets exactly those, a
// starved entity climbs until it wins (anti-starvation), ties break to the lower
// index, acc<=0 entities are never selected, output is descending priority, and
// the whole thing is deterministic for a fixed op sequence.

#include <array>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "net/PriorityAccumulator.h"

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net: priority accumulate grows each entity by its base", "[net]") {
    PriorityAccumulator pa;
    pa.resize(3);
    REQUIRE(pa.size() == 3);

    // Freshly resized -> all zero.
    CHECK(pa.priority(0) == Catch::Approx(0.f));
    CHECK(pa.priority(1) == Catch::Approx(0.f));
    CHECK(pa.priority(2) == Catch::Approx(0.f));

    const std::array<f32, 3> base{1.f, 2.f, 5.f};
    pa.accumulate(std::span<const f32>(base.data(), base.size()));
    CHECK(pa.priority(0) == Catch::Approx(1.f));
    CHECK(pa.priority(1) == Catch::Approx(2.f));
    CHECK(pa.priority(2) == Catch::Approx(5.f));

    // A second tick adds the base again — higher base climbs faster.
    pa.accumulate(std::span<const f32>(base.data(), base.size()));
    CHECK(pa.priority(0) == Catch::Approx(2.f));
    CHECK(pa.priority(1) == Catch::Approx(4.f));
    CHECK(pa.priority(2) == Catch::Approx(10.f));
}

TEST_CASE("net: priority accumulate clamps negative base and honours short span",
          "[net]") {
    PriorityAccumulator pa;
    pa.resize(3);

    // Negative base is clamped to 0 (max(0, base)) so it cannot drain anyone.
    const std::array<f32, 3> base{-4.f, 3.f, -1.f};
    pa.accumulate(std::span<const f32>(base.data(), base.size()));
    CHECK(pa.priority(0) == Catch::Approx(0.f));
    CHECK(pa.priority(1) == Catch::Approx(3.f));
    CHECK(pa.priority(2) == Catch::Approx(0.f));

    // A span shorter than size() updates only the leading entities.
    const std::array<f32, 1> shortb{10.f};
    pa.accumulate(std::span<const f32>(shortb.data(), shortb.size()));
    CHECK(pa.priority(0) == Catch::Approx(10.f));  // updated.
    CHECK(pa.priority(1) == Catch::Approx(3.f));   // untouched.
    CHECK(pa.priority(2) == Catch::Approx(0.f));   // untouched.
}

TEST_CASE("net: priority select returns top-K and resets exactly those", "[net]") {
    PriorityAccumulator pa;
    pa.resize(4);

    // Accumulated priorities: e0=1, e1=4, e2=2, e3=3.
    const std::array<f32, 4> base{1.f, 4.f, 2.f, 3.f};
    pa.accumulate(std::span<const f32>(base.data(), base.size()));

    std::vector<u32> sent;
    pa.select(/*max_count=*/2, sent);

    // Top-2 by accumulated priority -> e1 (4), e3 (3), in descending order.
    REQUIRE(sent.size() == 2);
    CHECK(sent[0] == 1u);
    CHECK(sent[1] == 3u);

    // Exactly the selected entities were reset; the others kept their value.
    CHECK(pa.priority(1) == Catch::Approx(0.f));  // selected -> reset.
    CHECK(pa.priority(3) == Catch::Approx(0.f));  // selected -> reset.
    CHECK(pa.priority(0) == Catch::Approx(1.f));  // not selected -> kept.
    CHECK(pa.priority(2) == Catch::Approx(2.f));  // not selected -> kept.
}

TEST_CASE("net: priority select emits descending priority order", "[net]") {
    PriorityAccumulator pa;
    pa.resize(4);

    // e0=5, e1=1, e2=9, e3=3 -> selecting all sendable must come out 9,5,3,1.
    const std::array<f32, 4> base{5.f, 1.f, 9.f, 3.f};
    pa.accumulate(std::span<const f32>(base.data(), base.size()));

    std::vector<u32> sent;
    pa.select(/*max_count=*/16, sent);  // budget exceeds candidate count.
    REQUIRE(sent.size() == 4);
    CHECK(sent[0] == 2u);  // 9
    CHECK(sent[1] == 0u);  // 5
    CHECK(sent[2] == 3u);  // 3
    CHECK(sent[3] == 1u);  // 1
}

TEST_CASE("net: priority select breaks ties to the lower index", "[net]") {
    PriorityAccumulator pa;
    pa.resize(4);

    // All four share the same accumulated priority -> ties resolve ascending idx.
    const std::array<f32, 4> base{7.f, 7.f, 7.f, 7.f};
    pa.accumulate(std::span<const f32>(base.data(), base.size()));

    std::vector<u32> sent;
    pa.select(/*max_count=*/2, sent);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0] == 0u);  // lowest index wins the tie.
    CHECK(sent[1] == 1u);

    // The two losers retain their accumulated priority and keep climbing.
    CHECK(pa.priority(2) == Catch::Approx(7.f));
    CHECK(pa.priority(3) == Catch::Approx(7.f));
}

TEST_CASE("net: priority select never picks an acc<=0 entity", "[net]") {
    PriorityAccumulator pa;
    pa.resize(3);

    // e1 has a zero base — it never rises on its own, so it is never sendable.
    const std::array<f32, 3> base{4.f, 0.f, 2.f};
    pa.accumulate(std::span<const f32>(base.data(), base.size()));

    std::vector<u32> sent;
    pa.select(/*max_count=*/3, sent);  // budget could fit all three.

    // Only the two positive-priority entities are returned; the zero one is not.
    REQUIRE(sent.size() == 2);
    CHECK(sent[0] == 0u);  // 4
    CHECK(sent[1] == 2u);  // 2
    for (u32 idx : sent) CHECK(idx != 1u);
    CHECK(pa.priority(1) == Catch::Approx(0.f));  // still zero, never sent.
}

TEST_CASE("net: priority anti-starvation - every entity sent within a bound",
          "[net]") {
    constexpr usize kEntities = 5;
    PriorityAccumulator pa;
    pa.resize(kEntities);

    // One hog (e0) has a much higher base than the rest. Without anti-starvation
    // the low-base entities would never be picked under a tight budget; with the
    // accumulator they each eventually climb high enough to win.
    const std::array<f32, kEntities> base{10.f, 1.f, 1.f, 1.f, 1.f};

    std::array<bool, kEntities> ever_sent{};
    constexpr usize kBudget = 1;     // ship a single entity per tick — worst case.
    constexpr usize kMaxRounds = 64; // generous bound; should converge far sooner.

    std::vector<u32> sent;
    usize round = 0;
    for (; round < kMaxRounds; ++round) {
        pa.accumulate(std::span<const f32>(base.data(), base.size()));
        pa.select(kBudget, sent);
        for (u32 idx : sent) ever_sent[idx] = true;

        bool all = true;
        for (bool s : ever_sent) all = all && s;
        if (all) break;
    }

    // Every entity — even the starved low-base ones — got sent at least once.
    for (usize i = 0; i < kEntities; ++i) {
        CHECK(ever_sent[i]);
    }
    CHECK(round < kMaxRounds);  // converged within the bound.
}

TEST_CASE("net: priority reset zeroes all accumulators", "[net]") {
    PriorityAccumulator pa;
    pa.resize(3);
    const std::array<f32, 3> base{1.f, 2.f, 3.f};
    pa.accumulate(std::span<const f32>(base.data(), base.size()));
    pa.accumulate(std::span<const f32>(base.data(), base.size()));

    pa.reset();
    CHECK(pa.size() == 3);  // reset keeps the size.
    CHECK(pa.priority(0) == Catch::Approx(0.f));
    CHECK(pa.priority(1) == Catch::Approx(0.f));
    CHECK(pa.priority(2) == Catch::Approx(0.f));
}

TEST_CASE("net: priority out-of-range query returns zero", "[net]") {
    PriorityAccumulator pa;
    pa.resize(2);
    const std::array<f32, 2> base{3.f, 4.f};
    pa.accumulate(std::span<const f32>(base.data(), base.size()));

    CHECK(pa.priority(0) == Catch::Approx(3.f));
    CHECK(pa.priority(1) == Catch::Approx(4.f));
    CHECK(pa.priority(2) == Catch::Approx(0.f));    // == size(), out of range.
    CHECK(pa.priority(9999) == Catch::Approx(0.f)); // far out of range.
}

TEST_CASE("net: priority select on an empty / all-zero set returns nothing",
          "[net]") {
    PriorityAccumulator pa;

    std::vector<u32> sent;
    pa.select(4, sent);             // no entities at all.
    CHECK(sent.empty());

    pa.resize(3);                   // entities exist but never accumulated.
    pa.select(4, sent);
    CHECK(sent.empty());

    // A zero budget selects nothing even when entities are sendable.
    const std::array<f32, 3> base{1.f, 2.f, 3.f};
    pa.accumulate(std::span<const f32>(base.data(), base.size()));
    pa.select(/*max_count=*/0, sent);
    CHECK(sent.empty());
    CHECK(pa.priority(2) == Catch::Approx(3.f));  // untouched by a no-op select.
}

TEST_CASE("net: priority same op sequence is deterministic", "[net]") {
    const std::array<f32, 6> base{2.f, 9.f, 9.f, 1.f, 4.f, 9.f};

    auto run = [](const std::array<f32, 6>& b) {
        PriorityAccumulator pa;
        pa.resize(6);
        std::vector<u32> trace;
        std::vector<u32> sent;
        for (int tick = 0; tick < 8; ++tick) {
            pa.accumulate(std::span<const f32>(b.data(), b.size()));
            pa.select(/*max_count=*/2, sent);
            for (u32 idx : sent) trace.push_back(idx);
        }
        return trace;
    };

    const std::vector<u32> a = run(base);
    const std::vector<u32> c = run(base);
    REQUIRE(a.size() == c.size());
    CHECK(a == c);  // identical op sequence -> identical selection trace.
}
