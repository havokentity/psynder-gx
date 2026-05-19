// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/linux/LinuxCommon.cpp
//
// Lane 24 — utilities shared by all Linux build modes (graphical + dedicated server):
//   - Session-type detection (Wayland vs X11)
//   - Signal handler installation + quit flag
//   - Monotonic clock (CLOCK_MONOTONIC_RAW)
//   - Filesystem helpers (executable_path, user_config_dir, user_data_dir)
//
// All code is guarded by #if PSYNDER_PLATFORM_LINUX.
// No Vulkan / Wayland / XCB headers included here — those live in their
// respective translation units.

#if PSYNDER_PLATFORM_LINUX

#include "LinuxPlatform.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>

#include <unistd.h>    // readlink, access
#include <signal.h>    // sigaction
#include <time.h>      // clock_gettime, CLOCK_MONOTONIC_RAW
#include <sys/stat.h>  // mkdir

namespace psynder::platform::linux_platform {

// ─── Session-type detection ───────────────────────────────────────────────

bool is_wayland_session() noexcept
{
    // Check WAYLAND_DISPLAY first — the most reliable indicator that a
    // Wayland compositor is running.
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    if (wayland_display && wayland_display[0] != '\0') {
        return true;
    }

    // Fallback: XDG_SESSION_TYPE=wayland (set by login managers on Ubuntu).
    const char* session_type = std::getenv("XDG_SESSION_TYPE");
    if (session_type && std::strcmp(session_type, "wayland") == 0) {
        return true;
    }

    return false;
}

// ─── Signal handling + quit flag ─────────────────────────────────────────

static std::atomic<bool> g_quit_flag{false};

static void signal_handler(int /*sig*/) noexcept
{
    g_quit_flag.store(true, std::memory_order_relaxed);
}

void install_signal_handlers()
{
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
    // SIGPIPE is common in network code — ignore it; the net lane handles
    // per-socket errors via return values.
    signal(SIGPIPE, SIG_IGN);
}

bool should_quit() noexcept
{
    return g_quit_flag.load(std::memory_order_relaxed);
}

// ─── Monotonic clock ─────────────────────────────────────────────────────

std::uint64_t clock_ns()
{
    struct timespec ts{};
    // CLOCK_MONOTONIC_RAW: not adjusted by NTP, suitable for delta timing.
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec)  * 1'000'000'000ULL
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

double clock_s()
{
    return static_cast<double>(clock_ns()) * 1e-9;
}

// ─── Filesystem helpers ───────────────────────────────────────────────────

std::string executable_path()
{
    // /proc/self/exe is available on Linux 2.2+.
    char buf[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0) {
        std::fprintf(stderr,
            "[platform-linux] executable_path: readlink failed: %s\n",
            std::strerror(errno));
        return {};
    }
    buf[len] = '\0';
    return std::string{buf};
}

std::string user_config_dir()
{
    // XDG Base Directory Specification §3.
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0') {
            return {};
        }
        base = std::string{home} + "/.config";
    }
    std::string dir = base + "/psynder-gx";
    // Attempt to create if missing (best-effort; errors silently ignored).
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::string user_data_dir()
{
    // XDG Base Directory Specification §3.
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0') {
            return {};
        }
        base = std::string{home} + "/.local/share";
    }
    std::string dir = base + "/psynder-gx";
    mkdir(dir.c_str(), 0755);
    return dir;
}

} // namespace psynder::platform::linux_platform

#endif // PSYNDER_PLATFORM_LINUX
