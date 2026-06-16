// SPDX-License-Identifier: MIT
// Psynder — deterministic input-stream recorder / player (ADR-019 golden replay).
//
// Lockstep determinism is the #1 pillar (ADR-019): a match is reproducible iff
// the same ordered input stream, fed into a clean sim, reproduces the same
// end-state bit-for-bit. This header freezes the recorder/player contract that
// the golden-replay gate rides on. The sim itself is owned elsewhere; this only
// owns the *input* timeline (and a keyframe index for fast seeking).
//
// DOTS contract: InputFrame / StateKeyframe are trivially-copyable POD; the
// per-tick record/playback path does NOT allocate (reserve capacity up front),
// throws nothing, and iterates in stable tick order.

#pragma once

#include "core/Types.h"

#include <cstring>
#include <span>
#include <vector>

namespace psynder::scene {

// ─── Input frame ──────────────────────────────────────────────────────────
// One tick of player intent. `buttons` is a bitset (see kReplayBtn* below).
// move_x/move_z are the world-space horizontal move direction in [-1,1] (already
// yaw-rotated; 1 unit = 1 metre after the sim scales by walk/run speed).
// yaw_deg/pitch_deg are the look angles at this tick (drive the camera + the
// hitscan ray on replay). All f32/u32 so the recorded stream is byte-identical
// to what the live sim consumed ("same bytes in, same bytes out").
struct InputFrame {
    psynder::u32 tick;
    f32          move_x;
    f32          move_z;
    f32          yaw_deg;
    f32          pitch_deg;
    psynder::u32 buttons;
};
static_assert(std::is_trivially_copyable_v<InputFrame>,
              "InputFrame must be POD for deterministic record/replay");

// Button bits for InputFrame::buttons.
inline constexpr psynder::u32 kReplayBtnJump   = 1u << 0;
inline constexpr psynder::u32 kReplayBtnFire   = 1u << 1;
inline constexpr psynder::u32 kReplayBtnRun    = 1u << 2;
inline constexpr psynder::u32 kReplayBtnCrouch = 1u << 3;

// ─── State keyframe ─────────────────────────────────────────────────────────
// A periodic full-sim snapshot so a player can seek without replaying from
// tick 0. Only the tick index is wired today; the opaque snapshot payload is a
// stub until the sim exposes a serialiser.
// TODO(ADR-019/#38): carry an opaque snapshot blob (offset/size into a side
// arena), and a restore(World&) hook, so seek() can rewind the sim, not just
// the input cursor.
struct StateKeyframe {
    psynder::u32 tick;
    // TODO(ADR-019/#38): opaque snapshot bytes (sim state at `tick`).
};
static_assert(std::is_trivially_copyable_v<StateKeyframe>,
              "StateKeyframe must be POD");

// ─── Recorder ───────────────────────────────────────────────────────────────
// Append-only timeline of InputFrames. Reserve up front (reserve()) so the
// steady-state push_back is allocation-free; frames() hands back a contiguous,
// read-only span for the player / wire serialiser.
class ReplayRecorder {
public:
    void reserve(usize ticks) { frames_.reserve(ticks); }

    // Record one tick's input. Caller guarantees monotonically increasing ticks
    // (the timeline is dense + ordered); we do not sort or dedup on the hot path.
    void record(const InputFrame& frame) { frames_.push_back(frame); }

    void clear() noexcept { frames_.clear(); }

    [[nodiscard]] usize size() const noexcept { return frames_.size(); }
    [[nodiscard]] bool  empty() const noexcept { return frames_.empty(); }

    [[nodiscard]] std::span<const InputFrame> frames() const noexcept {
        return {frames_.data(), frames_.size()};
    }

private:
    std::vector<InputFrame> frames_;
};

// ─── Player ─────────────────────────────────────────────────────────────────
// Replays a recorded timeline. Non-owning: it points at a recorder's (or any
// other) contiguous frame span, so the player never copies the stream and never
// allocates. Frames are dense and tick-ordered, so frame_for_tick is an O(1)
// index when `tick` lines up with stream position; it falls back to a linear
// scan only if the stream is sparse/offset.
class ReplayPlayer {
public:
    ReplayPlayer() = default;
    explicit ReplayPlayer(std::span<const InputFrame> frames) noexcept : frames_(frames) {}

    void rewind() noexcept { cursor_ = 0; }
    [[nodiscard]] usize cursor() const noexcept { return cursor_; }
    [[nodiscard]] usize size() const noexcept { return frames_.size(); }
    [[nodiscard]] bool  done() const noexcept { return cursor_ >= frames_.size(); }

    // Advance through the stream in order. Returns the next frame and bumps the
    // cursor; caller checks done() first. No bounds branch in release past the
    // done() contract — keep the loop `while (!p.done()) sim.step(p.next());`.
    [[nodiscard]] const InputFrame& next() noexcept { return frames_[cursor_++]; }

    // Yield the frame whose `.tick == tick`, or nullptr if absent. Used for
    // seek / out-of-order lookups; the linear path is fine for a sparse seek and
    // the dense fast-path is a single index check.
    [[nodiscard]] const InputFrame* frame_for_tick(psynder::u32 tick) const noexcept {
        // Dense fast-path: tick maps directly to stream position.
        if (tick < frames_.size() && frames_[tick].tick == tick) {
            return &frames_[tick];
        }
        for (const InputFrame& f : frames_) {
            if (f.tick == tick) return &f;
        }
        return nullptr;
    }

private:
    std::span<const InputFrame> frames_{};
    usize                       cursor_ = 0;
};

// ─── Keyframe seek index ─────────────────────────────────────────────────────
// Sparse, tick-ordered list of StateKeyframes. seek_keyframe returns the latest
// keyframe at or before `tick` (the nearest one to resume from) — the input
// cursor for the remaining ticks is then `keyframe.tick`.
class KeyframeIndex {
public:
    void reserve(usize n) { keys_.reserve(n); }

    // Caller appends in increasing tick order (a periodic snapshot cadence).
    void add(const StateKeyframe& k) { keys_.push_back(k); }

    void clear() noexcept { keys_.clear(); }

    [[nodiscard]] usize size() const noexcept { return keys_.size(); }
    [[nodiscard]] std::span<const StateKeyframe> keyframes() const noexcept {
        return {keys_.data(), keys_.size()};
    }

    // Nearest keyframe at-or-before `tick`, or nullptr if none precedes it.
    // Backward linear scan over the (small) sorted index; a binary search is the
    // obvious upgrade once cadence is fixed.
    // TODO(ADR-019/#38): binary search once keyframe cadence is locked in.
    [[nodiscard]] const StateKeyframe* seek_keyframe(psynder::u32 tick) const noexcept {
        const StateKeyframe* best = nullptr;
        for (const StateKeyframe& k : keys_) {
            if (k.tick <= tick) best = &k;
            else break;  // sorted: nothing further can be <= tick
        }
        return best;
    }

private:
    std::vector<StateKeyframe> keys_;
};

}  // namespace psynder::scene
