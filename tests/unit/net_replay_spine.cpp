// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / replay-spine hook tests.
//
// The integration sample needs one net-owned handoff point: per tick, record
// the ECS snapshot bytes plus a deterministic command stream. These tests
// keep that contract small and prove it is independent of caller iteration
// order at both supported server tick rates.

#include <catch2/catch_test_macros.hpp>

#include "net/DemoReader.h"
#include "net/DemoWriter.h"
#include "net/Snapshot.h"
#include "net/TickConfig.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

std::string tmp_replay_path(u32 hz, const char* suffix) {
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path()
               / ("psynder_gx_replay_spine_" + std::to_string(hz) + "_" + suffix
                  + ".psydem");
    std::error_code ec;
    fs::remove(p, ec);
    return p.string();
}

SnapshotFrame make_snapshot(u32 tick) {
    SnapshotFrame s{};
    s.tick = tick;
    s.entities.push_back(SnapshotEntity{
        100u,
        math::Vec3{static_cast<f32>(tick), 2.0f, 3.0f},
        0xA000u | tick});
    s.entities.push_back(SnapshotEntity{
        200u,
        math::Vec3{4.0f, static_cast<f32>(tick * 2u), 6.0f},
        0xB000u | tick});
    return s;
}

std::vector<PlayerInputEntry> make_commands(u32 tick, bool reverse_order) {
    std::vector<PlayerInputEntry> commands(3);

    commands[0].entity_id    = 300u;
    commands[0].aim_frac_u16 = static_cast<u16>(tick * 31u);
    commands[0].button_mask  = 0x0001u;
    commands[0].yaw_delta    = static_cast<i16>(tick * 2u);
    commands[0].weapon_slot  = 2u;

    commands[1].entity_id    = 100u;
    commands[1].aim_frac_u16 = static_cast<u16>(tick * 17u);
    commands[1].button_mask  = 0x0004u;
    commands[1].pitch_delta  = static_cast<i16>(-static_cast<i32>(tick));
    commands[1].weapon_slot  = 1u;

    commands[2].entity_id    = 200u;
    commands[2].aim_frac_u16 = static_cast<u16>(tick * 23u);
    commands[2].button_mask  = 0x0002u;
    commands[2].yaw_delta    = static_cast<i16>(tick * 3u);
    commands[2].weapon_slot  = 0u;

    if (reverse_order) {
        std::reverse(commands.begin(), commands.end());
    }
    return commands;
}

struct ReplayReadback {
    std::vector<std::vector<u8>> snapshots;
    std::vector<std::vector<PlayerInputEntry>> inputs;
};

void write_replay_demo(const std::string& path,
                       const TickConfig& cfg,
                       bool reverse_order) {
    DemoWriterConfig wcfg{};
    wcfg.path              = path.c_str();
    wcfg.tick_cfg          = cfg;
    wcfg.map_id            = 0x5151u;
    wcfg.server_build      = 7u;
    wcfg.keyframe_interval = 2u;

    DemoWriter writer;
    REQUIRE(writer.open(wcfg));
    for (u32 tick = 1; tick <= 5; ++tick) {
        SnapshotFrame snapshot = make_snapshot(tick);
        std::vector<PlayerInputEntry> commands =
            make_commands(tick, reverse_order);
        REQUIRE(writer.write_replay_tick(
            snapshot,
            /*baseline_tick=*/0u,
            std::span<const PlayerInputEntry>(commands.data(), commands.size())));
    }
    writer.finalise();
}

