# SPDX-License-Identifier: MIT
# psynder_world_outdoor — Lane 11. Owns: heightmap CDLOD mesh + heightmap raymarcher + spline tracks + scatter.
psynder_add_lane(world_outdoor LINKS psynder_core psynder_math psynder_simd psynder_jobs psynder_scene psynder_asset)

# Determinism: this lane now hosts HeightfieldQuery — the authoritative CPU
# terrain collision (height / normal / raycast / clamp) the agents + match +
# physics use on the lockstep tick. Strict FP keeps those algebraic queries
# bitwise identical across arm64 / x86_64 / MSVC (DESIGN §14), matching the
# other sim lanes. (Generation via sin() is offline content — see
# HeightfieldQuery.h — so it is not a cross-platform-determinism path.)
psynder_determinism_fp(psynder_world_outdoor)
