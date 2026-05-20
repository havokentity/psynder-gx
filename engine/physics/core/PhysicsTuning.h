// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/PhysicsTuning.h
//
// Lane 15 — physics-core INTERNAL tuning API (Wave B, M2/M3).
//
// PublicPhysicsCore.h is the FROZEN cross-lane contract and intentionally
// carries no solver / narrowphase knobs — gameplay code should never need
// them. This header is the lane-internal surface that lets the engine
// (and the lane's own tests) tune the Jolt-backed simulation without
// reaching into Jolt headers directly:
//
//   (b) Narrowphase (GJK/EPA) + collision-detection tolerances and the
//       island / constraint solver iteration counts, mapped onto Jolt's
//       PhysicsSettings.
//   (a) The kinematic capsule character controller's ground-snap, step
//       offset, slope limit and lean knobs, plus read-only ground / stance
//       queries.
//
// It is NOT a cross-lane contract: only physics-core TUs and this lane's
// tests include it. Like PublicPhysicsCore.h it never exposes a Jolt type,
// so consumers stay independent of the upstream tree.
//
// All values are real metric units (1 unit = 1 metre, kg, N, m/s, degrees).
//
// See DESIGN-PSYNDER-GX.md §10.1.

#pragma once

#include "physics/core/PublicPhysicsCore.h"

#include <cstdint>

namespace psynder::physics {

// ─── (b) Narrowphase (GJK/EPA) + collision-detection tuning ──────────────
//
// Maps onto JPH::PhysicsSettings. Defaults mirror Jolt v5.5.0's defaults so
// reading the config straight after create_world() round-trips exactly.
struct NarrowphaseConfig {
    // Radius within which speculative contacts are generated (GJK closest-
    // point query). Too large produces ghost contacts. (metres)
    float speculative_contact_distance_m = 0.02f;
    // How far bodies may sink into one another before the solver pushes
    // them apart. (metres)
    float penetration_slop_m = 0.02f;
    // Max distance for two faces to be treated as coplanar when building a
    // contact manifold. (metres)
    float manifold_tolerance_m = 1.0e-3f;
    // Max penetration corrected in a single position-solve iteration.
    // (metres)
    float max_penetration_distance_m = 0.2f;
    // LinearCast (CCD) gating: fraction of inner radius a body must move per
    // step before casting kicks in, and the penetration it may then accept.
    float linear_cast_threshold = 0.75f;
    float linear_cast_max_penetration = 0.25f;
    // Collapse near-parallel manifolds into one. Disable to debug contact
    // generation.
    bool use_manifold_reduction = true;
    // Reuse last frame's narrowphase result when a body pair barely moved.
    bool use_body_pair_cache = true;
    // Collide against non-active (shared) mesh edges. Mostly a debug toggle.
    bool check_active_edges = true;
};

NarrowphaseConfig narrowphase_config(const World* world);
void              set_narrowphase_config(World* world, const NarrowphaseConfig& cfg);

// ─── (b) Island / constraint-solver tuning ───────────────────────────────
//
// Maps onto JPH::PhysicsSettings. The island splitter parallelises large
// islands of touching bodies across worker threads.
struct IslandSolverConfig {
    // Velocity iterations must be >= 2 for friction to work.
    std::uint32_t velocity_steps = 10;
    std::uint32_t position_steps = 2;
    // Fraction of positional error fixed per step (0 = none, 1 = all).
    float baumgarte = 0.2f;
    // Relative contact-normal speed below which a collision is inelastic.
    // (m/s)
    float min_velocity_for_restitution_mps = 1.0f;
    // Sleep thresholds.
    float time_before_sleep_s = 0.5f;
    float point_velocity_sleep_threshold_mps = 0.03f;
    // Split large islands into parallel batches.
    bool use_large_island_splitter = true;
    // Warm-start constraints with the previous frame's impulses.
    bool constraint_warm_start = true;
    bool allow_sleeping = true;
    // Deterministic ordering — load-bearing for lockstep replay; leave ON
    // unless profiling a non-networked tool.
    bool deterministic = true;
};

IslandSolverConfig island_solver_config(const World* world);
void               set_island_solver_config(World* world, const IslandSolverConfig& cfg);

// ─── (a) Kinematic capsule character-controller tuning ───────────────────
//
// Defaults are real FPS-scale metrics for an ~80 kg adult. Applied at
// create_character(); override per-character via set_character_tuning().
struct CharacterTuning {
    // Steepest walkable incline. Above this the controller treats the
    // surface as a wall and slides instead of climbing. (degrees)
    float max_slope_angle_deg = 45.0f;
    // Tallest curb / stair the controller auto-steps up. (metres)
    float step_offset_m = 0.30f;
    // How far the controller projects downward each tick to stay glued to
    // the floor when descending steps / ramps (ground snap). (metres)
    float ground_snap_dist_m = 0.30f;
    // Mass used to push dynamic bodies the character stands on. (kg)
    float mass_kg = 80.0f;
    // Ceiling on the force the character can exert on other bodies. (N)
    float max_push_strength_n = 200.0f;
    // Lateral capsule peek at full lean. (metres)
    float lean_offset_m = 0.22f;
    // Rate the lean eases toward its target. (m/s)
    float lean_speed_mps = 2.5f;
};

CharacterTuning character_tuning(const CharacterController* cc);
void            set_character_tuning(CharacterController* cc, const CharacterTuning& cfg);

// ─── Read-only character queries (gameplay + tests) ──────────────────────

// Mirror of JPH::CharacterBase::EGroundState, kept Jolt-free so callers
// don't include Jolt headers.
enum class CharacterGroundState : std::uint8_t {
    OnGround,      // supported by a walkable surface
    OnSteepSlope,  // touching ground too steep to climb; should slide
    NotSupported,  // touching something but not held up by it; should fall
    InAir,         // touching nothing
};

CharacterGroundState character_ground_state(const CharacterController* cc);
CharacterStance      character_stance(const CharacterController* cc);
// Current capsule height for the active stance (shrinks when crouched /
// prone). (metres)
float                character_capsule_height_m(const CharacterController* cc);
// Eased lean, -1 (full left) .. +1 (full right).
float                character_lean(const CharacterController* cc);

} // namespace psynder::physics
