// SPDX-License-Identifier: MIT
// Psynder-GX — Behavior IR → DOTS proving spine (see docs/adr/ADR-018).

#include "script/behavior/BehaviorSpine.h"

#include <array>

namespace psynder::script::behavior {

namespace {
// Uniform slots for the recorded core program.
constexpr u16 kUniformGravity = 0;  // vec3 uniform
constexpr u16 kUniformDt = 0;       // f32 uniform
}  // namespace

void ProjectileChunk::resize(usize n) {
    count = n;
    pos_x.assign(n, 0.0f);
    pos_y.assign(n, 0.0f);
    pos_z.assign(n, 0.0f);
    vel_x.assign(n, 0.0f);
    vel_y.assign(n, 0.0f);
    vel_z.assign(n, 0.0f);
}

void ProjectileBehavior::compile(f32 fixed_dt, math::Vec3 gravity, f32 ground_y) {
    // ── Record the arithmetic core ONCE. The builder captures the entity-scalar
    //    sequence; compile() lowers it to an op-major program executed in bulk.
    math::MathLogicKernelBuilder builder;
    builder.begin_record();

    math::LogicVec3Ref pos = builder.vec3_stream(kStreamPos);
    math::LogicVec3Ref vel = builder.vec3_stream(kStreamVel);
    math::LogicVec3 g = builder.vec3_uniform(kUniformGravity, gravity);
    math::LogicScalar dt = builder.f32_uniform(kUniformDt, fixed_dt);

    // Semi-implicit (symplectic) Euler — update velocity first, then integrate
    // position with the new velocity. Deterministic and stable.
    vel = vel + g * dt;    // pass 1: vel += gravity * dt
    pos = pos + vel * dt;  // pass 2: pos += vel * dt   (reads the updated vel)

    kernel_ = builder.compile();
    kernel_.set_vec3_uniform(kUniformGravity, gravity);
    kernel_.set_f32_uniform(kUniformDt, fixed_dt);
    ground_y_ = ground_y;
}

void ProjectileBehavior::tick(ProjectileChunk& chunk, CommandBuffer& cmd) noexcept {
    if (chunk.count == 0) {
        return;
    }

    // ── Core: op-major SIMD passes over the whole chunk (no allocation; the
    //    kernel pre-sized its scratch at compile time).
    std::array<math::MutableVec3SoaView, 2> streams{};
    streams[kStreamPos] = math::MutableVec3SoaView{
        chunk.pos_x.data(), chunk.pos_y.data(), chunk.pos_z.data(), chunk.count};
    streams[kStreamVel] = math::MutableVec3SoaView{
        chunk.vel_x.data(), chunk.vel_y.data(), chunk.vel_z.data(), chunk.count};
    kernel_.execute(std::span<math::MutableVec3SoaView>(streams.data(), streams.size()));

    // ── Branch (ground-hit mask) + Effect (deferred destroy). We never mutate
    //    structure inline — flagged rows go to the pooled command buffer to be
    //    replayed deterministically (rows are appended in ascending order, so
    //    the destroy list is already sorted by entity row id).
    for (usize i = 0; i < chunk.count; ++i) {
        if (chunk.pos_y[i] <= ground_y_) {
            cmd.destroy.push_back(static_cast<u32>(i));
        }
    }
}

}  // namespace psynder::script::behavior
