// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / reliability test under 30% packet drop.
//
// Per task spec: drop 30% of packets and verify all reliably-sent payloads
// still arrive (in order, no duplicates).
//
// Runs against the in-process LoopbackBus so we can inject a deterministic
// drop policy. The real UdpSocket path is covered by net_udp_loopback.cpp;
// the reliability+RTO layer is transport-agnostic so testing it via
// LoopbackBus is the right abstraction.

#include <catch2/catch_test_macros.hpp>

#include "net/HostImpl.h"
#include "net/Loopback.h"
#include "net/Net.h"

#include <cstdint>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

struct Xs32 {
    u32 state;
    u32 next() {
        u32 x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }
};

void pump(HostImpl& a, HostImpl& b,
          std::vector<InboundMessage>& a_in,
          std::vector<InboundMessage>& b_in) {
    a.poll(a_in);
    b.poll(b_in);
    a.tick();
    b.tick();
}

}  // namespace

TEST_CASE("net: reliable channel delivers everything under 30% packet drop",
          "[net][reliability][drop30]") {
    LoopbackBus::Get().reset();

    // 30% drop rate. Use a deterministic PRNG so the test is reproducible.
    Xs32 prng{0xFEEDF00Du};
    LoopbackBus::Get().set_loss_policy(
        [&prng](u16 /*src*/, u16 /*dst*/, std::span<const u8> /*bytes*/,
                u32 /*attempt*/) {
            return (prng.next() % 100) >= 30;  // deliver 70%, drop 30%
        });

    HostDesc da{}; da.port = 44001; da.max_peers = 4;
    HostDesc db{}; db.port = 44002; db.max_peers = 4;
    HostImpl* a = make_test_host(da);
    HostImpl* b = make_test_host(db);
    REQUIRE(a);
    REQUIRE(b);

    PeerId a_to_b = a->connect(db.port);
    REQUIRE(a_to_b.valid());

    // Send a window-sized burst. kSendWindow == 64; send 60 to stay below
    // the cap (the test isn't checking window backpressure here, just
    // RTO + selective-ACK convergence).
    constexpr u32 kCount = 60;
    for (u32 i = 0; i < kCount; ++i) {
        u8 byte = static_cast<u8>(i);
        a->send(a_to_b, std::span<const u8>(&byte, 1), /*reliable=*/true);
    }

    std::vector<InboundMessage> a_in, b_in;
    constexpr u32 kMaxTicks = 10000;
    u32 ticks = 0;
    while (b_in.size() < kCount && ticks++ < kMaxTicks) {
        pump(*a, *b, a_in, b_in);
    }
    INFO("drop30 ticks=" << ticks << " delivered=" << b_in.size()
         << " expected=" << kCount);

    REQUIRE(b_in.size() == kCount);

    // Verify in-order delivery + no duplicates.
    for (u32 i = 0; i < kCount; ++i) {
        REQUIRE(b_in[i].bytes.size() == 1);
        CHECK(b_in[i].bytes[0] == static_cast<u8>(i));
        CHECK(b_in[i].reliable);
    }

    destroy_test_host(a);
    destroy_test_host(b);
    LoopbackBus::Get().set_loss_policy({});
    LoopbackBus::Get().reset();
}
