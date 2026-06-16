// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Influence.h
//
// A 2D (XZ-plane) influence map — the classic RTS/FPS tactical "threat map".
// Sources (enemies, control points, objectives) stamp a contribution into nearby
// cells that falls off linearly with Euclidean distance; agents then query the
// accumulated influence at a cell and steer DOWN the gradient to flee danger or
// UP the gradient to seek control. Positive strength = danger/threat, negative
// strength = friendly control/pull. Multiple sources sum, so one field encodes
// the whole tactical picture and every agent samples it in O(1).
//
// This is the spatial-reasoning complement to FlowField/GridAStar: those answer
// "how do I get to the goal?", this answers "where is it safe / contested?".
//
// Falloff shape: a single source adds `strength * max(0, 1 - dist/radius)` to
// every cell within Euclidean `radius` of its centre — a linear cone that peaks
// at the centre (full strength) and reaches exactly 0 at distance == radius and
// beyond. Euclidean (not Chebyshev) is chosen so the stamp is radially symmetric
// and the gradient it induces points cleanly along the radius.
//
// Determinism (ai lane, lockstep pillar): pure +,-,*,/ and a single std::sqrt
// for the falloff distance; no RNG, no trig. Same sources => a bit-identical
// field and bit-identical gradients on every run/platform. add_source is pure
// accumulation so the result is independent of stamping order. Gradient queries
// scan the 8 neighbours in a fixed order and break ties toward the lowest
// neighbour index, mirroring FlowField. Built -fno-fast-math; the only float
// beyond the field math is the final normalised direction.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

class InfluenceMap {
public:
    // Lay out a width x height grid (cell (cx,cz) is row-major at cz*width+cx).
    // Zeroes the field and sizes scratch. Safe to call repeatedly to re-lay-out.
    void resize(u32 width, u32 height);

    // Zero the field in place, keeping the grid dimensions.
    void clear() noexcept;

    // Accumulate `strength * max(0, 1 - dist/radius)` into every cell within
    // Euclidean `radius` of (cx,cz). `strength` may be negative (friendly control
    // pulls the field down). An out-of-range centre or a zero radius is ignored;
    // the stamped box is clamped to the grid. Order-independent (pure sum).
    void add_source(u32 cx, u32 cz, f32 strength, u32 radius) noexcept;

    // Accumulated influence at (cx,cz); 0 when out of range.
    f32 value(u32 cx, u32 cz) const noexcept;

    // Unit XZ direction (y == 0) toward the LOWEST-influence of the 8 neighbours
    // — flee danger / move toward safety. Zero vector when (cx,cz) is out of
    // range or is a local minimum (no strictly-lower neighbour). Deterministic
    // lowest-neighbour-index tie-break.
    math::Vec3 down_gradient(u32 cx, u32 cz) const noexcept;

    // Unit XZ direction toward the HIGHEST-influence neighbour — seek danger /
    // contest control. Same out-of-range / local-extremum / tie-break rules.
    math::Vec3 up_gradient(u32 cx, u32 cz) const noexcept;

    u32 width() const noexcept { return w_; }
    u32 height() const noexcept { return h_; }

private:
    std::vector<f32> field_;
    u32              w_ = 0;
    u32              h_ = 0;
};

}  // namespace psynder::ai
