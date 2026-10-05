#!/usr/bin/env python3
"""Generate src/sql_texts.inc from the G3 capture (the `?` form of each Rust statement)."""
import re, sys
raw = sys.argv[1]
def ends(f):
    out = []
    for l in open(f'{raw}/sql/{f}'):
        m = re.match(r'^[0-9.]+ SQLTRACE end (reader|writer) ThreadId\(\d+\) rows=\d+ (.*)$', l.rstrip('\n'))
        if m and not m.group(2).startswith('--'):
            out.append(m.group(2))
    return out
room = ends('get-room-gzip-3.txt'); post = ends('post-message-3.txt')
names = {}
def find(lst, prefix, nth=0):
    c=[x for x in lst if x.startswith(prefix) and "'main'" not in x]
    return c[nth]
def add(n, v): names.setdefault(n, v)
add('SessionByToken', find(room,'SELECT "sessions"'))
add('UserById', find(room,'SELECT "users"'))
add('RoomForUser', find(room,'SELECT "rooms"'))
add('Messages40', find(room,'SELECT "messages"'))
add('RoomFirst', find(room,'SELECT "rooms"',1))
add('AccountFirst', find(room,'SELECT "accounts"'))
add('Attachment', find(room,'SELECT b.id'))
add('Bans', find(post,'SELECT 1 AS one'))
add('MembershipFor', find(post,'SELECT "memberships"'))
add('RoomById', find(post,'SELECT "rooms"'))
add('InsertMessage', find(post,'INSERT INTO "messages"'))
add('InsertRichText', find(post,'INSERT INTO "action_text_rich_texts"'))
add('TouchMessage', find(post,'UPDATE "messages"'))
add('TouchRoom', find(post,'UPDATE "rooms"'))
add('RichTextFor', find(post,'SELECT "action_text_rich_texts"'))
add('FtsInsert', find(post,'insert into message_search_index'))
add('UpdateUnread', find(post,'UPDATE "memberships"'))
add('MessageById', find(post,'SELECT "messages"'))
add('PushSubs', find(post,'SELECT "push_subscriptions"'))
add('UnreadCount', find(post,'SELECT COUNT'))
add('BoostsFor', find(post,'SELECT "boosts"'))
add('MembershipsOfRoom', find(post,'SELECT "memberships"',1))
with open('src/sql_texts.inc','w') as f:
    for n, s in names.items():
        assert ')"' not in s
        f.write(f'X({n}, R"SQL({s})SQL")\n')
print(len(names), 'statements')
