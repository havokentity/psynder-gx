// SPDX-License-Identifier: MIT
// Psynder-GX — Behavior IR → DOTS proving spine (see docs/adr/ADR-018).
//
// The smallest end-to-end demonstration of the engine's execution model: a
// "projectile" behavior, authored as an entity-scalar sequence
//     vel += gravity * dt;   // semi-implicit Euler (deterministic)
//     pos += vel * dt;
//     if (pos.y <= ground)   // branch
//         destroy(self);     // deferred effect
// lowered into the three execution buckets and run op-major over an SoA chunk:
//   * arithmetic core  -> recorded once into a math::MathLogicKernel, executed
//                         as SIMD passes over the whole chunk (no per-frame alloc)
//   * branch           -> a ground-hit mask
//   * effect           -> appended to a pooled command buffer, replayed later
//
// This is deliberately hand-lowered: it proves the runtime spine (transpose,
// bulk SIMD, no-alloc, determinism) before the visual-graph / C++ front-ends
// that will emit this shape automatically.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "math/MathLogicKernel.h"

#include <vector>

namespace psynder::script::behavior {

// One archetype chunk's component columns, laid out SoA exactly as the ECS
// would store them. Position + Velocity for a batch of projectile entities.
struct ProjectileChunk {
    std::vector<f32> pos_x, pos_y, pos_z;
    std::vector<f32> vel_x, vel_y, vel_z;
    usize count = 0;

    void resize(usize n);
};

// Pooled per-frame command buffer (the effect bucket). Records deferred
// structural changes; replayed deterministically at a sync point. Reserve once
// at load; tick() never grows it in steady state.
struct CommandBuffer {
    std::vector<u32> destroy;  // entity row indices flagged for destruction

    void reserve(usize n) { destroy.reserve(n); }
    void clear() noexcept { destroy.clear(); }
};

// The compiled projectile behavior. compile() records the arithmetic core once;
// tick() runs one fixed simulation step over a chunk with zero allocation.
class ProjectileBehavior {
   public:
    void compile(f32 fixed_dt, math::Vec3 gravity, f32 ground_y);

    // Op-major SIMD core, then ground-hit mask, then deferred-destroy emit.
    // Entities at or below ground_y are appended to cmd.destroy.
    void tick(ProjectileChunk& chunk, CommandBuffer& cmd) noexcept;

    [[nodiscard]] const math::MathLogicKernel& kernel() const noexcept { return kernel_; }

   private:
    // Stream slot ids — index into the span handed to MathLogicKernel::execute.
    static constexpr u16 kStreamPos = 0;
    static constexpr u16 kStreamVel = 1;

    math::MathLogicKernel kernel_;
    f32 ground_y_ = 0.0f;
};

}  // namespace psynder::script::behavior
