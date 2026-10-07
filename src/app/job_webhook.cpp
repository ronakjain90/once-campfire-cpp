// Bot::WebhookJob and Webhook#deliver. Rails: app/jobs/bot/webhook_job.rb, app/models/webhook.rb,
// app/models/user/bot.rb. Rust: crates/campfire/src/integrations/jobs.rs (deliver_webhook).
#include "app/job_support.hpp"
#include "app/message_actions.hpp"
#include "app/message_presenter.hpp"
#include "app/webhook.hpp"
#include "compat/json.hpp"
#include "core/log.hpp"
#include "core/time_format.hpp"
#include "models/room_ref.hpp"
#include "models/storage_records.hpp"
#include "models/user_admin.hpp"
#include "richtext/richtext.hpp"
#include "routes/routes.hpp"
#include "storage/storage.hpp"

namespace campfire::app::job {

namespace {

using compat::json::Value;

// `Webhook#payload`: the JSON that the bot gets, in the order of the Ruby hash.
std::string payload_of(MessagePresenter& presenter, db::Connection& conn, Arena& arena, const models::RoomRef& room,
                       const models::Message& message, const models::User& bot) {
  const models::User creator = must(presenter.user(message.creator_id));
  const auto html = must(models::messages::body_html(conn, arena, message.id));
  const std::string plain = richtext::without_recipient_mentions(must(presenter.plain_text_body(message)), bot.name);
  Value::Object user{{"id", Value(creator.id)}, {"name", Value(creator.name)}};
  Value::Object room_object{{"id", Value(room.id)},
                            {"name", room.name ? Value(*room.name) : Value(nullptr)},
                            {"path", Value(campfire::routes::room_bot_messages(room.id, models::users::bot_key(bot)))}};
  Value::Object body{{"html", html ? Value(*html) : Value(nullptr)}, {"plain", Value(plain)}};
  Value::Object message_object{{"id", Value(message.id)},
                               {"body", Value(std::move(body))},
                               {"path", Value(campfire::routes::room_at_message(room.id, message.id))}};
  Value::Object root{{"user", Value(std::move(user))},
                     {"room", Value(std::move(room_object))},
                     {"message", Value(std::move(message_object))}};
  return compat::json::encode(Value(std::move(root)));
}

// `room.messages.create!(body: text, creator: bot)`: the text is assigned as the rich text body.
models::Message create_text_reply(App& app, MessagePresenter& presenter, const models::RoomRef& room,
                                  const models::User& bot, const std::string& text) {
  models::NewMessage attributes;
  attributes.room_id = room.id;
  attributes.creator_id = bot.id;
  const std::string body = messages::canonical_body(presenter, text);
  attributes.body = body;
  attributes.plain_text = messages::plain_text_for(presenter, body, "");
  models::JobSink* sink = app.job_sink.get();
  return must(write(app, [&](db::Tx& tx) { return models::messages::create(tx, attributes, sink); }));
}

// `ActiveStorage::Blob.create_and_upload!` (its own save), then `room.messages.create_with_attachment!`, which
// processes the attachment.
models::Message create_attachment_reply(App& app, MessagePresenter& presenter, db::Connection& conn, Arena& arena,
                                        const models::RoomRef& room, const models::User& bot,
                                        const jobs::webhook::Attachment& attachment) {
  const storage::Storage& store = *app.storage;
  auto staged = must(store.stage_bytes(attachment.data, storage::Filename::from_bytes(attachment.filename),
                                       std::string_view(attachment.content_type)));
  const std::string filename = staged.blob().filename.sanitized();
  std::optional<storage::Blob> saved;
  must(write(app, [&](db::Tx& tx) -> Status {
    models::DbRecords records(tx.conn());
    auto inserted = staged.insert(records, models::to_compat(tx.now()));
    if (!inserted) return std::unexpected(inserted.error());
    saved = std::move(*inserted);
    tx.after_commit([&staged] { staged.keep(); });
    return {};
  }));

  models::NewMessage attributes;
  attributes.room_id = room.id;
  attributes.creator_id = bot.id;
  attributes.attachment_blob_id = saved->id;
  attributes.plain_text = messages::plain_text_for(presenter, "", filename);
  models::JobSink* sink = app.job_sink.get();
  models::Message message =
      must(write(app, [&](db::Tx& tx) { return models::messages::create(tx, attributes, sink); }));
  JobThread& thread = job_thread();
  must(run(messages::process_attachment_detached(app, thread.scheduler, conn, *saved, message, attributes.plain_text)));
  auto reloaded = must(models::messages::find_by_id(conn, arena, message.id));
  if (!reloaded) raise("Couldn't find Message with 'id'=" + std::to_string(message.id));
  return std::move(*reloaded);
}

}  // namespace

void deliver_webhook(App& app, std::int64_t bot_id, std::int64_t message_id) {
  JobThread& thread = job_thread();
  db::Connection& conn = thread.reader;
  Arena arena(8192);
  auto bot = must(models::users::find_by_id(conn, arena, bot_id));
  if (!bot) raise("Couldn't find User with 'id'=" + std::to_string(bot_id));
  auto message = must(models::messages::find_by_id(conn, arena, message_id));
  if (!message) raise("Couldn't find Message with 'id'=" + std::to_string(message_id));
  auto room = must(models::room_refs::find(conn, arena, message->room_id));
  if (!room) raise("Couldn't find Room with 'id'=" + std::to_string(message->room_id));
  const auto url = must(models::users::webhook_url(conn, arena, bot_id));
  if (!url) raise("undefined method 'deliver' for nil (the bot has no webhook)");

  MessagePresenter presenter(conn, arena, app, std::string());
  std::string payload = payload_of(presenter, conn, arena, *room, *message, *bot);
  auto delivery = must(webhook::deliver(app.webhook_network, *url, std::move(payload)));
  models::Message reply;
  switch (delivery.reply.kind) {
    case jobs::webhook::Reply::Kind::None: return;
    case jobs::webhook::Reply::Kind::Text:
      reply = create_text_reply(app, presenter, *room, *bot, delivery.reply.text);
      break;
    case jobs::webhook::Reply::Kind::Attachment:
      reply = create_attachment_reply(app, presenter, conn, arena, *room, *bot, delivery.reply.attachment);
      break;
  }
  (void)must(messages::broadcast_create_detached(app, conn, arena, *room, reply));
}

}  // namespace campfire::app::job
