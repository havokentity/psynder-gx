// SPDX-License-Identifier: MIT
// Psynder — flight recorder impl. See FlightRecorder.h.

#include "FlightRecorder.h"

#include "Profiler.h"  // prof::current_frame

#include <csignal>
#include <cstdio>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#endif

namespace psynder::diag {

const char* flight_category_name(FlightCategory cat) noexcept {
    switch (cat) {
        case FlightCategory::Engine:  return "engine";
        case FlightCategory::Frame:   return "frame";
        case FlightCategory::Render:  return "render";
        case FlightCategory::Physics: return "physics";
        case FlightCategory::Audio:   return "audio";
        case FlightCategory::Net:     return "net";
        case FlightCategory::Asset:   return "asset";
        case FlightCategory::Script:  return "script";
        case FlightCategory::Memory:  return "memory";
        case FlightCategory::Log:     return "log";
        case FlightCategory::User:    return "user";
        case FlightCategory::Count:   return "?";
    }
    return "?";
}

const char* flight_severity_name(FlightSeverity sev) noexcept {
    switch (sev) {
        case FlightSeverity::Trace: return "trace";
        case FlightSeverity::Info:  return "info";
        case FlightSeverity::Warn:  return "warn";
        case FlightSeverity::Error: return "error";
    }
    return "?";
}

namespace {

// Crash-target path. The recorder is a process-global singleton, so this is
// process-global too; install_crash_handler copies the caller's path in.
char g_crash_path[260] = "psynder_flight.log";

#if defined(_WIN32)
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_seh = nullptr;

LONG WINAPI crash_seh_filter(EXCEPTION_POINTERS* info) {
    FlightRecorder::get().record(FlightCategory::Engine, FlightSeverity::Error,
                                 "unhandled SEH exception");
    FlightRecorder::get().dump_to_file(g_crash_path);
    if (g_prev_seh) return g_prev_seh(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS};
constexpr usize kNumCrashSignals = sizeof(kCrashSignals) / sizeof(kCrashSignals[0]);
using SigHandler = void (*)(int);
SigHandler g_prev_sig[kNumCrashSignals] = {};

void crash_signal_handler(int sig) {
    FlightRecorder::get().record(FlightCategory::Engine, FlightSeverity::Error,
                                 "fatal signal");
    FlightRecorder::get().dump_to_file(g_crash_path);
    // Restore the prior disposition and re-raise so the default action (core
    // dump / terminate) still happens.
    for (usize i = 0; i < kNumCrashSignals; ++i) {
        if (kCrashSignals[i] == sig) {
            std::signal(sig, g_prev_sig[i] ? g_prev_sig[i] : SIG_DFL);
            break;
        }
    }
    std::raise(sig);
}
#endif

// Format one event line into `buf` (NUL-terminated). Returns bytes written
// (excluding the NUL), clamped to cap-1. Literal format string keeps
// -Wformat happy and snprintf bounded.
usize format_line(char* buf, usize cap, u64 micros, u64 frame,
                  FlightCategory cat, FlightSeverity sev, const char* text,
                  usize len) {
    if (cap == 0) return 0;
    const int wrote =
        std::snprintf(buf, cap, "%012llu  f%-8llu %-7s %-5s  %.*s\n",
                      static_cast<unsigned long long>(micros),
                      static_cast<unsigned long long>(frame),
                      flight_category_name(cat), flight_severity_name(sev),
                      static_cast<int>(len), text);
    if (wrote < 0) {
        buf[0] = '\0';
        return 0;
    }
    usize w = static_cast<usize>(wrote);
    if (w >= cap) w = cap - 1;  // snprintf truncated to fit
    return w;
}

void log_tap_sink(log::Level lv, const std::string& line) {
    const FlightSeverity sev = (lv == log::Level::Error) ? FlightSeverity::Error
                               : (lv == log::Level::Warn) ? FlightSeverity::Warn
                                                          : FlightSeverity::Info;
    FlightRecorder::get().record(FlightCategory::Log, sev,
                                 std::string_view(line));
}

}  // namespace

FlightRecorder::FlightRecorder() noexcept
    : epoch_(std::chrono::steady_clock::now()) {}

FlightRecorder& FlightRecorder::get() noexcept {
    static FlightRecorder inst;
    return inst;
}

void FlightRecorder::record(FlightCategory cat, FlightSeverity sev,
                            std::string_view msg) noexcept {
    const u64 ticket = write_seq_.fetch_add(1, std::memory_order_relaxed);
    Slot& slot       = ring_[ticket % kCapacity];

    usize n = msg.size();
    if (n > kMessageCap - 1) n = kMessageCap - 1;
    for (usize i = 0; i < n; ++i) slot.text[i] = msg[i];
    slot.text[n] = '\0';
    slot.len     = static_cast<u16>(n);
    slot.cat     = cat;
    slot.sev     = sev;
    slot.frame   = prof::current_frame();
    slot.micros  = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - epoch_)
            .count());
    // Publish last with release: a reader that acquire-loads this seq sees
    // every payload write above.
    slot.seq.store(ticket + 1, std::memory_order_release);
}

