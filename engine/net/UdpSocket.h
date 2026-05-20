// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — real Berkeley / Winsock UDP socket wrapper. Lane 18 (net).
//
// DESIGN-PSYNDER-GX.md §10.4:
//   "Reliable + unreliable UDP channels with sequence numbers, sliding
//    window, selective acks."
//
// This wraps `sendto` / `recvfrom` over an AF_INET DGRAM socket. POSIX impl
// uses <sys/socket.h>; Windows impl uses <winsock2.h> (ws2_32 already linked
// in engine/net/CMakeLists.txt). The socket is set non-blocking so the
// server tick loop never stalls on receive — if no datagram is pending,
// `recv()` returns 0 immediately.
//
// Endpoints are kept as a flat `Endpoint` struct (host-order IPv4 + port) so
// callers don't have to traffic in OS sockaddr types. IPv4 only in this
// pass — IPv6 fits in a follow-up Issue when we need it (DESIGN §11.5 ships
// v1 on IPv4-only routes).
//
// Thread-safety: each UdpSocket instance is owned by one thread (the server
// tick thread or a single client thread). Multiple sockets in the same
// process are fine.
//
// Compile flag: this TU MUST be compiled with
//   -fno-fast-math -ffp-contract=off
// per DESIGN-PSYNDER-GX.md §14 (no float math here, but the flag is uniform
// across the net library).

#pragma once

#include "core/Types.h"

#include <cstdint>
#include <span>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// Endpoint — host-order IPv4 + port.
// host_ipv4 == 0x7F000001 (127.0.0.1) for loopback tests.
// ──────────────────────────────────────────────────────────────────────────
struct Endpoint {
    u32 host_ipv4 = 0;   // host byte order (e.g. 0x7F000001)
    u16 port      = 0;
    u16 reserved  = 0;

    bool operator==(const Endpoint& o) const noexcept {
        return host_ipv4 == o.host_ipv4 && port == o.port;
    }
    bool operator!=(const Endpoint& o) const noexcept { return !(*this == o); }
};

inline constexpr u32 kLoopbackIpv4 = 0x7F000001u;  // 127.0.0.1

// ──────────────────────────────────────────────────────────────────────────
// UdpSocket — non-blocking AF_INET DGRAM wrapper.
//
// open(bind_port) creates the socket, binds to 0.0.0.0:bind_port, and sets
// O_NONBLOCK (POSIX) / FIONBIO (Windows). `bind_port == 0` lets the OS
// assign a port — read it back via local_port() after open().
//
// close() shuts the socket down. Idempotent.
//
// send_to() and recv_from() are non-blocking. recv_from() returns 0 when no
// datagram is pending (errno == EAGAIN / EWOULDBLOCK / WSAEWOULDBLOCK).
// Negative return is a hard error (caller should log + close).
// ──────────────────────────────────────────────────────────────────────────
class UdpSocket {
public:
    UdpSocket() noexcept = default;
    ~UdpSocket() noexcept;

    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&)                 noexcept;
    UdpSocket& operator=(UdpSocket&&)      noexcept;

    // Open the socket on `bind_port` (0 == ephemeral). Returns true on
    // success. After this the socket is bound + non-blocking + ready for
    // send_to / recv_from.
    bool open(u16 bind_port) noexcept;

    // Close the socket. Safe to call on a never-opened or already-closed
    // socket.
    void close() noexcept;

    // True if the socket is open.
    bool is_open() const noexcept { return fd_ >= 0; }

    // Port the socket is bound to (host order). Valid only after open().
    u16 local_port() const noexcept { return local_port_; }

    // Send a datagram to `dst`. Returns the number of bytes sent on success
    // (always == bytes.size() for UDP). Returns -1 on hard error, 0 if the
    // send buffer is momentarily full (rare for UDP; caller drops).
    isize send_to(const Endpoint& dst, std::span<const u8> bytes) noexcept;

    // Receive one datagram into `out`. Returns the number of bytes received
    // (1 to out.size()) and writes the sender into `out_src`. Returns 0 if
    // no datagram is pending (non-blocking). Returns -1 on hard error.
    isize recv_from(std::span<u8> out, Endpoint& out_src) noexcept;

    // Returns the underlying OS socket descriptor (POSIX int / Winsock
    // SOCKET cast). Test introspection only — production code should not
    // touch this.
    std::intptr_t native_handle() const noexcept { return fd_; }

private:
    // -1 means closed.  Storage MUST be pointer-sized so a Win64 SOCKET
    // (UINT_PTR, 64-bit) round-trips without truncation — the prior
    // `int fd_` would corrupt valid handles whose high bits were set,
    // causing is_open()/close() to spuriously fail.  POSIX's `int`
    // socket descriptor widens cleanly into the wider type and
    // `fd_ >= 0` remains the correct open-sentinel check on both
    // platforms because Winsock's INVALID_SOCKET == (UINT_PTR)~0 maps
    // to intptr_t -1 under the documented C cast rules.
    //
    // Copilot's PR #10 review caught the bug.  The reserved_ slot stays
    // to preserve struct padding for any callers that introspect layout.
    std::intptr_t fd_         = -1;
    u16           local_port_ = 0;
    u16           reserved_   = 0;
};

// ──────────────────────────────────────────────────────────────────────────
// Process-wide init / shutdown.
//
// Winsock requires WSAStartup / WSACleanup. POSIX is a no-op. Calling
// UdpSocket::open() implicitly inits on first use; ensure_initialized() is
// exposed for the server boot path so callers can fail fast at startup
// rather than on the first socket.
// ──────────────────────────────────────────────────────────────────────────
bool ensure_initialized() noexcept;
void shutdown_subsystem() noexcept;

}  // namespace psynder::net
