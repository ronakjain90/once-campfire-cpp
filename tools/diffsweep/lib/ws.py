"""A small WebSocket client for Action Cable (text frames, subprotocol actioncable-v1-json)."""
import base64
import os
import queue
import socket
import struct
import threading
import time

from httpx import Response

FIXED_KEY = base64.b64encode(b"diffsweep-key-16").decode()  # same key on both sides: same accept header


class CableSession:
    def __init__(self, port, host_header, cookie, origin, path="/cable", extra_headers=None, timeout=10):
        self.frames = queue.Queue()
        self.sock = None
        self.closed = False
        self.handshake = self._connect(port, host_header, cookie, origin, path, extra_headers or {}, timeout)

    def _connect(self, port, host, cookie, origin, path, extra, timeout):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        except OSError as e:
            return Response(0, [], b"", error=str(e))
        lines = [f"GET {path} HTTP/1.1", f"Host: {host}", "Upgrade: websocket", "Connection: Upgrade",
                 f"Sec-WebSocket-Key: {FIXED_KEY}", "Sec-WebSocket-Version: 13",
                 "Sec-WebSocket-Protocol: actioncable-v1-json, actioncable-unsupported"]
        if origin:
            lines.append(f"Origin: {origin}")
        if cookie:
            lines.append(f"Cookie: {cookie}")
        for k, v in extra.items():
            lines.append(f"{k}: {v}")
        s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        text = head.decode("latin-1").split("\r\n")
        try:
            status = int(text[0].split()[1])
        except (IndexError, ValueError):
            return Response(0, [], b"", error="bad handshake: " + repr(buf[:100]))
        headers = []
        for line in text[1:]:
            k, _, v = line.partition(":")
            headers.append((k.strip().lower(), v.strip()))
        if status != 101:
            # Read the rejection body if the server sends one with a length.
            clen = next((int(v) for k, v in headers if k == "content-length"), 0)
            while len(rest) < clen:
                chunk = s.recv(4096)
                if not chunk:
                    break
                rest += chunk
            s.close()
            return Response(status, headers, rest[:clen] if clen else rest)
        self.sock = s
        s.settimeout(None)
        self.pending = rest
        threading.Thread(target=self._read_loop, daemon=True).start()
        return Response(status, headers, b"")

    def _read_exact(self, n):
        while len(self.pending) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError
            self.pending += chunk
        out, self.pending = self.pending[:n], self.pending[n:]
        return out

    def _read_loop(self):
        message, mtype = b"", None
        try:
            while True:
                b1, b2 = self._read_exact(2)
                fin, op = b1 & 0x80, b1 & 0x0F
                ln = b2 & 0x7F
                if ln == 126:
                    ln = struct.unpack(">H", self._read_exact(2))[0]
                elif ln == 127:
                    ln = struct.unpack(">Q", self._read_exact(8))[0]
                payload = self._read_exact(ln)
                if op == 0x9:
                    self._send(0xA, payload)
                elif op == 0x8:
                    self.frames.put(("close", payload[2:].decode("utf-8", "replace") if len(payload) > 2 else "",
                                     struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 0))
                    break
                elif op in (0x1, 0x2, 0x0):
                    if op != 0x0:
                        mtype, message = op, b""
                    message += payload
                    if fin:
                        self.frames.put(("text", message.decode("utf-8", "replace"), 0))
                        message = b""
        except (EOFError, OSError):
            pass
        self.frames.put(("eof", "", 0))

    def _send(self, op, payload):
        mask = os.urandom(4)
        head = bytes([0x80 | op])
        n = len(payload)
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def send_text(self, text):
        self._send(0x1, text.encode())

    def collect(self, count, timeout, ignore_types=("ping",), settle=0.0):
        """Wait for `count` frames (excluding ignored types) or `timeout` seconds; with count 0,
        wait `settle` seconds and return what arrived. Returns a list of frame strings."""
        import json
        out = []
        deadline = time.time() + (settle if count == 0 else timeout)
        while count == 0 or len(out) < count:
            left = deadline - time.time()
            if left <= 0:
                break
            try:
                kind, text, code = self.frames.get(timeout=left)
            except queue.Empty:
                break
            if kind == "text":
                try:
                    if json.loads(text).get("type") in ignore_types:
                        continue
                except (ValueError, AttributeError):
                    pass
                out.append(text)
            elif kind == "close":
                out.append(f"<close {code} {text}>")
            else:
                out.append("<eof>")
                break
        return out

    def close(self):
        if self.sock and not self.closed:
            self.closed = True
            try:
                self._send(0x8, struct.pack(">H", 1000))
                self.sock.close()
            except OSError:
                pass
