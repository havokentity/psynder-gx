// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / lag compensation ring buffer test.
//
// Push 200 ms worth of synthetic world snapshots into the ring buffer,
// then rewind to various view times and verify the rewound state matches
// the snapshot we pushed at that tick.
//
// Until lane 17 publishes the concrete WorldState layout, we treat the
// snapshot bytes as opaque (the lag comp impl byte-copies; the
// "interpolation" is currently nearest-bracket).

#include <catch2/catch_test_macros.hpp>

#include "net/LagComp.h"
#include "net/TickConfig.h"

#include <array>
#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

// Snapshot is a small payload: 8 bytes of synthetic per-tick state.
struct FakeWorld {
    u32 tick = 0;
    u32 cookie = 0;
};

}  // namespace

TEST_CASE("net: lag-comp ring buffer stores then retrieves recent snapshots",
          "[net][lagcomp]") {
    const auto cfg = tick_config_128();
    // 200ms / floor(7.8125)ms per tick = 29 ticks (factory uses integer
    // floor of frame_ms for ceiling division, see TickConfig.h).
    CHECK(cfg.lag_comp_ticks >= 26u);
    CHECK(cfg.lag_comp_ticks <= 30u);

    LagCompContext ctx(cfg);

    // Push 30 ticks of snapshots. The ring should evict the oldest few.
    for (u32 t = 1; t <= 30; ++t) {
        FakeWorld w{t, 0xDEAD0000u | t};
        OpaqueWorldState s{};
        s.data = &w;
        s.size = sizeof(w);
        push_world_snapshot(ctx, s, t);
    }
    CHECK(ctx.newest_tick() == 30u);
    CHECK(ctx.stored_count() <= cfg.lag_comp_ticks + 1u);

    // Rewind to a tick well inside the buffer. Server current_tick = 30;
    // view time corresponds to tick 25 (≈ 39ms in the past at 128Hz).
    const f64 view_time_ms = 25.0 * cfg.frame_ms;
    FakeWorld out_world{};
    OpaqueWorldState out_state{};
    out_state.data = &out_world;
    out_state.size = sizeof(out_world);
    const RewindResult rr = rewind_world_to(ctx, out_state, view_time_ms,
                                            /*current_tick=*/30u,
                                            /*sub_tick=*/0u);
    CHECK(rr == RewindResult::Ok);
    CHECK(out_world.tick == 25u);
    CHECK(out_world.cookie == (0xDEAD0000u | 25u));
}

TEST_CASE("net: lag-comp rewind clamps requests older than 200ms",
          "[net][lagcomp][clamp]") {
    const auto cfg = tick_config_128();
    LagCompContext ctx(cfg);

    for (u32 t = 1; t <= 40; ++t) {
        FakeWorld w{t, 0xCAFE0000u | t};
        OpaqueWorldState s{};
        s.data = &w;
        s.size = sizeof(w);
        push_world_snapshot(ctx, s, t);
    }

    // Ask for a view time 500 ms in the past. Should clamp to ~tick 14
    // (40 - 26).
    const f64 now_ms      = 40.0 * cfg.frame_ms;
    const f64 view_ms     = now_ms - 500.0;
    FakeWorld out_world{};
    OpaqueWorldState out_state{};
    out_state.data = &out_world;
    out_state.size = sizeof(out_world);
    const RewindResult rr = rewind_world_to(ctx, out_state, view_ms,
                                            /*current_tick=*/40u, 0u);
    CHECK(rr == RewindResult::Clamped);
    CHECK(out_world.tick >= 14u);  // clamped to 200ms window
}

TEST_CASE("net: lag-comp returns Unavailable on empty buffer",
          "[net][lagcomp][empty]") {
    LagCompContext ctx(tick_config_64());
    FakeWorld out_world{};
    OpaqueWorldState out_state{};
    out_state.data = &out_world;
    out_state.size = sizeof(out_world);
    const RewindResult rr = rewind_world_to(ctx, out_state, 0.0, 0, 0);
    CHECK(rr == RewindResult::Unavailable);
}
