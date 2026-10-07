"""Write flows as scenarios. Each scenario mutates, so each runs on a fresh app pair."""

FORM = "application/x-www-form-urlencoded"
TURBO = {"Accept": "text/vnd.turbo-stream.html, text/html, application/xhtml+xml"}
# File paths are relative to the workspace (the folder that holds this repo and once-campfire-rust).
MOON = "once-campfire-rust/reference/test/fixtures/files/moon.jpg"
PIXEL = "once-campfire-rust/reference/test/fixtures/files/pixel.bmp"
LOGIN = lambda a: {"op": "login", "actor": a}


def s(name, steps, seed="default", **kw):
    d = {"name": name, "seed": seed, "mutates": True, "steps": steps}
    d.update(kw)
    return d


def q(name, method, path, actor="david", **kw):
    st = {"name": name, "actor": actor, "method": method, "path": path}
    st.update(kw)
    return st


def get(name, path, actor="david", **kw):
    return q(name, "GET", path, actor, **kw)


def loc(var, regex):
    return {var: {"from": "header:location", "regex": regex}}


def body(var, regex):
    return {var: {"regex": regex}}


def msg(room, text, actor="david", name=None, **kw):
    return q(name or f"post message {text[:20]!r}", "POST", f"/rooms/{room}/messages", actor, headers=TURBO,
             multipart={"fields": {"message[body]": f"<p>{text}</p>", "message[client_message_id]": "cm-" + text[:8].replace(" ", "")}},
             **kw)


def a1():
    bad = lambda n: q(f"bad password {n}", "POST", "/session", "visitor", form={
        "email_address": "david@37signals.com", "password": "wrong"}, headers={"Sec-Fetch-Site": "same-origin"})
    return [
        s("sign in and out", [get("home signed out", "/", "visitor"), LOGIN("david"), get("home", "/"),
                              get("profile", "/users/me/profile"), q("sign out", "DELETE", "/session"),
                              get("home after sign out", "/"), get("room after sign out", "/rooms/{{rooms.hq}}")]),
        s("sign in failures", [get("form", "/session/new", "visitor")] + [bad(i) for i in range(1, 13)]),
        s("sign in with a bad email", [get("form", "/session/new", "visitor"),
                                       q("unknown email", "POST", "/session", "visitor", form={
                                           "email_address": "nobody@example.com", "password": "x"},
                                         headers={"Sec-Fetch-Site": "same-origin"}),
                                       q("banned user", "POST", "/session", "visitor", form={
                                           "email_address": "{{emails.mallory}}", "password": "{{passwords.all}}"},
                                         headers={"Sec-Fetch-Site": "same-origin"})]),
        s("transfer sign in", [get("transfer page", "/session/transfers/{{transfers.david}}", "visitor"),
                               q("transfer update", "PATCH", "/session/transfers/{{transfers.david}}", "visitor"),
                               get("home", "/", "visitor"),
                               get("expired transfer", "/session/transfers/{{transfers.david_expired}}", "other"),
                               get("bad transfer", "/session/transfers/nope", "other")]),
        s("join the account", [get("join form", "/join/{{join_codes.signal}}", "visitor"),
                               get("join bad code", "/join/nope", "visitor"),
                               q("join create", "POST", "/join/{{join_codes.signal}}", "visitor", form={
                                   "user[name]": "New Person", "user[email_address]": "new@example.com",
                                   "user[password]": "secret123456"}),
                               q("join invalid", "POST", "/join/{{join_codes.signal}}", "visitor", form={
                                   "user[name]": "", "user[email_address]": "new@example.com", "user[password]": "x"}),
                               get("home after join", "/", "visitor"),
                               q("join bad code create", "POST", "/join/nope", "visitor", form={"user[name]": "X"})]),
        s("first run", [get("redirect to first run", "/", "visitor"), get("first run form", "/first_run", "visitor"),
                        q("first run create", "POST", "/first_run", "visitor", form={
                            "user[name]": "Owner", "user[email_address]": "owner@example.com",
                            "user[password]": "secret123456"}),
                        get("home", "/", "visitor"), get("first run again", "/first_run", "visitor")],
          seed="first_run"),
    ]


