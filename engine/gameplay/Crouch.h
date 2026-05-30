// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Crouch.h — crouch state machine.
//
// A Crouch sits ALONGSIDE the pawn's other gameplay state (additive). It eases
// the pawn's standing height smoothly down to a crouched height and back up,
// makes the pawn move slower while crouched, and — crucially — refuses to stand
// up while there is no headroom (the caller supplies a `blocked_above` flag,
// e.g. from a ceiling sweep / capsule overhead check). The easing and the speed
// multiplier are pure algebra on the authoritative lockstep tick: no RNG, no
// per-entity cross state, so the same inputs produce identical state
// (determinism is a hard pillar — see Damage.cpp / Stamina.cpp).
//
// Driving model: the input/AI layer knows what the pawn wants (crouch held) and
// whether something is overhead. Each tick it calls
// crouch_update(pawn.Crouch, want_crouch, blocked_above, dt) for that pawn, then
// reads height_m (to size the capsule / lower the camera) and
// crouch_speed_mult(...) (to scale movement speed).

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "core/Types.h"

namespace psynder::gameplay {

// Crouch state. `height_m` is the current eased pawn height in metres, always
// kept in [crouch_height_m, stand_height_m]. It moves toward the target
// (crouched -> crouch_height_m, standing -> stand_height_m) at
// `transition_rate_mps` metres/second. `crouched` is the desired/last state
// (0 = standing, 1 = crouched) latched by crouch_update; the pawn stays crouched
// while there is no headroom even if crouch is released.
PSYNDER_COMPONENT(Crouch) {
    f32 height_m;            // current eased height, in [crouch_height_m, stand_height_m]
    f32 stand_height_m;      // full standing height
    f32 crouch_height_m;     // fully crouched height
    f32 transition_rate_mps; // ease speed between the two heights (m/s)
    u32 crouched;            // desired/last state: 0 standing, 1 crouched
};
static_assert(sizeof(Crouch) == 20, "Crouch layout frozen");

// Reset to standing: height_m = stand_height_m, crouched = 0.
void crouch_init(Crouch& c) noexcept;

// Advance the crouch state by dt_s.
//   - desired = want_crouch OR (currently crouched AND blocked_above): you can
//     ask to crouch, and you cannot stand back up while something is overhead.
//   - crouched is latched to (desired ? 1 : 0).
//   - height_m eases toward (desired ? crouch_height_m : stand_height_m) by
//     transition_rate_mps*dt, clamped so it never overshoots the target, then
//     clamped to [crouch_height_m, stand_height_m].
// Non-finite or non-positive dt_s is a no-op (state is left untouched).
// Deterministic: pure algebra, no RNG.
void crouch_update(Crouch& c, bool want_crouch, bool blocked_above, f32 dt_s) noexcept;

// True iff the pawn is in the crouched state (crouched != 0).
bool is_crouched(const Crouch& c) noexcept;

// How far through the crouch the pawn is: 0 fully standing, 1 fully crouched.
//   (stand_height_m - height_m) / (stand_height_m - crouch_height_m), clamped to
// [0, 1]. Returns 0 when the two heights are equal (guarded /0).
f32 crouch_fraction(const Crouch& c) noexcept;

// Movement speed multiplier for the current crouch fraction: a linear blend from
// `stand_speed_mult` (fully standing) to `crouch_speed_mult_full` (fully
// crouched) by crouch_fraction(c). Lets movement slow smoothly as the pawn
// lowers and is fastest standing / slowest fully crouched.
f32 crouch_speed_mult(const Crouch& c, f32 stand_speed_mult,
                      f32 crouch_speed_mult_full) noexcept;

}  // namespace psynder::gameplay
