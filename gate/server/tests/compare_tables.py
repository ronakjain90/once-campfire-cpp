#!/usr/bin/env python3
"""Compare the rows that posts change in two databases, ignoring ids and timestamps."""
import sqlite3, sys

def conn(p):
    c = sqlite3.connect(f'file:{p}?mode=ro', uri=True)
    return c

def rows(c, sql):
    return sorted((tuple(r) for r in c.execute(sql)), key=repr)

QUERIES = {
    'messages (no id, no timestamps)': 'SELECT client_message_id, creator_id, room_id FROM messages',
    'messages count per room': 'SELECT room_id, COUNT(*) FROM messages GROUP BY room_id',
    'action_text_rich_texts (no id, no timestamps)': 'SELECT body, name, record_type FROM action_text_rich_texts',
    'rich text record_id equals message id': 'SELECT COUNT(*) FROM action_text_rich_texts r JOIN messages m ON m.id = r.record_id AND r.record_type = "Message" AND r.body = ("bench write " || substr(m.client_message_id, 5))',
    'memberships (id, involvement, unread flag, connections)': 'SELECT id, room_id, user_id, involvement, unread_at IS NOT NULL, connected_at IS NULL, connections FROM memberships',
    'rooms (id, name, type)': 'SELECT id, name, type, creator_id FROM rooms',
    'rooms: updated_at changed': 'SELECT id, updated_at > "2026-06-01" FROM rooms',
    'message_search_index_content (no id)': 'SELECT c0 FROM message_search_index_content',
    'message_search_index MATCH bench': 'SELECT COUNT(*) FROM message_search_index WHERE message_search_index MATCH "bench"',
    'message_search_index MATCH write (rowids join messages)': 'SELECT m.client_message_id FROM message_search_index s JOIN messages m ON m.id = s.rowid WHERE message_search_index MATCH "write"',
    'sessions count': 'SELECT COUNT(*) FROM sessions',
}

a, b = conn(sys.argv[1]), conn(sys.argv[2])
bad = 0
for name, sql in QUERIES.items():
    ra, rb = rows(a, sql), rows(b, sql)
    ok = ra == rb
    print(('EQUAL    ' if ok else 'DIFFERENT'), name, f'({len(ra)} rows)')
    if not ok:
        bad += 1
        print('  rust :', ra[:5]); print('  gate :', rb[:5])
# id sequences: same ids for the same posts
ia = rows(a, 'SELECT id FROM messages ORDER BY id DESC LIMIT 3'); ib = rows(b, 'SELECT id FROM messages ORDER BY id DESC LIMIT 3')
print('last message ids rust', ia, 'gate', ib)
sys.exit(1 if bad else 0)
