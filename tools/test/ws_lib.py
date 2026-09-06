"""Minimal RFC 6455 client.

The Day 7 plan's snippets use the `websocket-client` package, which is not
installed and would be an extra dependency for three short scripts. This uses
only the standard library.

Handles fragmentation: the gateway's snapshot JSON exceeds a single frame once
all three sources are present, and a reader that ignores continuation frames
silently truncates the payload mid-token.
"""
import base64
import json
import os
import socket
import struct
import time


class WsClient:
    def __init__(self, host, port=80, path="/ws", timeout=15):
        key = base64.b64encode(os.urandom(16)).decode()
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.sendall(
            f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode()
        )
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed during handshake")
            buf += chunk
        head, _, self._rest = buf.partition(b"\r\n\r\n")
        status = head.split(b"\r\n")[0]
        if b"101" not in status:
            raise ConnectionError(f"handshake failed: {status!r}")

    def _need(self, n):
        while len(self._rest) < n:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed")
            self._rest += chunk
        out, self._rest = self._rest[:n], self._rest[n:]
        return out

    def recv_json(self):
        """Return (arrival_time, parsed_json) for the next complete text message."""
        assembled, first_op = b"", None
        while True:
            hdr = self._need(2)
            fin, opcode = hdr[0] & 0x80, hdr[0] & 0x0F
            length = hdr[1] & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._need(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._need(8))[0]
            payload = self._need(length)
            arrival = time.time()
            if opcode == 0x8:
                raise ConnectionError("server closed the websocket")
            if opcode in (0x9, 0xA):
                continue  # ping/pong
            if opcode != 0x0:
                assembled, first_op = payload, opcode
            else:
                assembled += payload
            if fin and first_op == 0x1:
                return arrival, json.loads(assembled)

    def close(self):
        self._sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
