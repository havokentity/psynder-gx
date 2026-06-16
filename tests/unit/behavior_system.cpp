// SPDX-License-Identifier: MIT
// Psynder-GX — run a PsyGraph-authored behavior as a SYSTEM over the live ECS
// (engine/script/behavior BehaviorIR strided execute + lower_graph_to_ir). A
// graph compiles to a Behavior IR whose streams bind to real gameplay::Health
// component columns; the program runs IN PLACE over for_each_chunk storage — no
// gather/scatter. Closes ADR-018's "drive a gameplay behavior from a graph".

#include "script/internal/VisualGraphCompiler.h"
#include "script/behavior/BehaviorIR.h"

#include "gameplay/GameplayComponents.h"

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
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

// Run a lowered program over every Health entity, binding stream "hp"->Health.hp
// and "max"->Health.max_hp in place (Health is two contiguous f32 -> stride 2).
void run_over_health(const script::detail::GraphIrResult& r, World& w) {
    const int hp_slot = slot(r, "hp");
    const int max_slot = slot(r, "max");
    constexpr usize kStride = sizeof(Health) / sizeof(f32);  // 2
    w.for_each_chunk<Health>([&](usize n, Health* h) {
        std::vector<beh::StreamColumn> cols(r.program.num_streams);
        if (hp_slot >= 0) cols[static_cast<usize>(hp_slot)] = {&h[0].hp, kStride};
        if (max_slot >= 0)
            cols[static_cast<usize>(max_slot)] = {&h[0].max_hp, kStride};
        beh::execute(r.program,
                     std::span<const beh::StreamColumn>(cols.data(), cols.size()), n);
    });
}
}  // namespace

TEST_CASE("behavior-system: a graph heals low Health entities in place",
          "[graph][script][gameplay]") {
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

    run_over_health(r, w);

    REQUIRE(w.get<Health>(ents[0])->hp == Catch::Approx(60.0f));  // 10 -> +50
    REQUIRE(w.get<Health>(ents[1])->hp == Catch::Approx(75.0f));  // 25 -> +50
    REQUIRE(w.get<Health>(ents[2])->hp == Catch::Approx(26.0f));  // > thresh, kept
    REQUIRE(w.get<Health>(ents[3])->hp == Catch::Approx(80.0f));  // kept
    REQUIRE(w.get<Health>(ents[4])->hp == Catch::Approx(40.0f));  // 5+50 capped @40
    // max_hp (the odd stride lane) is never written.
    for (Entity e : ents) REQUIRE(w.get<Health>(e)->max_hp > 0.0f);
    REQUIRE(w.get<Health>(ents[4])->max_hp == Catch::Approx(40.0f));
}

TEST_CASE("behavior-system: running the graph is idempotent once healed",
          "[graph][script][gameplay]") {
    const auto r = script::detail::lower_graph_to_ir(kHealGraph);
    World w;
    const Entity e = w.create();
    w.add(e, Health{10.0f, 100.0f});
    run_over_health(r, w);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(60.0f));
    run_over_health(r, w);  // 60 > 25 threshold -> unchanged
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(60.0f));
}

TEST_CASE("behavior-system: graph-over-ECS is deterministic across worlds",
          "[graph][script][determinism]") {
    const auto r = script::detail::lower_graph_to_ir(kHealGraph);
    const auto run = [&]() {
        World w;
        std::vector<Entity> ents;
        for (u32 i = 0; i < 200; ++i) {
            const Entity e = w.create();
            w.add(e, Health{static_cast<f32>(i % 60), 100.0f});
            ents.push_back(e);
        }
        for (int pass = 0; pass < 3; ++pass) run_over_health(r, w);
        std::vector<f32> hp;
        for (Entity e : ents) hp.push_back(w.get<Health>(e)->hp);
        return hp;
    };
    REQUIRE(run() == run());
}
