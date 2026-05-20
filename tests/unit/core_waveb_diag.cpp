// SPDX-License-Identifier: MIT
// Unit tests for the Wave B core diagnostics: allocator heatmap, flight
// recorder, profiler frame hook, and the diag console surface.
//
// TEST_CASE names are ASCII-only so ctest discovery is robust on Windows
// (see AGENTS.md). State lives in process-global singletons (Heatmap,
// FlightRecorder, Console, log sinks), so each case sets up the state it
// needs and cleans up what it registered.

#include "core/DiagCommands.h"
#include "core/FlightRecorder.h"
#include "core/Log.h"
#include "core/Profiler.h"
#include "core/alloc/Allocator.h"
#include "core/alloc/Heatmap.h"
#include "core/console/Console.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace mem  = psynder::mem;
namespace diag = psynder::diag;
namespace prof = psynder::prof;
namespace cn   = psynder::console;
namespace lg   = psynder::log;

using psynder::u32;
using psynder::usize;

namespace {
std::string_view sv(const char* p) { return std::string_view(p ? p : ""); }
}  // namespace

// ─── Allocator heatmap ──────────────────────────────────────────────────────

TEST_CASE("heatmap tag_name covers every tag in ASCII", "[core][diag][heatmap]") {
    REQUIRE(sv(mem::tag_name(mem::Tag::Render)) == "Render");
    REQUIRE(sv(mem::tag_name(mem::Tag::Misc)) == "Misc");
    for (u32 i = 0; i < static_cast<u32>(mem::Tag::Count); ++i) {
        REQUIRE(sv(mem::tag_name(static_cast<mem::Tag>(i))) != "?");
    }
}

TEST_CASE("heatmap samples a registered arena and remembers its peak",
          "[core][diag][heatmap]") {
    alignas(64) std::uint8_t buf[4096];
    mem::LinearArena arena(buf, sizeof buf, mem::Tag::Render);

    auto& hm           = mem::Heatmap::get();
    const usize before = hm.arena_count();

    REQUIRE(hm.register_arena(nullptr, &arena, mem::Tag::Render) == 0);
    REQUIRE(hm.register_arena("x", nullptr, mem::Tag::Render) == 0);

    const auto id = hm.register_arena("test_waveb_arena", &arena, mem::Tag::Render);
    REQUIRE(id != 0);
    REQUIRE(hm.arena_count() == before + 1);

    REQUIRE(arena.alloc(1000, 16) != nullptr);

    mem::HeatmapSnapshot snap;
    hm.sample(snap);

    // Tag rows are indexed by Tag value.
    for (usize i = 0; i < mem::kTagCount; ++i) {
        REQUIRE(snap.tags[i].tag == static_cast<mem::Tag>(i));
    }

    bool found = false;
    for (usize i = 0; i < snap.arena_count; ++i) {
        if (sv(snap.arenas[i].name) == "test_waveb_arena") {
            found = true;
            REQUIRE(snap.arenas[i].used >= 1000);
            REQUIRE(snap.arenas[i].capacity == sizeof buf);
            REQUIRE(snap.arenas[i].peak_used >= 1000);
            REQUIRE(snap.arenas[i].tag == mem::Tag::Render);
        }
    }
    REQUIRE(found);

    // The watermark survives an arena reset (the arena forgets, the heatmap
    // does not).
    arena.reset();
    hm.sample(snap);
    for (usize i = 0; i < snap.arena_count; ++i) {
        if (sv(snap.arenas[i].name) == "test_waveb_arena") {
            REQUIRE(snap.arenas[i].used == 0);
            REQUIRE(snap.arenas[i].peak_used >= 1000);
        }
    }

    hm.unregister_arena(id);
    REQUIRE(hm.arena_count() == before);
}

