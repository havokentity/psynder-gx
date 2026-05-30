// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net packet fragmentation + reassembly tests.

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/Fragment.h"

using namespace psynder;
using namespace psynder::net;

namespace {

// A deterministic byte ramp of `n` bytes (0, 1, 2, ..., wrapping at 256).
std::vector<u8> ramp(usize n) {
    std::vector<u8> v(n);
    for (usize i = 0; i < n; ++i) v[i] = static_cast<u8>(i & 0xFFu);
    return v;
}

// Feed every fragment of `frags` into `r` in the given index order.
void feed(FragmentReassembler& r,
          const std::vector<std::vector<u8>>& frags) {
    for (const auto& f : frags) r.add_fragment(f);
}

}  // namespace

TEST_CASE("net-fragment: a payload smaller than the MTU makes one fragment",
          "[net]") {
    const std::vector<u8> payload = ramp(10);

    std::vector<std::vector<u8>> frags;
    fragment_message(7, payload, 64, frags);

    REQUIRE(frags.size() == 1u);
    // Header (6 bytes) + the whole payload.
    CHECK(frags[0].size() == kFragmentHeaderBytes + payload.size());

    FragmentReassembler r;
    REQUIRE(r.add_fragment(frags[0]));
    REQUIRE(r.complete());

    std::vector<u8> out;
    REQUIRE(r.assemble(out));
    REQUIRE(out.size() == payload.size());
    CHECK(std::memcmp(out.data(), payload.data(), out.size()) == 0);
}

TEST_CASE("net-fragment: a large payload splits and reassembles exactly",
          "[net]") {
    const std::vector<u8> payload = ramp(1000);
    const usize mtu = 300;  // ceil(1000 / 300) == 4 fragments

    std::vector<std::vector<u8>> frags;
    fragment_message(11, payload, mtu, frags);

    REQUIRE(frags.size() == 4u);

    // Each fragment carries at most `mtu` data bytes; the last carries the rest.
    for (usize i = 0; i < frags.size(); ++i) {
        const usize data = frags[i].size() - kFragmentHeaderBytes;
        CHECK(data <= mtu);
    }

    FragmentReassembler r;
    feed(r, frags);
    REQUIRE(r.complete());

    std::vector<u8> out;
    REQUIRE(r.assemble(out));
    REQUIRE(out.size() == payload.size());
    CHECK(std::memcmp(out.data(), payload.data(), out.size()) == 0);
}

TEST_CASE("net-fragment: out-of-order fragments still reassemble correctly",
          "[net]") {
    const std::vector<u8> payload = ramp(777);

    std::vector<std::vector<u8>> frags;
    fragment_message(3, payload, 100, frags);
    REQUIRE(frags.size() > 2u);

    FragmentReassembler r;
    // Feed in reverse order.
    for (usize i = frags.size(); i-- > 0;) {
        r.add_fragment(frags[i]);
    }
    REQUIRE(r.complete());

    std::vector<u8> out;
    REQUIRE(r.assemble(out));
    REQUIRE(out.size() == payload.size());
    CHECK(std::memcmp(out.data(), payload.data(), out.size()) == 0);
}

TEST_CASE("net-fragment: an empty payload makes one fragment that round-trips",
          "[net]") {
    const std::vector<u8> payload;  // empty

    std::vector<std::vector<u8>> frags;
    fragment_message(99, payload, 256, frags);

    REQUIRE(frags.size() == 1u);
    // Header only — no data bytes.
    CHECK(frags[0].size() == kFragmentHeaderBytes);

    FragmentReassembler r;
    REQUIRE(r.add_fragment(frags[0]));
    REQUIRE(r.complete());

    std::vector<u8> out;
    out.assign(5, 0xAA);  // dirty it first to prove assemble() clears.
    REQUIRE(r.assemble(out));
    CHECK(out.empty());
}

TEST_CASE("net-fragment: a malformed too-short fragment is rejected",
          "[net]") {
    FragmentReassembler r;

    // Fewer than the 6 header bytes.
    std::vector<u8> stub = {0x00, 0x01, 0x02};
    CHECK_FALSE(r.add_fragment(stub));
    CHECK_FALSE(r.complete());

    // Exactly header-sized but with a zero fragment_count is also malformed.
    std::vector<u8> zero_count = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK_FALSE(r.add_fragment(zero_count));
    CHECK_FALSE(r.complete());
}

