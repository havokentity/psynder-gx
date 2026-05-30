// SPDX-License-Identifier: MIT
// Psynder-GX — BehaviorSystem<Comp>: the type-safe wrapper that runs a
// PsyGraph-authored Behavior IR program as a SYSTEM over the live ECS. A heal
// graph lowers to a behavior::BehaviorProgram; the system binds its "hp"/"max"
// stream slots to gameplay::Health fields via member pointers and runs in place
// over for_each_chunk storage. Mirrors behavior_system.cpp but exercises the
// reusable, hoisted-scratch BehaviorSystem template instead of hand-wiring the
// StreamColumns per call.

#include "script/behavior/BehaviorSystem.h"
#include "script/behavior/BehaviorIR.h"
#include "script/internal/VisualGraphCompiler.h"

#include "gameplay/GameplayComponents.h"

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <string>
#include <vector>

using namespace psynder;
using psynder::gameplay::Health;
using psynder::scene::World;
namespace beh = psynder::script::behavior;

namespace {
// A graph: if hp <= 25, heal by 50 capped at max_hp; otherwise leave hp.
const char* kHealGraph = R"({"nodes":[
    {"id":"hp","op":"input","stream":"hp"},
    {"id":"mx","op":"input","stream":"max"},
    {"id":"thr","op":"const","value":25},
    {"id":"low","op":"cmple","inputs":["hp","thr"]},
    {"id":"heal","op":"const","value":50},
    {"id":"raw","op":"add","inputs":["hp","heal"]},
    {"id":"capped","op":"min","inputs":["raw","mx"]},
    {"id":"new","op":"select","inputs":["low","capped","hp"]},
    {"id":"w","op":"output","stream":"hp","input":"new"}
]})";

int slot(const script::detail::GraphIrResult& r, const std::string& name) {
    for (usize i = 0; i < r.streams.size(); ++i)
        if (r.streams[i] == name) return static_cast<int>(i);
    return -1;
}

// Build a BehaviorSystem<Health> from a lowered heal graph, binding the "hp" and
// "max" stream slots to the matching Health fields.
beh::BehaviorSystem<Health> make_heal_system(
    const script::detail::GraphIrResult& r) {
    beh::BehaviorSystem<Health> sys(r.program, r.program.num_streams);
    const int hp_slot = slot(r, "hp");
    const int max_slot = slot(r, "max");
    REQUIRE(hp_slot >= 0);
    REQUIRE(max_slot >= 0);
    sys.bind(static_cast<u16>(hp_slot), &Health::hp);
    sys.bind(static_cast<u16>(max_slot), &Health::max_hp);
    return sys;
}
}  // namespace

TEST_CASE(
    "behavior-system-typed: BehaviorSystem<Health> heals low entities in place",
    "[behavior][script]") {
    const auto r = script::detail::lower_graph_to_ir(kHealGraph);
    REQUIRE(r.ok);
    REQUIRE(r.program.num_streams == 2);

    World w;
    struct Spec { f32 hp, max; } specs[] = {
        {10.0f, 100.0f}, {25.0f, 100.0f}, {26.0f, 100.0f}, {80.0f, 100.0f},
        {5.0f, 40.0f}};
    std::vector<Entity> ents;
    for (const Spec& s : specs) {
        const Entity e = w.create();
        w.add(e, Health{s.hp, s.max});
        ents.push_back(e);
    }

    beh::BehaviorSystem<Health> sys = make_heal_system(r);
    sys.run(w);

    REQUIRE(w.get<Health>(ents[0])->hp == Catch::Approx(60.0f));  // 10 -> +50
    REQUIRE(w.get<Health>(ents[1])->hp == Catch::Approx(75.0f));  // 25 -> +50
    REQUIRE(w.get<Health>(ents[2])->hp == Catch::Approx(26.0f));  // > thr, kept
    REQUIRE(w.get<Health>(ents[3])->hp == Catch::Approx(80.0f));  // kept
    REQUIRE(w.get<Health>(ents[4])->hp == Catch::Approx(40.0f));  // 5+50 cap @40
    // max_hp (the unbound-for-write lane) is never mutated.
    for (Entity e : ents) REQUIRE(w.get<Health>(e)->max_hp > 0.0f);
    REQUIRE(w.get<Health>(ents[4])->max_hp == Catch::Approx(40.0f));
}

TEST_CASE(
    "behavior-system-typed: BehaviorSystem run is idempotent once healed and "
    "reuses hoisted scratch across ticks",
    "[behavior][script]") {
    const auto r = script::detail::lower_graph_to_ir(kHealGraph);
    REQUIRE(r.ok);

    World w;
    const Entity e = w.create();
    w.add(e, Health{10.0f, 100.0f});

    beh::BehaviorSystem<Health> sys = make_heal_system(r);
    sys.run(w);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(60.0f));
    // 60 > 25 threshold -> the second tick leaves hp untouched. Re-running the
    // same system instance exercises the reused (hoisted) column scratch.
    sys.run(w);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(60.0f));
}

TEST_CASE(
    "behavior-system-typed: BehaviorSystem over the ECS is deterministic across "
    "worlds",
    "[behavior][script][determinism]") {
    const auto r = script::detail::lower_graph_to_ir(kHealGraph);
    REQUIRE(r.ok);

    const auto run = [&]() {
        World w;
        std::vector<Entity> ents;
        for (u32 i = 0; i < 200; ++i) {
            const Entity e = w.create();
            w.add(e, Health{static_cast<f32>(i % 60), 100.0f});
            ents.push_back(e);
        }
        beh::BehaviorSystem<Health> sys = make_heal_system(r);
        for (int pass = 0; pass < 3; ++pass) sys.run(w);
        std::vector<f32> hp;
        for (Entity e : ents) hp.push_back(w.get<Health>(e)->hp);
        return hp;
    };

    REQUIRE(run() == run());
}
