"""Write flows for areas A4 (accounts, users), A5 (search) and A6 (unfurl, errors)."""
from gen_writes import LOGIN, MOON, PIXEL, TURBO, get, loc, q, s


def a4():
    adm = lambda n, m, p, **kw: q(n, m, p, "david", **kw)
    return [
        s("profile edits", [
            LOGIN("jason"), get("profile", "/users/me/profile", "jason"),
            q("rename", "PATCH", "/users/me/profile", "jason", form={"user[name]": "Jason F", "user[bio]": "Hi"}),
            q("bad email", "PATCH", "/users/me/profile", "jason", form={"user[email_address]": "not-an-email"}),
            q("password", "PATCH", "/users/me/profile", "jason", form={"user[password]": "newsecret12345"}),
            q("avatar", "PATCH", "/users/me/profile", "jason", multipart={"files": [
                {"name": "user[avatar]", "filename": "moon.jpg", "content_type": "image/jpeg", "path": MOON}]}),
            get("profile after", "/users/me/profile", "jason"), get("user", "/users/{{users.jason}}", "jason"),
            get("avatar", "/users/{{avatar_tokens.jason}}/avatar", "jason", headers={"Accept": "image/*"}),
            q("delete avatar", "DELETE", "/users/{{avatar_tokens.jason}}/avatar", "jason"),
            get("avatar after", "/users/{{avatar_tokens.jason}}/avatar", "jason", headers={"Accept": "image/*"})]),
        s("account admin", [
            LOGIN("david"), get("edit", "/account/edit"),
            adm("rename account", "PATCH", "/account", form={"account[name]": "Renamed Co"}),
            adm("restrict rooms", "PATCH", "/account", form={"account[settings][restrict_room_creation_to_administrators]": "true"}),
            adm("logo", "PATCH", "/account", multipart={"files": [
                {"name": "account[logo]", "filename": "moon.jpg", "content_type": "image/jpeg", "path": MOON}]}),
            get("logo", "/account/logo", headers={"Accept": "image/*"}), get("logo small", "/account/logo?size=small", headers={"Accept": "image/*"}),
            adm("delete logo", "DELETE", "/account/logo"), get("logo after", "/account/logo", headers={"Accept": "image/*"}),
            get("custom styles", "/account/custom_styles/edit"),
            adm("set styles", "PATCH", "/account/custom_styles", form={"account[custom_styles]": "body { color: red; }"}),
            get("manifest", "/webmanifest"), get("rooms", "/rooms/{{rooms.designers}}"),
            adm("reset join code", "POST", "/account/join_code"), get("edit after", "/account/edit"),
            LOGIN("kevin"), q("member edits account", "PATCH", "/account", "kevin", form={"account[name]": "x"}),
            get("member opens users", "/account/users", "kevin")]),
        s("users and bans", [
            LOGIN("david"), get("users", "/account/users"), get("new user", "/account/users/new") ,
            adm("make admin", "PATCH", "/account/users/{{users.kevin}}", form={"user[role]": "administrator"}),
            adm("make member", "PATCH", "/account/users/{{users.kevin}}", form={"user[role]": "member"}),
            get("user edit", "/account/users/{{users.rita}}/edit"),
            adm("ban", "POST", "/users/{{users.rita}}/ban"), get("users after ban", "/account/users"),
            adm("unban", "DELETE", "/users/{{users.rita}}/ban"),
            adm("remove user", "DELETE", "/account/users/{{users.jz}}"), get("users after remove", "/account/users"),
            LOGIN("rita"), get("rita after ban", "/", "rita")]),
        s("bots", [
            LOGIN("david"), get("bots", "/account/bots"), get("new bot", "/account/bots/new"),
            adm("create bot", "POST", "/account/bots", form={"user[name]": "Robo", "user[webhook_url]": "http://127.0.0.1:9/hook"},
                capture=loc("bot", r"/account/bots/(\d+)")),
            get("bot", "/account/bots/{{bot}}"), get("bot edit", "/account/bots/{{bot}}/edit"),
            adm("update bot", "PATCH", "/account/bots/{{bot}}", form={"user[name]": "Robo 2"}),
            adm("reset key", "PUT", "/account/bots/{{bot}}/key"), get("bots after", "/account/bots"),
            adm("delete bot", "DELETE", "/account/bots/{{bot}}"), get("bots end", "/account/bots")]),
        s("push subscriptions", [
            LOGIN("david"), get("index", "/users/me/push_subscriptions"),
            q("create", "POST", "/users/me/push_subscriptions", headers={"Content-Type": "application/json"},
              json={"push_subscription": {"endpoint": "https://127.0.0.1:9/push/new", "p256dh_key": "k", "auth_key": "a"}},
              capture=loc("ps", r"/push_subscriptions/(\d+)")),
            get("index after", "/users/me/push_subscriptions"),
            q("test notification", "POST", "/users/me/push_subscriptions/{{push_subscriptions.david_chrome}}/test_notifications"),
            q("delete", "DELETE", "/users/me/push_subscriptions/{{push_subscriptions.david_chrome}}")]),
        s("restricted account", [
            LOGIN("kevin"), get("new room", "/rooms/opens/new", "kevin"),
            q("create room", "POST", "/rooms/opens", "kevin", form={"room[name]": "Nope"})], seed="restricted"),
        s("custom styles seed", [LOGIN("david"), get("room", "/rooms/{{rooms.designers}}"), get("edit", "/account/custom_styles/edit")],
          seed="custom_styles", mutates=False),
        s("crowd users", [LOGIN("david"), get("page 1", "/account/users"), get("page 2", "/account/users?page=2"),
                          get("page 2 stream", "/account/users?page=2", headers=TURBO)], seed="crowd", mutates=False),
    ]


def a5():
    return [s("search", [
        LOGIN("david"), get("form", "/searches"), q("search", "POST", "/searches", form={"q": "pizza"}),
        get("results", "/searches?q=pizza"), get("results many", "/searches?q=wifi"),
        get("no results", "/searches?q=zzzznothing"), get("symbols", "/searches?q=a%26b%22c"),
        q("clear", "DELETE", "/searches/clear"), get("after clear", "/searches")])]


def a6():
    return [s("unfurl and misc", [
        LOGIN("david"),
        q("unfurl unreachable", "POST", "/unfurl_link", headers={"Content-Type": "application/json"},
          json={"url": "http://127.0.0.1:9/page"}),
        q("unfurl no url", "POST", "/unfurl_link", headers={"Content-Type": "application/json"}, json={}),
        q("unfurl bad scheme", "POST", "/unfurl_link", headers={"Content-Type": "application/json"}, json={"url": "ftp://x"}),
        get("old route", "/rooms/{{rooms.designers}}/messages/{{messages.plain}}.json"),
        get("not found format", "/nope.json")])]
