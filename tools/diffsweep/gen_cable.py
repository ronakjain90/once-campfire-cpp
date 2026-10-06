"""Action Cable sessions (A7), front server behavior (A8) and jobs and integrations (A9)."""
from gen_writes import LOGIN, TURBO, get, msg, q, s

ROOM = "{{rooms.designers}}"


def ident(channel, **kw):
    d = {"channel": channel}
    d.update(kw)
    return d


def sub(cid, channel, **kw):
    return {"op": "cable_subscribe", "id": cid, "identifier": ident(channel, **kw)}


def a7():
    cap = {"room_stream": {"regex": r'channel="RoomMessagesChannel" signed-stream-name="([^"]+)"', "unescape": True},
           "rooms_stream": {"regex": r'channel="Turbo::StreamsChannel" signed-stream-name="([^"]+)"', "unescape": True,
                            "optional": True}}
    return [
        s("channels subscribe", [
            LOGIN("david"), get("room page", f"/rooms/{ROOM}", capture=cap),
            {"op": "cable_connect", "actor": "david", "id": "d"},
            sub("d", "HeartbeatChannel"), sub("d", "ReadRoomsChannel"), sub("d", "UnreadRoomsChannel"),
            sub("d", "PresenceChannel", room_id="{{rooms.designers}}"),
            sub("d", "TypingNotificationsChannel", room_id="{{rooms.designers}}"),
            sub("d", "RoomMessagesChannel", signed_stream_name="{{room_stream}}"),
            {"op": "cable_wait", "id": "d", "count": 8, "timeout": 8, "settle": 0.5, "name": "welcome, confirmations, read frame"},
            sub("d", "PresenceChannel", room_id="{{rooms.archive}}"),
            sub("d", "TypingNotificationsChannel", room_id=1),
            sub("d", "RoomMessagesChannel", signed_stream_name="bad"),
            sub("d", "NoSuchChannel"),
            {"op": "cable_wait", "id": "d", "count": 4, "timeout": 5, "settle": 0.5, "name": "rejections"}]),
        s("typing and presence", [
            LOGIN("david"), LOGIN("jason"),
            {"op": "cable_connect", "actor": "david", "id": "d"}, {"op": "cable_connect", "actor": "jason", "id": "j"},
            sub("d", "TypingNotificationsChannel", room_id="{{rooms.designers}}"),
            sub("j", "TypingNotificationsChannel", room_id="{{rooms.designers}}"),
            {"op": "cable_wait", "id": "d", "count": 2, "timeout": 8, "name": "d welcome and confirm"},
            {"op": "cable_wait", "id": "j", "count": 2, "timeout": 8, "name": "j welcome and confirm"},
            {"op": "cable_perform", "id": "j", "identifier": ident("TypingNotificationsChannel", room_id="{{rooms.designers}}"), "action": "start",
             "data": {}},
            {"op": "cable_perform", "id": "j", "identifier": ident("TypingNotificationsChannel", room_id="{{rooms.designers}}"), "action": "stop",
             "data": {}},
            {"op": "cable_wait", "id": "d", "count": 2, "timeout": 8, "name": "d sees typing"},
            {"op": "cable_wait", "id": "j", "count": 2, "timeout": 8, "name": "j sees own typing"},
            sub("d", "PresenceChannel", room_id="{{rooms.designers}}"),
            {"op": "cable_perform", "id": "d", "identifier": ident("PresenceChannel", room_id="{{rooms.designers}}"), "action": "refresh", "data": {}},
            {"op": "cable_perform", "id": "d", "identifier": ident("PresenceChannel", room_id="{{rooms.designers}}"), "action": "absent", "data": {}},
            {"op": "cable_wait", "id": "d", "count": 1, "timeout": 5, "settle": 0.5, "name": "d presence confirm"}]),
        s("message broadcasts", [
            LOGIN("david"), LOGIN("jason"), get("room page", f"/rooms/{ROOM}", capture=cap),
            {"op": "cable_connect", "actor": "david", "id": "d"},
            sub("d", "RoomMessagesChannel", signed_stream_name="{{room_stream}}"), sub("d", "UnreadRoomsChannel"),
            {"op": "cable_wait", "id": "d", "count": 3, "timeout": 8, "name": "welcome and confirmations"},
            msg(ROOM, "Cable one", "jason"),
            q("edit", "PUT", f"/rooms/{ROOM}/messages/{{{{messages.edited}}}}", "david", headers=TURBO,
              multipart={"fields": {"message[body]": "<p>Edited live</p>"}}),
            q("boost", "POST", "/messages/{{messages.unboosted}}/boosts", "jason", form={"boost[content]": "👍"}, headers=TURBO),
            q("delete", "DELETE", f"/rooms/{ROOM}/messages/{{{{messages.edited}}}}", "david", headers=TURBO),
            {"op": "cable_wait", "id": "d", "count": 5, "timeout": 8, "settle": 1.0, "ignore_types": ["ping"],
             "name": "broadcasts for create, edit, boost, delete, unread"}]),
        s("connect rejected", [
            {"op": "cable_connect", "actor": "visitor", "id": "v"},
            {"op": "cable_connect", "actor": "visitor", "id": "bad_origin", "origin": "http://evil.example"},
            LOGIN("david"), {"op": "cable_connect", "actor": "david", "id": "d2", "origin": "http://evil.example"},
            {"op": "cable_connect", "actor": "david", "id": "d3", "path": "/cable?x=1"}]),
    ]


def a8():
    enc = lambda n, ae: get(n, "/rooms/{{rooms.designers}}", headers={"Accept-Encoding": ae})
    return [s("encodings and conditional requests", [
        LOGIN("david"), enc("gzip", "gzip"), enc("zstd", "zstd"), enc("br", "br"), enc("all", "gzip, deflate, br, zstd"),
        enc("identity", "identity"), enc("none", ""), enc("gzip;q=0", "gzip;q=0, identity"),
        get("room revisit", "/rooms/{{rooms.designers}}", revisit=True),
        get("head", "/rooms/{{rooms.designers}}", headers={"Accept-Encoding": "gzip"}),
        get("range", "/rooms/{{rooms.designers}}", headers={"Range": "bytes=0-99"}),
        get("long url", "/rooms/{{rooms.designers}}?" + "a=b&" * 200),
        get("trailing slash", "/rooms/{{rooms.designers}}/"), get("double slash", "//rooms"),
        get("percent path", "/rooms/%41"), get("dotfile", "/.env"), get("traversal", "/assets/../etc/passwd"),
        q("options", "OPTIONS", "/rooms/{{rooms.designers}}"), q("unknown method", "PROPFIND", "/"),
    ], mutates=False)]


def a9():
    return [s("webhook and push on message", [
        LOGIN("kevin"),
        msg("{{rooms.bender_and_kevin}}", "ping the bot", "kevin"),
        msg("{{rooms.designers}}", "push me", "kevin"),
        get("room", "/rooms/{{rooms.bender_and_kevin}}", "kevin"), get("unreads", "/users/me/sidebar", "kevin",
                                                                      headers={"Turbo-Frame": "user_sidebar"}),
        LOGIN("david"), q("test push", "POST", "/users/me/push_subscriptions/{{push_subscriptions.david_chrome}}/test_notifications"),
        q("banned content removal: ban", "POST", "/users/{{users.rita}}/ban"),
        get("room after ban", "/rooms/{{rooms.designers}}")])]
