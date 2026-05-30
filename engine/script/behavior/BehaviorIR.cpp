// SPDX-License-Identifier: MIT
// Psynder-GX — Behavior IR interpreter. See BehaviorIR.h.

#include "script/behavior/BehaviorIR.h"

namespace psynder::script::behavior {

void BehaviorChunk::configure(u16 num_streams, usize n) {
    streams.assign(num_streams, std::vector<f32>(n, 0.0f));
    count = n;
}

void execute(const BehaviorProgram& prog, BehaviorChunk& chunk) noexcept {
    if (chunk.count == 0 || prog.num_registers == 0) return;

    // One up-front register-scratch allocation, reused for every entity row.
    std::vector<f32> r(prog.num_registers, 0.0f);
    f32* reg = r.data();

    const Instr* code = prog.code.data();
    const usize n_code = prog.code.size();
    const f32* uni = prog.uniforms.empty() ? nullptr : prog.uniforms.data();
    const usize n_uni = prog.uniforms.size();

    for (usize i = 0; i < chunk.count; ++i) {
        for (usize pc = 0; pc < n_code; ++pc) {
            const Instr& in = code[pc];
            switch (in.op) {
                case Op::LoadStream:
                    reg[in.a] = chunk.streams[in.b][i];
                    break;
                case Op::StoreStream:
                    chunk.streams[in.b][i] = reg[in.a];
                    break;
                case Op::LoadConst:
                    reg[in.a] = in.imm;
                    break;
                case Op::LoadUniform:
                    reg[in.a] = (uni != nullptr && in.b < n_uni) ? uni[in.b] : 0.0f;
                    break;
                case Op::Move:
                    reg[in.a] = reg[in.b];
                    break;
                case Op::Add:
                    reg[in.a] = reg[in.b] + reg[in.c];
                    break;
                case Op::Sub:
                    reg[in.a] = reg[in.b] - reg[in.c];
                    break;
                case Op::Mul:
                    reg[in.a] = reg[in.b] * reg[in.c];
                    break;
                case Op::Div:
                    reg[in.a] = reg[in.c] != 0.0f ? reg[in.b] / reg[in.c] : 0.0f;
                    break;
                case Op::Madd:
                    reg[in.a] = reg[in.b] + reg[in.c] * reg[in.d];
                    break;
                case Op::Min:
                    reg[in.a] = reg[in.b] < reg[in.c] ? reg[in.b] : reg[in.c];
                    break;
                case Op::Max:
                    reg[in.a] = reg[in.b] > reg[in.c] ? reg[in.b] : reg[in.c];
                    break;
                case Op::CmpLE:
                    reg[in.a] = reg[in.b] <= reg[in.c] ? 1.0f : 0.0f;
                    break;
                case Op::CmpLT:
                    reg[in.a] = reg[in.b] < reg[in.c] ? 1.0f : 0.0f;
                    break;
                case Op::CmpGE:
                    reg[in.a] = reg[in.b] >= reg[in.c] ? 1.0f : 0.0f;
                    break;
                case Op::CmpGT:
                    reg[in.a] = reg[in.b] > reg[in.c] ? 1.0f : 0.0f;
                    break;
                case Op::Select:
                    reg[in.a] = reg[in.b] != 0.0f ? reg[in.c] : reg[in.d];
                    break;
            }
        }
    }
}

}  // namespace psynder::script::behavior