ReplayReadback read_replay_demo(const std::string& path,
                                const TickConfig& expected_cfg) {
    DemoReader reader;
    REQUIRE(reader.open(path.c_str()) == DemoReaderResult::Ok);
    CHECK(reader.tick_config().tick_hz == expected_cfg.tick_hz);

    ReplayReadback out{};
    while (true) {
        DemoTickData td{};
        const DemoReaderResult rr = reader.advance_tick(td);
        if (rr == DemoReaderResult::EndOfFile) {
            break;
        }
        REQUIRE(rr == DemoReaderResult::Ok);
        REQUIRE(td.has_frame);

        SnapshotFrame decoded{};
        REQUIRE(decode_snapshot(
            std::span<const u8>(td.snapshot_bytes.data(), td.snapshot_bytes.size()),
            decoded));
        CHECK(decoded.tick == td.tick);
        REQUIRE(decoded.entities.size() == 2u);
        CHECK(decoded.entities[0].entity_id == 100u);
        CHECK(decoded.entities[1].entity_id == 200u);

        out.snapshots.push_back(td.snapshot_bytes);
        out.inputs.push_back(td.inputs);
    }
    return out;
}

}  // namespace

TEST_CASE("net: replay spine canonicalizes commands and snapshots",
          "[net][replay][demo]") {
    const TickConfig cfgs[] = {tick_config_64(), tick_config_128()};

    for (const TickConfig& cfg : cfgs) {
        DYNAMIC_SECTION("tick_hz=" << cfg.tick_hz) {
            const std::string a_path = tmp_replay_path(cfg.tick_hz, "a");
            const std::string b_path = tmp_replay_path(cfg.tick_hz, "b");

            write_replay_demo(a_path, cfg, /*reverse_order=*/false);
            write_replay_demo(b_path, cfg, /*reverse_order=*/true);

            ReplayReadback a = read_replay_demo(a_path, cfg);
            ReplayReadback b = read_replay_demo(b_path, cfg);

            REQUIRE(a.snapshots.size() == 5u);
            REQUIRE(a.inputs.size() == 5u);
            CHECK(a.snapshots == b.snapshots);
            REQUIRE(a.inputs.size() == b.inputs.size());

            for (usize i = 0; i < a.inputs.size(); ++i) {
                REQUIRE(a.inputs[i].size() == 3u);
                REQUIRE(b.inputs[i].size() == 3u);
                CHECK(a.inputs[i][0].entity_id == 100u);
                CHECK(a.inputs[i][1].entity_id == 200u);
                CHECK(a.inputs[i][2].entity_id == 300u);
                for (usize j = 0; j < a.inputs[i].size(); ++j) {
                    CHECK(a.inputs[i][j].entity_id == b.inputs[i][j].entity_id);
                    CHECK(a.inputs[i][j].aim_frac_u16 == b.inputs[i][j].aim_frac_u16);
                    CHECK(a.inputs[i][j].button_mask == b.inputs[i][j].button_mask);
                    CHECK(a.inputs[i][j].pitch_delta == b.inputs[i][j].pitch_delta);
                    CHECK(a.inputs[i][j].yaw_delta == b.inputs[i][j].yaw_delta);
                    CHECK(a.inputs[i][j].weapon_slot == b.inputs[i][j].weapon_slot);
                }
            }

            std::error_code ec;
            std::filesystem::remove(a_path, ec);
            std::filesystem::remove(b_path, ec);
        }
    }
}

TEST_CASE("net: demo reader rejects unsupported replay tick rates",
          "[net][replay][demo]") {
    const std::string path = tmp_replay_path(96u, "bad_hz");

    DemoWriterConfig wcfg{};
    wcfg.path     = path.c_str();
    wcfg.tick_cfg = tick_config_64();

    DemoWriter writer;
    REQUIRE(writer.open(wcfg));
    SnapshotFrame snapshot = make_snapshot(1u);
    REQUIRE(writer.write_replay_tick(
        snapshot,
        /*baseline_tick=*/0u,
        std::span<const PlayerInputEntry>()));
    writer.finalise();

    std::FILE* f = std::fopen(path.c_str(), "rb+");
    REQUIRE(f != nullptr);
    REQUIRE(std::fseek(f, 6, SEEK_SET) == 0);
    const u8 unsupported_hz = 96u;
    REQUIRE(std::fwrite(&unsupported_hz, 1, 1, f) == 1);
    std::fclose(f);

    DemoReader reader;
    CHECK(reader.open(path.c_str()) == DemoReaderResult::CorruptFile);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
