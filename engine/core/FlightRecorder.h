// SPDX-License-Identifier: MIT
// Psynder — flight recorder: a fixed-capacity ring of the most recent engine
// events / log lines, dumpable on demand (console, editor) or from a crash
// handler. Think of it as the black box: when something goes wrong you want
// the last few hundred things that happened, not a fresh empty log.
//
// Design notes:
//   - The ring lives in static storage (not the heap), so it survives heap
//     corruption and is intact inside a crash handler.
//   - record() is lock-free and allocation-free: a relaxed fetch_add hands
//     out a slot ticket, the payload is written, then a release-store on the
//     slot's sequence number publishes it. Messages are copied into a fixed
//     inline buffer (truncated past kMessageCap-1) — no per-event heap.
//   - Reads (dump) are best-effort: under heavy concurrent recording a slot
//     can tear into a garbled-but-bounded line. That is the deliberate
//     diagnostics posture (same as the allocator's racy peak counter); the
//     recorder never gates anything on its own data.
//
// Builds on the existing Log sink API and the Profiler frame counter; it does
// not modify either.

#pragma once

#include "Log.h"
#include "Types.h"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace psynder::diag {

// Event source. One byte; rendered as a short tag in the dump.
enum class FlightCategory : u8 {
    Engine,
    Frame,
    Render,
    Physics,
    Audio,
    Net,
    Asset,
    Script,
    Memory,
    Log,
    User,
    Count,
};

enum class FlightSeverity : u8 {
    Trace,
    Info,
    Warn,
    Error,
};

const char* flight_category_name(FlightCategory cat) noexcept;
const char* flight_severity_name(FlightSeverity sev) noexcept;

class FlightRecorder {
public:
    // Ring depth (power of two) and per-event message cap (bytes incl. NUL).
    static constexpr usize kCapacity   = 512;
    static constexpr usize kMessageCap = 120;

    static FlightRecorder& get() noexcept;

    // Record one event. Lock-free, allocation-free. `msg` is copied and
    // truncated to kMessageCap-1 bytes.
    void record(FlightCategory cat, FlightSeverity sev,
                std::string_view msg) noexcept;
    void record(FlightCategory cat, FlightSeverity sev,
                const char* msg) noexcept;

    // fmt-formatted record. Formats into a stack buffer (no heap) and
    // truncates. Prefer the PSY_FLIGHT macro at call sites.
    template <class... Args>
    void recordf(FlightCategory cat, FlightSeverity sev,
                 fmt::format_string<Args...> f, Args&&... args) noexcept {
        char tmp[kMessageCap];
        auto res = fmt::format_to_n(tmp, kMessageCap - 1, f,
                                    std::forward<Args>(args)...);
        record(cat, sev,
               std::string_view(tmp, static_cast<usize>(res.out - tmp)));
    }

    // Total events recorded since construction (monotonic). Only the last
    // kCapacity are retained.
    u64 total_recorded() const noexcept;

    // Drop all retained events. Admin op — not safe against concurrent record.
    void clear() noexcept;

    // Render retained events oldest -> newest into a string (on demand).
    std::string dump() const;

    // Write a header + the retained events to `path`. Returns true on success.
    // Uses stdio, so it is NOT strictly async-signal-safe; it is nonetheless
    // the routine the crash handler calls (best-effort — the ring data itself
    // is in static storage and intact). For the on-demand "save" path it is
    // perfectly safe.
    bool dump_to_file(const char* path) const noexcept;

    // ─── Log tap ───────────────────────────────────────────────────────────
    // Forward every psynder::log line into the recorder (FlightCategory::Log).
    // install adds a sink; uninstall calls log::remove_all_sinks() — the log
    // API only supports remove-all — so only uninstall when the recorder owns
    // the sink set.
    void install_log_tap();
    void uninstall_log_tap();
    bool log_tap_installed() const noexcept;

    // ─── Crash handler ───────────────────────────────────────────────────────
    // Install POSIX signal / Windows SEH handlers that dump the ring to
    // `crash_path` and then chain to the previously-installed handler (POSIX:
    // restore + re-raise to keep the default core/terminate action). The path
    // is copied into a fixed internal buffer. uninstall restores the prior
    // handlers.
    void install_crash_handler(const char* crash_path = "psynder_flight.log");
    void uninstall_crash_handler();
    bool crash_handler_installed() const noexcept;

private:
    FlightRecorder() noexcept;

    struct Slot {
        std::atomic<u64> seq{0};  // 0 = empty; otherwise ticket + 1 (publish)
        u64              micros = 0;
        u64              frame  = 0;
        FlightCategory   cat    = FlightCategory::Engine;
        FlightSeverity   sev    = FlightSeverity::Info;
        u16              len    = 0;
        char             text[kMessageCap]{};
    };

    // A slot decoded into stable locals by a seqlock read.
    struct DecodedEvent {
        u64            micros = 0;
        u64            frame  = 0;
        FlightCategory cat    = FlightCategory::Engine;
        FlightSeverity sev    = FlightSeverity::Info;
        usize          len    = 0;
        char           text[kMessageCap]{};
    };

    // Read the slot holding `ticket` (= the t-th event). Returns false if the
    // slot is empty or was overwritten by a newer event mid-read.
    bool read_event(u64 ticket, DecodedEvent& out) const noexcept;

    std::atomic<u64>                      write_seq_{0};
    Slot                                  ring_[kCapacity];
    std::chrono::steady_clock::time_point epoch_;
    std::atomic<bool>                     log_tap_{false};
    std::atomic<bool>                     crash_installed_{false};
};

}  // namespace psynder::diag

// Convenience: PSY_FLIGHT(cat, sev, "fmt {}", args...) records a formatted
// event into the global recorder. Compiles to a single recordf() call.
#define PSY_FLIGHT(cat, sev, ...)                                              \
    ::psynder::diag::FlightRecorder::get().recordf((cat), (sev), __VA_ARGS__)
