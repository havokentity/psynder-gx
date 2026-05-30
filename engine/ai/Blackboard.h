// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Blackboard.h
//
// A tiny per-agent key->value scratch store for behavior trees / FSMs. A bot's
// tree nodes stash and read small facts ("target id", "last-seen pos", "is
// suppressed", "morale") under stable integer keys while a tick walks the tree;
// the Blackboard is the shared memory those nodes write to and read from. It is
// deliberately small and flat — a handful of values per agent, not a general
// property bag — so the whole thing fits in cache and never touches the heap
// once constructed.
//
// The ai lane links only core + math, so this is a pure container over plain
// structs (no ECS / gameplay dependency); the caller owns whatever the integer
// keys MEAN (a per-game enum of well-known slot ids, typically).
//
// TYPE-TAG RULE (the one contract to remember): each key carries the TYPE it was
// last set as. A get only succeeds if the key is currently set AND was stored as
// that exact type. Store a key as a float then read it with get_int() and the
// read FAILS (returns false, leaves out untouched) — it does NOT reinterpret the
// bits. Re-setting a key as a different type retags it: set_float(k, 1.5f) then
// set_int(k, 7) leaves the key holding an int 7, and get_float(k, ...) then
// fails. has(k) is true as long as ANY value is set under k, regardless of type.
//
// Determinism (lockstep pillar): fixed capacity reserved once at construction,
// no per-call heap, no RNG, no trig. A set to an existing key reuses that key's
// slot in place (no growth, no reorder). get returns the stored value
// bit-exactly. Built -fno-fast-math.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

// The fixed maximum number of distinct keys a Blackboard holds. A behavior tree
// for one agent rarely needs more than a dozen live facts; this is sized
// generously and reserved up front so the store never reallocates in the hot
// path. A set to a brand-new key when already full is dropped (see set_*).
inline constexpr usize kBlackboardCapacity = 32;

// A tiny per-agent key->value store. Values are tagged by type; see the
// TYPE-TAG RULE in the file header. Trivially copyable; copy = snapshot.
class Blackboard {
public:
    // An empty store. Reserves kBlackboardCapacity slots once; no live keys.
    Blackboard() noexcept;

    // Forget every key. O(size). Capacity is retained (no realloc on refill).
    void clear() noexcept;

    // Store `v` under `key`, tagging the key as a float. If `key` already exists
    // (as any type) its slot is reused in place. If it is new and the store is
    // full (size() == kBlackboardCapacity) the set is dropped. No heap.
    void set_float(u32 key, f32 v) noexcept;
    // Write the float under `key` to `out` and return true ONLY if `key` is set
    // AND was last stored as a float; otherwise return false and leave `out`
    // untouched (wrong-type or missing key both fail — the type-tag rule).
    bool get_float(u32 key, f32& out) const noexcept;

    void set_int(u32 key, i32 v) noexcept;
    bool get_int(u32 key, i32& out) const noexcept;

    void set_vec3(u32 key, math::Vec3 v) noexcept;
    bool get_vec3(u32 key, math::Vec3& out) const noexcept;

    void set_bool(u32 key, bool v) noexcept;
    bool get_bool(u32 key, bool& out) const noexcept;

    // True if ANY value is currently set under `key`, regardless of its type.
    bool has(u32 key) const noexcept;

    // Forget `key` if present (no-op otherwise). O(size); preserves the relative
    // order of the surviving keys (swap-with-last would be faster but order is
    // kept for a stable, easy-to-reason-about internal layout).
    void remove(u32 key) noexcept;

    // How many distinct keys are currently set. O(1).
    usize size() const noexcept { return entries_.size(); }

    // The fixed maximum number of keys this store can hold.
    static constexpr usize capacity() noexcept { return kBlackboardCapacity; }

private:
    // The type a key currently holds. None means a free / never-used entry,
    // but entries_ only ever contains live keys, so it is used as a get filter.
    enum class Type : u32 { None = 0, Float, Int, Vec3, Bool };

    struct Entry {
        u32  key;       // caller's integer slot id
        Type type;      // which member of `payload` is live
        // A trivially-copyable tagged payload. Vec3 is the widest member, so the
        // struct stays small (a key + tag + three floats) and POD-copyable.
        struct Payload {
            f32        f;
            i32        i;
            math::Vec3 v3;
            bool       b;
        } payload;
    };

    // Find the live entry for `key`, or nullptr. Linear scan over a tiny vector.
    Entry*       find(u32 key) noexcept;
    const Entry* find(u32 key) const noexcept;

    // Locate `key`'s slot (reuse) or append a fresh one, returning it; returns
    // nullptr only when `key` is new AND the store is full. Callers then tag +
    // fill the payload. Centralises the "reuse vs append vs drop" policy.
    Entry* slot_for(u32 key) noexcept;

    std::vector<Entry> entries_;
};

}  // namespace psynder::ai
