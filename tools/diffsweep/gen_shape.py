"""The request set of reference-tools/http_shape/sweep.py as diffsweep scenarios (seed default)."""
import gen_lists as g

TURBO = "text/vnd.turbo-stream.html, text/html, application/xhtml+xml"
HTML = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
FORM = "application/x-www-form-urlencoded"
OLD_UA = "Mozilla/5.0 (Windows NT 10.0) Chrome/40.0 Safari/537.36"


def r(name, path, method="GET", headers=None, **kw):
    step = {"name": name, "actor": "david", "path": path}
    if method != "GET":
        step["method"] = method
    if headers:
        step["headers"] = headers
    step.update(kw)
    return step


def anon(step):
    step.pop("actor", None)
    step["cookies"] = False
    return step


def steps():
    hq, pets, direct = "{{rooms.hq}}", "{{rooms.pets}}", "{{rooms.david_and_jason}}"
    des, wc = "{{rooms.designers}}", "{{rooms.watercooler}}"
    first, image = "{{messages.first}}", "{{messages.image}}"
    since = 1772467200000
    stream, js = {"Accept": TURBO}, {"Accept": "application/json"}
    discover = [
        r("discover room page", f"/rooms/{des}", capture={
            "css": {"regex": r'href="(/assets/[^"]+\.css)"', "unescape": True, "optional": True},
            "js": {"regex": r'"(/assets/[^"]+\.js)"', "unescape": True, "optional": True},
            "image": {"regex": r'src="(/assets/[^"]+\.(?:svg|png))"', "unescape": True, "optional": True},
            "avatar_initials": {"regex": r'src="(/users/[^"/]+/avatar\?v=[^"]+)"[^>]*alt="David',
                                "unescape": True, "optional": True},
            "representation": {"regex": r'"(/rails/active_storage/representations/[^"]+)"', "unescape": True, "optional": True},
            "blob": {"regex": r'"(/rails/active_storage/blobs/[^"]+)"', "unescape": True, "optional": True}}),
        {"op": "set", "vars": {"avatar_image": "/users/{{avatar_tokens.jason}}/avatar?v=1"}},
    ]
    img = {"Accept": "image/*"}
    out = {a: [] for a in g.AREAS}
    out["A6"] += discover + [
        r("css", "{{css}}", headers={"Accept": "text/css,*/*;q=0.1"}, revisit=True),
        r("js", "{{js}}", headers={"Accept": "*/*"}, revisit=True),
        r("image asset", "{{image}}", headers=img, revisit=True),
        r("robots", "/robots.txt", revisit=True),
        r("blob representation", "{{representation}}", headers=img),
        r("blob redirect", "{{blob}}"),
        r("head css", "{{css}}", "HEAD"),
        r("webmanifest", "/webmanifest", revisit=True),
        r("service worker", "/service-worker", revisit=True),
        anon(r("health", "/up")),
        r("404 room", "/rooms/1"), r("404 route", "/nope"), r("404 route json", "/nope.json"),
        r("406 format", f"/rooms/{hq}.xml"), r("406 yaml", f"/rooms/{hq}.yaml"),
        r("406 stream only", f"/rooms/{hq}/refresh?since={since}", headers={"Accept": "application/pdf"}),
        r("head 404 any", "/nope", "HEAD", {"Accept": "*/*"}), r("head 406 xml", f"/rooms/{hq}.xml", "HEAD"),
        r("422 csrf", f"/rooms/{hq}/messages", "POST", {"Content-Type": FORM, "Sec-Fetch-Site": "cross-site"},
          body="message[body]=x", csrf=False),
        r("blob 404", "/rails/active_storage/blobs/redirect/nope/x.png"),
        r("old browser", f"/rooms/{hq}", user_agent=OLD_UA),
        r("avatar missing user 404", "/users/nope/avatar", headers=img),
    ]
    out["A4"] += [
        r("avatar initials", "{{avatar_initials}}", headers=img, revisit=True),
        r("avatar initials browser", "{{avatar_initials}}", revisit=True,
          headers={"Accept": "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8"}),
        r("avatar image", "{{avatar_image}}", headers=img, revisit=True),
        r("account logo", "/account/logo?size=small", headers=img, revisit=True),
        r("profile", "/users/me/profile"), r("user", "/users/{{users.jason}}"),
        r("account edit", "/account/edit"), r("account users", "/account/users"),
        r("account bots", "/account/bots"), r("custom styles", "/account/custom_styles/edit"),
        r("account users stream", "/account/users?page=2", headers=stream),
        r("push subscriptions", "/users/me/push_subscriptions"),
        r("qr code", "/qr_code/aHR0cDovL2V4YW1wbGUuY29t", headers=img, revisit=True),
        r("qr code bad", "/qr_code/aHR0cDovL2V4YW1wbGUuY29t2"),
    ]
    out["A1"] += [
        r("root", "/"), anon(r("anon root", "/")), anon(r("anon sign in", "/session/new", revisit=True)),
        anon(r("anon join bad", "/join/nope")),
        {"name": "401 bad sign in", "actor": "visitor", "method": "POST", "path": "/session",
         "headers": {"Content-Type": FORM, "Sec-Fetch-Site": "same-origin"},
         "body": "email_address=nobody%40example.com&password=x"},
    ]
    out["A2"] += [
        r("room", f"/rooms/{des}", revisit=True), r("room empty", f"/rooms/{hq}"), r("room busy", f"/rooms/{wc}"),
        r("room direct", f"/rooms/{direct}"), r("room at message", f"/rooms/{des}/@{first}"),
        r("room settings", f"/rooms/{hq}/settings"), r("room involvement", f"/rooms/{hq}/involvement"),
        r("room edit", f"/rooms/opens/{pets}/edit"), r("new open room", "/rooms/opens/new"),
        r("new direct", "/rooms/directs/new"),
        r("sidebar frame", "/users/me/sidebar", headers={"Turbo-Frame": "user_sidebar"}, revisit=True),
        r("refresh", f"/rooms/{des}/refresh?since=0", headers=stream, revisit=True),
        r("refresh unchanged", f"/rooms/{hq}/refresh?since={since}", headers=stream, revisit=True),
        r("refresh empty", f"/rooms/{hq}/refresh?since=99999999999999", headers=stream, revisit=True),
        r("refresh html", f"/rooms/{hq}/refresh"), r("head room", f"/rooms/{des}", "HEAD"),
        r("head refresh", f"/rooms/{hq}/refresh?since={since}", "HEAD", stream),
        anon(r("anon room", f"/rooms/{hq}")),
    ]
    out["A3"] += [
        r("messages before", f"/rooms/{des}/messages?before={image}", revisit=True),
        r("messages after", f"/rooms/{des}/messages?after={first}"),
        r("messages empty page", f"/rooms/{des}/messages?after={{{{messages.boosted_by_david}}}}"),
        r("messages none", f"/rooms/{hq}/messages?before={image}"),
        r("xhr", f"/rooms/{des}/messages?before={image}", headers={"X-Requested-With": "XMLHttpRequest"}),
        r("autocomplete json", "/autocompletable/users?query=j", headers=js, revisit=True),
        r("autocomplete html", "/autocompletable/users?query=j"),
        anon(r("bot messages", f"/rooms/{hq}/{{{{bot_keys.bender}}}}/messages")),
        anon(r("bot messages bad key", f"/rooms/{hq}/1-nope/messages")),
    ]
    out["A5"] += [r("searches", "/searches?q=pizza"), r("searches empty", "/searches")]
    out["A8"] += [
        r("406 encoding", f"/rooms/{hq}", headers={"Accept-Encoding": "identity;q=0"}),
        r("no gzip", f"/rooms/{hq}", headers={"Accept-Encoding": ""}),
    ]
    return out


def generate(write):
    for area, st in steps().items():
        if not st:
            continue
        write(area, "shape", [{"name": "http_shape requests", "seed": "default",
                               "steps": [{"op": "login", "actor": "david"}] + st}])
