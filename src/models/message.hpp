// Rails: app/models/message.rb and app/models/message/{attachment,mentionee,pagination,searchable}.rb, the callbacks of
// Room#receive (app/models/room.rb). Rust: crates/db/src/models/message.rs.
//
// Every write here runs on the writer thread in one transaction: the callbacks that Rails runs after the commit (the
// search index, `room.receive`) run in the same transaction, which gives the same rows with fewer statements. The text
// for the search index is made by the caller before the write (it needs rich text rendering, which must stay off the
// writer thread).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"
#include "models/job_sink.hpp"

namespace campfire::models {

// `Message::Pagination::PAGE_SIZE`
inline constexpr std::int64_t kMessagePageSize = 40;

struct Message {
  std::int64_t id = 0;
  std::int64_t room_id = 0;
  std::int64_t creator_id = 0;
  std::string client_message_id;
  std::string created_at;
  std::string updated_at;

  [[nodiscard]] static Message from_row(const db::schema::MessageRow& row);
};

// The attributes of `room.messages.create!` (and `create_with_attachment!`).
struct NewMessage {
  std::int64_t room_id = 0;
  std::int64_t creator_id = 0;
  // `before_create -> { self.client_message_id ||= Random.uuid }`
  std::optional<std::string> client_message_id;
  // The stored Action Text body (canonical HTML), if the body was assigned.
  std::optional<std::string> body;
  std::optional<std::int64_t> attachment_blob_id;
  // `plain_text_body`, for the search index.
  std::string plain_text;
};

namespace messages {

[[nodiscard]] Result<std::optional<Message>> find_by_id(db::Connection& conn, Arena& arena, std::int64_t id);
// `room.messages.find_by(id:)`
[[nodiscard]] Result<std::optional<Message>> find_in_room(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                                          std::int64_t id);
// `Current.user.reachable_messages.find_by(id:)`
[[nodiscard]] Result<std::optional<Message>> find_reachable(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                            std::int64_t id);
// `room.messages.count`
[[nodiscard]] Result<std::int64_t> count_in_room(db::Connection& conn, Arena& arena, std::int64_t room_id);

// `room.messages.with_creator.last_page` and its page_before and page_after variants: oldest first.
[[nodiscard]] Result<std::vector<Message>> last_page(db::Connection& conn, Arena& arena, std::int64_t room_id);
[[nodiscard]] Result<std::vector<Message>> page_before(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                                       const Message& message);
[[nodiscard]] Result<std::vector<Message>> page_after(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                                      const Message& message);
// `room.messages.page_created_since(time)` and `room.messages.without(that).page_updated_since(time)`. `time` is the
// text of the database. The updated page is oldest first.
[[nodiscard]] Result<std::vector<Message>> page_created_since(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                                              std::string_view time);
[[nodiscard]] Result<std::vector<Message>> page_updated_since_without_new(db::Connection& conn, Arena& arena,
                                                                          std::int64_t room_id, std::string_view time);
// `room.messages.before(message).exists?` and `.after(message).exists?`
[[nodiscard]] Result<bool> exists_before(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                         const Message& message);
[[nodiscard]] Result<bool> exists_after(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                        const Message& message);

// The stored body of the message (`message.body.body`), if it has one.
[[nodiscard]] Result<std::optional<std::string>> body_html(db::Connection& conn, Arena& arena, std::int64_t id);

// `room.messages.create!`: the message, its body (which touches the message), its attachment, the room touch, then the
// callbacks: the search index and `room.receive` (the unread memberships, the push job).
[[nodiscard]] Result<Message> create(db::Tx& tx, const NewMessage& attributes, JobSink* jobs);

// `message.update!(body:)`: nothing if the body is the same. Else the body changes and `touch` runs.
// `plain_text` is the new `plain_text_body`.
[[nodiscard]] Status update_body(db::Tx& tx, Message& message, std::string_view body, std::string_view plain_text);

// `touch` (from a boost, an attachment change, or a body change): the message, then its room, then the search index.
// `plain_text` is the current `plain_text_body`.
[[nodiscard]] Status touch(db::Tx& tx, Message& message, std::string_view plain_text);

// `update!(attachment:)`: `has_one_attached` replaces the attachment. The old blob id is returned when there was one,
// for the purge job. `blob_id` nothing removes the attachment.
struct ReplacedAttachment {
  std::optional<std::int64_t> purged_blob_id;
};
[[nodiscard]] Result<ReplacedAttachment> replace_attachment(db::Tx& tx, Message& message,
                                                            std::optional<std::int64_t> blob_id,
                                                            std::string_view plain_text);

// `message.destroy`: its attachment, boosts and body, then the message. The room is touched. The search index entry
// goes too. Gives the blob id of the attachment, for the purge job.
[[nodiscard]] Result<ReplacedAttachment> destroy(db::Tx& tx, const Message& message);

}  // namespace messages
}  // namespace campfire::models
