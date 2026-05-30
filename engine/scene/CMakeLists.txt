# SPDX-License-Identifier: MIT
# psynder_scene — Lane 06. Owns: archetype-chunked ECS, queries, spatial indices, scene graph.
psynder_add_lane(scene LINKS psynder_core psynder_math psynder_jobs psynder_asset)

# Determinism: the broadphase TUs (Bvh / Sap / SpatialIndex / UniformGrid) do
# float coordinate hashing + bounds math on the authoritative tick — they feed
# the agent steering and AoI that lockstep replay depends on — and World.cpp
# composes transforms. Strict FP (no FMA contraction / reassociation) keeps
# these bitwise identical across arm64 / x86_64 / MSVC (DESIGN §14, ROADMAP
# item P), matching the physics/agents/ai/gameplay lanes that already opt in.
psynder_determinism_fp(psynder_scene)
