// SPDX-License-Identifier: MIT
// Psynder-GX — lane 18 / net: end-to-end client prediction + server
// reconciliation on the widened Input schema (#41, ADR-020).
//
// These tests model the full loop a client runs each frame: it predicts its own
// movement locally (applying inputs immediately), and when the server acks a
// tick it snaps to the authoritative state and re-simulates the still-unacked
// inputs on top of it (predict_present). The step functor is a simple
// deterministic mover (pos += velocity_from_input * dt; yaw follows the input)
// so the replay is bit-reproducible. Real gameplay plugs its own step.

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "net/Prediction.h"

using namespace psynder;
using namespace psynder::net;

namespace {

constexpr f32 kDt = 1.0f / 128.0f;  // 128-tick lockstep

// Minimal predicted player state the deterministic step integrates.
struct PlayerState {
    f32 pos[3] = {0.f, 0.f, 0.f};
    f32 yaw_deg = 0.f;
    bool operator==(const PlayerState& o) const noexcept {
        // Bit-exact: lockstep determinism forbids epsilon fuzz.
        return pos[0] == o.pos[0] && pos[1] == o.pos[1] &&
               pos[2] == o.pos[2] && yaw_deg == o.yaw_deg;
    }
};

// Deterministic per-tick kinematics: integrate the velocity command over dt and
// adopt the input's look yaw. Identical bytes in -> identical bytes out.
void step(PlayerState& s, const Input& in) {
    s.pos[0] += in.move[0] * kDt;
    s.pos[1] += in.move[1] * kDt;
    s.pos[2] += in.move[2] * kDt;
    s.yaw_deg = in.yaw_deg;
}

// Run the same input stream forward from a known start — the "server" replays
// the authoritative inputs to produce ground truth we compare the client to.
PlayerState run_forward(PlayerState s, std::span<const Input> inputs) {
    for (const Input& in : inputs) step(s, in);
    return s;
}

}  // namespace

// ── widened schema is still POD on the wire ─────────────────────────────────
TEST_CASE("net: widened Input stays trivially copyable POD",
          "[net][prediction][schema]") {
    static_assert(std::is_trivially_copyable_v<Input>);
    // Button bits mirror engine/scene/Replay.h's kReplayBtn* order.
    CHECK(kInputBtnJump   == (1u << 0));
    CHECK(kInputBtnFire   == (1u << 1));
    CHECK(kInputBtnRun    == (1u << 2));
    CHECK(kInputBtnCrouch == (1u << 3));
    Input in{};
    in.buttons = kInputBtnFire | kInputBtnJump;
    CHECK((in.buttons & kInputBtnFire) != 0);
    CHECK((in.buttons & kInputBtnCrouch) == 0);
}

