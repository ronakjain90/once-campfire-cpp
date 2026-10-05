#!/usr/bin/env python3
"""Runs inside a container with --network host. Sends the G3 requests to the traced app on 127.0.0.1:4490
and saves raw request/response bytes plus wall-clock windows (unix seconds) under /out/raw."""
import gzip, hashlib, json, os, socket, subprocess, sys, time, zlib

OUT = "/out/raw"
BASE = "127.0.0.1:4490"
LABELS = json.load(open("/seed/labels.json"))
ROOM, HQ = LABELS["rooms.watercooler"], LABELS["rooms.hq"]
LG = "/out/bin/loadgen"
os.makedirs(OUT, exist_ok=True)
windows = []  # {name, start, end}

def urlencode(s):
    return "".join(chr(b) if (48 <= b <= 57 or 65 <= b <= 90 or 97 <= b <= 122 or chr(b) in "-_.~") else "%%%02X" % b for b in s.encode())

def raw_request(method, path, headers, body=b""):
    """Same header order as loadgen (hyper): host, then the caller's headers, lower-case names."""
    lines = [f"{method} {path} HTTP/1.1", f"host: {BASE}"] + [f"{k}: {v}" for k, v in headers]
    if body or method == "POST":
        lines.append(f"content-length: {len(body)}")
    req = ("\r\n".join(lines) + "\r\n\r\n").encode() + body
    s = socket.create_connection(("127.0.0.1", 4490))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    t0 = time.time()
    s.sendall(req)
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = s.recv(65536)
        if not d:
            raise RuntimeError("closed early")
        buf += d
    head, rest = buf.split(b"\r\n\r\n", 1)
    hl = head.decode("latin1").split("\r\n")
    status = int(hl[0].split()[1])
    hdrs = [tuple(l.split(": ", 1)) for l in hl[1:]]
    hd = {k.lower(): v for k, v in hdrs}
    if hd.get("transfer-encoding") == "chunked":
        body_b = b""
        while True:
            while b"\r\n" not in rest:
                rest += s.recv(65536)
            size_line, rest = rest.split(b"\r\n", 1)
            n = int(size_line.split(b";")[0], 16)
            while len(rest) < n + 2:
                rest += s.recv(65536)
            body_b += rest[:n]
            rest = rest[n + 2:]
            if n == 0:
                break
    else:
        n = int(hd.get("content-length", 0))
        while len(rest) < n:
            rest += s.recv(65536)
        body_b = rest[:n]
    t1 = time.time()
    s.close()
    return dict(req=req, status=status, head=head, headers=hdrs, body=body_b, t0=t0, t1=t1)

def mark(n):
    t0 = time.time()
    r = raw_request("GET", f"/up?mark={n}", [])
    windows.append(dict(name=f"mark {n}", start=t0, end=time.time(), status=r["status"]))

def save(prefix, r):
    open(f"{OUT}/{prefix}.request.txt", "wb").write(r["req"])
    open(f"{OUT}/{prefix}.response-headers.txt", "wb").write(r["head"] + b"\r\n")
    open(f"{OUT}/{prefix}.body.raw", "wb").write(r["body"])
    if r["headers"] and dict((k.lower(), v) for k, v in r["headers"]).get("content-encoding") == "gzip":
        open(f"{OUT}/{prefix}.body.decoded", "wb").write(gzip.decompress(r["body"]))
    else:
        open(f"{OUT}/{prefix}.body.decoded", "wb").write(r["body"])

def lg(*args):
    return subprocess.run([LG, *args], check=True, capture_output=True, text=True).stdout

def timed(name, fn):
    mark(name + ".before")
    time.sleep(0.3)
    t0 = time.time()
    r = fn()
    t1 = time.time()
    windows.append(dict(name=name, start=t0, end=t1, status=r["status"]))
    time.sleep(1.0)
    mark(name + ".after")
    time.sleep(0.3)
    return r

cookie = json.loads(lg("login", "--base", f"http://{BASE}", "--email", LABELS["emails.david"], "--password", LABELS["passwords.all"]))["cookie"]
scrape = json.loads(lg("scrape", "--base", f"http://{BASE}", "--cookie", cookie, "--room", str(ROOM)))
csrf = scrape["csrf"] or ""
json.dump(dict(scrape=scrape, cookie_names=[c.split("=")[0] for c in cookie.split("; ")]), open(f"{OUT}/login.json", "w"), indent=1)
time.sleep(2)

summary = {}
for enc, tag in (("gzip", "gz"), ("identity", "id")):
    hashes = []
    for i in (1, 2, 3):
        r = timed(f"GET room {enc} #{i}", lambda: raw_request("GET", f"/rooms/{ROOM}", [("cookie", cookie), ("accept-encoding", enc)]))
        save(f"room-{tag}-{i}", r)
        dec = gzip.decompress(r["body"]) if enc == "gzip" and dict((k.lower(), v) for k, v in r["headers"]).get("content-encoding") == "gzip" else r["body"]
        hashes.append(dict(status=r["status"], raw_bytes=len(r["body"]), decoded_bytes=len(dec), sha256_decoded=hashlib.sha256(dec).hexdigest()))
    summary[f"room_{enc}"] = hashes

# Validation with the real load generator: one POST, which loadgen counts as ok when status < 400.
mark("loadgen-validate.before")
v = json.loads(lg("http", "--base", f"http://{BASE}", "--cookie", cookie, "--post-room", str(HQ), "--csrf", csrf, "--conc", "1", "--requests", "1", "--duration", "5"))
open(f"{OUT}/loadgen-validate.json", "w").write(json.dumps(v, indent=1))
time.sleep(2)
mark("loadgen-validate.after")
open(f"{OUT}/pre-posts-marker.txt", "w").write(str(time.time()))
time.sleep(1)

posts = []
for i in range(1, 12):
    nonce = "%x%x" % (time.time_ns(), i)
    body = "&".join(f"{urlencode(k)}={urlencode(v)}" for k, v in (("message[body]", f"bench write {i}"), ("message[client_message_id]", nonce), ("authenticity_token", csrf))).encode()
    hdrs = [("cookie", cookie), ("content-type", "application/x-www-form-urlencoded"),
            ("accept", "text/vnd.turbo-stream.html, text/html, application/xhtml+xml"),
            ("x-csrf-token", csrf), ("sec-fetch-site", "same-origin"), ("accept-encoding", "gzip")]
    r = timed(f"POST message #{i}", lambda: raw_request("POST", f"/rooms/{HQ}/messages", hdrs, body))
    save(f"post-{i}", r)
    posts.append(dict(i=i, status=r["status"], content_type=dict((k.lower(), v) for k, v in r["headers"]).get("content-type")))
    time.sleep(1.5)
summary["posts"] = posts
json.dump(dict(summary=summary, windows=windows), open(f"{OUT}/windows.json", "w"), indent=1)
print(json.dumps(summary, indent=1))
