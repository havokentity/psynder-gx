// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_hitscan_falloff.cpp — distance-falloff damage on the
// hitscan path. Verifies the opt-in `falloff` parameter scales credited damage
// by hit distance, that point-blank still deals full damage, that the default
// (nullptr) path credits exactly the weapon's flat damage, and that two
// identical falloff shots stay bit-identical across worlds (determinism).

#include "gameplay/GameplayComponents.h"
#include "gameplay/RangedDamage.h"
#include "gameplay/Weapons.h"

#include "scene/GxComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

// Mirror of the gameplay_weapons.cpp harness: a ready shooter (cooldown 0) and a
// plain Health target at `pos`. Targets carry NO Armor, so the credited damage
// lands one-for-one on health — the post-hit health delta IS the applied damage.
Entity spawn_shooter(World& w, f32 damage, f32 interval, i32 ammo) {
    const Entity e = w.create();
    w.add(e, Weapon{damage, interval, 0.0f, ammo, 0.5f});
    return e;
}
Entity spawn_target(World& w, math::Vec3 pos, f32 hp) {
    const Entity e = w.create();
    w.add(e, Health{hp, hp});
    TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    return e;
}

}  // namespace

TEST_CASE("gameplay: falloff reduces long-range hitscan damage vs full damage",
          "[gameplay][weapons]") {
    // A harsh shotgun ramp (full to 6 m, floor 15% by 18 m). The target sits at
    // 15 m on +X — well into the ramp — so a falloff shot must hurt less than the
    // flat-damage (nullptr) shot of the same weapon.
    const math::Vec3 far_pos{15.0f, 0.0f, 0.0f};

    World wf;  // full damage (no falloff)
    const Entity sf = spawn_shooter(wf, 100.0f, 0.5f, 10);
    const Entity tf = spawn_target(wf, far_pos, 100.0f);
    REQUIRE(fire_hitscan(wf, sf, {0, 0, 0}, {1, 0, 0}) == tf);
    const f32 full_remaining = wf.get<Health>(tf)->hp;

    World wr;  // distance falloff
    const Entity sr = spawn_shooter(wr, 100.0f, 0.5f, 10);
    const Entity tr = spawn_target(wr, far_pos, 100.0f);
    REQUIRE(fire_hitscan(wr, sr, {0, 0, 0}, {1, 0, 0}, kNoTeam, 0.0f, 0u,
                         &kShotgunFalloff) == tr);
    const f32 falloff_remaining = wr.get<Health>(tr)->hp;

    // Falloff bit off less, so the victim keeps MORE health than the full shot.
    REQUIRE(falloff_remaining > full_remaining);
    // Sanity: the flat shot credited the whole 100 (no armor, full damage).
    REQUIRE(full_remaining == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: point-blank hitscan deals full damage even with falloff",
          "[gameplay][weapons]") {
    // Inside falloff_start_m (6 m for the shotgun ramp): at 3 m the profile is in
    // its full-damage band, so the falloff shot must match the flat damage.
    const math::Vec3 close_pos{3.0f, 0.0f, 0.0f};

    World w;
    const Entity shooter = spawn_shooter(w, 40.0f, 0.5f, 10);
    const Entity target = spawn_target(w, close_pos, 100.0f);

    REQUIRE(fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0}, kNoTeam, 0.0f, 0u,
                         &kShotgunFalloff) == target);
    // 100 - 40 == 60: the full weapon damage landed despite the falloff profile.
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(60.0f));
}

TEST_CASE("gameplay: nullptr falloff credits exactly the weapon damage",
          "[gameplay][weapons]") {
    // The default path must route the flat wp->damage untouched: with an
    // armor-free target the health delta equals the weapon damage exactly. Fire
    // far enough out that a (hypothetical) falloff WOULD have scaled it, proving
    // nullptr bypasses the distance arithmetic entirely.
    World w;
    const Entity shooter = spawn_shooter(w, 37.0f, 0.5f, 10);
    const Entity target = spawn_target(w, {25.0f, 0.0f, 0.0f}, 100.0f);

    REQUIRE(fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0}) == target);
    const f32 delta = 100.0f - w.get<Health>(target)->hp;
    REQUIRE(delta == Catch::Approx(37.0f));  // exactly the weapon's damage
}

TEST_CASE("gameplay: identical falloff shots are bit-identical across worlds",
          "[gameplay][weapons][determinism]") {
    const auto run = []() {
        World w;
        const Entity shooter = spawn_shooter(w, 80.0f, 0.5f, 10);
        const Entity target = spawn_target(w, {11.0f, 0.0f, 0.0f}, 100.0f);
        fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0}, kNoTeam, 0.0f, 0u,
                     &kRifleFalloff);
        return w.get<Health>(target)->hp;
    };
    // Bitwise equality (==, not Approx): pure algebra, no RNG/trig, must match.
    REQUIRE(run() == run());
}
