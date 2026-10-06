// Rails: app/models/message.rb and app/models/message/{attachment,mentionee,pagination,searchable}.rb, Room#receive.
// Rust: crates/db/src/models/message.rs, room.rs.
#include "models/message.hpp"

#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include "core/time_format.hpp"

namespace campfire::models {

namespace {

using db::schema::MessageRow;

#define CF_MESSAGE_COLUMNS                                                                                     \
  "\"messages\".\"id\", \"messages\".\"client_message_id\", \"messages\".\"created_at\", "                    \
  "\"messages\".\"creator_id\", \"messages\".\"room_id\", \"messages\".\"updated_at\""
#define CF_IN_ROOM "SELECT " CF_MESSAGE_COLUMNS " FROM \"messages\" WHERE \"messages\".\"room_id\" = ?"

const db::Query<MessageRow(std::int64_t)> kById{"SELECT " CF_MESSAGE_COLUMNS
                                                " FROM \"messages\" WHERE \"messages\".\"id\" = ? LIMIT 1"};
const db::Query<MessageRow(std::int64_t, std::int64_t)> kInRoom{
    CF_IN_ROOM " AND \"messages\".\"id\" = ? LIMIT 1"};
const db::Query<MessageRow(std::int64_t, std::int64_t)> kReachable{
    "SELECT " CF_MESSAGE_COLUMNS
    " FROM \"messages\" INNER JOIN \"rooms\" ON \"messages\".\"room_id\" = \"rooms\".\"id\" INNER JOIN "
    "\"memberships\" ON \"rooms\".\"id\" = \"memberships\".\"room_id\" WHERE \"memberships\".\"user_id\" = ? AND "
    "\"messages\".\"id\" = ? LIMIT 1"};
const db::Query<std::int64_t(std::int64_t)> kCountInRoom{
    "SELECT COUNT(*) FROM \"messages\" WHERE \"messages\".\"room_id\" = ?"};
const db::Query<MessageRow(std::int64_t)> kLastPage{
    CF_IN_ROOM " ORDER BY \"messages\".\"created_at\" DESC LIMIT 40"};
const db::Query<MessageRow(std::int64_t, std::string_view)> kPageBefore{
    CF_IN_ROOM " AND (created_at < ?) ORDER BY \"messages\".\"created_at\" DESC LIMIT 40"};
const db::Query<MessageRow(std::int64_t, std::string_view)> kPageAfter{
    CF_IN_ROOM " AND (created_at > ?) ORDER BY \"messages\".\"created_at\" ASC LIMIT 40"};
const db::Query<std::int64_t(std::int64_t, std::string_view)> kExistsBefore{
    "SELECT 1 FROM \"messages\" WHERE \"messages\".\"room_id\" = ? AND (created_at < ?) LIMIT 1"};
const db::Query<std::int64_t(std::int64_t, std::string_view)> kExistsAfter{
    "SELECT 1 FROM \"messages\" WHERE \"messages\".\"room_id\" = ? AND (created_at > ?) LIMIT 1"};
const db::Query<std::optional<std::string_view>(std::int64_t)> kBodyHtml{
    "SELECT \"action_text_rich_texts\".\"body\" FROM \"action_text_rich_texts\" WHERE "
    "\"action_text_rich_texts\".\"record_id\" = ? AND \"action_text_rich_texts\".\"record_type\" = 'Message' AND "
    "\"action_text_rich_texts\".\"name\" = 'body' LIMIT 1"};

const db::Query<std::int64_t(std::string_view, std::string_view, std::int64_t, std::int64_t, std::string_view)>
    kInsert{
        "INSERT INTO \"messages\" (\"client_message_id\", \"created_at\", \"creator_id\", \"room_id\", "
        "\"updated_at\") VALUES (?, ?, ?, ?, ?) RETURNING \"id\""};
const db::Query<std::int64_t(std::string_view, std::string_view, std::string_view, std::int64_t, std::string_view,
                             std::string_view)>
    kInsertBody{
        "INSERT INTO \"action_text_rich_texts\" (\"body\", \"created_at\", \"name\", \"record_id\", "
        "\"record_type\", \"updated_at\") VALUES (?, ?, ?, ?, ?, ?) RETURNING \"id\""};
const db::Query<void(std::string_view, std::string_view, std::int64_t)> kUpdateBodyRow{
    "UPDATE \"action_text_rich_texts\" SET \"body\" = ?, \"updated_at\" = ? WHERE "
    "\"action_text_rich_texts\".\"id\" = ?"};
const db::Query<std::int64_t(std::int64_t)> kBodyRecord{
    "SELECT \"action_text_rich_texts\".\"id\" FROM \"action_text_rich_texts\" WHERE "
    "\"action_text_rich_texts\".\"record_id\" = ? AND \"action_text_rich_texts\".\"record_type\" = 'Message' AND "
    "\"action_text_rich_texts\".\"name\" = 'body' LIMIT 1"};
const db::Query<std::optional<std::string_view>(std::int64_t)> kBodyRecordText{
    "SELECT \"action_text_rich_texts\".\"body\" FROM \"action_text_rich_texts\" WHERE "
    "\"action_text_rich_texts\".\"id\" = ? LIMIT 1"};
const db::Query<void(std::string_view, std::int64_t)> kTouchMessage{
    "UPDATE \"messages\" SET \"updated_at\" = ? WHERE \"messages\".\"id\" = ?"};
const db::Query<void(std::string_view, std::int64_t)> kTouchRoom{
    "UPDATE \"rooms\" SET \"updated_at\" = ? WHERE \"rooms\".\"id\" = ?"};
const db::Query<void(std::int64_t, std::string_view, std::int64_t)> kAttach{
    "INSERT INTO \"active_storage_attachments\" (\"blob_id\", \"created_at\", \"name\", \"record_id\", "
    "\"record_type\") VALUES (?, ?, 'attachment', ?, 'Message')"};
struct AttachmentRef {
  std::int64_t id;
  std::int64_t blob_id;
  static AttachmentRef read(db::RowReader& r) { return {r.i64(0), r.i64(1)}; }
};
const db::Query<AttachmentRef(std::int64_t)> kAttachmentOf{
    "SELECT \"active_storage_attachments\".\"id\", \"active_storage_attachments\".\"blob_id\" FROM "
    "\"active_storage_attachments\" WHERE \"active_storage_attachments\".\"record_id\" = ? AND "
    "\"active_storage_attachments\".\"record_type\" = 'Message' AND \"active_storage_attachments\".\"name\" = "
    "'attachment' LIMIT 1"};
const db::Query<void(std::int64_t)> kDeleteAttachment{
    "DELETE FROM \"active_storage_attachments\" WHERE \"active_storage_attachments\".\"id\" = ?"};
const db::Query<void(std::int64_t)> kDeleteBoosts{"DELETE FROM \"boosts\" WHERE \"boosts\".\"message_id\" = ?"};
const db::Query<void(std::int64_t)> kDeleteBodies{
    "DELETE FROM \"action_text_rich_texts\" WHERE \"action_text_rich_texts\".\"record_id\" = ? AND "
    "\"action_text_rich_texts\".\"record_type\" = 'Message' AND \"action_text_rich_texts\".\"name\" = 'body'"};
const db::Query<void(std::int64_t)> kDeleteMessage{"DELETE FROM \"messages\" WHERE \"messages\".\"id\" = ?"};

// message_search_index (Message::Searchable)
const db::Query<void(std::int64_t, std::string_view)> kIndexInsert{
    "insert into message_search_index(rowid, body) values (?, ?)"};
const db::Query<void(std::string_view, std::int64_t)> kIndexUpdate{
    "update message_search_index set body = ? where rowid = ?"};
const db::Query<void(std::int64_t)> kIndexDelete{"delete from message_search_index where rowid = ?"};

// `Room#unread_memberships`: `memberships.visible.disconnected.where.not(user: message.creator).update_all(...)`
const db::Query<void(std::string_view, std::string_view, std::int64_t, std::string_view, std::int64_t)> kUnread{
    "UPDATE \"memberships\" SET \"unread_at\" = ?, \"updated_at\" = ? WHERE \"memberships\".\"room_id\" = ? AND "
    "\"memberships\".\"involvement\" != 'invisible' AND (\"memberships\".\"connected_at\" IS NULL OR "
    "\"memberships\".\"connected_at\" < ?) AND \"memberships\".\"user_id\" != ?"};

// `Membership::Connectable::CONNECTION_TTL`
constexpr std::int64_t kConnectionTtlSeconds = 60;

Result<std::vector<Message>> collect(Result<std::pmr::vector<MessageRow>> rows, bool reverse) {
  if (!rows) return std::unexpected(rows.error());
  std::vector<Message> out;
  out.reserve(rows->size());
  for (const MessageRow& row : *rows) out.push_back(Message::from_row(row));
  if (reverse) std::reverse(out.begin(), out.end());
  return out;
}

Result<std::optional<Message>> wrap(Result<std::optional<MessageRow>> row) {
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<Message>{};
  return std::optional<Message>(Message::from_row(**row));
}

// `Random.uuid`: a version 4 UUID.
std::string random_uuid() {
  std::array<unsigned char, 16> b{};
  std::size_t got = 0;
  while (got < b.size()) {
    const ssize_t n = ::getrandom(b.data() + got, b.size() - got, 0);
    if (n > 0) got += static_cast<std::size_t>(n);
  }
  b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
  char text[37];
  std::snprintf(text, sizeof text, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return text;
}

}  // namespace

Message Message::from_row(const MessageRow& row) {
  return {row.id, row.room_id, row.creator_id, std::string(row.client_message_id), std::string(row.created_at),
          std::string(row.updated_at)};
}

namespace messages {

Result<std::optional<Message>> find_by_id(db::Connection& conn, Arena& arena, std::int64_t id) {
  return wrap(conn.first(kById, arena, id));
}

Result<std::optional<Message>> find_in_room(db::Connection& conn, Arena& arena, std::int64_t room_id, std::int64_t id) {
  return wrap(conn.first(kInRoom, arena, room_id, id));
}

Result<std::optional<Message>> find_reachable(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                              std::int64_t id) {
  return wrap(conn.first(kReachable, arena, user_id, id));
}

Result<std::int64_t> count_in_room(db::Connection& conn, Arena& arena, std::int64_t room_id) {
  auto n = conn.first(kCountInRoom, arena, room_id);
  if (!n) return std::unexpected(n.error());
  return n->value_or(0);
}

Result<std::vector<Message>> last_page(db::Connection& conn, Arena& arena, std::int64_t room_id) {
  return collect(conn.all(kLastPage, arena, room_id), true);
}

Result<std::vector<Message>> page_before(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                         const Message& message) {
  return collect(conn.all(kPageBefore, arena, room_id, std::string_view(message.created_at)), true);
}

Result<std::vector<Message>> page_after(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                        const Message& message) {
  return collect(conn.all(kPageAfter, arena, room_id, std::string_view(message.created_at)), false);
}

Result<bool> exists_before(db::Connection& conn, Arena& arena, std::int64_t room_id, const Message& message) {
  auto one = conn.first(kExistsBefore, arena, room_id, std::string_view(message.created_at));
  if (!one) return std::unexpected(one.error());
  return one->has_value();
}

Result<bool> exists_after(db::Connection& conn, Arena& arena, std::int64_t room_id, const Message& message) {
  auto one = conn.first(kExistsAfter, arena, room_id, std::string_view(message.created_at));
  if (!one) return std::unexpected(one.error());
  return one->has_value();
}

Result<std::optional<std::string>> body_html(db::Connection& conn, Arena& arena, std::int64_t id) {
  auto row = conn.first(kBodyHtml, arena, id);
  if (!row) return std::unexpected(row.error());
  if (!*row || !**row) return std::optional<std::string>{};
  return std::optional<std::string>(std::string(***row));
}

Result<Message> create(db::Tx& tx, const NewMessage& attributes, JobSink* jobs) {
  Arena arena(512);
  const Timestamp now = tx.now();
  const std::string now_text = format_db(now);
  const std::string client_message_id = attributes.client_message_id ? *attributes.client_message_id : random_uuid();
  auto id = tx.conn().first(kInsert, arena, client_message_id, now_text, attributes.creator_id, attributes.room_id,
                            now_text);
  if (!id) return std::unexpected(id.error());
  Message message{**id, attributes.room_id, attributes.creator_id, client_message_id, now_text, now_text};

  bool touched = false;
  if (attributes.body) {
    const std::string at = tx.now_db();
    auto body = tx.conn().first(kInsertBody, arena, std::string_view(*attributes.body), at, "body", message.id,
                                "Message", at);
    if (!body) return std::unexpected(body.error());
    touched = true;
  }
  if (attributes.attachment_blob_id) {
    if (auto r = tx.conn().exec(kAttach, *attributes.attachment_blob_id, tx.now_db(), message.id);
        !r) {
      return std::unexpected(r.error());
    }
    touched = true;
  }
  if (touched) {
    message.updated_at = tx.now_db();
    if (auto r = tx.conn().exec(kTouchMessage, std::string_view(message.updated_at), message.id); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto r = tx.conn().exec(kTouchRoom, tx.now_db(), message.room_id); !r) return std::unexpected(r.error());

  // after_create_commit: `create_in_index`, then `room.receive(self)`.
  if (auto r = tx.conn().exec(kIndexInsert, message.id, std::string_view(attributes.plain_text)); !r) {
    return std::unexpected(r.error());
  }
  const Timestamp receive_now = tx.now();
  const std::string cutoff = format_db(receive_now.plus_seconds(-kConnectionTtlSeconds));
  if (auto r = tx.conn().exec(kUnread, std::string_view(message.created_at), format_db(receive_now), message.room_id,
                              std::string_view(cutoff), message.creator_id);
      !r) {
    return std::unexpected(r.error());
  }
  if (jobs != nullptr) {
    const std::int64_t room_id = message.room_id;
    const std::int64_t message_id = message.id;
    tx.after_commit([jobs, room_id, message_id] { jobs->push_message(room_id, message_id); });
  }
  return message;
}

Status touch(db::Tx& tx, Message& message, std::string_view plain_text) {
  message.updated_at = tx.now_db();
  if (auto r = tx.conn().exec(kTouchMessage, std::string_view(message.updated_at), message.id); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = tx.conn().exec(kTouchRoom, tx.now_db(), message.room_id); !r) return std::unexpected(r.error());
  // after_update_commit: `update_in_index`
  if (auto r = tx.conn().exec(kIndexUpdate, plain_text, message.id); !r) return std::unexpected(r.error());
  return {};
}

Status update_body(db::Tx& tx, Message& message, std::string_view body, std::string_view plain_text) {
  Arena arena(256);
  auto record = tx.conn().first(kBodyRecord, arena, message.id);
  if (!record) return std::unexpected(record.error());
  if (*record) {
    const std::int64_t record_id = **record;
    auto current = tx.conn().first(kBodyRecordText, arena, record_id);
    if (!current) return std::unexpected(current.error());
    if (*current && **current && ***current == body) return {};
    const std::string at = tx.now_db();
    if (auto r = tx.conn().exec(kUpdateBodyRow, body, at, record_id); !r) return std::unexpected(r.error());
  } else {
    const std::string at = tx.now_db();
    auto r = tx.conn().first(kInsertBody, arena, body, at, "body", message.id, "Message", at);
    if (!r) return std::unexpected(r.error());
  }
  return touch(tx, message, plain_text);
}

Result<ReplacedAttachment> replace_attachment(db::Tx& tx, Message& message, std::optional<std::int64_t> blob_id,
                                              std::string_view plain_text) {
  Arena arena(256);
  ReplacedAttachment replaced;
  auto old = tx.conn().first(kAttachmentOf, arena, message.id);
  if (!old) return std::unexpected(old.error());
  if (*old) {
    if (auto r = tx.conn().exec(kDeleteAttachment, (*old)->id); !r) return std::unexpected(r.error());
    replaced.purged_blob_id = (*old)->blob_id;
    if (auto r = touch(tx, message, plain_text); !r) return std::unexpected(r.error());
  }
  if (blob_id) {
    if (auto r = tx.conn().exec(kAttach, *blob_id, tx.now_db(), message.id); !r) {
      return std::unexpected(r.error());
    }
    if (auto r = touch(tx, message, plain_text); !r) return std::unexpected(r.error());
  }
  return replaced;
}

Result<ReplacedAttachment> destroy(db::Tx& tx, const Message& message) {
  Arena arena(256);
  ReplacedAttachment replaced;
  auto old = tx.conn().first(kAttachmentOf, arena, message.id);
  if (!old) return std::unexpected(old.error());
  if (*old) {
    if (auto r = tx.conn().exec(kDeleteAttachment, (*old)->id); !r) return std::unexpected(r.error());
    replaced.purged_blob_id = (*old)->blob_id;
  }
  if (auto r = tx.conn().exec(kDeleteBoosts, message.id); !r) return std::unexpected(r.error());
  if (auto r = tx.conn().exec(kDeleteBodies, message.id); !r) return std::unexpected(r.error());
  if (auto r = tx.conn().exec(kDeleteMessage, message.id); !r) return std::unexpected(r.error());
  if (auto r = tx.conn().exec(kTouchRoom, tx.now_db(), message.room_id); !r) return std::unexpected(r.error());
  if (auto r = tx.conn().exec(kIndexDelete, message.id); !r) return std::unexpected(r.error());
  return replaced;
}

}  // namespace messages
}  // namespace campfire::models
