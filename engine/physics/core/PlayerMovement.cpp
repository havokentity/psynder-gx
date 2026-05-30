// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/PlayerMovement.cpp — see PlayerMovement.h. Faithful metric
// port of Quake3 bg_pmove.c PM_Friction + PM_Accelerate. Strict FP (lane flags),
// no allocation, no RNG — deterministic on the lockstep tick.

#include "physics/core/PlayerMovement.h"

#include <algorithm>
#include <cmath>

namespace psynder::physics {

void pm_friction(math::Vec3& vel, const MoveTuning& t, f32 dt) noexcept {
    // Quake3 PM_Friction operates on the horizontal speed (it zeroes Y for the
    // ground case before measuring); we keep Y and only scale X/Z.
    const f32 speed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
    if (speed < t.stop_epsilon_mps) {
        vel.x = 0.0f;
        vel.z = 0.0f;
        return;
    }
    // control = max(speed, stop_speed): below stop_speed the player decelerates
    // at a fixed rate so they come to rest crisply instead of asymptotically.
    const f32 control = speed < t.stop_speed_mps ? t.stop_speed_mps : speed;
    f32 newspeed = speed - control * t.friction * dt;
    if (newspeed < 0.0f) newspeed = 0.0f;
    newspeed /= speed;  // safe: speed >= stop_epsilon > 0
    vel.x *= newspeed;
    vel.z *= newspeed;
}

void pm_accelerate(math::Vec3& vel, math::Vec3 wish_dir, f32 wish_speed,
                   f32 accel, f32 dt) noexcept {
    // currentspeed is the velocity component already along the wish direction.
    const f32 currentspeed = vel.x * wish_dir.x + vel.z * wish_dir.z;
    const f32 addspeed = wish_speed - currentspeed;
    if (addspeed <= 0.0f) return;  // already at/above wish speed in that dir
    f32 accelspeed = accel * dt * wish_speed;
    if (accelspeed > addspeed) accelspeed = addspeed;
    vel.x += accelspeed * wish_dir.x;
    vel.z += accelspeed * wish_dir.z;
}

void pm_move(MoveState& s, const MoveCmd& cmd, const MoveTuning& t,
             f32 dt) noexcept {
    // Normalize the wish direction in the XZ plane (Y intent is ignored — jump
    // is a discrete command, not a wish-dir component).
    math::Vec3 wd{cmd.wish_dir.x, 0.0f, cmd.wish_dir.z};
    const f32 wl = std::sqrt(wd.x * wd.x + wd.z * wd.z);
    if (wl > 0.0f) {
        wd.x /= wl;
        wd.z /= wl;
    }

    const f32 frac = std::clamp(cmd.wish_speed_frac, 0.0f, 1.0f);
    const f32 speed_scale = cmd.crouch ? t.duck_speed_scale : 1.0f;
    const f32 wish_speed = t.max_speed_mps * frac * speed_scale;

    // Jump: launches before friction, and SKIPS friction this tick so a
    // well-timed hop preserves horizontal speed (the bunnyhop invariant).
    bool jumped = false;
    if (s.grounded && cmd.jump) {
        s.velocity.y = t.jump_speed_mps;
        s.grounded = false;
        jumped = true;
    }

    if (s.grounded && !jumped) {
        pm_friction(s.velocity, t, dt);
    }

    const f32 accel = s.grounded ? t.ground_accel : t.air_accel;
    pm_accelerate(s.velocity, wd, wish_speed, accel, dt);

    if (!s.grounded) {
        s.velocity.y -= t.gravity_mps2 * dt;
    }
}

}  // namespace psynder::physics
