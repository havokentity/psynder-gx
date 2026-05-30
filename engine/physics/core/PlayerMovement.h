// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/PlayerMovement.h — the deterministic Quake3-style player
// movement kernel (the "tight arena feel": ground friction + acceleration, air
// acceleration / air-strafe, jump, crouch). A faithful metric port of id
// Software's bg_pmove.c (PM_Friction + PM_Accelerate — Quake3 uses one
// accelerate routine for both ground and air, only the accel constant differs;
// the air-strafe speed gain falls out of the addspeed = wishspeed - dot(v,dir)
// mechanic). Source: id-Software/Quake-III-Arena code/game/bg_pmove.c.
//
// PURE + deterministic (strict FP via the lane flags, no RNG, no allocation, no
// Jolt dependency): it computes the player's new velocity from the current
// velocity + this tick's wish command, so it is unit-testable headlessly and
// drives the Jolt CharacterVirtual each tick (the solver still owns collision /
// ground contact; this owns the FEEL). Metric: 1 unit = 1 metre, real gravity.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

namespace psynder::physics {

// Movement constants. Defaults are the Quake3 ratios ported to metric (1u=1m):
// the dimensionless accel/friction rates carry over directly; speeds are scaled
// to a ~7 m/s run (Quake3's 320 u/s). Tune per weapon/class/gamemode later.
struct MoveTuning {
    f32 max_speed_mps = 7.0f;       // ground run-speed cap (Quake3 320 u/s)
    f32 ground_accel = 10.0f;       // PM_Accelerate accel on ground (per s)
    f32 air_accel = 1.0f;           // PM_Accelerate accel in air (per s) — the
                                    // small value is what enables air-strafe.
    f32 friction = 6.0f;            // PM_Friction coefficient (per s)
    f32 stop_speed_mps = 2.0f;      // friction "control" floor (Quake3 100 u/s)
    f32 stop_epsilon_mps = 0.1f;    // below this, friction snaps to a dead stop
    f32 jump_speed_mps = 4.5f;      // launch vertical velocity (≈1 m hop @ real g)
    f32 gravity_mps2 = 9.81f;       // real gravity (metric pillar)
    f32 duck_speed_scale = 0.25f;   // crouch speed multiplier (Quake3 pm_duckScale)
};

// Mutable per-player movement state. `velocity` is world-space m/s (XZ = ground
// plane, Y = up). `grounded` is the solver's previous-tick ground result fed
// back in; pm_move clears it on a jump.
struct MoveState {
    math::Vec3 velocity{0.0f, 0.0f, 0.0f};
    bool       grounded = false;
};

// One tick of player intent. `wish_dir` is the desired heading in world XZ (need
// not be unit; Y ignored). `wish_speed_frac` scales the target speed (analog
// stick / partial input), clamped to [0,1].
struct MoveCmd {
    math::Vec3 wish_dir{0.0f, 0.0f, 0.0f};
    f32        wish_speed_frac = 1.0f;
    bool       jump = false;
    bool       crouch = false;
};

// Quake3 PM_Friction on the horizontal velocity (XZ). Y is left untouched.
void pm_friction(math::Vec3& vel, const MoveTuning& t, f32 dt) noexcept;

// Quake3 PM_Accelerate: accelerate `vel` (XZ) toward `wish_dir` (unit, XZ) up to
// `wish_speed`, adding at most `accel * dt * wish_speed` this tick. The
// addspeed-vs-dot mechanic is what both caps ground speed AND lets a player gain
// speed in air by steering off-axis (air-strafe / bunnyhop).
void pm_accelerate(math::Vec3& vel, math::Vec3 wish_dir, f32 wish_speed,
                   f32 accel, f32 dt) noexcept;

// Full per-tick player move: friction (when grounded, unless jumping), jump
// (sets vertical velocity + clears grounded, skipping friction so a hop
// preserves horizontal speed — the bunnyhop invariant), accelerate with the
// ground or air constant, and gravity while airborne. Deterministic.
void pm_move(MoveState& s, const MoveCmd& cmd, const MoveTuning& t,
             f32 dt) noexcept;

}  // namespace psynder::physics
