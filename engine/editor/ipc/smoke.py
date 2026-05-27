#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# psynder_editor_ipc — runtime smoke. Connects to 127.0.0.1:7655, completes
# the WebSocket upgrade, sends a single binary frame carrying msgpack
# `[1, "hello"]`, and reads back any broadcast.
#
# Usage:
#   python3 engine/editor/ipc/smoke.py [--token TOKEN] [--port PORT]
#
# This script intentionally has zero non-stdlib deps (no `websockets` /
# `msgpack-python`). It implements just enough of RFC 6455 to verify the
# handshake + a single roundtrip — runnable anywhere Python 3.8+ is.
#
# Run AFTER spawning a host process that calls
# `psynder::editor::ipc::Server::Get().start(...)`. There is no editor host
# binary yet (M3); for now this script verifies the protocol shape by
# pointing it at a hand-rolled test harness or, once landed, the editor
# binary itself.

import argparse
import base64
import hashlib
import os
import socket
import struct
import sys


def msgpack_encode(value):
    """Tiny msgpack encoder — handles int / str / list only (enough for the
    smoke message `[1, "hello"]`). Returns bytes."""
    if isinstance(value, bool):
        return b"\xc3" if value else b"\xc2"
    if isinstance(value, int):
        if 0 <= value < 128:
            return bytes([value])
        if -32 <= value < 0:
            return bytes([0xE0 | (value & 0x1F)])
        if -128 <= value < 128:
            return b"\xd0" + struct.pack(">b", value)
        if -32768 <= value < 32768:
            return b"\xd1" + struct.pack(">h", value)
        return b"\xd2" + struct.pack(">i", value)
    if isinstance(value, str):
        b = value.encode("utf-8")
        if len(b) < 32:
            return bytes([0xA0 | len(b)]) + b
        return b"\xda" + struct.pack(">H", len(b)) + b
    if isinstance(value, list):
        if len(value) < 16:
            head = bytes([0x90 | len(value)])
        else:
            head = b"\xdc" + struct.pack(">H", len(value))
        return head + b"".join(msgpack_encode(v) for v in value)
    raise TypeError(f"unsupported msgpack type: {type(value)}")


def ws_handshake(sock, port, token):
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    path = "/" + (f"?token={token}" if token else "")
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode("ascii"))

    # Read response headers.
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            sys.exit("server closed during handshake")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    if not head.startswith(b"HTTP/1.1 101"):
        sys.exit(f"handshake failed:\n{head.decode('latin-1')}")
    # Verify Sec-WebSocket-Accept (sanity, not strictly needed for smoke).
    expected = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
    ).decode("ascii")
    if expected.encode() not in head:
        sys.exit("Sec-WebSocket-Accept mismatch")
    print("[smoke] handshake OK")
    return rest


def send_frame(sock, payload, opcode=0x2):
    header = bytes([0x80 | opcode])
    # Client→server frames MUST be masked per RFC 6455.
    mask = os.urandom(4)
    n = len(payload)
    if n < 126:
        header += bytes([0x80 | n])
    elif n <= 0xFFFF:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    sock.sendall(header + mask + masked)


def recv_frame(sock, leftover=b"", timeout=2.0):
    sock.settimeout(timeout)
    buf = leftover
    while len(buf) < 2:
        buf += sock.recv(4096)
    b0, b1 = buf[0], buf[1]
    masked = (b1 & 0x80) != 0
    n = b1 & 0x7F
    off = 2
    if n == 126:
        while len(buf) < off + 2:
            buf += sock.recv(4096)
        n = struct.unpack(">H", buf[off:off + 2])[0]
        off += 2
    elif n == 127:
        while len(buf) < off + 8:
            buf += sock.recv(4096)
        n = struct.unpack(">Q", buf[off:off + 8])[0]
        off += 8
    if masked:
        off += 4
    while len(buf) < off + n:
        buf += sock.recv(4096)
    payload = buf[off:off + n]
    return b0 & 0x0F, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7655)
    ap.add_argument("--token", default="")
    args = ap.parse_args()

    sock = socket.create_connection(("127.0.0.1", args.port), timeout=2.0)
    leftover = ws_handshake(sock, args.port, args.token)
    payload = msgpack_encode([1, "hello"])
    print(f"[smoke] sending msgpack {payload.hex()}")
    send_frame(sock, payload)

    try:
        opcode, data = recv_frame(sock, leftover, timeout=1.0)
        print(f"[smoke] received frame opcode=0x{opcode:x} bytes={data.hex()}")
    except socket.timeout:
        print("[smoke] no broadcast received (expected if host did not echo)")

    sock.close()
    print("[smoke] OK")


if __name__ == "__main__":
    main()
