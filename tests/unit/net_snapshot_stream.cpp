// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net stateful baseline-tracking snapshot stream tests.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/SnapshotQuantized.h"
#include "net/SnapshotReplication.h"
#include "net/SnapshotStream.h"

using namespace psynder;
using namespace psynder::net;

namespace {

EntityState make_state(u32 id, f32 x, f32 y, f32 z, f32 yaw) {
    EntityState s{};
    s.id = id;
    s.pos[0] = x;
    s.pos[1] = y;
    s.pos[2] = z;
    s.yaw_deg = yaw;
    return s;
}

// Find a state by id; nullptr if absent.
const EntityState* find_by_id(const std::vector<EntityState>& v, u32 id) {
    for (const EntityState& s : v) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

// Serialize a state set to its quantized byte image so two baselines can be
// proved BIT-IDENTICAL (not just float-approx-equal). Quantizing both sides at
// the same resolution and memcmp'ing the integer records is the strongest form
// of the sender==receiver baseline invariant.
std::vector<u8> quantized_image(const std::vector<EntityState>& v, f32 res) {
    std::vector<QuantizedState> q;
    q.reserve(v.size());
    for (const EntityState& s : v) q.push_back(quantize_state(s, res));
    std::sort(q.begin(), q.end(),
              [](const QuantizedState& a, const QuantizedState& b) { return a.id < b.id; });
    std::vector<u8> bytes(q.size() * sizeof(QuantizedState));
    if (!q.empty()) std::memcpy(bytes.data(), q.data(), bytes.size());
    return bytes;
}

// Assert two baselines are bit-identical via their quantized images.
void require_baselines_identical(const std::vector<EntityState>& a,
                                 const std::vector<EntityState>& b, f32 res) {
    const std::vector<u8> ia = quantized_image(a, res);
    const std::vector<u8> ib = quantized_image(b, res);
    REQUIRE(ia.size() == ib.size());
    CHECK(std::memcmp(ia.data(), ib.data(), ia.size()) == 0);
}

// Compare a reconstructed set against an expected (dequantized) set: same ids,
// positions within half a quantization step, yaw close.
void require_set_matches(const std::vector<EntityState>& got,
                         const std::vector<EntityState>& expect, f32 res) {
    REQUIRE(got.size() == expect.size());
    for (const EntityState& e : expect) {
        const EntityState* g = find_by_id(got, e.id);
        REQUIRE(g != nullptr);
        CHECK(std::fabs(g->pos[0] - e.pos[0]) <= res / 2.f);
        CHECK(std::fabs(g->pos[1] - e.pos[1]) <= res / 2.f);
        CHECK(std::fabs(g->pos[2] - e.pos[2]) <= res / 2.f);
        CHECK(g->yaw_deg == Catch::Approx(e.yaw_deg).margin(0.001));
    }
}

// A baseline spread of ids / positions / yaws, including negatives.
std::vector<EntityState> tick0_states() {
    std::vector<EntityState> v;
    v.push_back(make_state(1,    12.345f,  -7.89f,   0.001f,  271.5f));
    v.push_back(make_state(42,  -100.25f,   0.0f,   33.333f,  -45.125f));
    v.push_back(make_state(7,     0.0f,     0.0f,    0.0f,     0.0f));
    v.push_back(make_state(256,  -3.14159f, 2.71828f,-0.123f, -180.0f));
    return v;
}

}  // namespace

TEST_CASE("net-stream: tick0 keyframe reconstructs the full set within half a step",
          "[net]") {
    const f32 res = 0.001f;
    SnapshotStreamSender sender;
    SnapshotStreamReceiver receiver;
    sender.reset();
    receiver.reset();

    const std::vector<EntityState> curr = tick0_states();

    std::vector<u8> wire;
    sender.encode(curr, res, wire);

    std::vector<EntityState> out;
    REQUIRE(receiver.decode(wire, res, out));

    // Receiver reconstructs every entity within the half-step / yaw bound, and
    // both ends now hold the same dequantized baseline.
    require_set_matches(out, sender.baseline(), res);
    require_baselines_identical(sender.baseline(), receiver.baseline(), res);
    REQUIRE(out.size() == curr.size());
}

TEST_CASE("net-stream: a multi-tick move/add/remove sequence keeps both baselines equal",
          "[net]") {
    const f32 res = 0.001f;
    SnapshotStreamSender sender;
    SnapshotStreamReceiver receiver;
    sender.reset();
    receiver.reset();

    std::vector<u8> wire;
    std::vector<EntityState> out;

    // ── Tick 0: full keyframe. ───────────────────────────────────────────────
    std::vector<EntityState> t0 = tick0_states();
    sender.encode(t0, res, wire);
    REQUIRE(receiver.decode(wire, res, out));
    require_set_matches(out, sender.baseline(), res);
    require_baselines_identical(sender.baseline(), receiver.baseline(), res);

    // ── Tick 1: move id 7 (beyond a step), add id 9999, remove id 42. ────────
    std::vector<EntityState> t1;
    for (const EntityState& s : t0) {
        if (s.id == 42) continue;  // remove 42
        EntityState n = s;
        if (s.id == 7) { n.pos[0] = 5.0f; n.pos[1] = -2.5f; n.pos[2] = 0.25f; n.yaw_deg = 90.0f; }
        t1.push_back(n);
    }
    t1.push_back(make_state(9999, 1000.5f, -1000.5f, -0.5f, 359.999f));  // add

    sender.encode(t1, res, wire);
    REQUIRE(receiver.decode(wire, res, out));

    // The receiver's reconstructed set matches the sender's DEQUANTIZED baseline
    // exactly, the move/add/remove all took effect, and the baselines stay equal.
    require_set_matches(out, sender.baseline(), res);
    require_baselines_identical(sender.baseline(), receiver.baseline(), res);
    CHECK(find_by_id(out, 42) == nullptr);
    CHECK(find_by_id(out, 9999) != nullptr);
    {
        const EntityState* moved = find_by_id(out, 7);
        REQUIRE(moved != nullptr);
        CHECK(std::fabs(moved->pos[0] - 5.0f) <= res / 2.f);
        CHECK(moved->yaw_deg == Catch::Approx(90.0f).margin(0.001));
    }

    // ── Tick 2: an UNCHANGED tick produces a minimal (two zero-count) delta and
    //    leaves the receiver set unchanged. ────────────────────────────────────
    const std::vector<EntityState> prev_out = out;
    std::vector<EntityState> t2 = sender.baseline();  // feed back exactly the baseline
    sender.encode(t2, res, wire);
    REQUIRE(wire.size() == 8u);  // removed_count(0) + changed_count(0), nothing else
    REQUIRE(receiver.decode(wire, res, out));
    require_set_matches(out, prev_out, res);
    require_baselines_identical(sender.baseline(), receiver.baseline(), res);
    REQUIRE(out.size() == prev_out.size());
}

TEST_CASE("net-stream: a sub-step jitter tick stays empty on the wire and baselines hold",
          "[net]") {
    const f32 res = 0.01f;  // 1 cm steps
    SnapshotStreamSender sender;
    SnapshotStreamReceiver receiver;
    sender.reset();
    receiver.reset();

    std::vector<u8> wire;
    std::vector<EntityState> out;

    std::vector<EntityState> t0 = tick0_states();
    // Snap every position onto its quantization lattice CENTRE (x = q*res) so a
    // sub-half-step nudge provably stays in the same cell regardless of where the
    // source positions happened to fall relative to a cell boundary.
    for (EntityState& s : t0) {
        for (int i = 0; i < 3; ++i) {
            s.pos[i] = std::floor(s.pos[i] / res + 0.5f) * res;
        }
    }
    sender.encode(t0, res, wire);
    REQUIRE(receiver.decode(wire, res, out));
    require_baselines_identical(sender.baseline(), receiver.baseline(), res);

    // Nudge every entity by less than half a step so each quantizes unchanged.
    std::vector<EntityState> t1 = t0;
    for (EntityState& s : t1) {
        s.pos[0] += 0.002f;  // 2 mm < 5 mm half-step
        s.pos[2] -= 0.003f;  // 3 mm < 5 mm half-step
    }

    sender.encode(t1, res, wire);
    REQUIRE(wire.size() == 8u);  // both counts zero — the jitter never hits the wire
    REQUIRE(receiver.decode(wire, res, out));

    // This is the core of the dequantized-baseline argument: even though the
    // sender saw moved floats, its baseline tracks the dequantized lattice point,
    // so it never diverges from the receiver that only ever sees that point.
    require_baselines_identical(sender.baseline(), receiver.baseline(), res);
    require_set_matches(out, sender.baseline(), res);
}

TEST_CASE("net-stream: a corrupted delta makes decode return false and preserves the baseline",
          "[net]") {
    const f32 res = 0.001f;
    SnapshotStreamSender sender;
    SnapshotStreamReceiver receiver;
    sender.reset();
    receiver.reset();

    std::vector<u8> wire;
    std::vector<EntityState> out;

    // Establish a shared baseline at tick 0.
    sender.encode(tick0_states(), res, wire);
    REQUIRE(receiver.decode(wire, res, out));
    const std::vector<EntityState> good_baseline = receiver.baseline();
    const std::vector<EntityState> good_out = out;

    // Build a real tick-1 delta with a change, then truncate it to corrupt it.
    std::vector<EntityState> t1 = tick0_states();
    t1.push_back(make_state(9999, 1.0f, 2.0f, 3.0f, 12.0f));
    sender.encode(t1, res, wire);
    REQUIRE(wire.size() > 8u);  // there IS a record to truncate

    std::vector<u8> truncated(wire.begin(), wire.end() - 1);  // drop a record byte
    CHECK_FALSE(receiver.decode(truncated, res, out));

    // The failed decode left BOTH the tracked baseline and `out` untouched.
    require_baselines_identical(receiver.baseline(), good_baseline, res);
    require_set_matches(out, good_out, res);

    // A buffer too short for even the first header also fails cleanly.
    std::vector<u8> tiny(wire.begin(), wire.begin() + 2);
    CHECK_FALSE(receiver.decode(tiny, res, out));
    require_baselines_identical(receiver.baseline(), good_baseline, res);
}

TEST_CASE("net-stream: two independent senders emit byte-identical streams",
          "[net][determinism]") {
    const f32 res = 0.001f;

    // Drive two senders through the exact same tick sequence and concatenate
    // each tick's wire bytes; the full streams must be byte-for-byte identical.
    auto run = []() {
        SnapshotStreamSender sender;
        sender.reset();
        std::vector<u8> stream;
        std::vector<u8> wire;

        std::vector<EntityState> t0 = tick0_states();
        sender.encode(t0, 0.001f, wire);
        stream.insert(stream.end(), wire.begin(), wire.end());

        std::vector<EntityState> t1;
        for (const EntityState& s : t0) {
            if (s.id == 42) continue;
            EntityState n = s;
            if (s.id == 7) n.pos[0] = 5.0f;
            t1.push_back(n);
        }
        t1.push_back(make_state(9999, 1.0f, 2.0f, 3.0f, 12.0f));
        sender.encode(t1, 0.001f, wire);
        stream.insert(stream.end(), wire.begin(), wire.end());

        // A no-op tick (feed the baseline back) — must also be reproducible.
        sender.encode(sender.baseline(), 0.001f, wire);
        stream.insert(stream.end(), wire.begin(), wire.end());
        return stream;
    };

    const std::vector<u8> a = run();
    const std::vector<u8> b = run();
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}
