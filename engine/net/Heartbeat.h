// SPDX-License-Identifier: MIT
// Psynder-GX — connection heartbeat / timeout tracker. Lane 18 (net).
//
// Tracks when a peer last SENT and last RECEIVED traffic so a connection can
//   (a) schedule a keepalive when the link has been send-idle longer than an
//       interval (so an otherwise-quiet connection stays warm + measurable), and
//   (b) declare the peer DEAD when nothing has arrived for a timeout.
// Any real packet doubles as a heartbeat — call on_sent/on_received for ALL
// traffic, and only emit an explicit keepalive when should_send_keepalive() is
// true. Pure f64-time state, no wall clock (the caller drives `now_s`), no RNG.

#pragma once

#include "core/Types.h"

namespace psynder::net {

struct HeartbeatConfig {
    f64 keepalive_interval_s = 1.0;  // emit a keepalive after this much send-idle
    f64 timeout_s = 5.0;             // declare dead after this much recv-idle
};

class Heartbeat {
public:
    Heartbeat() noexcept = default;

    // Set the keepalive/timeout policy (retained across resets).
    void configure(const HeartbeatConfig& cfg) noexcept { cfg_ = cfg; }

    // Start (or restart) tracking with both stamps at `now_s` — the peer counts
    // as freshly alive and freshly sent-to. Until reset() is called the tracker
    // reports "not started": no keepalive needed, never timed out.
    void reset(f64 now_s) noexcept {
        last_recv_s_ = now_s;
        last_sent_s_ = now_s;
        started_ = true;
    }

    // Record inbound traffic (keeps the peer alive) / outbound traffic (resets the
    // keepalive schedule). Separate stamps: a sent packet does NOT keep the peer
    // alive, and a received packet does NOT satisfy the keepalive.
    void on_received(f64 now_s) noexcept { last_recv_s_ = now_s; }
    void on_sent(f64 now_s) noexcept { last_sent_s_ = now_s; }

    // True once the link has been send-idle for >= keepalive_interval_s (so the
    // caller should emit an explicit keepalive). False before reset().
    bool should_send_keepalive(f64 now_s) const noexcept {
        return started_ && (now_s - last_sent_s_) >= cfg_.keepalive_interval_s;
    }

    // True once nothing has arrived for >= timeout_s — the peer is presumed gone.
    // False before reset().
    bool is_timed_out(f64 now_s) const noexcept {
        return started_ && (now_s - last_recv_s_) >= cfg_.timeout_s;
    }

    f64 time_since_recv(f64 now_s) const noexcept { return now_s - last_recv_s_; }
    f64 time_since_sent(f64 now_s) const noexcept { return now_s - last_sent_s_; }
    f64 last_recv_s() const noexcept { return last_recv_s_; }
    f64 last_sent_s() const noexcept { return last_sent_s_; }
    bool started() const noexcept { return started_; }

private:
    HeartbeatConfig cfg_{};
    f64  last_recv_s_ = 0.0;
    f64  last_sent_s_ = 0.0;
    bool started_ = false;
};

}  // namespace psynder::net
