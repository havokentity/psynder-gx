// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / .psydem writer → reader round-trip test.
//
// Writes a synthetic match (10 ticks of snapshots + inputs) to a temporary
// .psydem file, then reads it back via DemoReader and verifies every
// snapshot reconstructs byte-for-byte. Also exercises seek_to_tick().

#include <catch2/catch_test_macros.hpp>

#include "net/DemoFormat.h"
#include "net/DemoReader.h"
#include "net/DemoWriter.h"
#include "net/TickConfig.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

std::string tmp_demo_path() {
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path() / "psynder_gx_demo_roundtrip.psydem";
    // Clear any leftover from a previous run.
    std::error_code ec;
    fs::remove(p, ec);
    return p.string();
}

}  // namespace

TEST_CASE("net: .psydem write then read reconstructs every frame",
          "[net][demo][roundtrip]") {
    const std::string path = tmp_demo_path();

    DemoWriterConfig wcfg{};
    wcfg.path           = path.c_str();
    wcfg.tick_cfg       = tick_config_128();
    wcfg.map_id         = 0xABCDu;
    wcfg.server_build   = 1u;
    wcfg.match_start_ns = 1'700'000'000ull * 1'000'000'000ull;
    wcfg.player_count   = 2;
    wcfg.roster[0] = DemoRosterEntry{0x11ull, 100u, 0xDEADBEEFu};
    wcfg.roster[1] = DemoRosterEntry{0x22ull, 200u, 0xBADC0DE0u};
    wcfg.keyframe_interval = 4;

    DemoWriter w;
    REQUIRE(w.open(wcfg));

    // 10 ticks of synthetic snapshots. Each snapshot is 32 bytes; we vary
    // a few bytes per tick so the XOR delta does meaningful work.
    constexpr u32 kTicks = 10;
    constexpr usize kSnapBytes = 32;
    std::vector<std::vector<u8>> snapshots(kTicks, std::vector<u8>(kSnapBytes));
    for (u32 t = 0; t < kTicks; ++t) {
        std::vector<u8>& s = snapshots[t];
        for (usize i = 0; i < kSnapBytes; ++i) {
            s[i] = static_cast<u8>((i * 7u + t * 13u) & 0xFFu);
        }
    }

    std::vector<PlayerInputEntry> inputs(2);
    inputs[0].entity_id   = 100u;
    inputs[1].entity_id   = 200u;
    for (u32 t = 0; t < kTicks; ++t) {
        const u32 tick = t + 1u;
        inputs[0].aim_frac_u16 = static_cast<u16>(tick * 257u);
        inputs[0].yaw_delta    = static_cast<i16>(tick * 11);
        inputs[1].aim_frac_u16 = static_cast<u16>(tick * 511u);
        inputs[1].pitch_delta  = static_cast<i16>(static_cast<i32>(tick) * -7);
        CHECK(w.write_frame(tick,
                            /*baseline_tick=*/0u,  // let writer decide
                            std::span<const u8>(snapshots[t].data(),
                                                snapshots[t].size())));
        CHECK(w.write_inputs(tick,
                             std::span<const PlayerInputEntry>(inputs.data(),
                                                               inputs.size())));
    }
    w.finalise();

    // Reader.
    DemoReader r;
    REQUIRE(r.open(path.c_str()) == DemoReaderResult::Ok);
    CHECK(r.header().map_id == 0xABCDu);
    CHECK(r.header().player_count == 2);
    CHECK(r.tick_config().tick_hz == 128u);
    CHECK(r.roster().size() == 2u);
    CHECK(r.roster()[0].entity_id == 100u);
    CHECK(r.roster()[1].entity_id == 200u);

    for (u32 t = 0; t < kTicks; ++t) {
        DemoTickData td{};
        const auto rr = r.advance_tick(td);
        REQUIRE(rr == DemoReaderResult::Ok);
        CHECK(td.tick == t + 1u);
        REQUIRE(td.has_frame);
        REQUIRE(td.snapshot_bytes.size() == kSnapBytes);
        const bool match = std::equal(td.snapshot_bytes.begin(),
                                      td.snapshot_bytes.end(),
                                      snapshots[t].begin());
        INFO("tick=" << td.tick << " is_keyframe=" << td.is_keyframe);
        CHECK(match);
        REQUIRE(td.inputs.size() == 2u);
        CHECK(td.inputs[0].entity_id == 100u);
        CHECK(td.inputs[1].entity_id == 200u);
    }
    DemoTickData eof_td{};
    CHECK(r.advance_tick(eof_td) == DemoReaderResult::EndOfFile);

    // Clean up the temp file.
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("net: .psydem seek_to_tick lands on the keyframe boundary",
          "[net][demo][seek]") {
    const std::string path = tmp_demo_path();
    DemoWriterConfig wcfg{};
    wcfg.path             = path.c_str();
    wcfg.tick_cfg         = tick_config_64();
    wcfg.player_count     = 0;
    wcfg.keyframe_interval = 3;

    DemoWriter w;
    REQUIRE(w.open(wcfg));
    constexpr usize kSnapBytes = 16;
    for (u32 t = 1; t <= 9; ++t) {
        std::vector<u8> s(kSnapBytes, static_cast<u8>(t));
        REQUIRE(w.write_frame(t, 0u,
                              std::span<const u8>(s.data(), s.size())));
    }
    w.finalise();

    DemoReader r;
    REQUIRE(r.open(path.c_str()) == DemoReaderResult::Ok);
    CHECK(r.header().end_tick == 9u);
    // Keyframes land at ticks 1, 4, 7 (interval = 3). Seek to 6: TOC
    // entry should be 4.
    REQUIRE(r.seek_to_tick(6u) == DemoReaderResult::Ok);

    DemoTickData td{};
    REQUIRE(r.advance_tick(td) == DemoReaderResult::Ok);
    // After seek to 6, advance picks up tick 4 (the latest keyframe at or
    // before 6).
    CHECK(td.tick == 4u);
    CHECK(td.is_keyframe);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