TEST_CASE("heatmap reset_peaks clears the per-arena watermark",
          "[core][diag][heatmap]") {
    alignas(64) std::uint8_t buf[2048];
    mem::LinearArena arena(buf, sizeof buf, mem::Tag::Misc);

    auto& hm      = mem::Heatmap::get();
    const auto id = hm.register_arena("peak_test_arena", &arena, mem::Tag::Misc);
    REQUIRE(id != 0);

    REQUIRE(arena.alloc(512, 16) != nullptr);
    mem::HeatmapSnapshot snap;
    hm.sample(snap);

    arena.reset();
    hm.reset_peaks();
    hm.sample(snap);
    for (usize i = 0; i < snap.arena_count; ++i) {
        if (sv(snap.arenas[i].name) == "peak_test_arena") {
            REQUIRE(snap.arenas[i].peak_used == 0);
        }
    }
    hm.unregister_arena(id);
}

TEST_CASE("heatmap format renders the tag table and a registered arena",
          "[core][diag][heatmap]") {
    alignas(64) std::uint8_t buf[2048];
    mem::LinearArena arena(buf, sizeof buf, mem::Tag::Audio);

    auto& hm      = mem::Heatmap::get();
    const auto id = hm.register_arena("fmt_test_arena", &arena, mem::Tag::Audio);
    REQUIRE(arena.alloc(256, 16) != nullptr);

    const std::string s = hm.format();
    REQUIRE(s.find("allocator heatmap") != std::string::npos);
    REQUIRE(s.find("Render") != std::string::npos);
    REQUIRE(s.find("Audio") != std::string::npos);
    REQUIRE(s.find("fmt_test_arena") != std::string::npos);
    REQUIRE(s.find("fill") != std::string::npos);

    hm.unregister_arena(id);
}

// ─── Flight recorder ────────────────────────────────────────────────────────

TEST_CASE("flight recorder records and dumps oldest-first",
          "[core][diag][flight]") {
    auto& fr = diag::FlightRecorder::get();
    fr.clear();
    REQUIRE(fr.total_recorded() == 0);

    fr.record(diag::FlightCategory::Engine, diag::FlightSeverity::Info, "alpha");
    fr.record(diag::FlightCategory::Render, diag::FlightSeverity::Warn, "beta");
    fr.recordf(diag::FlightCategory::Physics, diag::FlightSeverity::Error,
               "gamma {}", 42);

    REQUIRE(fr.total_recorded() == 3);

    const std::string d = fr.dump();
    const auto pa = d.find("alpha");
    const auto pb = d.find("beta");
    const auto pg = d.find("gamma 42");
    REQUIRE(pa != std::string::npos);
    REQUIRE(pb != std::string::npos);
    REQUIRE(pg != std::string::npos);
    REQUIRE(pa < pb);  // chronological order
    REQUIRE(pb < pg);
    REQUIRE(d.find("error") != std::string::npos);    // severity rendered
    REQUIRE(d.find("physics") != std::string::npos);  // category rendered
}

TEST_CASE("flight recorder retains only the last kCapacity events",
          "[core][diag][flight]") {
    auto& fr         = diag::FlightRecorder::get();
    const usize cap  = diag::FlightRecorder::kCapacity;
    fr.clear();
    for (usize i = 0; i < cap + 50; ++i) {
        fr.recordf(diag::FlightCategory::User, diag::FlightSeverity::Info,
                   "evt{}", i);
    }
    REQUIRE(fr.total_recorded() == cap + 50);

    const std::string d = fr.dump();
    REQUIRE(d.find("evt0\n") == std::string::npos);  // the oldest was dropped
    REQUIRE(d.find("evt" + std::to_string(cap + 49)) != std::string::npos);
}

TEST_CASE("flight recorder truncates an oversized message",
          "[core][diag][flight]") {
    auto& fr = diag::FlightRecorder::get();
    fr.clear();
    const std::string big(500, 'x');
    fr.record(diag::FlightCategory::User, diag::FlightSeverity::Info, big);

    const std::string d = fr.dump();
    const auto first = d.find('x');
    const auto last  = d.find_last_of('x');
    REQUIRE(first != std::string::npos);
    REQUIRE(last - first + 1 <= diag::FlightRecorder::kMessageCap - 1);
}

