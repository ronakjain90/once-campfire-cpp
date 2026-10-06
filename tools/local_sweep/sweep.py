# Local diff sweep of the room routes: the Rust binary against the C++ binary, with no Docker. See README.md.
import subprocess, os, sys, time, json, shutil, http.client, urllib.parse, re, signal

HERE = os.path.dirname(os.path.abspath(__file__))
RUN_DIR = os.environ.get("SWEEP_RUN_DIR", "/tmp/local_sweep")
RUST = os.environ.get("RUST_BIN", "/tmp/rustbuild/debug/campfire")
CPP = os.environ.get("CPP_BIN", "/tmp/cfbuild/campfire")
ENV = dict(os.environ, SECRET_KEY_BASE="5335c3b1ad35b4ad170c3413bd651ef3b6ed64e257261871a6de3f978cf3868ee417a927040935fb30b0f7debdedb34a2a403e9f34b16cf594c917c2ecd4a995",
           RAILS_ENV="production", DISABLE_SSL="true", APP_VERSION="parity", GIT_REVISION="parity", RAILS_LOG_LEVEL="warn",
           CAMPFIRE_FROZEN_TIME="2026-09-26T13:05:00Z", SKIP_TELEMETRY="true", VAPID_PUBLIC_KEY="BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz_qYBVicY02_VxTQ=",
           VAPID_PRIVATE_KEY="qfXLHghuG1rSHZUVo9SscNRI-0EIHRbIrfeGCqbAwak=", WEB_CONCURRENCY="1", JOB_CONCURRENCY="1")

