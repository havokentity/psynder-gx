// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — end-to-end snapshot replication pipeline. Lane 18 (net).
//
// This header is the CAPSTONE that composes the net lane's send and receive
// stacks into one headless loop: "the server ships bandwidth-managed snapshots,
// the client interpolates them". Nothing here is new mechanism — it is pure
// glue that wires already-tested primitives together in the order a real
// server/client pair runs them, with reused scratch buffers so the steady-state
// tick grows no heap.
//
// ── Server (ReplicationServer) ─────────────────────────────────────────────
//   each tick:
//     1. budget.refill(dt)          — pour bytes_per_s * dt into the token bucket
//        (BandwidthBudget) so a slow tick lets the next one send a little more,
//        capped at the burst size.
//     2. scheduler.tick(...)        — accumulate per-entity priorities, take the
//        highest-priority entities the budget can afford (one quantized record
//        each), charge the budget, quantize+pack them, and fragment the payload
//        to the link MTU (SnapshotScheduler over PriorityAccumulator + pack +
//        Fragment). The MTU-sized fragments are the bytes that go "on the wire".
//   A tick whose budget is too small drops the low-priority entities; their
//   accumulators keep climbing so they are sent on a later tick (no starvation).
//
// ── Wire ────────────────────────────────────────────────────────────────────
//   The server's `out_fragments` for tick T, plus the server time stamp for that
//   tick, are what the transport delivers to the client. Fragments may arrive in
//   any order; the client's reassembler tolerates that.
//
// ── Client (ReplicationClient) ──────────────────────────────────────────────
//   receive(fragments, server_time):
//     1. FragmentReassembler        — feed every fragment; once complete(),
//        assemble() the original packed payload back byte-exact.
//     2. unpack_quantized(...)      — dequantize the payload back to EntityStates
//        (position within half a quantization step of the server's value).
//     3. SnapshotInterpBuffer::push — stamp those states into the interp ring at
//        the snapshot's server time.
//     4. PlayoutClock::on_snapshot  — record server_time as the newest data the
//        render clock should trail by the interpolation delay.
//   advance(dt):
//     5. PlayoutClock::advance(dt)  — move the local render clock forward by frame
//        dt and re-sync it toward (newest_server_time - delay).
//   view(out):
//     6. SnapshotInterpBuffer::sample(render_time) — LERP every entity between the
//        two buffered snapshots that bracket the render time, yielding a smooth
//        interpolated view that trails the server by the interpolation delay.
//
// Determinism: every stage (priority select, pack, fragment, reassemble, unpack,
// interp, playout clock) is deterministic strict-FP net-lane code, so the same
// op sequence yields bit-identical fragments on the server and a bit-identical
// interpolated view on the client across runs and platforms. Net TUs that
// include this header MUST build -fno-fast-math -ffp-contract=off
// (DESIGN-PSYNDER-GX.md §14).

#pragma once

#include "net/BandwidthBudget.h"
#include "net/Fragment.h"
#include "net/PlayoutClock.h"
#include "net/SnapshotInterp.h"
#include "net/SnapshotReplication.h"  // EntityState
#include "net/SnapshotScheduler.h"

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// ReplicationServer — owns a SnapshotScheduler + a BandwidthBudget. Each tick
// it refills the budget by the frame dt and schedules a bandwidth-managed
// snapshot, emitting the MTU-sized fragments to send. The most recent tick's
// SendPlan (which entities were sent, how many fragments, bytes spent) is kept
// for tests / metrics. No per-tick heap beyond the scheduler's reused scratch
// and the caller-owned `out_fragments`.
// ──────────────────────────────────────────────────────────────────────────
class ReplicationServer {
public:
    ReplicationServer() noexcept = default;

    // Size the priority space (dense entity slots [0, entity_count)), set the
    // send-rate token bucket (bytes_per_s refill, burst cap), the position
    // quantization step (metres) and the link MTU (max DATA bytes per fragment).
    // Re-callable to reconfigure; resets the scheduler's accumulators.
    void configure(usize entity_count, f32 bytes_per_s, usize burst,
                   f32 pos_res, usize mtu);

