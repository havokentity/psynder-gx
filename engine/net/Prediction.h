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
// Input — one tick of player intent. POD, trivially copyable. `move` is a
// metric velocity command (1 unit = 1 metre) the deterministic step integrates
// over the fixed tick dt. TODO(ADR-020/#41): widen to the real input schema
// (look angles, button bitmask, fire) as gameplay lands in Wave B.
// ──────────────────────────────────────────────────────────────────────────
struct Input {
    u32 tick    = 0;
    f32 move[3] = {0.f, 0.f, 0.f};
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

}  // namespace psynder::net
