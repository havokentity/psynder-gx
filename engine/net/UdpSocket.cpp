// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — UdpSocket impl. Lane 18 (net).
//
// POSIX path uses <sys/socket.h>; Win32 path uses <winsock2.h>. The two
// share the same UdpSocket interface; close()/send_to()/recv_from() resolve
// to the appropriate native call via macro mapping kept inside this TU.

#include "UdpSocket.h"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
    // Order matters: <winsock2.h> before any <windows.h>; we don't include
    // windows.h directly here, but be defensive in case a transitive include
    // pulls it in.
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socklen_t_local = int;
    #define PSY_SOCK_ERRNO()  WSAGetLastError()
    #define PSY_SOCK_EAGAIN  WSAEWOULDBLOCK
    #define PSY_SOCK_EINTR   WSAEINTR
    #define PSY_INVALID_SOCK INVALID_SOCKET
    using native_sock_t = SOCKET;
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socklen_t_local = socklen_t;
    #define PSY_SOCK_ERRNO() errno
    #define PSY_SOCK_EAGAIN  EAGAIN
    #define PSY_SOCK_EINTR   EINTR
    #define PSY_INVALID_SOCK (-1)
    using native_sock_t = int;
#endif

namespace psynder::net {

namespace {

#if defined(_WIN32)
// One-shot Winsock init guard.
struct SubsystemGuard {
    std::atomic<int> state{0};  // 0 = uninit, 1 = inited, 2 = failed
};

SubsystemGuard& subsystem() {
    static SubsystemGuard s;
    return s;
}
#endif

bool do_init_once() noexcept {
#if defined(_WIN32)
    auto& s = subsystem();
    int expected = 0;
    if (s.state.compare_exchange_strong(expected, -1)) {
        WSADATA d{};
        int rc = WSAStartup(MAKEWORD(2, 2), &d);
        s.state.store(rc == 0 ? 1 : 2);
        return rc == 0;
    }
    // Spin until the other thread finishes init. compare_exchange above
    // wrote -1 only on the winner; losers wait for state to settle.
    while (s.state.load() == -1) {
        // Busy-spin briefly. Subsystem init is one-shot at startup so this
        // is rarely contended.
    }
    return s.state.load() == 1;
#else
    return true;
#endif
}

native_sock_t to_native(std::intptr_t fd) noexcept {
    return static_cast<native_sock_t>(fd);
}

std::intptr_t from_native(native_sock_t s) noexcept {
    // Reinterpret-style cast through intptr_t.  On POSIX, native_sock_t is
    // a non-negative int and widens cleanly.  On Windows native_sock_t is
    // a UINT_PTR; INVALID_SOCKET (== (UINT_PTR)~0) converts to intptr_t -1
    // by the C standard's signed/unsigned cast rules at the same bit width.
    return static_cast<std::intptr_t>(s);
}

bool set_nonblocking(native_sock_t s) noexcept {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void native_close(native_sock_t s) noexcept {
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

}  // namespace

bool ensure_initialized() noexcept { return do_init_once(); }

void shutdown_subsystem() noexcept {
#if defined(_WIN32)
    auto& s = subsystem();
    int prev = s.state.exchange(0);
    if (prev == 1) {
        WSACleanup();
    }
#endif
}

UdpSocket::~UdpSocket() noexcept { close(); }

UdpSocket::UdpSocket(UdpSocket&& o) noexcept
    : fd_(o.fd_), local_port_(o.local_port_), reserved_(o.reserved_) {
    o.fd_         = -1;
    o.local_port_ = 0;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
        close();
        fd_         = o.fd_;
        local_port_ = o.local_port_;
        reserved_   = o.reserved_;
        o.fd_         = -1;
        o.local_port_ = 0;
    }
    return *this;
}

bool UdpSocket::open(u16 bind_port) noexcept {
    if (!do_init_once()) return false;
    if (fd_ >= 0) return false;  // already open

    native_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == PSY_INVALID_SOCK) return false;

    // Allow rebinding the same port quickly across test runs.
    int on = 1;
#if defined(_WIN32)
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&on), sizeof(on));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(bind_port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        native_close(s);
        return false;
    }

    // Read the bound port back (matters when bind_port == 0).
    sockaddr_in actual{};
    socklen_t_local alen = sizeof(actual);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&actual), &alen) != 0) {
        native_close(s);
        return false;
    }
    local_port_ = ntohs(actual.sin_port);

    if (!set_nonblocking(s)) {
        native_close(s);
        return false;
    }

    fd_ = from_native(s);
    return true;
}

void UdpSocket::close() noexcept {
    if (fd_ < 0) return;
    native_close(to_native(fd_));
    fd_         = -1;
    local_port_ = 0;
}

isize UdpSocket::send_to(const Endpoint& dst, std::span<const u8> bytes) noexcept {
    if (fd_ < 0) return -1;
    if (bytes.empty()) return 0;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(dst.host_ipv4);
    addr.sin_port        = htons(dst.port);

    native_sock_t s = to_native(fd_);
#if defined(_WIN32)
    int rc = sendto(s,
                    reinterpret_cast<const char*>(bytes.data()),
                    static_cast<int>(bytes.size()),
                    0,
                    reinterpret_cast<const sockaddr*>(&addr),
                    sizeof(addr));
    if (rc == SOCKET_ERROR) {
        int e = PSY_SOCK_ERRNO();
        if (e == PSY_SOCK_EAGAIN || e == PSY_SOCK_EINTR) return 0;
        return -1;
    }
    return static_cast<isize>(rc);
#else
    ssize_t rc;
    do {
        rc = sendto(s,
                    bytes.data(),
                    bytes.size(),
                    0,
                    reinterpret_cast<const sockaddr*>(&addr),
                    sizeof(addr));
    } while (rc < 0 && PSY_SOCK_ERRNO() == PSY_SOCK_EINTR);
    if (rc < 0) {
        int e = PSY_SOCK_ERRNO();
        if (e == PSY_SOCK_EAGAIN) return 0;
        return -1;
    }
    return static_cast<isize>(rc);
#endif
}

isize UdpSocket::recv_from(std::span<u8> out, Endpoint& out_src) noexcept {
    if (fd_ < 0) return -1;
    if (out.empty()) return 0;

    sockaddr_in addr{};
    socklen_t_local alen = sizeof(addr);

    native_sock_t s = to_native(fd_);
#if defined(_WIN32)
    int rc = recvfrom(s,
                      reinterpret_cast<char*>(out.data()),
                      static_cast<int>(out.size()),
                      0,
                      reinterpret_cast<sockaddr*>(&addr),
                      &alen);
    if (rc == SOCKET_ERROR) {
        int e = PSY_SOCK_ERRNO();
        if (e == PSY_SOCK_EAGAIN || e == PSY_SOCK_EINTR) return 0;
        return -1;
    }
#else
    ssize_t rc;
    do {
        rc = recvfrom(s,
                      out.data(),
                      out.size(),
                      0,
                      reinterpret_cast<sockaddr*>(&addr),
                      &alen);
    } while (rc < 0 && PSY_SOCK_ERRNO() == PSY_SOCK_EINTR);
    if (rc < 0) {
        int e = PSY_SOCK_ERRNO();
        if (e == PSY_SOCK_EAGAIN) return 0;
        return -1;
    }
#endif
    out_src.host_ipv4 = ntohl(addr.sin_addr.s_addr);
    out_src.port      = ntohs(addr.sin_port);
    out_src.reserved  = 0;
    return static_cast<isize>(rc);
}

}  // namespace psynder::net