def a2():
    hq, des = "{{rooms.hq}}", "{{rooms.designers}}"
    return [
        s("open room lifecycle", [
            LOGIN("david"), get("new form", "/rooms/opens/new"),
            q("create open room", "POST", "/rooms/opens", form={"room[name]": "Launch"}, capture=loc("rid", r"/rooms/(\d+)")),
            get("show", "/rooms/{{rid}}"), get("edit", "/rooms/opens/{{rid}}/edit"),
            q("rename", "PATCH", "/rooms/opens/{{rid}}", form={"room[name]": "Launch 2"}),
            get("sidebar", "/users/me/sidebar", headers={"Turbo-Frame": "user_sidebar"}),
            q("create with empty name", "POST", "/rooms/opens", form={"room[name]": ""}),
            q("delete", "DELETE", "/rooms/closeds/{{rid}}"), get("show deleted", "/rooms/{{rid}}")]),
        s("closed and direct rooms", [
            LOGIN("david"), get("new closed", "/rooms/closeds/new"),
            q("create closed", "POST", "/rooms/closeds", form={"room[name]": "Secret", "user_ids[]": ["{{users.jason}}", "{{users.kevin}}"]},
              capture=loc("cid", r"/rooms/(\d+)")),
            get("closed edit", "/rooms/closeds/{{cid}}/edit"),
            q("update members", "PATCH", "/rooms/closeds/{{cid}}", form={"room[name]": "Secret", "user_ids[]": ["{{users.jason}}"]}),
            get("closed show", "/rooms/{{cid}}"),
            get("new direct", "/rooms/directs/new"),
            q("create direct", "POST", "/rooms/directs", form={"user_ids[]": ["{{users.rita}}"]}, capture=loc("did", r"/rooms/(\d+)")),
            q("create direct again", "POST", "/rooms/directs", form={"user_ids[]": ["{{users.rita}}"]}),
            q("create group direct", "POST", "/rooms/directs", form={"user_ids[]": ["{{users.rita}}", "{{users.jz}}"]}),
            get("direct show", "/rooms/{{did}}"), get("direct edit", "/rooms/directs/{{did}}/edit")]),
        s("involvement", [
            LOGIN("david"), get("show", f"/rooms/{hq}/involvement"),
            q("set invisible", "PUT", f"/rooms/{hq}/involvement", form={"involvement": "invisible"}),
            get("sidebar", "/users/me/sidebar", headers={"Turbo-Frame": "user_sidebar"}),
            q("set everything", "PUT", f"/rooms/{hq}/involvement", form={"involvement": "everything"}),
            q("set mentions", "PUT", f"/rooms/{hq}/involvement", form={"involvement": "mentions"}),
            q("set bad value", "PUT", f"/rooms/{hq}/involvement", form={"involvement": "nonsense"})]),
        s("room access limits", [
            LOGIN("kevin"), get("kevin opens hq", f"/rooms/{hq}", "kevin"),
            q("kevin renames designers", "PATCH", f"/rooms/opens/{{{{rooms.pets}}}}", "kevin", form={"room[name]": "x"}),
            q("kevin deletes pets", "DELETE", "/rooms/opens/{{rooms.pets}}", "kevin"),
            LOGIN("rita"), get("rita opens designers", f"/rooms/{des}", "rita")]),
    ]


