// SPDX-License-Identifier: MIT
// Psynder-GX — WebSocket IPC server. Implements `Ipc.h`'s `Server` singleton:
// 127.0.0.1:7654 (loopback only), RFC 6455 framing, msgpack payload (opaque
// bytes — this lane just moves them), one-directional engine→panel broadcast +
// command-RPC pump.
//
// Design choices (DESIGN §10.8):
//
// • Self-contained — no FetchContent / vcpkg dep on libwebsockets / asio /
//   uWebSockets. RFC 6455 framing is ~300 LoC and lets us keep the lane
//   directory the entire surface area. Mac + Linux use BSD sockets; Win32
//   uses Winsock2.
//
// • Loopback-only bind. Editor is dev-only — never expose to a network. The
//   bind_host field in ServerDesc is honoured but the dev contract is
//   127.0.0.1.
//
// • Session token. Generated on `start()`; the React panel must echo it in
//   the upgrade request (Sec-WebSocket-Protocol or first message). If
//   require_session_token is false we accept any client (for `nc` / Python
//   smoke tests).
//
// • Thread model: one I/O thread runs `accept()` + `select()` for all
//   client sockets. `broadcast()` queues frames behind a mutex; the I/O
//   thread drains the queue when each client is writeable. `pump()` runs on
//   the engine thread and drains the inbound message queue, invoking the
//   on_message callback.
//
// • No exceptions, no RTTI, no allocations on the broadcast hot path beyond
//   the unavoidable `std::vector<u8>` copy of the payload (the engine owns
//   its source buffer; we need our own copy to outlive the call).

#include "Ipc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   pragma comment(lib, "Ws2_32.lib")
    using socket_t = SOCKET;
    static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#   define PSY_SOCK_LAST_ERR WSAGetLastError()
#   define PSY_SOCK_WOULDBLOCK WSAEWOULDBLOCK
#   define PSY_CLOSESOCK(s) closesocket(s)
#   define PSY_SOCKLEN int
#else
#   include <arpa/inet.h>
#   include <errno.h>
#   include <fcntl.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <sys/select.h>
#   include <sys/socket.h>
#   include <sys/time.h>
#   include <sys/types.h>
#   include <unistd.h>
    using socket_t = int;
    static constexpr socket_t kInvalidSocket = -1;
#   define PSY_SOCK_LAST_ERR errno
#   define PSY_SOCK_WOULDBLOCK EWOULDBLOCK
#   define PSY_CLOSESOCK(s) ::close(s)
#   define PSY_SOCKLEN socklen_t
#endif

