# Local diff sweep of Action Cable: the Rust binary against the C++ binary on the same small database. Each scenario
# opens WebSockets on both apps, does the same steps, and compares the handshake and the frames. See README.md.
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "diffsweep", "lib"))
import sweep  # noqa: E402
from ws import CableSession  # noqa: E402

HOST = "campfire.test"
ORIGIN = "http://" + HOST
DESIGNERS, ARCHIVE, DIRECT = 4, 5, 3  # HQ (everybody is in), Secret (David is not in), a direct room


def ident(**kw):
    return json.dumps(kw, separators=(",", ":"))


class Pair:
    """The same steps on both apps."""

    def __init__(self, rust, cpp):
        self.apps = (rust, cpp)
        self.sessions = {}
        self.failures = 0

    def login(self, email):
        for a in self.apps:
            a.sign_in(email)

    def cookie(self, app):
        return app.cookie

    def connect(self, name, origin=ORIGIN, path="/cable", cookie=True, expect=None):
        pair = []
        for a in self.apps:
            pair.append(CableSession(a.port, HOST, a.cookie if cookie else None, origin, path))
        self.sessions[name] = pair
        self.compare_handshake(name, pair)

    def compare_handshake(self, name, pair):
        a, b = pair[0].handshake, pair[1].handshake
        keep = lambda r: [(k, v) for k, v in r.headers if k not in ("date", "x-request-id", "x-runtime")]
        problems = []
        if a.status != b.status:
            problems.append("status %s != %s" % (a.status, b.status))
        if [k for k, _ in keep(a)] != [k for k, _ in keep(b)]:
            problems.append("header order: rust=%s cpp=%s" % ([k for k, _ in keep(a)], [k for k, _ in keep(b)]))
        for (k1, v1), (k2, v2) in zip(keep(a), keep(b)):
            if k1 == k2 and v1 != v2:
                problems.append("header %s: rust=%r cpp=%r" % (k1, v1, v2))
        if a.body != b.body:
            problems.append("body: rust=%r cpp=%r" % (a.body, b.body))
        self.report("connect " + name, problems)

    def subscribe(self, name, identifier):
        for s in self.sessions[name]:
            if s.sock:
                s.send_text(json.dumps({"command": "subscribe", "identifier": identifier}, separators=(",", ":")))

    def perform(self, name, identifier, action, data=None):
        d = dict(data or {})
        d["action"] = action
        for s in self.sessions[name]:
            if s.sock:
                s.send_text(json.dumps({"command": "message", "identifier": identifier,
                                        "data": json.dumps(d, separators=(",", ":"))}, separators=(",", ":")))

    def wait(self, name, label, count=1, timeout=8, settle=0.0, sorted_=False, ignore=("ping",)):
        frames = [s.collect(count, timeout, ignore, settle) if s.sock else ["<not connected>"] for s in self.sessions[name]]
        a, b = frames
        if sorted_:
            a, b = sorted(a), sorted(b)
        problems = [] if a == b else ["rust=%s\n      cpp= %s" % (a, b)]
        self.report("%s: %s (%d/%d frames)" % (name, label, len(frames[0]), len(frames[1])), problems)
        return frames

    def close(self, name):
        for s in self.sessions.pop(name, []):
            s.close()

    def http(self, actor_email, method, path, headers=None, body=None):
        out = []
        for a in self.apps:
            a.sign_in(actor_email)
            out.append(a.req(method, path, headers, body))
        return out

    def report(self, label, problems):
        print(("DIFF " if problems else "same ") + label)
        for p in problems:
            print("   " + p)
        self.failures += bool(problems)


FORM = {"Content-Type": "application/x-www-form-urlencoded", "Sec-Fetch-Site": "same-origin"}
SAME = {"Sec-Fetch-Site": "same-origin"}