void FlightRecorder::record(FlightCategory cat, FlightSeverity sev,
                            const char* msg) noexcept {
    record(cat, sev, std::string_view(msg ? msg : ""));
}

bool FlightRecorder::read_event(u64 ticket, DecodedEvent& out) const noexcept {
    const Slot& s = ring_[ticket % kCapacity];
    if (s.seq.load(std::memory_order_acquire) != ticket + 1) return false;
    out.micros = s.micros;
    out.frame  = s.frame;
    out.cat    = s.cat;
    out.sev    = s.sev;
    usize len  = s.len;
    if (len > kMessageCap - 1) len = kMessageCap - 1;
    for (usize i = 0; i < len; ++i) out.text[i] = s.text[i];
    out.text[len] = '\0';
    out.len       = len;
    // Re-validate: if a far-future writer reused this slot mid-copy, discard.
    return s.seq.load(std::memory_order_acquire) == ticket + 1;
}

u64 FlightRecorder::total_recorded() const noexcept {
    return write_seq_.load(std::memory_order_acquire);
}

void FlightRecorder::clear() noexcept {
    for (auto& s : ring_) s.seq.store(0, std::memory_order_relaxed);
    write_seq_.store(0, std::memory_order_release);
}

std::string FlightRecorder::dump() const {
    const u64 total = write_seq_.load(std::memory_order_acquire);
    const u64 count = (total < kCapacity) ? total : kCapacity;
    const u64 start = total - count;

    std::string out;
    out.reserve(static_cast<usize>(count) * 48 + 96);

    char line[kMessageCap + 64];
    const int hn = std::snprintf(
        line, sizeof(line),
        "flight recorder: %llu events total, last %llu retained\n",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(count));
    if (hn > 0) {
        usize hl = static_cast<usize>(hn);
        if (hl >= sizeof(line)) hl = sizeof(line) - 1;
        out.append(line, hl);
    }

    DecodedEvent ev;
    for (u64 t = start; t < total; ++t) {
        if (!read_event(t, ev)) continue;
        const usize w = format_line(line, sizeof(line), ev.micros, ev.frame,
                                    ev.cat, ev.sev, ev.text, ev.len);
        out.append(line, w);
    }
    return out;
}

bool FlightRecorder::dump_to_file(const char* path) const noexcept {
    if (!path) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;

    const u64 total = write_seq_.load(std::memory_order_acquire);
    const u64 count = (total < kCapacity) ? total : kCapacity;
    const u64 start = total - count;

    char line[kMessageCap + 64];
    const int hn = std::snprintf(
        line, sizeof(line),
        "flight recorder: %llu events total, last %llu retained\n",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(count));
    if (hn > 0) {
        usize hl = static_cast<usize>(hn);
        if (hl >= sizeof(line)) hl = sizeof(line) - 1;
        std::fwrite(line, 1, hl, f);
    }

    DecodedEvent ev;
    for (u64 t = start; t < total; ++t) {
        if (!read_event(t, ev)) continue;
        const usize w = format_line(line, sizeof(line), ev.micros, ev.frame,
                                    ev.cat, ev.sev, ev.text, ev.len);
        std::fwrite(line, 1, w, f);
    }
    std::fclose(f);
    return true;
}

void FlightRecorder::install_log_tap() {
    if (log_tap_.exchange(true)) return;
    log::add_sink(&log_tap_sink);
}

void FlightRecorder::uninstall_log_tap() {
    if (!log_tap_.exchange(false)) return;
    log::remove_all_sinks();
}

bool FlightRecorder::log_tap_installed() const noexcept {
    return log_tap_.load(std::memory_order_relaxed);
}

void FlightRecorder::install_crash_handler(const char* crash_path) {
    if (crash_path) {
        usize i = 0;
        for (; crash_path[i] && i + 1 < sizeof(g_crash_path); ++i) {
            g_crash_path[i] = crash_path[i];
        }
        g_crash_path[i] = '\0';
    }
    if (crash_installed_.exchange(true)) return;
#if defined(_WIN32)
    g_prev_seh = SetUnhandledExceptionFilter(&crash_seh_filter);
#else
    for (usize i = 0; i < kNumCrashSignals; ++i) {
        g_prev_sig[i] = std::signal(kCrashSignals[i], &crash_signal_handler);
    }
#endif
}

void FlightRecorder::uninstall_crash_handler() {
    if (!crash_installed_.exchange(false)) return;
#if defined(_WIN32)
    SetUnhandledExceptionFilter(g_prev_seh);
    g_prev_seh = nullptr;
#else
    for (usize i = 0; i < kNumCrashSignals; ++i) {
        std::signal(kCrashSignals[i], g_prev_sig[i] ? g_prev_sig[i] : SIG_DFL);
        g_prev_sig[i] = nullptr;
    }
#endif
}

bool FlightRecorder::crash_handler_installed() const noexcept {
    return crash_installed_.load(std::memory_order_relaxed);
}

}  // namespace psynder::diag
