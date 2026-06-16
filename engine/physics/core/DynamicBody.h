// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/DynamicBody.h
//
// Lane: engine/physics/core — DYNAMIC Jolt rigid-body lifecycle (ADR-019
// class 2: pushable bodies built from the ECS, synced back to TransformWS).
//
// This is a small, self-contained spine that mirrors CharacterSpine's Jolt
// wrapper patterns (an opaque `World`/handle pair, a fixed-tick `step_fixed`,
// `add_static_*` -> StaticBodyHandle, never-reused stable ids) but presents a
// math::Vec3-based surface tuned for the dynamic class: create a mass > 0 box /
// sphere, step the world, and read the body's solved world position back so the
// caller can write it into the ECS `scene::TransformWS` every tick. Plus a
// remove and an instantaneous impulse for shot knockback.
//
// Determinism: the underlying psynder::physics core is built with
// JPH_CROSS_PLATFORM_DETERMINISTIC and stepped at a fixed dt (1/tick_hz). Body
// creation and the step are order-stable here (bodies are kept in a vector,
// ids are monotonic and never reused), so the same input stream produces the
// same trajectory across worlds and platforms — a precondition for lockstep
// replay. Real metric units throughout: 1 world unit = 1 metre, kg, m/s,
// kg·m/s for impulses, gravity 9.81 m/s².
//
// This header intentionally sits beside CharacterSpine.h (not in the frozen
// PublicPhysicsCore.h contract): samples/tests need a focused dynamic-body
// spine for crate / prop bring-up, and the public surface stays stable.

#pragma once

#include "math/Math.h"

#include <cstdint>

namespace psynder::physics::dynamic_body {

// Forward-declared opaque world. Created with create_world(), torn down with
// destroy_world(). Wraps a psynder::physics::World plus the dynamic/static body
// bookkeeping; never constructed by value by callers.
struct World;

struct WorldDesc {
    float gravity_y_m_per_s2 = -9.81f;  // real gravity, metric.
    std::uint32_t max_bodies = 256;
    std::uint32_t tick_hz = 120;        // fixed-tick rate; dt = 1 / tick_hz.
};

// Opaque handle to a static body added via add_static_box. A default
// (zero-id) handle is invalid. The id is stable: it is never reused for the
// life of the world, so a stale handle can't alias a later body. This mirrors
// CharacterSpine::StaticBodyHandle exactly.
struct StaticBodyHandle {
    std::uint64_t id = 0;
    constexpr bool valid() const noexcept { return id != 0; }
    constexpr explicit operator bool() const noexcept { return id != 0; }
    friend constexpr bool operator==(StaticBodyHandle,
                                     StaticBodyHandle) noexcept = default;
};

// Opaque handle to a dynamic (mass > 0) body. Same never-reused-id rule as
// StaticBodyHandle: stable for the life of the world. Hand it to
// dynamic_body_position() each tick to sync the solved transform into the ECS,
// to apply_impulse() for knockback, and to remove_dynamic_body() to delete it.
struct DynamicBodyHandle {
    std::uint64_t id = 0;
    constexpr bool valid() const noexcept { return id != 0; }
    constexpr explicit operator bool() const noexcept { return id != 0; }
    friend constexpr bool operator==(DynamicBodyHandle,
                                     DynamicBodyHandle) noexcept = default;
};

// ─── World lifecycle + step entrypoint ──────────────────────────────────────
// create_world returns nullptr on allocation / backend failure (no exceptions
// in this hot lane). step_fixed advances the Jolt simulation by exactly one
// fixed dt (1 / tick_hz); call it once per fixed tick. Both are no-ops on a
// null world.
World* create_world(const WorldDesc& desc = {}) noexcept;
void   destroy_world(World* world) noexcept;
void   step_fixed(World* world) noexcept;

// ─── Static collider (so a dynamic body has a floor to rest on) ──────────────
// Adds an immovable (mass 0) box at `pos` with the given half-extents. Returns
// an invalid handle if any half-extent is non-positive. Provided so this spine
// can stand alone (a dynamic body needs something to land on) without reaching
// into CharacterSpine; it mirrors add_static_box's contract.
StaticBodyHandle add_static_box(World* world, math::Vec3 half_extents,
                                math::Vec3 pos) noexcept;

// Adds a 1 m thick static ground slab centred at the origin whose top face sits
// at `top_y`. Convenience wrapper over add_static_box.
StaticBodyHandle add_static_ground(World* world, float half_extent_x,
                                   float half_extent_z,
                                   float top_y = 0.0f) noexcept;

// ─── Dynamic rigid bodies (ADR-019 class 2) ─────────────────────────────────
// Create a gravity-driven, impulse-responsive body. `mass_kg` must be > 0
// (and extents/radius > 0) or an invalid handle is returned. `pos` is the body
// centre in world metres. The created body is active and falls under gravity on
// the next step_fixed.
DynamicBodyHandle create_dynamic_box(World* world, math::Vec3 half_extents,
                                     math::Vec3 pos, float mass_kg) noexcept;
DynamicBodyHandle create_dynamic_sphere(World* world, float radius,
                                        math::Vec3 pos, float mass_kg) noexcept;

// Removes a dynamic body previously added to `world`. Returns false if the
// handle is invalid or the body is not present (e.g. already removed).
bool remove_dynamic_body(World* world, DynamicBodyHandle handle) noexcept;

// Reads the body's current solved world position (metres) — the value a caller
// writes into the ECS TransformWS each tick. Returns the origin {0,0,0} for an
// invalid / unknown handle (a separate dynamic_body_exists() check distinguishes
// "at origin" from "gone" if needed).
math::Vec3 dynamic_body_position(const World* world,
                                 DynamicBodyHandle handle) noexcept;

// True iff `handle` names a live dynamic body in `world`. Lets a caller tell a
// genuine origin position apart from a removed/invalid handle.
bool dynamic_body_exists(const World* world,
                         DynamicBodyHandle handle) noexcept;

// Applies an instantaneous impulse (kg·m/s) to the body, consumed by the next
// step_fixed — used for shot knockback. No-op for an invalid handle.
void apply_impulse(World* world, DynamicBodyHandle handle,
                   math::Vec3 impulse) noexcept;

}  // namespace psynder::physics::dynamic_body