class App:
    def __init__(self, name, binary, port):
        self.name, self.binary, self.port = name, binary, port
        self.dir = os.path.join(RUN_DIR, "run-" + name)
        shutil.rmtree(self.dir, ignore_errors=True)
        subprocess.check_call([sys.executable, os.path.join(HERE, "seed.py"), self.dir], stdout=subprocess.DEVNULL)
        env = dict(ENV, CAMPFIRE_STORAGE_PATH=self.dir, HTTP_PORT=str(port), TARGET_PORT=str(port + 100))
        self.proc = subprocess.Popen([binary] if name == "cpp" else [binary], env=env, cwd=self.dir,
                                     stdout=open(os.path.join(self.dir, "log"), "w"), stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                c = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                c.request("GET", "/up"); r = c.getresponse(); r.read()
                if r.status == 200:
                    break
            except Exception:
                time.sleep(0.2)
        else:
            raise SystemExit(name + " did not start; see " + self.dir + "/log")
        self.cookie = None
    def req(self, method, path, headers=None, body=None):
        h = {"Host": "campfire.test", "Accept": "text/html"}
        h.update(headers or {})
        if self.cookie and "Cookie" not in h:
            h["Cookie"] = self.cookie
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        return r.status, [(k.lower(), v) for k, v in r.getheaders()], data
    def sign_in(self, email="david@example.com"):
        self.cookie = None
        s, hs, _ = self.req("POST", "/session", {"Content-Type": "application/x-www-form-urlencoded", "Sec-Fetch-Site": "same-origin"},
                            "email_address=%s&password=secret123456" % email)
        assert s == 302, (self.name, s)
        for k, v in hs:
            if k == "set-cookie" and v.startswith("session_token="):
                self.cookie = v.split(";")[0]
        assert self.cookie, self.name
    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        try: self.proc.wait(5)
        except Exception: self.proc.kill()

IGNORE = {"date", "x-request-id", "x-runtime", "etag", "set-cookie"}
def normalize_body(b):
    return b

def compare(label, a, b, show):
    (s1, h1, b1), (s2, h2, b2) = a, b
    diffs = []
    if s1 != s2: diffs.append("status %s != %s" % (s1, s2))
    n1 = [k for k, v in h1 if k not in ("date",)]
    n2 = [k for k, v in h2 if k not in ("date",)]
    if n1 != n2: diffs.append("header names/order: rust=%s cpp=%s" % (n1, n2))
    d1 = {k: v for k, v in h1 if k not in IGNORE}
    d2 = {k: v for k, v in h2 if k not in IGNORE}
    for k in sorted(set(d1) | set(d2)):
        if d1.get(k) != d2.get(k):
            diffs.append("header %s: rust=%r cpp=%r" % (k, d1.get(k), d2.get(k)))
    if b1 != b2:
        i = 0
        while i < min(len(b1), len(b2)) and b1[i] == b2[i]: i += 1
        diffs.append("body differs at %d (rust %d bytes, cpp %d bytes)\n   rust: %r\n   cpp:  %r" % (i, len(b1), len(b2), b1[max(0,i-60):i+100], b2[max(0,i-60):i+100]))
    print(("DIFF " if diffs else "same ") + label)
    if diffs:
        for d in diffs: print("   " + d)
    return not diffs

FORM = {"Content-Type": "application/x-www-form-urlencoded", "Sec-Fetch-Site": "same-origin"}
SAME = {"Sec-Fetch-Site": "same-origin"}
def scenarios():
    g = lambda p, **kw: ("GET " + p, "GET", p, kw.get("headers"), None)
    return [
        g("/rooms"), g("/rooms/opens"), g("/rooms/closeds"), g("/rooms/directs"),
        g("/rooms/opens/1"), g("/rooms/closeds/2"), g("/rooms/directs/3"), g("/rooms/opens/3"), g("/rooms/closeds/99"), g("/rooms/opens/abc"),
        g("/rooms/opens/new"), g("/rooms/closeds/new"), g("/rooms/directs/new"),
        g("/rooms/opens/1/edit"), g("/rooms/opens/4/edit"), g("/rooms/closeds/2/edit"), g("/rooms/closeds/5/edit"), g("/rooms/directs/3/edit"), g("/rooms/directs/1/edit"),
        g("/rooms/new"), g("/rooms/1/edit"), g("/rooms/1/settings"),
        g("/users/me/sidebar"), g("/users/me/sidebar", headers={"Turbo-Frame": "user_sidebar"}),
        g("/rooms/opens/new", headers={"Turbo-Frame": "x"}),
        g("/rooms/1/involvement"), g("/rooms/3/involvement"), g("/rooms/2/involvement", headers={"Turbo-Frame": "involvement"}), g("/rooms/99/involvement"),
        g("/rooms/opens/1.json"),
        g("/rooms/opens/new", headers={"Accept": "application/json"}), g("/users/me/sidebar", headers={"Accept": "application/json"}),
        g("/rooms/1/involvement", headers={"Accept": "text/vnd.turbo-stream.html"}), g("/rooms/opens/1/edit", headers={"Accept": "*/*"}),
        g("/rooms/opens/new?x=1"), g("/rooms/directs/new", headers={"Turbo-Frame": "direct_rooms_control"}),
        g("/rooms/closeds/2/edit", headers={"Turbo-Frame": "x", "If-None-Match": "W/\"abc\""}),
        ("HEAD /rooms/opens/new", "HEAD", "/rooms/opens/new", None, None),
        ("POST sidebar", "POST", "/users/me/sidebar", FORM, "x=1"),
        ("cross-site POST", "POST", "/rooms/opens", {"Content-Type": "application/x-www-form-urlencoded", "Sec-Fetch-Site": "cross-site"}, "room%5Bname%5D=Evil"),
        ("JSON POST", "POST", "/rooms/opens", {"Content-Type": "application/json", "Sec-Fetch-Site": "same-origin"}, '{"room":{"name":"J"}}'),
    ]
def mutating():
    return [
        ("POST open", "POST", "/rooms/opens", FORM, "room%5Bname%5D=Lounge"),
        ("POST open noparam", "POST", "/rooms/opens", FORM, "x=1"),
        ("POST closed", "POST", "/rooms/closeds", FORM, "room%5Bname%5D=Club&user_ids%5B%5D=1&user_ids%5B%5D=3"),
        ("POST closed none", "POST", "/rooms/closeds", FORM, "room%5Bname%5D=Club"),
        ("POST direct", "POST", "/rooms/directs", FORM, "user_ids%5B%5D=4"),
        ("POST direct again", "POST", "/rooms/directs", FORM, "user_ids%5B%5D=2"),
        ("POST direct self", "POST", "/rooms/directs", FORM, "x=1"),
        ("PATCH open", "PATCH", "/rooms/opens/1", FORM, "room%5Bname%5D=Zoo"),
        ("PATCH open to closed target", "PATCH", "/rooms/closeds/1", FORM, "room%5Bname%5D=Zoo2&user_ids%5B%5D=1"),
        ("PATCH closed", "PATCH", "/rooms/closeds/2", FORM, "room%5Bname%5D=Dz&user_ids%5B%5D=1&user_ids%5B%5D=4"),
        ("PATCH closed to open", "PATCH", "/rooms/opens/2", FORM, "room%5Bname%5D=Dz"),
        ("PATCH direct", "PATCH", "/rooms/directs/3", FORM, "room%5Bname%5D=x"),
        ("PUT invol", "PUT", "/rooms/1/involvement?involvement=everything", SAME, None),
        ("PUT invol invisible", "PUT", "/rooms/1/involvement?involvement=invisible", SAME, None),
        ("PUT invol blank", "PUT", "/rooms/1/involvement?involvement=", SAME, None),
        ("PUT invol bad", "PUT", "/rooms/1/involvement?involvement=loud", SAME, None),
        ("PUT invol direct", "PUT", "/rooms/3/involvement?involvement=nothing", SAME, None),
        ("DELETE room", "DELETE", "/rooms/4", SAME, None),
        ("DELETE other's room", "DELETE", "/rooms/5", SAME, None),
        ("DELETE direct", "DELETE", "/rooms/directs/3", SAME, None),
        ("DELETE opens", "DELETE", "/rooms/opens/1", SAME, None),
        ("POST rooms", "POST", "/rooms", FORM, "room%5Bname%5D=x"),
    ]

def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    ok = True
    rust = App("rust", RUST, 18081); cpp = App("cpp", CPP, 18082)
    try:
        for email in ("david@example.com", "jason@example.com"):
            rust.sign_in(email); cpp.sign_in(email)
            print("== signed in as", email)
            for label, method, path, hd, body in scenarios():
                if only in label:
                    ok &= compare(label, rust.req(method, path, hd, body), cpp.req(method, path, hd, body), True)
            for label, method, path, hd, body in mutating():
                if only in label:
                    a = rust.req(method, path, hd, body); b = cpp.req(method, path, hd, body)
                    ok &= compare(email.split("@")[0] + " " + label, a, b, True)
                    # the state after the change
                    ok &= compare("  after: sidebar", rust.req("GET", "/users/me/sidebar"), cpp.req("GET", "/users/me/sidebar"), True)
        # signed out, and an account that limits room creation
        for label, method, path, hd, body in scenarios()[:40]:
            if only in label:
                rust.cookie = cpp.cookie = None
                ok &= compare("anon " + label, rust.req(method, path, hd, body), cpp.req(method, path, hd, body), True)
        import sqlite3
        for app in (rust, cpp):
            db = sqlite3.connect(os.path.join(app.dir, "db", "production.sqlite3"))
            db.execute("update accounts set settings = '{\"restrict_room_creation_to_administrators\":true}'"); db.commit(); db.close()
        rust.sign_in("jason@example.com"); cpp.sign_in("jason@example.com")
        for label, method, path, hd, body in [("restricted GET open new", "GET", "/rooms/opens/new", None, None), ("restricted GET closed new", "GET", "/rooms/closeds/new", None, None),
                ("restricted POST open", "POST", "/rooms/opens", FORM, "room%5Bname%5D=x"), ("restricted GET sidebar", "GET", "/users/me/sidebar", None, None),
                ("restricted GET direct new", "GET", "/rooms/directs/new", None, None)]:
            ok &= compare(label, rust.req(method, path, hd, body), cpp.req(method, path, hd, body), True)
        rust.sign_in("david@example.com"); cpp.sign_in("david@example.com")
        ok &= compare("restricted admin sidebar", rust.req("GET", "/users/me/sidebar"), cpp.req("GET", "/users/me/sidebar"), True)
    finally:
        rust.stop(); cpp.stop()
    print("ALL SAME" if ok else "DIFFERENCES")
if __name__ == "__main__":
    main()