    // Zero the priority accumulators and empty the budget bucket (e.g. on a peer
    // (re)connect). Configured rate/cap/resolution/MTU are retained.
    void reset() noexcept;

    // One send tick. `states[i]` is entity slot i's authoritative state and
    // `base_priorities[i]` how badly it wants sending (higher = more often).
    // Refills the budget by `dt_s`, then schedules: accumulate priorities, take
    // the highest-priority entities the budget affords, charge the budget,
    // quantize+pack them, and fragment to the MTU. `msg_id` stamps the fragment
    // set. `out_fragments` is cleared then filled (empty on an empty selection).
    void tick(std::span<const EntityState> states,
              std::span<const f32>         base_priorities,
              f32                          dt_s,
              u16                          msg_id,
              std::vector<std::vector<u8>>& out_fragments);

    // The plan produced by the most recent tick(): sent indices (descending
    // priority), record/fragment counts, bytes charged to the budget.
    const SnapshotScheduler::SendPlan& last_plan() const noexcept { return plan_; }

    // Bytes currently available in the send budget (after the last refill/spend).
    f32 budget_available() const noexcept { return budget_.available(); }

private:
    SnapshotScheduler           scheduler_;
    BandwidthBudget             budget_;
    SnapshotScheduler::SendPlan plan_;
    f32                         pos_res_ = 0.01f;  // position quantization (m)
    usize                       mtu_     = 1200;   // max data bytes / fragment
};

// ──────────────────────────────────────────────────────────────────────────
// ReplicationClient — owns a FragmentReassembler, a SnapshotInterpBuffer and a
// PlayoutClock. receive() reassembles + unpacks one tick's fragments and stamps
// them into the interp ring at the server time; advance() ticks the playout
// clock; view() samples the smoothed interpolated state at the playout render
// time. Reuses scratch buffers; the steady-state path grows no heap beyond the
// caller's `out` in view().
// ──────────────────────────────────────────────────────────────────────────
class ReplicationClient {
public:
    ReplicationClient() noexcept = default;

    // Set the position quantization step (must match the server's) and the
    // interpolation delay (seconds) the render clock trails the newest snapshot
    // by. Re-callable; resets the reassembler/buffer/clock state.
    void configure(f32 pos_res, f32 interp_delay_s);

    // Drop all buffered snapshots, reassembly progress and clock state. The
    // configured resolution / interp delay are retained.
    void reset() noexcept;

    // Ingest one tick's fragments (any order) stamped at `server_time_s`:
    // reassemble + unpack them and push the recovered states into the interp
    // buffer at that server time, recording it as the newest data for the
    // playout clock. A truncated / incomplete fragment set is dropped (no push,
    // no clock update) and the call is a no-op. Returns true iff a snapshot was
    // accepted and buffered.
    bool receive(std::span<const std::vector<u8>> fragments, f64 server_time_s);

    // Advance the playout (interpolation) render clock by frame `dt_s`.
    void advance(f32 dt_s);

    // Sample the interp buffer at the current playout render time into `out`
    // (cleared then filled in ascending id order). Returns false (and clears
    // `out`) when no snapshot has been buffered yet.
    bool view(std::vector<EntityState>& out) const;

    // The time the buffer is sampled at (the playout render clock).
    f64 render_time_s() const noexcept { return clock_.render_time_s(); }

    // Newest server time recorded so far (0 before any snapshot).
    f64 latest_server_time_s() const noexcept { return clock_.latest_server_time_s(); }

    // Number of snapshots currently buffered for interpolation.
    usize buffered() const noexcept { return buffer_.size(); }

private:
    FragmentReassembler   reassembler_;
    SnapshotInterpBuffer  buffer_;
    PlayoutClock          clock_;
    std::vector<u8>       payload_scratch_;  // reused: assembled packed payload
    std::vector<EntityState> states_scratch_;  // reused: unpacked states
    f32                   pos_res_ = 0.01f;   // position quantization (m)
};

}  // namespace psynder::net
