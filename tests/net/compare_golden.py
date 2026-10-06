#!/usr/bin/env python3
"""Compare the replies of a server with the golden captures of the Rust port.
Usage: compare_golden.py PORT   (checks status line, header names and order, header values
except x-request-id, x-runtime and date, and the body bytes)."""
import os
import re
import socket
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = [
    ("up-identity", "GET /up HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"),
    ("up-gzip", "GET /up HTTP/1.1\r\nHost: x\r\nConnection: close\r\nAccept-Encoding: gzip\r\n\r\n"),
    ("up-head", "HEAD /up HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"),
    ("nope", "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"),
    ("nope-gzip", "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\nAccept-Encoding: gzip\r\n\r\n"),
]
VARIABLE = {"x-request-id", "x-runtime", "date"}


def exchange(port, request):
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(request.encode())
    s.settimeout(3)
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except OSError:
        pass
    return data


def normalize(raw):
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    out = [lines[0]]
    for line in lines[1:]:
        name = line.split(":", 1)[0].lower()
        out.append(name + ": <var>" if name in VARIABLE else line)
    return out, body


def main():
    port = int(sys.argv[1])
    failed = 0
    for name, request in CASES:
        golden = open(os.path.join(HERE, "golden", name + ".raw"), "rb").read()
        reply = exchange(port, request)
        g_head, g_body = normalize(golden)
        r_head, r_body = normalize(reply)
        ok = g_head == r_head and g_body == r_body
        print(("PASS " if ok else "FAIL ") + name)
        if not ok:
            failed += 1
            print("  golden head:", g_head)
            print("  reply  head:", r_head)
            print("  body equal:", g_body == r_body, len(g_body), len(r_body))
    return 1 if failed else 0


sys.exit(main())