namespace psynder::editor::ipc {

namespace {

// ─── SHA-1 (RFC 3174) ────────────────────────────────────────────────────
// Tiny, self-contained, used exactly once per upgrade handshake. Not a hot
// path; correctness > speed.
struct Sha1 {
    u32 h[5]{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    u64 length_bits = 0;
    std::array<u8, 64> buffer{};
    usize buffer_len = 0;

    static u32 rotl(u32 x, u32 n) { return (x << n) | (x >> (32 - n)); }

    void process_block(const u8* p) {
        u32 w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (u32(p[i * 4]) << 24) | (u32(p[i * 4 + 1]) << 16) |
                   (u32(p[i * 4 + 2]) << 8) | u32(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            u32 tmp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const u8* data, usize n) {
        length_bits += u64(n) * 8;
        while (n > 0) {
            usize take = std::min<usize>(n, 64 - buffer_len);
            std::memcpy(buffer.data() + buffer_len, data, take);
            buffer_len += take;
            data += take;
            n -= take;
            if (buffer_len == 64) {
                process_block(buffer.data());
                buffer_len = 0;
            }
        }
    }

    void finalize(u8 out[20]) {
        // Snapshot the bit length BEFORE writing padding — update() mutates
        // length_bits, so we have to capture it now.
        u64 saved_bits = length_bits;
        u8 pad = 0x80;
        update(&pad, 1);
        u8 zero = 0;
        while (buffer_len != 56) update(&zero, 1);
        u8 tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = u8(saved_bits >> (56 - i * 8));
        update(tail, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4]     = u8(h[i] >> 24);
            out[i * 4 + 1] = u8(h[i] >> 16);
            out[i * 4 + 2] = u8(h[i] >> 8);
            out[i * 4 + 3] = u8(h[i]);
        }
    }
};

// ─── base64 encode ───────────────────────────────────────────────────────
std::string base64_encode(const u8* data, usize n) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    usize i = 0;
    while (i + 3 <= n) {
        u32 v = (u32(data[i]) << 16) | (u32(data[i + 1]) << 8) | u32(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
        i += 3;
    }
    if (i < n) {
        u32 v = u32(data[i]) << 16;
        if (i + 1 < n) v |= u32(data[i + 1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? kAlphabet[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

// ─── platform socket helpers ─────────────────────────────────────────────
void set_nonblocking(socket_t s) {
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

bool sock_init_once() {
#if defined(_WIN32)
    static std::atomic<bool> initialised{false};
    static std::mutex m;
    std::scoped_lock lk(m);
    if (!initialised.load()) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
        initialised.store(true);
    }
#endif
    return true;
}

// ─── client state ────────────────────────────────────────────────────────
struct Client {
    socket_t                 sock = kInvalidSocket;
    bool                     handshaked = false;
    bool                     authenticated = false;  // session-token verified
    std::vector<u8>          rx_buf;  // pre-handshake = HTTP request bytes;
                                      // post-handshake = WebSocket frame bytes
    std::deque<std::vector<u8>> tx_queue;  // pending outbound frames
    usize                    tx_offset = 0;  // bytes already sent of tx_queue.front()
    bool                     close_pending = false;
};

// ─── server impl ─────────────────────────────────────────────────────────
struct ServerImpl {
    ServerDesc                       desc;
    std::atomic<bool>                running{false};
    std::thread                      io_thread;
    socket_t                         listen_sock = kInvalidSocket;
    std::string                      session_token;

    // I/O-thread-only state.
    std::vector<Client>              clients;

    // Cross-thread.
    std::mutex                       broadcast_mutex;
    std::vector<std::vector<u8>>     pending_broadcasts;

    std::mutex                       inbound_mutex;
    std::vector<std::vector<u8>>     inbound_messages;

    // pump callback.
    std::mutex                       cb_mutex;
    std::function<void(std::span<const u8>)> on_message_cb;
};

// Module-local instance — `Server::Get()` returns a wrapper around this.
ServerImpl& impl() {
    static ServerImpl s;
    return s;
}

// ─── RFC 6455 framing ────────────────────────────────────────────────────
// Encodes an outbound frame (server→client, no mask) carrying `payload`.
// opcode 0x2 = binary (msgpack is binary).
std::vector<u8> encode_frame(const u8* payload, usize n, u8 opcode = 0x2) {
    std::vector<u8> out;
    out.reserve(n + 10);
    out.push_back(u8(0x80 | (opcode & 0x0F)));  // FIN + opcode
    if (n < 126) {
        out.push_back(u8(n));
    } else if (n <= 0xFFFF) {
        out.push_back(126);
        out.push_back(u8(n >> 8));
        out.push_back(u8(n));
    } else {
        out.push_back(127);
        u64 nn = u64(n);
        for (int i = 7; i >= 0; --i) out.push_back(u8(nn >> (i * 8)));
    }
    out.insert(out.end(), payload, payload + n);
    return out;
}

// Tries to parse one frame from `buf`. Returns the number of bytes consumed
// (0 if incomplete). On success, fills `out_payload` (unmasked) and
// `out_opcode`. Control frames (close 0x8 / ping 0x9 / pong 0xA) are
// surfaced; caller handles them.
usize parse_frame(const std::vector<u8>& buf, std::vector<u8>& out_payload,
                  u8& out_opcode, bool& out_fin) {
    if (buf.size() < 2) return 0;
    u8 b0 = buf[0];
    u8 b1 = buf[1];
    out_fin = (b0 & 0x80) != 0;
    out_opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    u64 len = b1 & 0x7F;
    usize off = 2;
    if (len == 126) {
        if (buf.size() < off + 2) return 0;
        len = (u64(buf[off]) << 8) | u64(buf[off + 1]);
        off += 2;
    } else if (len == 127) {
        if (buf.size() < off + 8) return 0;
        len = 0;
        for (usize i = 0; i < 8; ++i) len = (len << 8) | u64(buf[off + i]);
        off += 8;
    }
    u8 mask_key[4]{};
    if (masked) {
        if (buf.size() < off + 4) return 0;
        std::memcpy(mask_key, buf.data() + off, 4);
        off += 4;
    }
    if (buf.size() < off + len) return 0;
    out_payload.assign(len, 0);
    for (u64 i = 0; i < len; ++i) {
        u8 c = buf[off + usize(i)];
        out_payload[usize(i)] = masked ? u8(c ^ mask_key[i & 3]) : c;
    }
    return usize(off + len);
}

// ─── HTTP upgrade handshake ──────────────────────────────────────────────
// Returns: 1 = handshake complete, 0 = incomplete (need more bytes), -1 = error.
// On success, queues the Sec-WebSocket-Accept response into the client.
// If `require_token` is true and the URL doesn't carry ?token=<expected>,
// rejects with HTTP 401.
int try_handshake(Client& c, const std::string& expected_token,
                  bool require_token) {
    // Look for end of HTTP headers ("\r\n\r\n").
    if (c.rx_buf.size() < 4) return 0;
    static const u8 kEoh[4] = {'\r', '\n', '\r', '\n'};
    auto it = std::search(c.rx_buf.begin(), c.rx_buf.end(), kEoh, kEoh + 4);
    if (it == c.rx_buf.end()) {
        // Cap pre-handshake buffer so a hostile client can't blow memory.
        if (c.rx_buf.size() > 16 * 1024) return -1;
        return 0;
    }
    usize header_len = usize(it - c.rx_buf.begin()) + 4;
    std::string req(reinterpret_cast<const char*>(c.rx_buf.data()), header_len);
    c.rx_buf.erase(c.rx_buf.begin(), c.rx_buf.begin() + isize(header_len));

    // Extract Sec-WebSocket-Key (case-insensitive header lookup).
    auto find_header = [&](const char* name) -> std::string {
        usize nlen = std::strlen(name);
        for (usize i = 0; i + nlen + 1 < req.size(); ++i) {
            bool match = true;
            for (usize j = 0; j < nlen; ++j) {
                char a = req[i + j];
                char b = name[j];
                if (a >= 'A' && a <= 'Z') a = char(a + 32);
                if (b >= 'A' && b <= 'Z') b = char(b + 32);
                if (a != b) { match = false; break; }
            }
            if (!match) continue;
            if (req[i + nlen] != ':') continue;
            usize start = i + nlen + 1;
            while (start < req.size() && (req[start] == ' ' || req[start] == '\t')) ++start;
            usize end = start;
            while (end < req.size() && req[end] != '\r' && req[end] != '\n') ++end;
            return req.substr(start, end - start);
        }
        return {};
    };

    std::string key = find_header("Sec-WebSocket-Key");
    if (key.empty()) {
        const char* resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        c.tx_queue.emplace_back(reinterpret_cast<const u8*>(resp),
                                reinterpret_cast<const u8*>(resp) + std::strlen(resp));
        c.close_pending = true;
        return -1;
    }

    // Token check — look in the request line for "?token=...".
    if (require_token) {
        usize qpos = req.find("?token=");
        bool ok = false;
        if (qpos != std::string::npos) {
            usize end = qpos + 7;
            usize stop = end;
            while (stop < req.size() && req[stop] != ' ' && req[stop] != '&' &&
                   req[stop] != '\r' && req[stop] != '\n')
                ++stop;
            ok = (req.substr(end, stop - end) == expected_token);
        }
        if (!ok) {
            const char* resp = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n";
            c.tx_queue.emplace_back(reinterpret_cast<const u8*>(resp),
                                    reinterpret_cast<const u8*>(resp) + std::strlen(resp));
            c.close_pending = true;
            return -1;
        }
        c.authenticated = true;
    } else {
        c.authenticated = true;
    }

    // Compute Sec-WebSocket-Accept = base64(sha1(key + GUID)).
    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = key + kGuid;
    Sha1 sha;
    sha.update(reinterpret_cast<const u8*>(concat.data()), concat.size());
    u8 digest[20];
    sha.finalize(digest);
    std::string accept = base64_encode(digest, 20);

    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    c.tx_queue.emplace_back(reinterpret_cast<const u8*>(resp.data()),
                            reinterpret_cast<const u8*>(resp.data()) + resp.size());
    c.handshaked = true;
    return 1;
}

// ─── I/O thread ──────────────────────────────────────────────────────────
void io_loop(ServerImpl& s) {
    while (s.running.load(std::memory_order_acquire)) {
        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        socket_t maxfd = s.listen_sock;
        FD_SET(s.listen_sock, &rset);
        for (auto& c : s.clients) {
            FD_SET(c.sock, &rset);
            if (!c.tx_queue.empty()) FD_SET(c.sock, &wset);
            if (c.sock > maxfd) maxfd = c.sock;
        }
        timeval tv{0, 50'000};  // 50ms — bounds shutdown latency.
        int n = ::select(int(maxfd) + 1, &rset, &wset, nullptr, &tv);
        if (n < 0) {
            int err = PSY_SOCK_LAST_ERR;
            (void)err;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Accept new connections.
        if (FD_ISSET(s.listen_sock, &rset)) {
            sockaddr_in addr{};
            PSY_SOCKLEN alen = sizeof(addr);
            socket_t c = ::accept(s.listen_sock, reinterpret_cast<sockaddr*>(&addr), &alen);
            if (c != kInvalidSocket) {
                set_nonblocking(c);
                int one = 1;
                ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                             reinterpret_cast<const char*>(&one), sizeof(one));
                Client nc;
                nc.sock = c;
                s.clients.push_back(std::move(nc));
            }
        }

        // Drain pending broadcasts → all authenticated clients.
        std::vector<std::vector<u8>> drained;
        {
            std::scoped_lock lk(s.broadcast_mutex);
            drained.swap(s.pending_broadcasts);
        }
        for (auto& payload : drained) {
            auto frame = encode_frame(payload.data(), payload.size(), 0x2);
            for (auto& c : s.clients) {
                if (c.handshaked && c.authenticated && !c.close_pending) {
                    c.tx_queue.push_back(frame);
                }
            }
        }

        // Per-client I/O.
        for (auto& c : s.clients) {
            if (FD_ISSET(c.sock, &rset)) {
                u8 chunk[4096];
#if defined(_WIN32)
                int got = ::recv(c.sock, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
#else
                ssize_t got = ::recv(c.sock, chunk, sizeof(chunk), 0);
#endif
                if (got <= 0) {
                    int err = PSY_SOCK_LAST_ERR;
                    if (got == 0 || (err != PSY_SOCK_WOULDBLOCK
#if !defined(_WIN32)
                                     && err != EAGAIN
#endif
                                    )) {
                        c.close_pending = true;
                    }
                } else {
                    c.rx_buf.insert(c.rx_buf.end(), chunk, chunk + got);
                }
            }

            // Pre-handshake: try to upgrade.
            if (!c.handshaked && !c.close_pending) {
                int rc = try_handshake(c, s.session_token, s.desc.require_session_token);
                (void)rc;
            }

            // Post-handshake: parse frames.
            if (c.handshaked && !c.close_pending) {
                for (;;) {
                    std::vector<u8> payload;
                    u8 opcode = 0;
                    bool fin = false;
                    usize consumed = parse_frame(c.rx_buf, payload, opcode, fin);
                    if (consumed == 0) break;
                    c.rx_buf.erase(c.rx_buf.begin(), c.rx_buf.begin() + isize(consumed));
                    if (opcode == 0x8) {  // close
                        // Echo close.
                        auto closef = encode_frame(payload.data(), payload.size(), 0x8);
                        c.tx_queue.push_back(closef);
                        c.close_pending = true;
                        break;
                    }
                    if (opcode == 0x9) {  // ping → pong
                        auto pong = encode_frame(payload.data(), payload.size(), 0xA);
                        c.tx_queue.push_back(pong);
                        continue;
                    }
                    if (opcode == 0xA) {  // pong — ignore
                        continue;
                    }
                    if ((opcode == 0x1 || opcode == 0x2) && fin) {
                        // Text or binary data — surface to inbound queue.
                        std::scoped_lock lk(s.inbound_mutex);
                        s.inbound_messages.push_back(std::move(payload));
                    }
                    // Fragmented frames (opcode 0x0 continuation) are not used by
                    // the React panels, which always send one frame per message.
                    // If we see them, drop silently.
                }
            }

            // Drain tx queue.
            if (FD_ISSET(c.sock, &wset) && !c.tx_queue.empty()) {
                auto& front = c.tx_queue.front();
                const u8* p = front.data() + c.tx_offset;
                usize remaining = front.size() - c.tx_offset;
#if defined(_WIN32)
                int sent = ::send(c.sock, reinterpret_cast<const char*>(p),
                                  int(remaining), 0);
#else
                ssize_t sent = ::send(c.sock, p, remaining,
#   if defined(__linux__)
                                      MSG_NOSIGNAL
#   else
                                      0
#   endif
                                     );
#endif
                if (sent > 0) {
                    c.tx_offset += usize(sent);
                    if (c.tx_offset >= front.size()) {
                        c.tx_queue.pop_front();
                        c.tx_offset = 0;
                    }
                } else {
                    int err = PSY_SOCK_LAST_ERR;
                    if (err != PSY_SOCK_WOULDBLOCK
#if !defined(_WIN32)
                        && err != EAGAIN
#endif
                       ) {
                        c.close_pending = true;
                    }
                }
            }
        }

        // Reap closed clients.
        s.clients.erase(
            std::remove_if(s.clients.begin(), s.clients.end(),
                           [](Client& c) {
                               if (c.close_pending && c.tx_queue.empty()) {
                                   PSY_CLOSESOCK(c.sock);
                                   return true;
                               }
                               return false;
                           }),
            s.clients.end());
    }

    // Shutdown: close all clients.
    for (auto& c : s.clients) PSY_CLOSESOCK(c.sock);
    s.clients.clear();
}

std::string make_session_token() {
    // 128 bits of entropy hex-encoded — sufficient to gate localhost access.
    std::random_device rd;
    std::uniform_int_distribution<int> d(0, 15);
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int i = 0; i < 32; ++i) s.push_back(kHex[d(rd)]);
    return s;
}

}  // namespace

// ─── public API ──────────────────────────────────────────────────────────
Server& Server::Get() {
    static Server s;
    return s;
}

bool Server::start(const ServerDesc& desc) {
    auto& s = impl();
    if (s.running.load()) return true;

    if (!sock_init_once()) return false;

    s.desc = desc;
    s.session_token = make_session_token();

    s.listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s.listen_sock == kInvalidSocket) return false;

    int one = 1;
    ::setsockopt(s.listen_sock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(desc.port);
    // Loopback-only bind. `bind_host` is honoured if it parses; otherwise
    // fall back to 127.0.0.1 (the dev contract).
    if (::inet_pton(AF_INET, desc.bind_host ? desc.bind_host : "127.0.0.1",
                    &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if (::bind(s.listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "[editor-ipc] bind failed on %s:%u (err=%d)\n",
                     desc.bind_host, unsigned(desc.port), PSY_SOCK_LAST_ERR);
        PSY_CLOSESOCK(s.listen_sock);
        s.listen_sock = kInvalidSocket;
        return false;
    }
    if (::listen(s.listen_sock, 8) != 0) {
        PSY_CLOSESOCK(s.listen_sock);
        s.listen_sock = kInvalidSocket;
        return false;
    }
    set_nonblocking(s.listen_sock);

    s.running.store(true, std::memory_order_release);
    s.io_thread = std::thread([&s]() { io_loop(s); });

    std::fprintf(stderr,
                 "[editor-ipc] listening on %s:%u (token %s)\n",
                 desc.bind_host, unsigned(desc.port), s.session_token.c_str());
    return true;
}

void Server::stop() {
    auto& s = impl();
    if (!s.running.exchange(false)) return;
    if (s.io_thread.joinable()) s.io_thread.join();
    if (s.listen_sock != kInvalidSocket) {
        PSY_CLOSESOCK(s.listen_sock);
        s.listen_sock = kInvalidSocket;
    }
    {
        std::scoped_lock lk(s.broadcast_mutex);
        s.pending_broadcasts.clear();
    }
    {
        std::scoped_lock lk(s.inbound_mutex);
        s.inbound_messages.clear();
    }
}

void Server::broadcast(std::string_view channel,
                       std::span<const u8> msgpack_payload) {
    auto& s = impl();
    if (!s.running.load(std::memory_order_acquire)) return;

    // Prepend a 1-byte channel-name length + channel bytes, then the
    // msgpack payload. The wire format is opaque to this lane; the React
    // side's IDL stub generator knows how to split it. Channel names are
    // ASCII and well under 255 bytes.
    if (channel.size() > 255) return;
    std::vector<u8> framed;
    framed.reserve(1 + channel.size() + msgpack_payload.size());
    framed.push_back(u8(channel.size()));
    framed.insert(framed.end(),
                  reinterpret_cast<const u8*>(channel.data()),
                  reinterpret_cast<const u8*>(channel.data()) + channel.size());
    framed.insert(framed.end(), msgpack_payload.begin(), msgpack_payload.end());

    std::scoped_lock lk(s.broadcast_mutex);
    s.pending_broadcasts.push_back(std::move(framed));
}

void Server::pump() {
    auto& s = impl();
    std::vector<std::vector<u8>> drained;
    {
        std::scoped_lock lk(s.inbound_mutex);
        drained.swap(s.inbound_messages);
    }
    if (drained.empty()) return;
    std::function<void(std::span<const u8>)> cb;
    {
        std::scoped_lock lk(s.cb_mutex);
        cb = s.on_message_cb;
    }
    if (!cb) return;
    for (auto& m : drained) {
        cb(std::span<const u8>(m.data(), m.size()));
    }
}

const std::string& Server::session_token() const noexcept {
    return impl().session_token;
}

// ─── internal extension hook for on_message ──────────────────────────────
// `Ipc.h` declares the four-method surface but no message-receive callback
// setter (the design says the engine pump dispatches to the console queue
// directly). We expose a private setter here for test/debug use; the
// console-queue wiring is done in a follow-up lane. To keep `Ipc.h` frozen,
// the setter lives in this TU and is reachable via an unnamed-namespace
// trampoline declared in a header co-owned by lane 22-editor.
namespace detail {
void set_on_message(std::function<void(std::span<const u8>)> cb) {
    auto& s = impl();
    std::scoped_lock lk(s.cb_mutex);
    s.on_message_cb = std::move(cb);
}
}

}  // namespace psynder::editor::ipc