// ── #41 end-to-end: predict, inject a diverging ack, reconcile to server ────
TEST_CASE("net: predict_present converges client onto authoritative server",
          "[net][prediction][reconcile]") {
    // The client builds an input timeline for ticks 1..8: move +x with a small
    // yaw sweep + intermittent fire. It predicts locally from the origin.
    InputRing<256> ring;
    for (u32 t = 1; t <= 8; ++t) {
        Input in{};
        in.tick = t;
        in.move[0] = 1.0f;  // +1 m/s along +x
        in.yaw_deg = static_cast<f32>(t) * 2.0f;
        if (t % 2 == 0) in.buttons = kInputBtnFire;
        ring.push(in);
    }

    // Gather the full timeline so we can compute ground truth the way the
    // server would (replay all inputs from the origin).
    std::array<Input, 256> all{};
    const usize n_all = ring.pending_after(/*last_acked_tick=*/0, std::span<Input>(all));
    REQUIRE(n_all == 8);

    // Server acked tick 5. Its authoritative state for tick 5 is the SAME
    // inputs replayed forward 1..5 — but to prove reconciliation actually
    // corrects divergence we hand the client a server state that DIFFERS from
    // what naive local prediction would have produced (e.g. the server resolved
    // a collision the client missed, nudging x and yaw).
    const u32 acked_tick = 5;
    PlayerState server_at_ack =
        run_forward(PlayerState{}, std::span<const Input>(all.data(), 5));
    server_at_ack.pos[0] += 0.25f;     // authoritative correction
    server_at_ack.yaw_deg = 33.0f;     // server's resolved facing

    // Ground truth for the PRESENT tick: the server state at the ack, with
    // ticks 6..8 replayed on top by the identical deterministic step.
    PlayerState ground_truth = run_forward(
        server_at_ack, std::span<const Input>(all.data() + 5, n_all - 5));

    // Client reconciles: snap to the server's acked state, then predict_present
    // replays the unacked inputs (6..8) on top.
    PlayerState client = server_at_ack;  // snap to authoritative
    std::array<Input, 256> scratch{};
    const usize replayed = predict_present(
        ring, acked_tick, client, std::span<Input>(scratch), step);
    CHECK(replayed == 3);  // ticks 6, 7, 8

    // BIT-EXACT match against the server's forward replay.
    CHECK(client == ground_truth);

    // Acked inputs (<= 5) are retired from the ring; 6..8 survive in order.
    const usize dropped = ring.drop_acked(acked_tick);
    CHECK(dropped == 5);
    CHECK(ring.size() == 3);
    std::array<Input, 256> rest{};
    const usize remaining = ring.pending_after(0, std::span<Input>(rest));
    REQUIRE(remaining == 3);
    CHECK(rest[0].tick == 6);
    CHECK(rest[1].tick == 7);
    CHECK(rest[2].tick == 8);
}

// ── prediction with zero divergence leaves the client visually still ────────
TEST_CASE("net: predict_present with matching ack reproduces local prediction",
          "[net][prediction][reconcile]") {
    InputRing<256> ring;
    for (u32 t = 1; t <= 6; ++t) {
        Input in{};
        in.tick = t;
        in.move[0] = 2.0f;
        in.yaw_deg = 90.0f;
        ring.push(in);
    }

    std::array<Input, 256> all{};
    const usize n_all = ring.pending_after(0, std::span<Input>(all));

    // Local prediction: replay the entire timeline from origin.
    PlayerState predicted = run_forward(PlayerState{}, std::span<const Input>(all.data(), n_all));

    // Server acked tick 4 with EXACTLY what the client predicted (no divergence).
    const u32 acked_tick = 4;
    PlayerState server_at_ack =
        run_forward(PlayerState{}, std::span<const Input>(all.data(), 4));

    PlayerState client = server_at_ack;
    std::array<Input, 256> scratch{};
    const usize replayed = predict_present(
        ring, acked_tick, client, std::span<Input>(scratch), step);
    CHECK(replayed == 2);  // ticks 5, 6

    // No divergence -> reconciled state equals the original local prediction.
    CHECK(client == predicted);
}

// ── determinism: same inputs + same ack -> bit-identical reconciled state ───
TEST_CASE("net: predict_present is deterministic across runs",
          "[net][prediction][reconcile][determinism]") {
    auto run = []() {
        InputRing<256> ring;
        for (u32 t = 1; t <= 10; ++t) {
            Input in{};
            in.tick = t;
            in.move[0] = 0.7f;
            in.move[2] = -0.3f;
            in.yaw_deg = static_cast<f32>(t) * 13.0f;
            ring.push(in);
        }
        std::array<Input, 256> all{};
        const usize n = ring.pending_after(0, std::span<Input>(all));
        PlayerState client = run_forward(PlayerState{}, std::span<const Input>(all.data(), 7));
        std::array<Input, 256> scratch{};
        predict_present(ring, /*acked_tick=*/7, client, std::span<Input>(scratch), step);
        return client;
    };
    CHECK(run() == run());
}
