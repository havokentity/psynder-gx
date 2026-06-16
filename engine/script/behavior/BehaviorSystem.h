// SPDX-License-Identifier: MIT
// Psynder-GX — type-safe wrapper that runs a graph-authored Behavior IR program
// as a SYSTEM over the live ECS (engine/script/behavior BehaviorIR strided
// execute). A graph compiles to a behavior::BehaviorProgram whose stream slots
// bind to f32 fields of an ECS component via member pointers; run() walks
// for_each_chunk<Comp> and executes the program IN PLACE over the SoA storage —
// no gather/scatter, no per-tick heap.
//
// The per-call scratch (the StreamColumn vector handed to execute()) is hoisted
// to a member, sized once in the ctor and reused across chunks and ticks, so
// run() is alloc-free after warmup. Deterministic (the IR interpreter is
// strict-FP, no RNG); no std::function / shared_ptr / exceptions / RTTI.

#pragma once

#include "script/behavior/BehaviorIR.h"
#include "scene/World.h"

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::script::behavior {

// Runs `program` over every `Comp` entity. Bind IR stream slots to f32 fields of
// Comp with bind(); run() points each bound StreamColumn at the field inside the
// chunk's component storage (stride = sizeof(Comp)/sizeof(f32)) and executes the
// program in place over the chunk.
template <class Comp>
class BehaviorSystem {
public:
    // Store the program and size the hoisted scratch column vector to the
    // program's stream count. `num_streams` mirrors program.num_streams; passing
    // it explicitly keeps the binding indices the caller resolves valid.
    BehaviorSystem(BehaviorProgram prog, u16 num_streams)
        : program_(std::move(prog)), cols_(num_streams) {}

    // Map IR stream slot `stream_slot` to the f32 member `field` of Comp.
    void bind(u16 stream_slot, f32 Comp::* field) {
        bindings_.push_back(Binding{stream_slot, field});
    }

    // Execute the program over every Comp entity in `w`, in place. Alloc-free:
    // the scratch column vector is the hoisted member `cols_`, never resized
    // here.
    void run(scene::World& w) {
        // Stride in floats from one Comp to the next within a chunk's SoA-packed
        // (here AoS-per-chunk) component column.
        constexpr usize kStride = sizeof(Comp) / sizeof(f32);
        w.template for_each_chunk<Comp>([&](usize n, Comp* col) {
            for (const Binding& binding : bindings_) {
                cols_[binding.slot] =
                    StreamColumn{&(col[0].*binding.field), kStride};
            }
            execute(program_,
                    std::span<const StreamColumn>(cols_.data(), cols_.size()),
                    n);
        });
    }

    [[nodiscard]] const BehaviorProgram& program() const noexcept {
        return program_;
    }

private:
    struct Binding {
        u16          slot;
        f32 Comp::*  field;
    };

    BehaviorProgram           program_;
    std::vector<Binding>      bindings_;
    std::vector<StreamColumn> cols_;  // hoisted per-call scratch (reused)
};

}  // namespace psynder::script::behavior
