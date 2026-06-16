// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/destruction/FractureSpawn.cpp
// See FractureSpawn.h for the contract.

#include "physics/destruction/FractureSpawn.h"

#include <vector>

namespace psynder::physics::destruction {

u32 spawn_fracture_shards(character_spine::World* phys,
                          scene::World& ecs,
                          const scene::TransformWS& source_xf,
                          const scene::Collider& source_col,
                          const scene::RenderMaterial& look,
                          const math::Vec3& shot_dir,
                          u64 seed,
                          std::vector<EcsDynamicBody>& out_bodies,
                          const ShardSpawnParams& params) {
    if (!phys) return 0;

    const math::Vec3 he = source_col.half_extents;
    const f32 extents[3] = {he.x * 2.0f, he.y * 2.0f, he.z * 2.0f};
    const FracturePattern pattern =
        make_grid_pattern(extents, params.source_mass_kg, params.divisions);

    std::vector<Shard> shards;
    fracture(pattern, seed, shards);

    const f32* m = source_xf.mtw.m;
    const f32 center[3] = {m[12], m[13], m[14]};
    const math::Quat rot = rotation_from_transform(source_xf);
    const f32 rot_q[4] = {rot.x, rot.y, rot.z, rot.w};

    std::vector<PlacedShard> placed;
    place_shards(shards, center, rot_q, placed);

    const math::Vec3 center_v{center[0], center[1], center[2]};
    u32 spawned = 0;
    for (const PlacedShard& ps : placed) {
        scene::DynamicPropDesc desc;
        desc.position = {ps.center_m[0], ps.center_m[1], ps.center_m[2]};
        desc.rotation = rot;
        desc.shape = scene::ShapeKind::Box;
        desc.half_extents = {ps.half_extents_m[0], ps.half_extents_m[1],
                             ps.half_extents_m[2]};
        desc.mass_kg = ps.mass_kg;
        desc.material = look;  // inherit the source body's look
        const Entity e = scene::spawn_dynamic_prop(ecs, desc);

        const EcsDynamicBody body = spawn_jolt_dynamic_for_entity(phys, ecs, e);
        if (!body.second.valid()) {
            // Out of Jolt body budget: drop the orphan ECS entity so it doesn't
            // render as a frozen shard with no solver behind it.
            ecs.destroy(e);
            continue;
        }
        out_bodies.push_back(body);

        // Impulse (kg*m/s) = mass * desired velocity change, so every shard gets
        // the same launch speed regardless of its mass: outward from the centre
        // (the burst) + a shove along the bullet + a small upward pop.
        const math::Vec3 out_dir =
            math::normalize(math::sub(desc.position, center_v));
        const f32 ix =
            (out_dir.x * params.burst_mps + shot_dir.x * params.shot_mps) * ps.mass_kg;
        const f32 iy = (out_dir.y * params.burst_mps + shot_dir.y * params.shot_mps +
                        params.pop_mps) * ps.mass_kg;
        const f32 iz =
            (out_dir.z * params.burst_mps + shot_dir.z * params.shot_mps) * ps.mass_kg;
        character_spine::dynamic_body_apply_impulse(phys, body.second, ix, iy, iz);
        ++spawned;
    }
    return spawned;
}

}  // namespace psynder::physics::destruction
