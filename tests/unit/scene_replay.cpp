// SPDX-License-Identifier: MIT
// scene_replay: the ADR-019 determinism pillar in miniature. Records an input
// timeline from a trivial deterministic sim, replays it through a *fresh* sim,
// and REQUIREs the end-state is bit-identical -- the golden-replay invariant.
// Also checks per-tick seek and nearest-keyframe seek.

#include "scene/Replay.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using psynder::f32;
using psynder::u32;
using psynder::scene::InputFrame;
using psynder::scene::KeyframeIndex;
using psynder::scene::ReplayPlayer;
using psynder::scene::ReplayRecorder;
using psynder::scene::StateKeyframe;

namespace {

// A trivially deterministic sim: an (x,z) accumulator plus a fire counter.
// No wall-clock, no RNG, no allocation -- the same input stream always yields
// the same bytes out. Mirrors how a real fixed-tick sim consumes InputFrame.
struct ToySim {
    f32 x = 0.0f;
    f32 z = 0.0f;
    u32 fired = 0;

    void step(const InputFrame& in) noexcept {
        // 1 unit = 1 metre; treat axes as metres-per-tick of intent.
        x += in.move_x;
        z += in.move_z;
        if (in.buttons & 0x1u) ++fired;  // bit0 = fire
    }
};

// Bit-identical comparison: the determinism contract is "same bytes out", so we
// compare the raw object representations, not float-with-epsilon.
bool bit_identical(const ToySim& a, const ToySim& b) noexcept {
    return std::memcmp(&a, &b, sizeof(ToySim)) == 0;
}

// Deterministic, reproducible "captured input" for tick t -- no randomness.
InputFrame make_input(u32 t) noexcept {
    const f32 mx = static_cast<f32>((t % 7u)) * 0.25f - 0.75f;
    const f32 mz = static_cast<f32>((t % 5u)) * -0.5f + 0.5f;
    const u32 btn = (t % 3u == 0u) ? 0x1u : 0x0u;  // fire every third tick
    return InputFrame{t, mx, mz, btn};
}

constexpr u32 kTicks = 240;

}  // namespace

TEST_CASE("scene_replay: recorded inputs replay to a bit-identical end-state",
          "[scene][replay][determinism]") {
    ReplayRecorder rec;
    rec.reserve(kTicks);

    // --- Live run: advance the sim and record exactly what it consumed. ---
    ToySim live;
    for (u32 t = 0; t < kTicks; ++t) {
        const InputFrame in = make_input(t);
        rec.record(in);
        live.step(in);
    }
    REQUIRE(rec.size() == kTicks);

    // --- Replay run: a fresh sim, fed the recorded stream in order. ---
    ToySim replayed;
    ReplayPlayer player(rec.frames());
    u32 stepped = 0;
    while (!player.done()) {
        replayed.step(player.next());
        ++stepped;
    }
    REQUIRE(stepped == kTicks);
    REQUIRE(player.done());

    // The golden-replay invariant: byte-for-byte identical sim state.
    REQUIRE(bit_identical(live, replayed));
}

TEST_CASE("scene_replay: player yields the exact frame for a tick",
          "[scene][replay]") {
    ReplayRecorder rec;
    rec.reserve(kTicks);
    for (u32 t = 0; t < kTicks; ++t) rec.record(make_input(t));

    ReplayPlayer player(rec.frames());

    // A representative seek: the frame for tick 137 must be the one we recorded.
    const u32 target = 137;
    const InputFrame* got = player.frame_for_tick(target);
    REQUIRE(got != nullptr);

    const InputFrame want = make_input(target);
    REQUIRE(got->tick == target);
    REQUIRE(got->move_x == want.move_x);
    REQUIRE(got->move_z == want.move_z);
    REQUIRE(got->buttons == want.buttons);

    // Out-of-range tick yields nullptr (no UB, no throw).
    REQUIRE(player.frame_for_tick(kTicks + 99) == nullptr);
}

TEST_CASE("scene_replay: keyframe index seeks to the nearest preceding snapshot",
          "[scene][replay]") {
    KeyframeIndex idx;
    idx.reserve(4);
    idx.add(StateKeyframe{0});
    idx.add(StateKeyframe{60});
    idx.add(StateKeyframe{120});
    idx.add(StateKeyframe{180});

    // Exact hit returns that keyframe.
    REQUIRE(idx.seek_keyframe(120) != nullptr);
    REQUIRE(idx.seek_keyframe(120)->tick == 120u);

    // Between cadence points -> the latest one at-or-before the tick.
    REQUIRE(idx.seek_keyframe(175)->tick == 120u);
    REQUIRE(idx.seek_keyframe(181)->tick == 180u);

    // Before the first keyframe -> nullptr.
    KeyframeIndex empty_before;
    empty_before.add(StateKeyframe{50});
    REQUIRE(empty_before.seek_keyframe(10) == nullptr);
}
