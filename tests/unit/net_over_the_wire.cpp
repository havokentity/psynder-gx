// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/net_over_the_wire.cpp — drive the replication payloads (Input +
// snapshot delta) through the REAL in-process rUDP transport (HostImpl over the
// LoopbackBus), not the deterministic in-memory channel. Proves:
//   (a) an Input and a snapshot delta serialize + cross the wire byte-intact;
//   (b) a move replicates end-to-end — the client drives the server's entity
//       with inputs over the wire, the server integrates with the shared
//       step_entity and ships snapshots back, and the client's view converges
//       bit-exactly to a reference integration of the same inputs.
//
// (The full ReplicationSession driven through this transport — replacing its
// in-memory channel — is the tracked follow-up; this pins the serialization +
// transport path the session will plug into.)

#include "net/HostImpl.h"
#include "net/Loopback.h"
#include "net/Net.h"
#include "net/Prediction.h"
#include "net/ReplicationSession.h"   // step_entity
#include "net/SnapshotReplication.h"
#include "net/TickConfig.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

u32 pump(HostImpl& a, HostImpl& b, std::vector<InboundMessage>& ai,
         std::vector<InboundMessage>& bi) {
    const u32 n = a.poll(ai) + b.poll(bi);
    a.tick();
    b.tick();
    return n;
}

template <class T>
std::vector<u8> to_bytes(const T& v) {
    std::vector<u8> b(sizeof(T));
    std::memcpy(b.data(), &v, sizeof(T));
    return b;
}

}  // namespace

TEST_CASE("net: Input + snapshot cross the loopback transport intact",
          "[net][loopback]") {
    LoopbackBus::Get().reset();
    HostDesc ds{}; ds.port = 42101; ds.max_peers = 4;
    HostDesc dc{}; dc.port = 42102; dc.max_peers = 4;
    HostImpl* server = make_test_host(ds);
    HostImpl* client = make_test_host(dc);
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);
    const PeerId c2s = client->connect(ds.port);
    REQUIRE(c2s.valid());

    std::vector<InboundMessage> s_in, c_in;

    // CLIENT -> SERVER: one Input.
    Input sent{};
    sent.tick = 7;
    sent.move[0] = 3.5f;
    sent.yaw_deg = 42.0f;
    sent.buttons = kInputBtnFire;
    client->send(c2s, std::span<const u8>(to_bytes(sent)), /*reliable=*/true);
    for (int i = 0; i < 12 && s_in.empty(); ++i) pump(*server, *client, s_in, c_in);
    REQUIRE(s_in.size() >= 1u);
    Input recv{};
    std::memcpy(&recv, s_in[0].bytes.data(), sizeof(Input));
    REQUIRE(recv.tick == sent.tick);
    REQUIRE(recv.move[0] == sent.move[0]);
    REQUIRE(recv.yaw_deg == sent.yaw_deg);
    REQUIRE(recv.buttons == sent.buttons);
    const PeerId server_to_client = s_in[0].from;  // reply to the sender
    REQUIRE(server_to_client.valid());

    // SERVER -> CLIENT: a 2-entity snapshot, delta-encoded vs empty.
    std::array<EntityState, 2> world{};
    world[0].id = 1; world[0].pos[0] = 5.0f; world[0].yaw_deg = 10.0f;
    world[1].id = 2; world[1].pos[2] = -3.0f;
    std::vector<u8> snap;
    encode_delta(std::span<const EntityState>(),
                 std::span<const EntityState>(world), snap);
    c_in.clear();
    server->send(server_to_client, std::span<const u8>(snap), /*reliable=*/true);
    for (int i = 0; i < 12 && c_in.empty(); ++i) pump(*server, *client, s_in, c_in);
    REQUIRE(c_in.size() >= 1u);
    std::vector<EntityState> got;
    REQUIRE(apply_delta(std::span<const EntityState>(),
                        std::span<const u8>(c_in[0].bytes), got));
    REQUIRE(got.size() == 2u);
    REQUIRE(got[0].id == 1u);
    REQUIRE(got[0].pos[0] == 5.0f);
    REQUIRE(got[0].yaw_deg == 10.0f);
    REQUIRE(got[1].id == 2u);
    REQUIRE(got[1].pos[2] == -3.0f);

    destroy_test_host(server);
    destroy_test_host(client);
    LoopbackBus::Get().reset();
}

TEST_CASE("net: a move replicates over the loopback transport", "[net][loopback]") {
    LoopbackBus::Get().reset();
    HostDesc ds{}; ds.port = 42201; ds.max_peers = 4;
    HostDesc dc{}; dc.port = 42202; dc.max_peers = 4;
    HostImpl* server = make_test_host(ds);
    HostImpl* client = make_test_host(dc);
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);
    const PeerId c2s = client->connect(ds.port);
    REQUIRE(c2s.valid());

    const TickConfig cfg = tick_config_128();
    const f32 dt = static_cast<f32>(cfg.frame_sec);
    constexpr u32 kInputs = 20;

    EntityState srv{}; srv.id = 1;  // server-authoritative entity
    EntityState ref{}; ref.id = 1;  // reference: same inputs, same step
    PeerId server_to_client{};
    u32 server_applied = 0;
    std::vector<InboundMessage> s_in, c_in;

    auto drain_inputs = [&]() {
        for (InboundMessage& m : s_in) {
            if (!server_to_client.valid()) server_to_client = m.from;
            Input ri{};
            std::memcpy(&ri, m.bytes.data(), sizeof(Input));
            step_entity(srv, ri, dt);
            ++server_applied;
        }
        s_in.clear();
    };

    for (u32 k = 0; k < kInputs; ++k) {
        Input in{};
        in.tick = k + 1u;
        in.move[0] = 6.0f;  // run +X
        step_entity(ref, in, dt);
        client->send(c2s, std::span<const u8>(to_bytes(in)), /*reliable=*/true);
        for (int i = 0; i < 3; ++i) pump(*server, *client, s_in, c_in);
        drain_inputs();
    }
    // Flush any inputs still in the reliable window.
    for (int i = 0; i < 60 && server_applied < kInputs; ++i) {
        pump(*server, *client, s_in, c_in);
        drain_inputs();
    }
    REQUIRE(server_applied == kInputs);
    REQUIRE(server_to_client.valid());

    // Server ships the final authoritative snapshot; the client applies it.
    std::array<EntityState, 1> w{srv};
    std::vector<u8> snap;
    encode_delta(std::span<const EntityState>(),
                 std::span<const EntityState>(w), snap);
    c_in.clear();
    server->send(server_to_client, std::span<const u8>(snap), /*reliable=*/true);
    EntityState cli{};  // id 0 until received
    for (int i = 0; i < 60 && cli.id == 0u; ++i) {
        pump(*server, *client, s_in, c_in);
        for (InboundMessage& m : c_in) {
            std::vector<EntityState> got;
            if (apply_delta(std::span<const EntityState>(),
                            std::span<const u8>(m.bytes), got) && !got.empty()) {
                cli = got[0];
            }
        }
        c_in.clear();
    }
    REQUIRE(cli.id == 1u);
    REQUIRE(cli.pos[0] > 0.1f);            // actually moved
    REQUIRE(cli.pos[0] == ref.pos[0]);     // bit-exact vs the reference integration

    destroy_test_host(server);
    destroy_test_host(client);
    LoopbackBus::Get().reset();
}
