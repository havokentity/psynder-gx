// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/linux/LinuxEvdev.cpp
//
// Lane 24 — evdev raw mouse + keyboard input via /dev/input/event*.
//
// Works in both graphical (Wayland/X11) and dedicated-server builds.
// Uses the Linux input_event ABI directly; no libevdev dependency required
// (though libevdev is preferred for a future cleanup — flag for M1).
//
// DESIGN §11.2 — Raw input via evdev.
//
// NEEDS PC VALIDATION:
//  - Requires the running user to be in the `input` group or have a
//    udev rule granting read access to /dev/input/event*.
//  - Tested device enumeration pattern assumes /dev/input/event[0-31].
//  - On Wayland the compositor may grab devices; relative motion still
//    works through the Wayland pointer protocol, but raw evdev is useful
//    for locked-mouse (FPS) mode.  NEEDS PC VALIDATION for pointer-lock
//    interaction.

#if PSYNDER_PLATFORM_LINUX

#include "LinuxPlatform.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>

// Linux input subsystem headers.
#include <linux/input.h>
#include <linux/input-event-codes.h>

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <poll.h>

namespace psynder::platform::linux_platform {

// ─── Constants ────────────────────────────────────────────────────────────

static constexpr int   k_max_devices = 32;
static constexpr char  k_input_dir[] = "/dev/input/";
static constexpr char  k_event_prefix[] = "event";

// ─── EvdevReader impl ─────────────────────────────────────────────────────

struct EvdevReader {
    int fds[k_max_devices];
    int fd_count = 0;

    // Current key state (bit array, KEY_CNT bits).
    // KEY_CNT is defined in linux/input.h (~768 keys max).
    std::uint8_t key_state[(KEY_CNT + 7) / 8] = {};

    // Accumulated mouse delta (reset by evdev_poll_mouse each call).
    std::int32_t acc_dx       = 0;
    std::int32_t acc_dy       = 0;
    std::int32_t acc_wheel = 0;
};

// ─── Public: evdev_open ───────────────────────────────────────────────────

EvdevReader* evdev_open()
{
    DIR* dir = opendir(k_input_dir);
    if (!dir) {
        std::fprintf(stderr,
            "[platform-linux] evdev_open: opendir(%s) failed: %s\n",
            k_input_dir, std::strerror(errno));
        return nullptr;
    }

    auto* reader = new EvdevReader{};
    std::memset(reader->fds, -1, sizeof(reader->fds));

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr && reader->fd_count < k_max_devices) {
        // Only open event* nodes.
        if (std::strncmp(ent->d_name, k_event_prefix,
                         sizeof(k_event_prefix) - 1) != 0) {
            continue;
        }

        char path[256];
        std::snprintf(path, sizeof(path), "%s%s", k_input_dir, ent->d_name);

        // Open non-blocking, read-only.
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            // Permission denied is common (need input group) — log once and skip.
            if (errno == EACCES) {
                std::fprintf(stderr,
                    "[platform-linux] evdev: no access to %s — "
                    "add user to 'input' group or add udev rule\n", path);
            }
            continue;
        }

        // Check this device has EV_REL (mouse) or EV_KEY (keyboard).
        std::uint8_t ev_bits[(EV_CNT + 7) / 8] = {};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
            close(fd);
            continue;
        }

        bool has_rel = (ev_bits[EV_REL / 8] & (1u << (EV_REL % 8))) != 0;
        bool has_key = (ev_bits[EV_KEY / 8] & (1u << (EV_KEY % 8))) != 0;

        if (!has_rel && !has_key) {
            close(fd);
            continue;
        }

        reader->fds[reader->fd_count++] = fd;
    }

    closedir(dir);

    if (reader->fd_count == 0) {
        std::fprintf(stderr,
            "[platform-linux] evdev_open: no suitable input devices found\n");
        // Return reader anyway; it's valid but empty.
        // NEEDS PC VALIDATION: verify this case on a system with restricted
        // /dev/input permissions.
    }

    return reader;
}

// ─── Public: evdev_close ─────────────────────────────────────────────────

void evdev_close(EvdevReader* reader)
{
    if (!reader) { return; }
    for (int i = 0; i < reader->fd_count; ++i) {
        if (reader->fds[i] >= 0) {
            close(reader->fds[i]);
        }
    }
    delete reader;
}

// ─── Public: evdev_poll_mouse ─────────────────────────────────────────────

RawMouseDelta evdev_poll_mouse(EvdevReader* reader)
{
    RawMouseDelta out{};
    if (!reader || reader->fd_count == 0) { return out; }

    // Build pollfd array.
    struct pollfd pfds[k_max_devices];
    for (int i = 0; i < reader->fd_count; ++i) {
        pfds[i].fd      = reader->fds[i];
        pfds[i].events  = POLLIN;
        pfds[i].revents = 0;
    }

    // Non-blocking poll.
    int n = poll(pfds, static_cast<nfds_t>(reader->fd_count), 0);
    if (n <= 0) {
        // Flush any previously accumulated delta.
        out.dx       = reader->acc_dx;
        out.dy       = reader->acc_dy;
        out.wheel = reader->acc_wheel;
        reader->acc_dx = reader->acc_dy = reader->acc_wheel = 0;
        return out;
    }

    // Read all pending events from every ready fd.
    for (int i = 0; i < reader->fd_count; ++i) {
        if (!(pfds[i].revents & POLLIN)) { continue; }

        input_event evs[64];
        ssize_t bytes;
        while ((bytes = read(pfds[i].fd, evs, sizeof(evs))) > 0) {
            int count = static_cast<int>(bytes / sizeof(input_event));
            for (int j = 0; j < count; ++j) {
                const input_event& e = evs[j];
                switch (e.type) {
                case EV_REL:
                    switch (e.code) {
                    case REL_X:     reader->acc_dx       += e.value; break;
                    case REL_Y:     reader->acc_dy       += e.value; break;
                    case REL_WHEEL: reader->acc_wheel += e.value; break;
                    default: break;
                    }
                    break;
                case EV_KEY:
                    if (e.code < KEY_CNT) {
                        if (e.value) {
                            // Key down or repeat.
                            reader->key_state[e.code / 8] |=
                                static_cast<std::uint8_t>(1u << (e.code % 8));
                        } else {
                            // Key up.
                            reader->key_state[e.code / 8] &=
                                static_cast<std::uint8_t>(~(1u << (e.code % 8)));
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        }
    }

    out.dx       = reader->acc_dx;
    out.dy       = reader->acc_dy;
    out.wheel = reader->acc_wheel;
    reader->acc_dx = reader->acc_dy = reader->acc_wheel = 0;
    return out;
}

// ─── Public: evdev_key_held ───────────────────────────────────────────────

bool evdev_key_held(EvdevReader* reader, std::uint16_t linux_key_code)
{
    if (!reader || linux_key_code >= KEY_CNT) { return false; }
    return (reader->key_state[linux_key_code / 8] &
            (1u << (linux_key_code % 8))) != 0;
}

} // namespace psynder::platform::linux_platform

#endif // PSYNDER_PLATFORM_LINUX
