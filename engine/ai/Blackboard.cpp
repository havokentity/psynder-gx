// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Blackboard.cpp — see Blackboard.h.

#include "ai/Blackboard.h"

namespace psynder::ai {

Blackboard::Blackboard() noexcept {
    // Reserve once so the store never reallocates while ticking a behavior tree;
    // it starts empty (no live keys), filled lazily by set_*.
    entries_.reserve(kBlackboardCapacity);
}

void Blackboard::clear() noexcept {
    // Drop every key but keep the reserved capacity (no realloc on the next set).
    entries_.clear();
}

Blackboard::Entry* Blackboard::find(u32 key) noexcept {
    for (Entry& e : entries_) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

const Blackboard::Entry* Blackboard::find(u32 key) const noexcept {
    for (const Entry& e : entries_) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

Blackboard::Entry* Blackboard::slot_for(u32 key) noexcept {
    // Reuse the existing slot for this key in place (a re-set, possibly retagging
    // to a new type), so no slot is ever duplicated and the vector never grows
    // for a key already present.
    if (Entry* existing = find(key)) return existing;

    // New key: append only if there is room. A full store silently drops the
    // set (deterministic: the caller asked for a 33rd live key in a 32-slot
    // store). reserve() in the ctor guarantees this push_back never reallocates.
    if (entries_.size() >= kBlackboardCapacity) return nullptr;

    Entry fresh{};
    fresh.key  = key;
    fresh.type = Type::None;  // tagged by the caller right after.
    entries_.push_back(fresh);
    return &entries_.back();
}

void Blackboard::set_float(u32 key, f32 v) noexcept {
    Entry* e = slot_for(key);
    if (e == nullptr) return;  // full + new key: dropped.
    e->type      = Type::Float;
    e->payload.f = v;
}

bool Blackboard::get_float(u32 key, f32& out) const noexcept {
    const Entry* e = find(key);
    if (e == nullptr || e->type != Type::Float) return false;  // missing/wrong type.
    out = e->payload.f;
    return true;
}

void Blackboard::set_int(u32 key, i32 v) noexcept {
    Entry* e = slot_for(key);
    if (e == nullptr) return;
    e->type      = Type::Int;
    e->payload.i = v;
}

bool Blackboard::get_int(u32 key, i32& out) const noexcept {
    const Entry* e = find(key);
    if (e == nullptr || e->type != Type::Int) return false;
    out = e->payload.i;
    return true;
}

void Blackboard::set_vec3(u32 key, math::Vec3 v) noexcept {
    Entry* e = slot_for(key);
    if (e == nullptr) return;
    e->type       = Type::Vec3;
    e->payload.v3 = v;
}

bool Blackboard::get_vec3(u32 key, math::Vec3& out) const noexcept {
    const Entry* e = find(key);
    if (e == nullptr || e->type != Type::Vec3) return false;
    out = e->payload.v3;
    return true;
}

void Blackboard::set_bool(u32 key, bool v) noexcept {
    Entry* e = slot_for(key);
    if (e == nullptr) return;
    e->type      = Type::Bool;
    e->payload.b = v;
}

bool Blackboard::get_bool(u32 key, bool& out) const noexcept {
    const Entry* e = find(key);
    if (e == nullptr || e->type != Type::Bool) return false;
    out = e->payload.b;
    return true;
}

bool Blackboard::has(u32 key) const noexcept {
    return find(key) != nullptr;
}

void Blackboard::remove(u32 key) noexcept {
    for (usize i = 0; i < entries_.size(); ++i) {
        if (entries_[i].key == key) {
            // Erase preserving the order of surviving keys (a stable, easy to
            // reason about layout for the determinism trace).
            entries_.erase(entries_.begin() + static_cast<isize>(i));
            return;
        }
    }
}

}  // namespace psynder::ai
