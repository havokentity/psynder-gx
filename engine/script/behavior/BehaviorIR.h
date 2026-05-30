// SPDX-License-Identifier: MIT
// Psynder-GX — Behavior IR: the executable middle of ADR-018 (PsyGraph → DOTS).
//
// VisualGraphCompiler.{h,cpp} is the FRONT-END (a node graph), and
// BehaviorSpine is a HAND-LOWERED proof of the DOTS execution model. This is the
// data-driven layer between them: a compact register-machine IR that a graph (or
// a C++ front-end) lowers to, and a deterministic interpreter that runs it
// op-major... no, entity-major over SoA component columns — exactly the shape a
// gameplay behavior takes on the lockstep tick.
//
// The IR is a flat instruction list over a fixed register file + scalar
// uniforms + named f32 component streams. Every op is +, -, *, /, min/max,
// compare, or select — pure algebra (no transcendentals, no RNG), so under the
// lane's strict-FP flags the result is bitwise identical across arm64 / x86_64 /
// MSVC. The interpreter allocates ONE register-scratch vector up front and never
// allocates per entity (a step toward the SIMD MathLogicKernel lowering that
// BehaviorSpine already demonstrates for the hand-written case).

#pragma once

#include "core/Types.h"

#include <vector>

namespace psynder::script::behavior {

enum class Op : u8 {
    LoadStream,   // reg[a] = stream[b][i]
    StoreStream,  // stream[b][i] = reg[a]
    LoadConst,    // reg[a] = imm
    LoadUniform,  // reg[a] = uniform[b]
    Move,         // reg[a] = reg[b]
    Add,          // reg[a] = reg[b] + reg[c]
    Sub,          // reg[a] = reg[b] - reg[c]
    Mul,          // reg[a] = reg[b] * reg[c]
    Div,          // reg[a] = reg[b] / reg[c]  (c==0 -> 0, deterministic guard)
    Madd,         // reg[a] = reg[b] + reg[c] * reg[d]  (fused multiply-add form)
    Min,          // reg[a] = min(reg[b], reg[c])
    Max,          // reg[a] = max(reg[b], reg[c])
    CmpLE,        // reg[a] = (reg[b] <= reg[c]) ? 1 : 0
    CmpLT,        // reg[a] = (reg[b] <  reg[c]) ? 1 : 0
    CmpGE,        // reg[a] = (reg[b] >= reg[c]) ? 1 : 0
    CmpGT,        // reg[a] = (reg[b] >  reg[c]) ? 1 : 0
    Select,       // reg[a] = (reg[b] != 0) ? reg[c] : reg[d]
};

struct Instr {
    Op  op = Op::Move;
    u16 a = 0, b = 0, c = 0, d = 0;  // register / stream / uniform indices
    f32 imm = 0.0f;                  // LoadConst literal
};

// A compiled behavior. `code` runs once per entity; `uniforms` are per-tick
// scalars (dt, gravity, thresholds…) set before execution.
struct BehaviorProgram {
    std::vector<Instr> code;
    std::vector<f32>   uniforms;
    u16                num_registers = 0;
    u16                num_streams = 0;
};

// SoA component columns the program reads/writes: `num_streams` parallel f32
// arrays of length `count`.
struct BehaviorChunk {
    std::vector<std::vector<f32>> streams;
    usize                         count = 0;

    void configure(u16 num_streams, usize n);
    std::vector<f32>&       stream(u16 s) noexcept { return streams[s]; }
    const std::vector<f32>& stream(u16 s) const noexcept { return streams[s]; }
};

// Run `prog` once per entity over `chunk`. Deterministic, strict-FP, with a
// single up-front register-scratch allocation (no per-entity heap).
void execute(const BehaviorProgram& prog, BehaviorChunk& chunk) noexcept;

// ── Ergonomic builder ───────────────────────────────────────────────────────
// Allocates registers/uniforms and appends instructions; keeps authored
// behaviors readable until the graph front-end emits IR directly.
class BehaviorBuilder {
public:
    explicit BehaviorBuilder(u16 num_streams) { prog_.num_streams = num_streams; }

    u16 reg() noexcept { return prog_.num_registers++; }
    u16 uniform(f32 v) {
        prog_.uniforms.push_back(v);
        return static_cast<u16>(prog_.uniforms.size() - 1);
    }

    void load_stream(u16 r, u16 s)  { emit(Op::LoadStream, r, s); }
    void store_stream(u16 s, u16 r) { emit(Op::StoreStream, r, s); }
    void load_const(u16 r, f32 v)   { Instr i{}; i.op = Op::LoadConst; i.a = r; i.imm = v; prog_.code.push_back(i); }
    void load_uniform(u16 r, u16 u) { emit(Op::LoadUniform, r, u); }
    void move(u16 d, u16 s)         { emit(Op::Move, d, s); }
    void add(u16 d, u16 x, u16 y)   { emit(Op::Add, d, x, y); }
    void sub(u16 d, u16 x, u16 y)   { emit(Op::Sub, d, x, y); }
    void mul(u16 d, u16 x, u16 y)   { emit(Op::Mul, d, x, y); }
    void div(u16 d, u16 x, u16 y)   { emit(Op::Div, d, x, y); }
    void madd(u16 d, u16 x, u16 y, u16 z) { emit(Op::Madd, d, x, y, z); }
    void min(u16 d, u16 x, u16 y)   { emit(Op::Min, d, x, y); }
    void max(u16 d, u16 x, u16 y)   { emit(Op::Max, d, x, y); }
    void cmp_le(u16 d, u16 x, u16 y){ emit(Op::CmpLE, d, x, y); }
    void cmp_lt(u16 d, u16 x, u16 y){ emit(Op::CmpLT, d, x, y); }
    void cmp_ge(u16 d, u16 x, u16 y){ emit(Op::CmpGE, d, x, y); }
    void cmp_gt(u16 d, u16 x, u16 y){ emit(Op::CmpGT, d, x, y); }
    void select(u16 d, u16 cond, u16 t, u16 f) { emit(Op::Select, d, cond, t, f); }

    BehaviorProgram build() const { return prog_; }

private:
    void emit(Op op, u16 a, u16 b = 0, u16 c = 0, u16 d = 0) {
        Instr i{};
        i.op = op; i.a = a; i.b = b; i.c = c; i.d = d;
        prog_.code.push_back(i);
    }
    BehaviorProgram prog_;
};

}  // namespace psynder::script::behavior