def streams_of(app, email, room):
    """The signed stream names that the room page prints, as the Rust page and the C++ page give them."""
    app.sign_in(email)
    status, _, body = app.req("GET", "/rooms/%d" % room)
    text = body.decode("utf-8", "replace")
    m = re.search(r'channel="RoomMessagesChannel" signed-stream-name="([^"]+)"', text)
    return m.group(1).replace("&amp;", "&") if m else None


def scenario_subscribe(p):
    p.login("david@example.com")
    stream = streams_of(p.apps[0], "david@example.com", DESIGNERS)
    p.connect("d")
    for ch in ({"channel": "HeartbeatChannel"}, {"channel": "ReadRoomsChannel"}, {"channel": "UnreadRoomsChannel"},
               {"channel": "PresenceChannel", "room_id": str(DESIGNERS)}, {"channel": "TypingNotificationsChannel", "room_id": str(DESIGNERS)},
               {"channel": "RoomChannel", "room_id": DESIGNERS}, {"channel": "RoomMessagesChannel", "signed_stream_name": stream}):
        p.subscribe("d", json.dumps(ch, separators=(",", ":")))
    p.wait("d", "welcome, confirmations, read frame", count=9, settle=0.5, sorted_=True)
    for ch in ({"channel": "PresenceChannel", "room_id": str(ARCHIVE)}, {"channel": "TypingNotificationsChannel", "room_id": 99},
               {"channel": "RoomMessagesChannel", "signed_stream_name": "bad"}, {"channel": "NoSuchChannel"},
               {"channel": "RoomChannel", "room_id": "abc"}, {"channel": "RoomChannel"}, {"channel": "::RoomChannel", "room_id": DESIGNERS},
               {"channel": "Turbo::StreamsChannel", "signed_stream_name": stream}, {"channel": "Turbo::StreamsChannel", "signed_stream_name": "x"},
               {"channel": "Turbo::StreamsChannel"}, {"channel": "RoomMessagesChannel", "signed_stream_name": 5}):
        p.subscribe("d", json.dumps(ch, separators=(",", ":")))
    p.wait("d", "rejections", count=11, settle=0.5)
    p.perform("d", ident(channel="RoomMessagesChannel", signed_stream_name=stream), "verified_stream_name_from_params")
    p.perform("d", ident(channel="RoomChannel", room_id=DESIGNERS), "nothing")
    p.wait("d", "unknown actions", count=0, settle=0.5)
    p.close("d")


def scenario_typing(p):
    p.login("david@example.com")
    p.connect("d")
    p.connect("j", cookie=True)
    for app in p.apps:
        pass
    # Jason connects with his own session.
    p.close("j")
    for a in p.apps:
        a.sign_in("jason@example.com")
    p.connect("j")
    for n in ("d", "j"):
        p.subscribe(n, ident(channel="TypingNotificationsChannel", room_id=str(DESIGNERS)))
    p.wait("d", "welcome and confirm", count=2)
    p.wait("j", "welcome and confirm", count=2)
    p.perform("j", ident(channel="TypingNotificationsChannel", room_id=str(DESIGNERS)), "start")
    p.perform("j", ident(channel="TypingNotificationsChannel", room_id=str(DESIGNERS)), "stop")
    p.wait("d", "sees typing", count=2)
    p.wait("j", "sees own typing", count=2)
    p.subscribe("d", ident(channel="PresenceChannel", room_id=str(DESIGNERS)))
    p.perform("d", ident(channel="PresenceChannel", room_id=str(DESIGNERS)), "refresh")
    p.perform("d", ident(channel="PresenceChannel", room_id=str(DESIGNERS)), "absent")
    p.perform("d", ident(channel="PresenceChannel", room_id=str(DESIGNERS)), "present")
    p.wait("d", "presence confirm", count=1, settle=0.8)
    p.close("d")
    p.close("j")


def scenario_rejected(p):
    p.connect("v", cookie=False)
    p.wait("v", "visitor is told to go", count=2)
    p.connect("bad_origin", origin="http://evil.example", cookie=False)
    p.login("david@example.com")
    p.connect("d2", origin="http://evil.example")
    p.connect("d3", path="/cable?x=1")
    p.wait("d3", "welcome", count=1)
    p.close("d3")


