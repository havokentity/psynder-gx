// SPDX-License-Identifier: MIT
// Psynder — deferred structural-change command buffer implementation.

#include "scene/CommandBuffer.h"

#include <cstring>

namespace psynder::scene {

void CommandBuffer::reserve(usize commands, usize payload_bytes) {
    commands_.reserve(commands);
    payload_.reserve(payload_bytes);
    temp_to_real_.reserve(commands);
}

Entity CommandBuffer::create() {
    const Entity temp{kTempBit | temp_count_};
    ++temp_count_;
    commands_.push_back(Command{Op::Create, Entity{}, 0u, 0u, nullptr});
    return temp;
}

void CommandBuffer::destroy(Entity e) {
    commands_.push_back(Command{Op::Destroy, e, 0u, 0u, nullptr});
}

u32 CommandBuffer::push_payload(const void* src, usize size, usize align) {
    // Align the write so the bytes can be read back as a properly-aligned T at
    // playback (components may be alignas(16), e.g. TransformWS).
    const usize aligned = (payload_.size() + (align - 1u)) & ~(align - 1u);
    payload_.resize(aligned + size);
    std::memcpy(payload_.data() + aligned, src, size);
    return static_cast<u32>(aligned);
}

Entity CommandBuffer::resolve(Entity e) const noexcept {
    if ((e.raw & kTempBit) == 0u) {
        return e;  // already a real handle
    }
    const u64 index = e.raw & ~kTempBit;
    if (index >= temp_to_real_.size()) {
        return Entity{};  // temp referenced before its create() — no-op target
    }
    return temp_to_real_[static_cast<usize>(index)];
}

void CommandBuffer::playback(World& world) {
    temp_to_real_.clear();
    for (const Command& command : commands_) {
        switch (command.op) {
            case Op::Create:
                // Create-order matches the temp indices handed out by create().
                temp_to_real_.push_back(world.create());
                break;
            case Op::Destroy:
                world.destroy(resolve(command.entity));
                break;
            case Op::Add:
                command.apply(world, resolve(command.entity),
                              payload_.data() + command.payload_offset);
                break;
            case Op::Remove:
                command.apply(world, resolve(command.entity), nullptr);
                break;
        }
    }
    clear();
}

void CommandBuffer::clear() noexcept {
    commands_.clear();
    payload_.clear();
    temp_to_real_.clear();
    temp_count_ = 0u;
}

}  // namespace psynder::scene