TEST_CASE("flight recorder writes and reads back a dump file",
          "[core][diag][flight]") {
    auto& fr = diag::FlightRecorder::get();
    fr.clear();
    fr.record(diag::FlightCategory::Net, diag::FlightSeverity::Warn, "diskline");

    const auto path =
        std::filesystem::temp_directory_path() / "psynder_waveb_flight.log";
    REQUIRE(fr.dump_to_file(path.string().c_str()));

    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    REQUIRE(f != nullptr);
    std::string content;
    char b[256];
    usize got = 0;
    while ((got = std::fread(b, 1, sizeof b, f)) > 0) content.append(b, got);
    std::fclose(f);

    REQUIRE(content.find("diskline") != std::string::npos);
    REQUIRE(content.find("net") != std::string::npos);

    std::filesystem::remove(path);
    REQUIRE_FALSE(fr.dump_to_file(nullptr));  // null path is rejected
}

TEST_CASE("flight recorder log tap captures emitted log lines",
          "[core][diag][flight]") {
    auto& fr = diag::FlightRecorder::get();
    lg::remove_all_sinks();  // start from a clean sink registry
    fr.clear();

    fr.install_log_tap();
    REQUIRE(fr.log_tap_installed());

    PSY_LOG_WARN("tapped {}", "line");

    const std::string d = fr.dump();
    REQUIRE(d.find("tapped line") != std::string::npos);
    REQUIRE(d.find("log") != std::string::npos);   // category
    REQUIRE(d.find("warn") != std::string::npos);  // severity

    fr.uninstall_log_tap();
    REQUIRE_FALSE(fr.log_tap_installed());
    lg::remove_all_sinks();  // leave the registry clean for other suites
}

TEST_CASE("flight recorder crash handler installs and uninstalls cleanly",
          "[core][diag][flight]") {
    auto& fr = diag::FlightRecorder::get();
    REQUIRE_FALSE(fr.crash_handler_installed());

    fr.install_crash_handler("psynder_waveb_crash.log");
    REQUIRE(fr.crash_handler_installed());
    fr.install_crash_handler("psynder_waveb_crash.log");  // idempotent
    REQUIRE(fr.crash_handler_installed());

    fr.uninstall_crash_handler();
    REQUIRE_FALSE(fr.crash_handler_installed());
}

// ─── Profiler frame hook ────────────────────────────────────────────────────

TEST_CASE("profiler end_frame advances the frame counter",
          "[core][diag][profiler]") {
    const auto f0 = prof::current_frame();
    const auto f1 = prof::end_frame();
    REQUIRE(f1 == f0 + 1);
    REQUIRE(prof::current_frame() == f1);
    prof::end_frame();
    REQUIRE(prof::current_frame() == f1 + 1);

    // No-ops without Tracy, but must remain callable / linkable.
    prof::plot_memory();
    prof::set_thread_name("waveb_test_thread");
}

// ─── Diag console surface ───────────────────────────────────────────────────

TEST_CASE("diag console commands dump the heatmap and flight recorder",
          "[core][diag][console]") {
    auto& C = cn::Console::Get();
    diag::register_console_commands(C);
    REQUIRE(C.FindCommand("mem_heatmap") != nullptr);
    REQUIRE(C.FindCommand("flightrecorder") != nullptr);

    const auto r1 = C.Execute("mem_heatmap");
    REQUIRE(r1.ok);
    REQUIRE(r1.output.find("allocator heatmap") != std::string::npos);

    auto& fr = diag::FlightRecorder::get();
    fr.clear();
    fr.record(diag::FlightCategory::User, diag::FlightSeverity::Info,
              "consoleline");

    const auto r2 = C.Execute("flightrecorder");
    REQUIRE(r2.ok);
    REQUIRE(r2.output.find("consoleline") != std::string::npos);

    const auto r3 = C.Execute("flightrecorder clear");
    REQUIRE(r3.ok);
    REQUIRE(r3.output.find("cleared") != std::string::npos);
    REQUIRE(fr.total_recorded() == 0);

    // Re-registration is a no-op (commands already present).
    diag::register_console_commands(C);
    REQUIRE(C.FindCommand("mem_heatmap") != nullptr);
}