TEST_CASE("net-fragment: a duplicate fragment is ignored and completes once",
          "[net]") {
    const std::vector<u8> payload = ramp(500);

    std::vector<std::vector<u8>> frags;
    fragment_message(5, payload, 200, frags);
    REQUIRE(frags.size() == 3u);

    FragmentReassembler r;
    REQUIRE(r.add_fragment(frags[0]));
    // Re-send fragment 0 — duplicate, must be ignored (returns false).
    CHECK_FALSE(r.add_fragment(frags[0]));
    CHECK(r.received_count() == 1u);

    REQUIRE(r.add_fragment(frags[1]));
    REQUIRE(r.add_fragment(frags[2]));
    REQUIRE(r.complete());

    std::vector<u8> out;
    REQUIRE(r.assemble(out));
    REQUIRE(out.size() == payload.size());
    CHECK(std::memcmp(out.data(), payload.data(), out.size()) == 0);
}

TEST_CASE("net-fragment: a new message id resets a partial reassembly",
          "[net]") {
    const std::vector<u8> first  = ramp(400);
    const std::vector<u8> second = ramp(250);

    std::vector<std::vector<u8>> frags_a;
    std::vector<std::vector<u8>> frags_b;
    fragment_message(1, first, 150, frags_a);   // 3 fragments
    fragment_message(2, second, 150, frags_b);  // 2 fragments

    FragmentReassembler r;
    // Partially deliver message 1 (only its first fragment).
    REQUIRE(r.add_fragment(frags_a[0]));
    CHECK_FALSE(r.complete());

    // A fragment of message 2 arrives — discards the partial message 1.
    feed(r, frags_b);
    REQUIRE(r.current_message_id() == 2u);
    REQUIRE(r.complete());

    std::vector<u8> out;
    REQUIRE(r.assemble(out));
    REQUIRE(out.size() == second.size());
    CHECK(std::memcmp(out.data(), second.data(), out.size()) == 0);
}

TEST_CASE("net-fragment: complete and assemble gate correctly",
          "[net]") {
    const std::vector<u8> payload = ramp(900);

    std::vector<std::vector<u8>> frags;
    fragment_message(8, payload, 256, frags);
    REQUIRE(frags.size() == 4u);

    FragmentReassembler r;

    // Nothing received yet.
    CHECK_FALSE(r.complete());
    std::vector<u8> out;
    CHECK_FALSE(r.assemble(out));

    // All but the last fragment — still incomplete, assemble must refuse.
    for (usize i = 0; i + 1 < frags.size(); ++i) r.add_fragment(frags[i]);
    CHECK_FALSE(r.complete());
    CHECK_FALSE(r.assemble(out));

    // The final fragment completes it.
    REQUIRE(r.add_fragment(frags.back()));
    REQUIRE(r.complete());
    REQUIRE(r.assemble(out));
    CHECK(out.size() == payload.size());

    // reset() drops everything.
    r.reset();
    CHECK_FALSE(r.complete());
    CHECK_FALSE(r.assemble(out));
}

TEST_CASE("net-fragment: fragmenting the same payload twice is byte-identical",
          "[net][determinism]") {
    const std::vector<u8> payload = ramp(1234);

    std::vector<std::vector<u8>> a;
    std::vector<std::vector<u8>> b;
    fragment_message(17, payload, 257, a);
    fragment_message(17, payload, 257, b);

    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].size() == b[i].size());
        CHECK(std::memcmp(a[i].data(), b[i].data(), a[i].size()) == 0);
    }
}

TEST_CASE("net-fragment: header carries the expected little-endian fields",
          "[net]") {
    const std::vector<u8> payload = ramp(50);

    std::vector<std::vector<u8>> frags;
    // message_id 0x0102 = 258, exercising both header bytes.
    fragment_message(0x0102, payload, 20, frags);
    REQUIRE(frags.size() == 3u);  // ceil(50/20) == 3

    for (usize i = 0; i < frags.size(); ++i) {
        const std::vector<u8>& f = frags[i];
        REQUIRE(f.size() >= kFragmentHeaderBytes);

        // message_id, little-endian.
        CHECK(f[0] == 0x02u);
        CHECK(f[1] == 0x01u);
        // fragment_index, little-endian.
        CHECK(f[2] == static_cast<u8>(i));
        CHECK(f[3] == 0x00u);
        // fragment_count, little-endian.
        CHECK(f[4] == 0x03u);
        CHECK(f[5] == 0x00u);
    }
}