def a3():
    des, wc = "{{rooms.designers}}", "{{rooms.watercooler}}"
    return [
        s("post edit delete", [
            LOGIN("david"), LOGIN("jason"),
            msg(des, "Hello from the sweep", capture=body("mid", r'data-message-id="(\d+)"')),
            get("fragment", f"/rooms/{des}/messages/{{{{mid}}}}"), get("edit form", f"/rooms/{des}/messages/{{{{mid}}}}/edit"),
            q("edit", "PUT", f"/rooms/{des}/messages/{{{{mid}}}}", multipart={"fields": {"message[body]": "<p>Edited</p>"}}, headers=TURBO),
            q("edit by other user", "PUT", f"/rooms/{des}/messages/{{{{mid}}}}", "jason", multipart={"fields": {"message[body]": "<p>x</p>"}}, headers=TURBO),
            get("room", f"/rooms/{des}"), get("page", f"/rooms/{des}/messages"),
            q("delete by other user", "DELETE", f"/rooms/{des}/messages/{{{{mid}}}}", "jason", headers=TURBO),
            q("delete", "DELETE", f"/rooms/{des}/messages/{{{{mid}}}}", headers=TURBO),
            get("deleted fragment", f"/rooms/{des}/messages/{{{{mid}}}}"), get("refresh", f"/rooms/{des}/refresh?since=1772467200000", headers=TURBO)]),
        s("message edge cases", [
            LOGIN("david"),
            msg(des, "", name="empty body"), msg(wc, "Mention <em>x</em> @david https://example.com/a?b=c&d=e"),
            q("plain text body", "POST", f"/rooms/{des}/messages", form={"message[body]": "plain", "message[client_message_id]": "cm-plain"}, headers=TURBO),
            q("room not found", "POST", "/rooms/1/messages", multipart={"fields": {"message[body]": "x"}}, headers=TURBO),
            q("no room access", "POST", "/rooms/{{rooms.archive}}/messages", multipart={"fields": {"message[body]": "x"}}, headers=TURBO)]),
        s("attachment", [
            LOGIN("david"),
            q("upload image", "POST", f"/rooms/{des}/messages", headers=TURBO, multipart={
                "fields": {"message[client_message_id]": "cm-img"},
                "files": [{"name": "message[attachment]", "filename": "moon.jpg", "content_type": "image/jpeg", "path": MOON}]},
              capture=body("blob", r'(/rails/active_storage/blobs/[^"\']+)')),
            q("upload bmp", "POST", f"/rooms/{des}/messages", headers=TURBO, multipart={
                "fields": {"message[client_message_id]": "cm-bmp"},
                "files": [{"name": "message[attachment]", "filename": "pixel.bmp", "content_type": "image/bmp", "path": PIXEL}]}),
            get("room with uploads", f"/rooms/{des}"), get("blob", "{{blob}}", headers={"Accept": "image/*"})]),
        s("boosts", [
            LOGIN("david"), LOGIN("jason"),
            get("message", f"/rooms/{des}/@{{{{messages.unboosted}}}}"),
            q("boost", "POST", "/messages/{{messages.unboosted}}/boosts", "jason", form={"boost[content]": "👍"}, headers=TURBO),
            get("boosts of message", "/messages/{{messages.unboosted}}/boosts", "jason", capture=body("bid", r'id="boost_(\d+)"')),
            q("boost again", "POST", "/messages/{{messages.unboosted}}/boosts", "david", form={"boost[content]": "🎉"}, headers=TURBO),
            get("boosts index", "/messages/{{messages.unboosted}}/boosts", "david"),
            q("delete other boost", "DELETE", "/messages/{{messages.unboosted}}/boosts/{{bid}}", "david", headers=TURBO),
            q("delete boost", "DELETE", "/messages/{{messages.unboosted}}/boosts/{{bid}}", "jason", headers=TURBO),
            q("boost too long", "POST", "/messages/{{messages.unboosted}}/boosts", "jason", form={"boost[content]": "x" * 40}, headers=TURBO),
            q("boost missing message", "POST", "/messages/1/boosts", "jason", form={"boost[content]": "x"}, headers=TURBO)]),
        s("bot api", [
            LOGIN("david"),
            q("create text", "POST", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages", "bot", csrf=False,
              headers={"Content-Type": "text/plain"}, body="Build 1044 passed"),
            q("create html", "POST", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages", "bot", csrf=False,
              headers={"Content-Type": "text/html"}, body="<strong>Bold</strong> news"),
            q("create upload", "POST", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages", "bot", csrf=False,
              multipart={"files": [{"name": "attachment", "filename": "moon.jpg", "content_type": "image/jpeg", "path": MOON}]}),
            q("create empty", "POST", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages", "bot", csrf=False, body=""),
            q("bad key", "POST", "/rooms/{{rooms.watercooler}}/1-nope/messages", "bot", csrf=False, body="x"),
            get("index", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages", "bot"),
            get("index after", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages?after={{messages.busy_060}}", "bot"),
            q("update", "PUT", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages/{{messages.bot_in_watercooler}}", "bot", csrf=False,
              headers={"Content-Type": "application/json"}, body='{"message":{"body":"edited"}}'),
            q("bot boost", "POST", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages/{{messages.thirteenth}}/boosts", "bot", csrf=False,
              headers={"Content-Type": "text/plain"}, body="🤖"),
            q("bot delete boost", "DELETE", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages/{{messages.thirteenth}}/boosts/{{boosts.thirteenth}}", "bot", csrf=False),
            q("bot delete", "DELETE", "/rooms/{{rooms.watercooler}}/{{bot_keys.bender}}/messages/{{messages.bot_in_watercooler}}", "bot", csrf=False),
            get("room", "/rooms/{{rooms.watercooler}}")]),
    ]


def generate(write):
    import gen_a4
    import gen_cable
    groups = {"A1": a1(), "A2": a2(), "A3": a3(), "A4": gen_a4.a4(), "A5": gen_a4.a5(), "A6": gen_a4.a6(),
              "A7": gen_cable.a7(), "A8": gen_cable.a8(), "A9": gen_cable.a9()}
    for area, scs in groups.items():
        write(area, "cable" if area == "A7" else "writes", scs)
