// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/net_sequence_buffer.cpp — wraparound-safe 16-bit sequence
// comparison + the sequence-indexed ring buffer.

#include "net/SequenceBuffer.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::net;

TEST_CASE("seq: greater handles 16-bit wraparound", "[net]") {
    CHECK(seq_greater(100, 50));
    CHECK_FALSE(seq_greater(50, 100));
    // Across the wrap: 0 is "more recent" than 65535 (it came after).
    CHECK(seq_greater(0, 65535));
    CHECK_FALSE(seq_greater(65535, 0));
    // Equal is not strictly greater.
    CHECK_FALSE(seq_greater(7, 7));
}

TEST_CASE("seq: diff is the signed shortest distance", "[net]") {
    CHECK(seq_diff(100, 50) == 50);
    CHECK(seq_diff(50, 100) == -50);
    CHECK(seq_diff(0, 65535) == 1);   // 0 is one ahead of 65535
    CHECK(seq_diff(65535, 0) == -1);
    CHECK(seq_diff(7, 7) == 0);
}

TEST_CASE("seq buffer: insert and find by sequence", "[net]") {
    SequenceBuffer<int> buf(8);
    CHECK_FALSE(buf.has_latest());
    int* a = buf.insert(2);
    REQUIRE(a != nullptr);
    *a = 22;
    int* b = buf.insert(3);
    REQUIRE(b != nullptr);
    *b = 33;
    CHECK(buf.has_latest());
    CHECK(buf.latest() == 3);
    REQUIRE(buf.find(2) != nullptr);
    CHECK(*buf.find(2) == 22);
    CHECK(*buf.find(3) == 33);
    CHECK(buf.exists(2));
    CHECK_FALSE(buf.exists(4));  // never inserted
}

TEST_CASE("seq buffer: a much newer insert invalidates stale slots", "[net]") {
    SequenceBuffer<int> buf(8);
    int* p2 = buf.insert(2);
    REQUIRE(p2 != nullptr);
    *p2 = 22;
    REQUIRE(buf.find(2) != nullptr);

    // 10 maps to the same slot as 2 (10 % 8 == 2). Inserting it (newer) must
    // clear the skipped span so the old seq-2 entry is not a false positive.
    int* p10 = buf.insert(10);
    REQUIRE(p10 != nullptr);
    *p10 = 100;
    CHECK(buf.find(2) == nullptr);  // stale 2 is gone, not mistaken for 10
    REQUIRE(buf.find(10) != nullptr);
    CHECK(*buf.find(10) == 100);
    CHECK(buf.latest() == 10);
}

TEST_CASE("seq buffer: a too-old insert is rejected", "[net]") {
    SequenceBuffer<int> buf(8);
    REQUIRE(buf.insert(20) != nullptr);  // latest = 20
    // 12 is exactly capacity (8) behind => outside the window => rejected.
    CHECK(buf.insert(12) == nullptr);
    // 13 is 7 behind => still in the window => accepted.
    int* p = buf.insert(13);
    REQUIRE(p != nullptr);
    *p = 13;
    CHECK(buf.exists(13));
    CHECK(buf.latest() == 20);  // an older insert does not advance latest
}

TEST_CASE("seq buffer: reset clears the ring", "[net]") {
    SequenceBuffer<int> buf(8);
    *buf.insert(5) = 55;
    REQUIRE(buf.exists(5));
    buf.reset();
    CHECK_FALSE(buf.has_latest());
    CHECK_FALSE(buf.exists(5));
    CHECK(buf.find(5) == nullptr);
}

TEST_CASE("seq buffer: identical op sequences yield identical state",
          "[net][determinism]") {
    SequenceBuffer<int> a(16), b(16);
    for (u16 s = 0; s < 40; ++s) {
        int* pa = a.insert(s);
        int* pb = b.insert(s);
        if (pa) *pa = static_cast<int>(s) * 3;
        if (pb) *pb = static_cast<int>(s) * 3;
    }
    CHECK(a.latest() == b.latest());
    for (u16 s = 24; s < 40; ++s) {
        const int* fa = a.find(s);
        const int* fb = b.find(s);
        REQUIRE((fa != nullptr) == (fb != nullptr));
        if (fa) CHECK(*fa == *fb);
    }
}
