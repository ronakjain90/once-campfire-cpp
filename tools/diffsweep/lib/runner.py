"""Runs the scenarios of a request list against the expected and the actual app, in lockstep."""
import json
import re
import urllib.parse

import compare
from httpx import Client, multipart, urlencode
from ws import CableSession

VAR = re.compile(r"\{\{\s*([^}|]+?)\s*(?:\|\s*(\w+))?\s*\}\}")
FILTERS = {"urlencode": lambda s: urllib.parse.quote(s, safe=""), "": lambda s: s}


class StepError(Exception):
    pass


class Run:
    def __init__(self, pair, labels, host_header, options):
        self.pair, self.labels, self.host = pair, labels, host_header
        self.options = options  # default compare options (ignored, random_cookies, normalizers)
        self.results = []
        self.reset_state()

    def reset_state(self):
        self.clients = {}   # actor -> [Client expected, Client actual]
        self.vars = [{}, {}]
        self.cables = {}    # id -> [CableSession, CableSession]

    # ---- helpers
    def client(self, actor):
        if actor not in self.clients:
            self.clients[actor] = [Client(a.port, self.host) for a in self.pair.sides]
        return self.clients[actor]

    def interp(self, value, side):
        if isinstance(value, str):
            def sub(m):
                key, flt = m.group(1), m.group(2) or ""
                if key in self.vars[side]:
                    v = self.vars[side][key]
                elif key in self.labels:
                    v = self.labels[key]
                else:
                    raise StepError(f"undefined variable {{{{{key}}}}}")
                return FILTERS[flt](str(v))
            return VAR.sub(sub, value)
        if isinstance(value, dict):
            return {k: self.interp(v, side) for k, v in value.items()}
        if isinstance(value, list):
            return [self.interp(v, side) for v in value]
        return value

    def opts(self, scenario, step):
        o = dict(self.options)
        for src in (scenario, step):
            o["ignored"] = set(o.get("ignored", ())) | {h.lower() for h in src.get("ignore_headers", ())}
            o["random_cookies"] = set(o.get("random_cookies", ())) | set(src.get("random_cookies", ()))
            o["normalizers"] = list(o.get("normalizers", ())) + list(src.get("normalize", ()))
        return o

    def record(self, ctx, name, method, path, exp, act, problems, report):
        self.results.append({"area": ctx["area"], "list": ctx["list"], "scenario": ctx["scenario"],
                             "name": name, "method": method, "path": path,
                             "status": [getattr(exp, "status", None), getattr(act, "status", None)],
                             "ok": not problems, "problems": problems, "report": report})

    # ---- capture
    def capture(self, spec, responses, ctx):
        for var, how in spec.items():
            for side, resp in enumerate(responses):
                src = how.get("from", "body")
                if src == "body":
                    text = resp.text()
                elif src == "status":
                    text = str(resp.status)
                elif src.startswith("header:"):
                    text = resp.get(src[7:].lower()) or ""
                elif src == "json":
                    text = resp.text()
                else:
                    raise StepError(f"bad capture source {src}")
                if "json" in how:
                    try:
                        node = json.loads(resp.text())
                        for part in how["json"].split("."):
                            node = node[int(part)] if isinstance(node, list) else node[part]
                        text = str(node)
                    except (ValueError, KeyError, IndexError, TypeError):
                        raise StepError(f"capture {var}: json path {how['json']} not found on side {side}")
                if "regex" in how:
                    m = re.search(how["regex"], text, re.S)
                    if not m and how.get("optional"):
                        self.vars[side][var] = ""
                        continue
                    if not m:
                        raise StepError(f"capture {var}: regex {how['regex']!r} not found on "
                                        f"{'expected' if side == 0 else 'actual'} (status {resp.status})")
                    text = m.group(1) if m.groups() else m.group(0)
                self.vars[side][var] = text.replace("&amp;", "&") if how.get("unescape") else text

    # ---- csrf
    def ensure_csrf(self, actor, side):
        c = self.client(actor)[side]
        if c.csrf:
            return c.csrf
        page = "/users/me/profile" if c.logged_in else "/session/new"
        r = c.request("GET", page)
        m = re.search(r'<meta name="csrf-token" content="([^"]+)"', r.text())
        if not m:
            raise StepError(f"no csrf token on {page} for {actor} (status {r.status})")
        c.csrf = m.group(1)
        return c.csrf

    # ---- request
    def build(self, step, side):
        method = step.get("method", "GET").upper()
        path = self.interp(step["path"], side)
        headers = dict(self.interp(step.get("headers", {}), side))
        body = None
        if "form" in step:
            body = urlencode(self.interp(step["form"], side))
            headers.setdefault("Content-Type", "application/x-www-form-urlencoded")
        elif "json" in step:
            body = json.dumps(self.interp(step["json"], side), separators=(",", ":")).encode()
            headers.setdefault("Content-Type", "application/json")
        elif "multipart" in step:
            m = self.interp(step["multipart"], side)
            files = []
            for f in m.get("files", []):
                data = f["text"].encode() if "text" in f else open(f["path"], "rb").read()
                files.append({"name": f["name"], "filename": f["filename"],
                              "content_type": f.get("content_type", "application/octet-stream"), "data": data})
            body, ctype = multipart(m.get("fields", {}), files)
            headers.setdefault("Content-Type", ctype)
        elif "body" in step:
            body = self.interp(step["body"], side).encode()
        if "user_agent" in step:
            headers["User-Agent"] = self.interp(step["user_agent"], side)
        if "accept" in step:
            headers["Accept"] = step["accept"]
        if "accept_encoding" in step:
            headers["Accept-Encoding"] = step["accept_encoding"]
        return method, path, headers, body

    def do_request(self, ctx, step):
        actor = step.get("actor", "anon")
        method = step.get("method", "GET").upper()
        name = step.get("name") or f"{method} {step['path']}"
        o = self.opts(ctx["scenario_def"], step)
        resps, revisits, shown_path = [], [], step["path"]
        for side in (0, 1):
            m, path, headers, body = self.build(step, side)
            shown_path = path if side == 0 else shown_path
            c = self.client(actor)[side]
            if m not in ("GET", "HEAD") and step.get("csrf", True) and "X-CSRF-Token" not in headers:
                headers["X-CSRF-Token"] = self.ensure_csrf(actor, side)
                headers.setdefault("Sec-Fetch-Site", "same-origin")
            r = c.request(m, path, headers, body, use_cookies=step.get("cookies", True))
            resps.append(r)
            if step.get("revisit") and not r.error:
                rv = dict(headers)
                if r.get("etag"):
                    rv["If-None-Match"] = r.get("etag")
                if r.get("last-modified"):
                    rv["If-Modified-Since"] = r.get("last-modified")
                revisits.append(c.request(m, path, rv, body, use_cookies=step.get("cookies", True)))
            if step.get("drop_session") or (m == "DELETE" and path.rstrip("/") == "/session"):
                c.csrf = None
                c.logged_in = False if m == "DELETE" else c.logged_in
        problems, report = compare.compare_responses(resps[0], resps[1], o)
        self.record(ctx, name, method, shown_path, resps[0], resps[1], problems, report)
        if revisits:
            p2, r2 = compare.compare_responses(revisits[0], revisits[1], o)
            self.record(ctx, name + " (revisit)", method, shown_path, revisits[0], revisits[1], p2, r2)
        if step.get("capture"):
            self.capture(step["capture"], resps, ctx)
        return resps

    # ---- login
    def do_login(self, ctx, step):
        actor = step["actor"]
        clients = self.client(actor)
        if all(c.logged_in for c in clients) and not step.get("force"):
            return
        email = step.get("email") or "{{emails." + actor + "}}"
        password = step.get("password") or "{{passwords.all}}"
        for c in clients:
            c.cookies.clear()
            c.csrf = None
        page = {"actor": actor, "path": "/session/new", "name": f"login {actor}: sign-in form"}
        resps = self.do_request(ctx, page)
        tokens = []
        for r in resps:
            m = re.search(r'name="authenticity_token" value="([^"]+)"', r.text()) or \
                re.search(r'<meta name="csrf-token" content="([^"]+)"', r.text())
            tokens.append(m.group(1) if m else "")
        for side in (0, 1):
            self.vars[side]["_login_token"] = tokens[side]
        post = {"actor": actor, "method": "POST", "path": "/session", "name": f"login {actor}: POST /session",
                "form": {"authenticity_token": "{{_login_token}}", "email_address": email, "password": password},
                "csrf": False, "headers": {"Sec-Fetch-Site": "same-origin"}}
        resps = self.do_request(ctx, post)
        self.pair.logins += 1
        for c, r in zip(clients, resps):
            c.logged_in = r.status in (302, 303)
            c.csrf = None

    # ---- cable
    def do_cable(self, ctx, step):
        op, cid = step["op"], step.get("id", "cable")
        o = self.opts(ctx["scenario_def"], step)
        if op == "cable_connect":
            actor = step.get("actor", "anon")
            sess = []
            for side, c in enumerate(self.client(actor)):
                sess.append(CableSession(c.port, self.host, c.cookie_header(), step.get("origin", f"http://{self.host}"),
                                         step.get("path", "/cable"), self.interp(step.get("headers", {}), side)))
            self.cables[cid] = sess
            problems, report = compare.compare_responses(sess[0].handshake, sess[1].handshake, o)
            self.record(ctx, f"cable {cid}: connect", "GET", step.get("path", "/cable"),
                        sess[0].handshake, sess[1].handshake, problems, report)
            return
        sess = self.cables.get(cid)
        if sess is None:
            raise StepError(f"no cable session {cid}")
        if op in ("cable_subscribe", "cable_perform"):
            for side, s in enumerate(sess):
                if not s.sock:
                    continue
                ident = self.interp(step["identifier"], side)
                ident = ident if isinstance(ident, str) else json.dumps(ident, separators=(",", ":"))
                msg = {"command": "subscribe" if op == "cable_subscribe" else "message", "identifier": ident}
                if op == "cable_perform":
                    data = dict(self.interp(step.get("data", {}), side))
                    data["action"] = step["action"]
                    msg["data"] = json.dumps(data, separators=(",", ":"))
                s.send_text(json.dumps(msg, separators=(",", ":")))
        elif op == "cable_wait":
            frames = [s.collect(step.get("count", 1), step.get("timeout", 10), tuple(step.get("ignore_types", ["ping"])),
                                step.get("settle", 0.0)) if s.sock else ["<not connected>"] for s in sess]
            problems, report = compare.compare_frames(frames[0], frames[1], o, step.get("sorted", False))
            self.record(ctx, step.get("name") or f"cable {cid}: wait {step.get('count', 1)} frame(s)", "WS",
                        step.get("label", "/cable"), _N(len(frames[0])), _N(len(frames[1])), problems, report)
            if step.get("capture"):
                for var, how in step["capture"].items():
                    for side in (0, 1):
                        text = "\n".join(frames[side])
                        m = re.search(how["regex"], text, re.S)
                        if not m:
                            raise StepError(f"capture {var}: not found in frames")
                        self.vars[side][var] = m.group(1) if m.groups() else m.group(0)
        elif op == "cable_close":
            for s in sess:
                s.close()
            del self.cables[cid]
        else:
            raise StepError(f"unknown cable op {op}")

    def close_cables(self):
        for sess in self.cables.values():
            for s in sess:
                s.close()
        self.cables.clear()

    # ---- scenario
    def run_scenario(self, ctx, scenario):
        ctx = dict(ctx, scenario=scenario["name"], scenario_def=scenario)
        before = len(self.results)
        try:
            for step in scenario["steps"]:
                op = step.get("op", "request")
                if op == "request":
                    self.do_request(ctx, step)
                elif op == "login":
                    self.do_login(ctx, step)
                elif op == "set":
                    for side in (0, 1):
                        for k, v in step["vars"].items():
                            self.vars[side][k] = self.interp(v, side)
                elif op.startswith("cable_"):
                    self.do_cable(ctx, step)
                else:
                    raise StepError(f"unknown op {op}")
        except Exception as e:  # a failed step ends the scenario and counts as a difference
            self.results.append({"area": ctx["area"], "list": ctx["list"], "scenario": scenario["name"],
                                 "name": f"ERROR in step {step.get('name') or step.get('path') or op}",
                                 "method": "", "path": "", "status": [None, None], "ok": False,
                                 "problems": ["step error"], "report": f"{type(e).__name__}: {e}"})
        finally:
            self.close_cables()
        return self.results[before:]


class _N:
    """Stands in for a response in frame results: .status holds the frame count."""
    def __init__(self, n):
        self.status = n