def scenario_room_broadcasts(p):
    """The sidebar streams: a room that is created and removed reaches the sockets that listen."""
    p.login("david@example.com")
    p.login("jason@example.com")
    for a in p.apps:
        a.sign_in("david@example.com")
    p.connect("d")
    # David's own `rooms` stream is the signed name that the sidebar prints.
    streams = []
    for a in p.apps:
        a.sign_in("david@example.com")
        _, _, body = a.req("GET", "/users/me/sidebar")
        names = re.findall(r'channel="Turbo::StreamsChannel" signed-stream-name="([^"]+)"', body.decode())
        streams.append([n.replace("&amp;", "&") for n in names])
    for i, s in enumerate(p.sessions["d"]):
        for n in streams[i]:
            s.send_text(json.dumps({"command": "subscribe", "identifier": ident(channel="Turbo::StreamsChannel", signed_stream_name=n)}, separators=(",", ":")))
    p.wait("d", "welcome and two confirmations", count=3, sorted_=True)
    for a in p.apps:
        a.sign_in("david@example.com")
    p.http("david@example.com", "POST", "/rooms/opens", FORM, "room%5Bname%5D=Lounge")
    p.http("david@example.com", "PATCH", "/rooms/opens/6", FORM, "room%5Bname%5D=Den")
    p.http("david@example.com", "POST", "/rooms/closeds", FORM, "room%5Bname%5D=Club&user_ids%5B%5D=1&user_ids%5B%5D=3")
    p.http("david@example.com", "DELETE", "/rooms/6", SAME)
    p.wait("d", "create, update, create, remove", count=4, settle=0.8)
    p.close("d")


def scenario_revocation(p):
    """A membership that goes, a sign-out and a deactivation close the sockets of the user."""
    p.login("kevin@example.com")
    p.connect("k")
    p.subscribe("k", ident(channel="TypingNotificationsChannel", room_id=str(ARCHIVE)))
    p.subscribe("k", ident(channel="PresenceChannel", room_id=str(ARCHIVE)))
    p.wait("k", "welcome and confirmations", count=3, settle=0.5)
    # Jason (the creator of room 5) takes Kevin out of it.
    p.http("jason@example.com", "PATCH", "/rooms/closeds/5", FORM, "room%5Bname%5D=Secret&user_ids%5B%5D=2")
    p.wait("k", "kevin is disconnected with reconnect", count=2, settle=0.8)
    p.close("k")
    # Kevin connects again and the room is turned away.
    p.login("kevin@example.com")
    p.connect("k2")
    p.subscribe("k2", ident(channel="TypingNotificationsChannel", room_id=str(ARCHIVE)))
    p.wait("k2", "welcome and rejection", count=2, settle=0.5)
    p.close("k2")
    # Sign-out closes the sockets of the user.
    p.login("anna@example.com")
    p.connect("a")
    p.wait("a", "welcome", count=1)
    for a in p.apps:
        a.req("DELETE", "/session", SAME)
    p.wait("a", "signed out", count=1, settle=0.8)
    p.close("a")


SCENARIOS = {"subscribe": scenario_subscribe, "typing": scenario_typing, "rejected": scenario_rejected,
             "rooms": scenario_room_broadcasts, "revocation": scenario_revocation}


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    failures = 0
    for name, fn in SCENARIOS.items():
        if only not in name:
            continue
        print("== scenario", name)
        rust = sweep.App("rust", sweep.RUST, 18081)
        cpp = sweep.App("cpp", sweep.CPP, 18082)
        pair = Pair(rust, cpp)
        try:
            fn(pair)
        finally:
            for n in list(pair.sessions):
                pair.close(n)
            rust.stop()
            cpp.stop()
        failures += pair.failures
    print("ALL SAME" if not failures else "%d DIFFERENCES" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
