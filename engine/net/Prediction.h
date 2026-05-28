// SPDX-License-Identifier: MIT
// Psynder-GX — client-side prediction + server reconciliation. Lane 18 (net).
// Issue #41, ADR-020.
//
// In a server-authoritative model the client predicts its own movement by
// applying local inputs immediately (no round-trip latency), then reconciles:
// when the server acks tick T, the client snaps its state to the server's
// authoritative state for T and RE-SIMULATES every still-unacked input on top
// of it. If prediction matched the server, nothing visibly moves; if it
// diverged, the client converges in one step. This header is a SCAFFOLD — the
// input ring + the reconcile driver; the per-game movement step is supplied by
// the caller as a deterministic functor.
//
// DOTS contract: Input is a small POD / trivially-copyable. The ring is a
// fixed-capacity inline array — no per-frame heap allocation. reconcile() is
// allocation-free and templated on the caller's step (no virtual dispatch /
// RTTI in the hot path).
//
// Determinism: net TUs build -fno-fast-math / -ffp-contract=off
// (cmake/HotLane.cmake, DESIGN-PSYNDER-GX.md §14). Inputs are tick-stamped, not
// wall-clock; re-simulation visits them in ascending tick order so the replay
// is bit-reproducible.

#pragma once

#include "core/Types.h"

#include <array>
#include <span>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// Button bits for Input::buttons. Mirrors engine/scene/Replay.h's
// kReplayBtn* convention (same bit order: Jump=0, Fire=1, Run=2, Crouch=2..)
// so a recorded replay frame and a predicted net input agree on the bitmask
// and one can be lowered to the other without a remap table.
// ──────────────────────────────────────────────────────────────────────────
inline constexpr u32 kInputBtnJump   = 1u << 0;
inline constexpr u32 kInputBtnFire    = 1u << 1;
inline constexpr u32 kInputBtnRun     = 1u << 2;
inline constexpr u32 kInputBtnCrouch  = 1u << 3;

// ──────────────────────────────────────────────────────────────────────────
// Input — one tick of player intent. POD, trivially copyable. `move` is a
// metric velocity command (1 unit = 1 metre) the deterministic step integrates
// over the fixed tick dt. `yaw_deg`/`pitch_deg` are the look angles at this
// tick (degrees, matching engine/scene/Replay.h's InputFrame); the step drives
// the camera + hitscan ray from them. `buttons` is the kInputBtn* bitmask.
// ──────────────────────────────────────────────────────────────────────────
struct Input {
    u32 tick      = 0;
    f32 move[3]   = {0.f, 0.f, 0.f};
    f32 yaw_deg   = 0.f;
    f32 pitch_deg = 0.f;
    u32 buttons   = 0;
};

static_assert(std::is_trivially_copyable_v<Input>,
              "Input must be trivially copyable for the DOTS contract");

// ──────────────────────────────────────────────────────────────────────────
// InputRing — fixed-capacity ring of the most recent inputs the client has
// applied locally but not yet seen acked. Sized for one RTT worth of ticks at
// 128 Hz with headroom. Overwrites the oldest entry when full (the server
// should have acked it by then; if not, the prediction window is exceeded).
// ──────────────────────────────────────────────────────────────────────────
template <usize Capacity = 256>
class InputRing {
public:
    static_assert(Capacity > 0, "ring capacity must be positive");

