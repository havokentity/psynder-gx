// SPDX-License-Identifier: MIT
//
// tests/unit/net_heartbeat.cpp — connection keepalive + timeout tracking.

#include "net/Heartbeat.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::net;

namespace {
Heartbeat make_hb() {
    Heartbeat hb;
    hb.configure(HeartbeatConfig{/*keepalive*/ 1.0, /*timeout*/ 5.0});
    hb.reset(0.0);
    return hb;
}
}  // namespace

TEST_CASE("heartbeat: before reset nothing is due and nothing times out", "[net]") {
    Heartbeat hb;
    hb.configure(HeartbeatConfig{1.0, 5.0});
    CHECK_FALSE(hb.started());
    CHECK_FALSE(hb.should_send_keepalive(100.0));
    CHECK_FALSE(hb.is_timed_out(100.0));
}

TEST_CASE("heartbeat: send-idle past the interval requests a keepalive", "[net]") {
    Heartbeat hb = make_hb();
    CHECK_FALSE(hb.should_send_keepalive(0.5));  // within the interval
    CHECK(hb.should_send_keepalive(1.0));        // at the interval
    CHECK(hb.should_send_keepalive(2.0));
    // Sending resets the keepalive schedule.
    hb.on_sent(2.0);
    CHECK_FALSE(hb.should_send_keepalive(2.5));
    CHECK(hb.should_send_keepalive(3.0));
}

TEST_CASE("heartbeat: receiving traffic keeps the peer alive", "[net]") {
    Heartbeat hb = make_hb();
    CHECK_FALSE(hb.is_timed_out(4.0));  // within timeout
    hb.on_received(4.0);                // fresh traffic
    CHECK(hb.time_since_recv(4.0) == Catch::Approx(0.0));
    CHECK_FALSE(hb.is_timed_out(8.0));  // 4 s since the 4.0 packet < 5 s timeout
    CHECK(hb.is_timed_out(9.0));        // 5 s since last recv -> dead
}

TEST_CASE("heartbeat: a timed-out peer is detected after the timeout", "[net]") {
    Heartbeat hb = make_hb();
    CHECK_FALSE(hb.is_timed_out(4.999));
    CHECK(hb.is_timed_out(5.0));   // exactly at the timeout
    CHECK(hb.is_timed_out(20.0));
}

TEST_CASE("heartbeat: send and recv stamps are independent", "[net]") {
    Heartbeat hb = make_hb();
    // Sending does not refresh the recv timeout.
    hb.on_sent(3.0);
    CHECK(hb.is_timed_out(5.0));           // still dead (no recv since reset at 0)
    CHECK(hb.time_since_recv(5.0) == Catch::Approx(5.0));
    CHECK(hb.time_since_sent(5.0) == Catch::Approx(2.0));
    // Receiving does not satisfy the keepalive (which is about send-idle).
    Heartbeat hb2 = make_hb();
    hb2.on_received(2.0);
    CHECK(hb2.should_send_keepalive(2.0));  // 2 s send-idle since reset
}

TEST_CASE("heartbeat: identical sequences decide identically", "[net][determinism]") {
    auto run = [] {
        Heartbeat hb = make_hb();
        u32 code = 0u;
        const double events[] = {0.5, 1.2, 1.2, 3.0, 6.5, 7.0};
        f64 t = 0.0;
        for (double e : events) {
            t = e;
            if (hb.should_send_keepalive(t)) { hb.on_sent(t); code = code * 2u + 1u; }
            else code = code * 2u;
            if (hb.is_timed_out(t)) code += 1000u;
            if (e == 6.5) hb.on_received(t);
        }
        return code;
    };
    CHECK(run() == run());
}
