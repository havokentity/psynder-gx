// SPDX-License-Identifier: MIT
// Psynder-GX — Behavior IR SIMD back-end (ADR-018).
//
// BehaviorSpine is a HAND-LOWERED proof that one authored behavior runs as
// op-major SIMD passes over an SoA chunk via math::MathLogicKernel. BehaviorIR
// is the data-driven generalisation (a register-machine over scalar f32 columns)
// with a deterministic SCALAR interpreter (BehaviorIR::execute). This file is the
// SIMD back-end for that IR: it runs an arbitrary BehaviorProgram over many
// entities in fixed-width lane batches (op-major over a block of W entities at a
// time, with a scalar tail), producing results BIT-IDENTICAL to the scalar
// interpreter.
//
// Why a lane-batch model and not a direct lowering onto MathLogicKernel: the
// math kernel is vec3-centric and only knows Add / Sub / MulScalar / Madd — it
// has no Div, Min, Max, compare, or Select, which the scalar IR interpreter
// supports. The lane-batch model is the same execution shape the kernel uses
// (registers held as a block of W lanes, processed op-major over a chunk, no
// per-entity allocation) generalised to the full scalar op set. Each lane
// evaluates EXACTLY the scalar expression BehaviorIR::execute would — same
// operator, same div-by-zero guard, same compare/select semantics — so under the
// script lane's strict-FP flags the SIMD path is bitwise identical to the scalar
// path (no reassociation, no FMA contraction, no reciprocal approximation: Madd
// stays b + c*d, two rounding steps, exactly as the interpreter does it).
//
// Coverage: ALL ops the scalar interpreter implements are SIMD-lane-lowered
// (LoadStream, StoreStream, LoadConst, LoadUniform, Move, Add, Sub, Mul, Div,
// Madd, Min, Max, CmpLE/LT/GE/GT, Select). There is no scalar fall-back path for
// individual ops — the scalar tail handles the ragged remainder of the entity
// count, exercising counts that are not a multiple of the lane width.

#pragma once

#include "script/behavior/BehaviorIR.h"

#include "core/Types.h"

#include <span>

namespace psynder::script::behavior {

// SIMD batch width (lanes processed per inner block). A portable, compile-time
// width — the inner block is written so the compiler auto-vectorises it, while
// the per-lane arithmetic is the same scalar float op the interpreter performs.
inline constexpr usize kBehaviorSimdWidth = 8;

// Run `prog` over `count` entities whose streams are the given strided columns,
// mirroring the scalar BehaviorIR::execute(prog, cols, count) signature. In-place,
// deterministic, single up-front register-scratch allocation. The result is
// bit-identical to the scalar interpreter for the same program + inputs.
void execute_simd(const BehaviorProgram& prog, std::span<const StreamColumn> cols,
                  usize count) noexcept;

// Convenience overload mirroring the scalar execute(prog, chunk): runs over a
// BehaviorChunk's contiguous (stride-1) stream columns.
void execute_simd(const BehaviorProgram& prog, BehaviorChunk& chunk) noexcept;

}  // namespace psynder::script::behavior