    void push(const Input& in) noexcept {
        buf_[head_] = in;
        head_ = (head_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
    }

    void clear() noexcept { head_ = 0; count_ = 0; }

    // Retire every input with tick <= `acked_tick` (the server has confirmed
    // them; they no longer need replaying). The surviving inputs are kept in
    // ascending-tick order. Allocation-free: compacts in place via the inline
    // buffer. Returns how many entries were dropped.
    usize drop_acked(u32 acked_tick) noexcept {
        const usize oldest = (count_ < Capacity) ? 0 : head_;
        std::array<Input, Capacity> keep{};
        usize kept = 0;
        for (usize i = 0; i < count_; ++i) {
            const Input& in = buf_[(oldest + i) % Capacity];
            if (in.tick > acked_tick) keep[kept++] = in;
        }
        const usize dropped = count_ - kept;
        for (usize i = 0; i < kept; ++i) buf_[i] = keep[i];
        head_  = kept % Capacity;
        count_ = kept;
        return dropped;
    }

    usize size() const noexcept { return count_; }
    bool  empty() const noexcept { return count_ == 0; }
    usize capacity() const noexcept { return Capacity; }

    // Copy the inputs with tick > `last_acked_tick` into `out` (caller-owned,
    // pre-sized) in ascending insertion order — i.e. the still-pending inputs
    // that reconcile() must replay. Returns how many were written. Writes at
    // most out.size() entries; a full ring of pending inputs needs out.size()
    // >= size().
    usize pending_after(u32 last_acked_tick, std::span<Input> out) const noexcept {
        usize written = 0;
        const usize oldest = (count_ < Capacity) ? 0 : head_;
        for (usize i = 0; i < count_ && written < out.size(); ++i) {
            const Input& in = buf_[(oldest + i) % Capacity];
            if (in.tick > last_acked_tick) out[written++] = in;
        }
        return written;
    }

private:
    std::array<Input, Capacity> buf_{};
    usize head_  = 0;  // next write slot
    usize count_ = 0;  // live entries
};

// ──────────────────────────────────────────────────────────────────────────
// reconcile — replay `pending` inputs on top of an acked state.
//
// `resim` is the caller's deterministic per-input step. It is invoked once per
// pending input WHOSE tick > last_acked_tick, in the order they appear in
// `pending` (callers pass ascending-tick spans, e.g. from pending_after).
// `pending` need not be pre-filtered: reconcile skips entries already covered
// by the ack, so passing the whole ring snapshot is fine.
//
// Signature: void resim(const Input& in) — typically captures the predicted
// state by reference and integrates one tick. reconcile performs no allocation
// and no float math itself (the step owns the integration). Returns the number
// of inputs replayed.
// ──────────────────────────────────────────────────────────────────────────
template <class Fn>
inline usize reconcile(u32 last_acked_tick, std::span<const Input> pending,
                       Fn&& resim) {
    usize replayed = 0;
    for (const Input& in : pending) {
        if (in.tick <= last_acked_tick) continue;  // already authoritative
        resim(in);
        ++replayed;
    }
    return replayed;
}

// ──────────────────────────────────────────────────────────────────────────
// predict_present — the end-to-end client-prediction + reconciliation driver.
//
// Given the SERVER's authoritative state for the acked tick (`acked_tick`,
// captured into `state` by the caller before calling) and the client's local
// `ring` of inputs, this snaps to the server state and RE-SIMULATES every
// still-unacked input (tick > acked_tick) on top of it, producing the predicted
// present-tick state. After the call the client's `state` is the corrected
// prediction; if the local prediction matched the server nothing visibly moves.
//
//   - `state`     : caller's mutable prediction state, ALREADY set to the
//                   server's authoritative value at `acked_tick`. `apply` reads
//                   and writes it in place; predict_present never touches it
//                   directly (it owns no float math — determinism lives in the
//                   caller's step).
//   - `apply`     : deterministic per-input step, void apply(State&, const Input&).
//                   Invoked once per unacked input in ascending-tick order.
//   - `scratch`   : caller-owned, pre-sized span used to gather the pending
//                   inputs (>= ring.size() to hold a full window). No heap
//                   allocation occurs here — the ring + scratch are inline /
//                   caller-reserved, satisfying the no-per-frame-alloc rule.
//
// Returns the number of inputs replayed. Inputs with tick <= acked_tick are now
// authoritative; the caller typically calls ring.drop_acked(acked_tick) after
// (or rebuilds the ring) to retire them. predict_present itself does not mutate
// the ring so it can be called read-only / re-run.
//
// Determinism: re-simulation visits inputs in ascending tick order (ring
// insertion order is monotonic by tick), the step is the caller's, and net TUs
// build -fno-fast-math / -ffp-contract=off — so the replay is bit-reproducible
// and independent of how many ticks were already acked.
// ──────────────────────────────────────────────────────────────────────────
template <usize Capacity, class State, class Fn>
inline usize predict_present(const InputRing<Capacity>& ring, u32 acked_tick,
                             State& state, std::span<Input> scratch, Fn&& apply) {
    const usize pend = ring.pending_after(acked_tick, scratch);
    return reconcile(acked_tick, std::span<const Input>(scratch.data(), pend),
                     [&](const Input& in) { apply(state, in); });
}

}  // namespace psynder::net
