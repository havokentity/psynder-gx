// SPDX-License-Identifier: MIT
// Psynder-GX — Behavior IR SIMD back-end. See BehaviorSimd.h.

#include "script/behavior/BehaviorSimd.h"

#include <vector>

namespace psynder::script::behavior {

namespace {

// Each register is a block of `W` lanes. The register file is `num_registers`
// such blocks laid end to end: reg[r] lanes live at file[r * W .. r * W + W).
//
// A block runs op-major over `n` active lanes (n == W for full blocks, n < W for
// the scalar tail). Every per-lane expression is the SAME scalar float operation
// BehaviorIR::execute performs — including the Div-by-zero guard and the
// compare/select truthiness — so the result is bitwise identical to the scalar
// interpreter. The loops are written branch-free over lanes (a plain `for i < n`
// with a uniform body) so the compiler can vectorise the full-block case; under
// strict FP there is no reassociation or FMA contraction, so the SIMD and scalar
// numeric results match to the bit.
//
// `lane[i]` is the global entity index of lane i in this block (== base + i).
void run_block(const BehaviorProgram& prog, std::span<const StreamColumn> cols,
               f32* file, usize base, usize n) noexcept {
    const Instr* code = prog.code.data();
    const usize n_code = prog.code.size();
    const f32* uni = prog.uniforms.empty() ? nullptr : prog.uniforms.data();
    const usize n_uni = prog.uniforms.size();
    const StreamColumn* col = cols.data();
    const usize n_col = cols.size();
    constexpr usize W = kBehaviorSimdWidth;

    for (usize pc = 0; pc < n_code; ++pc) {
        const Instr& in = code[pc];
        f32* ra = file + static_cast<usize>(in.a) * W;
        const f32* rb = file + static_cast<usize>(in.b) * W;
        const f32* rc = file + static_cast<usize>(in.c) * W;
        const f32* rd = file + static_cast<usize>(in.d) * W;

        switch (in.op) {
            case Op::LoadStream: {
                if (in.b < n_col && col[in.b].base != nullptr) {
                    f32* sbase = col[in.b].base;
                    const usize stride = col[in.b].stride;
                    for (usize i = 0; i < n; ++i)
                        ra[i] = sbase[(base + i) * stride];
                } else {
                    for (usize i = 0; i < n; ++i) ra[i] = 0.0f;
                }
                break;
            }
            case Op::StoreStream: {
                if (in.b < n_col && col[in.b].base != nullptr) {
                    f32* sbase = col[in.b].base;
                    const usize stride = col[in.b].stride;
                    for (usize i = 0; i < n; ++i)
                        sbase[(base + i) * stride] = ra[i];
                }
                break;
            }
            case Op::LoadConst: {
                const f32 imm = in.imm;
                for (usize i = 0; i < n; ++i) ra[i] = imm;
                break;
            }
            case Op::LoadUniform: {
                const f32 v = (uni != nullptr && in.b < n_uni) ? uni[in.b] : 0.0f;
                for (usize i = 0; i < n; ++i) ra[i] = v;
                break;
            }
            case Op::Move:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i];
                break;
            case Op::Add:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] + rc[i];
                break;
            case Op::Sub:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] - rc[i];
                break;
            case Op::Mul:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] * rc[i];
                break;
            case Op::Div:
                // Per-lane guard mirrors the scalar interpreter exactly:
                // c == 0 -> 0, else b / c (one division, no reciprocal estimate).
                for (usize i = 0; i < n; ++i)
                    ra[i] = rc[i] != 0.0f ? rb[i] / rc[i] : 0.0f;
                break;
            case Op::Madd:
                // b + c*d as two rounding steps (mul then add) — NOT std::fma and
                // NOT a contracted FMA, matching the scalar interpreter bitwise.
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] + rc[i] * rd[i];
                break;
            case Op::Min:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] < rc[i] ? rb[i] : rc[i];
                break;
            case Op::Max:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] > rc[i] ? rb[i] : rc[i];
                break;
            case Op::CmpLE:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] <= rc[i] ? 1.0f : 0.0f;
                break;
            case Op::CmpLT:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] < rc[i] ? 1.0f : 0.0f;
                break;
            case Op::CmpGE:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] >= rc[i] ? 1.0f : 0.0f;
                break;
            case Op::CmpGT:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] > rc[i] ? 1.0f : 0.0f;
                break;
            case Op::Select:
                for (usize i = 0; i < n; ++i) ra[i] = rb[i] != 0.0f ? rc[i] : rd[i];
                break;
        }
    }
}

}  // namespace

void execute_simd(const BehaviorProgram& prog, std::span<const StreamColumn> cols,
                  usize count) noexcept {
    if (count == 0 || prog.num_registers == 0) return;

    constexpr usize W = kBehaviorSimdWidth;
    // One up-front register-scratch allocation: num_registers blocks of W lanes,
    // reused for every batch (no per-entity / per-batch heap traffic).
    std::vector<f32> file(static_cast<usize>(prog.num_registers) * W, 0.0f);

    usize base = 0;
    for (; base + W <= count; base += W)
        run_block(prog, cols, file.data(), base, W);
    if (base < count)  // scalar tail: the ragged remainder (count % W lanes)
        run_block(prog, cols, file.data(), base, count - base);
}

void execute_simd(const BehaviorProgram& prog, BehaviorChunk& chunk) noexcept {
    if (chunk.count == 0) return;
    std::vector<StreamColumn> cols(chunk.streams.size());
    for (usize s = 0; s < chunk.streams.size(); ++s) {
        cols[s].base = chunk.streams[s].empty() ? nullptr : chunk.streams[s].data();
        cols[s].stride = 1;
    }
    execute_simd(prog, std::span<const StreamColumn>(cols.data(), cols.size()),
                 chunk.count);
}

}  // namespace psynder::script::behavior
