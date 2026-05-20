// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / real UDP loopback test.
//
// Spins up two UdpSocket instances on 127.0.0.1, exchanges a request +
// reply, and verifies both halves arrive intact. This proves the Berkeley
// / Winsock send_to / recv_from path works end-to-end without the
// in-process LoopbackBus.
//
// Strict on the assertions: we want bit-for-bit fidelity.

#include <catch2/catch_test_macros.hpp>

#include "net/UdpSocket.h"

#include <array>
#include <chrono>
#include <thread>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

// Wait up to `ms` for a datagram on `s`. Polls recv_from at 1ms cadence so
// we don't spin the CPU at 100% during the test.
isize poll_recv(UdpSocket& s, std::span<u8> out, Endpoint& src, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const isize rc = s.recv_from(out, src);
        if (rc > 0) return rc;
        if (rc < 0) return rc;
        if (std::chrono::steady_clock::now() >= deadline) return 0;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

}  // namespace

TEST_CASE("net: UDP socket open / close round-trips", "[net][udp]") {
    REQUIRE(ensure_initialized());
    UdpSocket s;
    REQUIRE(s.open(0));         // ephemeral
    CHECK(s.is_open());
    CHECK(s.local_port() != 0);
    s.close();
    CHECK(!s.is_open());
}

TEST_CASE("net: client -> server -> reply over real UDP loopback",
          "[net][udp][loopback]") {
    REQUIRE(ensure_initialized());
    UdpSocket server;
    UdpSocket client;
    REQUIRE(server.open(0));
    REQUIRE(client.open(0));
    const u16 server_port = server.local_port();
    const u16 client_port = client.local_port();
    REQUIRE(server_port != 0);
    REQUIRE(client_port != 0);
    REQUIRE(server_port != client_port);

    // Client -> server.
    const std::array<u8, 12> req{
        'P', 'S', 'Y', 'N',
        'D', 'E', 'R', '-',
        'G', 'X', '!', '\n'};
    Endpoint server_ep{};
    server_ep.host_ipv4 = kLoopbackIpv4;
    server_ep.port      = server_port;

    REQUIRE(client.send_to(server_ep, req) == static_cast<isize>(req.size()));

    std::vector<u8> rx(64);
    Endpoint src{};
    const isize got_req = poll_recv(server, rx, src, /*timeout_ms=*/1000);
    REQUIRE(got_req == static_cast<isize>(req.size()));
    rx.resize(static_cast<usize>(got_req));
    CHECK(std::equal(rx.begin(), rx.end(), req.begin()));
    CHECK(src.host_ipv4 == kLoopbackIpv4);
    CHECK(src.port      == client_port);

    // Server -> client (reply).
    const std::array<u8, 5> reply{'A', 'C', 'K', '-', '1'};
    Endpoint reply_to{};
    reply_to.host_ipv4 = src.host_ipv4;
    reply_to.port      = src.port;
    REQUIRE(server.send_to(reply_to, reply) == static_cast<isize>(reply.size()));

    std::vector<u8> rx2(64);
    Endpoint src2{};
    const isize got_reply = poll_recv(client, rx2, src2, /*timeout_ms=*/1000);
    REQUIRE(got_reply == static_cast<isize>(reply.size()));
    rx2.resize(static_cast<usize>(got_reply));
    CHECK(std::equal(rx2.begin(), rx2.end(), reply.begin()));
    CHECK(src2.host_ipv4 == kLoopbackIpv4);
    CHECK(src2.port      == server_port);
}

TEST_CASE("net: recv_from returns 0 with no pending datagram (non-blocking)",
          "[net][udp][nonblocking]") {
    REQUIRE(ensure_initialized());
    UdpSocket s;
    REQUIRE(s.open(0));
    std::array<u8, 32> rx{};
    Endpoint src{};
    const isize rc = s.recv_from(rx, src);
    CHECK(rc == 0);
}
