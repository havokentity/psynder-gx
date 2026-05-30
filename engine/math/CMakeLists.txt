# SPDX-License-Identifier: MIT
# psynder_math — Lane 02. Owns: vec/mat/quat, fixed-point Q24.8, AABB/sphere, transforms.
psynder_add_lane(math LINKS psynder_core)

# Determinism: this lane's non-inline TUs (Math.cpp matrix/transform kernels,
# Bounds.cpp, MathLogicKernel.cpp, VectorStack.cpp) sit on the authoritative
# sim path that agents/physics/gameplay/ai all build on. Without strict FP they
# compile with fused-multiply-add contraction + reassociation, which diverges
# between arm64 (NEON) and x86_64 (SSE/AVX2 FMA) — the cross-platform lockstep
# hole (DESIGN §14, ROADMAP item P). Strict-FP closes it at the source so a
# matrix multiply is bitwise identical on every target. Centralized in
# cmake/HotLane.cmake so the MSVC /fp:strict branch is never missed.
psynder_determinism_fp(psynder_math)
