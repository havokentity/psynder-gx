// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/macos/MacosPlatformFs.cpp
//
// Lane 25 — macOS process / filesystem helpers in plain C++ (no AppKit,
// no Foundation). Lives in a separate TU so the same helpers are usable
// from headless test binaries without pulling Cocoa.

#include "MacosPlatform_internal.h"

#include <mach-o/dyld.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::platform::macos {

std::string fs_executable_path() {
    // _NSGetExecutablePath returns the requested length on truncation. Two
    // calls: first sizes, second fills. Cap at the Darwin path limit of
    // 1024 bytes (PATH_MAX); realpath canonicalises symlinks (Homebrew etc).
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, 0);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::array<char, 1024> resolved{};
    if (realpath(buf.data(), resolved.data()) != nullptr) {
        return std::string{resolved.data()};
    }
    return std::string{buf.data()};
}

std::string fs_user_config_dir() {
    // ~/Library/Application Support/Psynder-GX per Apple's File System
    // Programming Guide. Fall back to $HOME / /tmp when home is missing so
    // callers always receive a usable path.
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        if (passwd* pw = getpwuid(getuid())) home = pw->pw_dir;
    }
    std::string base = (home && *home) ? std::string{home} : std::string{"/tmp"};
    return base + "/Library/Application Support/Psynder-GX";
}

std::string fs_current_working_directory() {
    std::error_code ec;
    auto p = std::filesystem::current_path(ec);
    if (ec) return {};
    return p.string();
}

bool fs_file_exists(std::string_view path) {
    if (path.empty()) return false;
    std::string p{path};
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

// ─── String-cache accessors for the C-style public API ──────────────────
// The public PublicPlatformMacos.h exposes `const char*` returns; we cache
// them in process-local statics so each accessor hands out a pointer valid
// until the next call.
namespace {
std::string& exe_cache()   { static std::string s; return s; }
std::string& cfg_cache()   { static std::string s; return s; }
std::string& cwd_cache()   { static std::string s; return s; }
} // namespace

const char* executable_path() {
    exe_cache() = fs_executable_path();
    return exe_cache().c_str();
}
const char* user_config_dir() {
    cfg_cache() = fs_user_config_dir();
    return cfg_cache().c_str();
}
const char* current_working_directory() {
    cwd_cache() = fs_current_working_directory();
    return cwd_cache().c_str();
}
bool file_exists(const char* path) {
    return path && fs_file_exists(std::string_view{path});
}

} // namespace psynder::platform::macos
