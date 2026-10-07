// Builds the view models of the message views from rows: what the Rails views read off the records, computed up front.
// Rails: app/views/messages/*, app/helpers/messages_helper.rb, app/models/message/*. Rust:
// crates/campfire/src/controllers/presenters.rs (Presenter) and presenters/rich_text.rs (DbResolver).
//
// One presenter lives for one request or one broadcast. It reads through one connection and keeps rows that it
// found (Rails preloads them with `with_creator` and `with_boosts`).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "app/app.hpp"
#include "compat/json.hpp"
#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "models/boost.hpp"
#include "models/message.hpp"
#include "models/room_ref.hpp"
#include "models/user.hpp"
#include "richtext/resolver.hpp"
#include "views/messages/types.hpp"

namespace campfire::app {

// `String#all_emoji?` (reference/lib/rails_ext/string.rb).
[[nodiscard]] bool all_emoji(std::string_view text);
// `User#title`: `[ name, bio ].compact_blank.join(" – ")`.
[[nodiscard]] std::string user_title(const models::User& user);
// `Array#to_sentence`, with `two_words_connector`.
[[nodiscard]] std::string to_sentence(const std::vector<std::string>& items, std::string_view two_words_connector);
// `user.avatar_token`: `signed_id(purpose: :avatar)`.
[[nodiscard]] std::string avatar_token(const App& app, std::int64_t user_id);
// `fresh_user_avatar_path(user)`.
[[nodiscard]] std::string avatar_path(const App& app, const models::User& user);
[[nodiscard]] views::messages::UserView user_view(const App& app, const models::User& user);

// The records that Action Text rendering needs: users for the mention attachments.
class DbRecordLookup final : public richtext::RecordLookup {
 public:
  DbRecordLookup(db::Connection& conn, Arena& arena, const App& app) : conn_(&conn), arena_(&arena), app_(&app) {}
  [[nodiscard]] std::optional<richtext::MentionUser> user(std::int64_t id) const override;
  [[nodiscard]] bool record_exists(std::string_view model, std::int64_t id) const override;

 private:
  db::Connection* conn_;
  Arena* arena_;
  const App* app_;
};

class MessagePresenter {
 public:
  // `request_host` is `Current.request_host`: opengraph embeds are checked against it. Empty when unset.
  MessagePresenter(db::Connection& conn, Arena& arena, const App& app, std::string request_host);

  [[nodiscard]] Result<models::User> user(std::int64_t id);
  [[nodiscard]] Result<views::messages::UserView> user_view(std::int64_t id);
  // `room_display_name(room, for_user: nil)`
  [[nodiscard]] Result<std::string> room_display_name(const models::RoomRef& room);
  [[nodiscard]] richtext::RenderContext render_context() const { return {resolver_, request_host_}; }

  // `message.body.body`, or empty when the message has no body.
  [[nodiscard]] Result<std::string> body_html(const models::Message& message);
  // `message.plain_text_body`
  [[nodiscard]] Result<std::string> plain_text_body(const models::Message& message);
  // A message as `messages/_message` shows it.
  [[nodiscard]] Result<views::messages::MessageView> message(const models::Message& message);
  [[nodiscard]] Result<views::messages::BoostView> boost(const models::Boost& boost);
  // `message.attachment` as `Messages::AttachmentPresentation` needs it.
  [[nodiscard]] Result<std::optional<views::messages::AttachmentView>> attachment(const models::Message& message);
  // `message.body.to_s`
  [[nodiscard]] Result<std::string> body_to_s(const models::Message& message);
  // `editable_body(message)` for the `value` of the editor.
  [[nodiscard]] Result<std::string> editable_body(const models::Message& message);

  // `messages/_message.json.jbuilder`
  [[nodiscard]] Result<compat::json::Value> message_json(const models::Message& message, std::string_view base_url);
  // `messages/boosts/_boost.json.jbuilder`
  [[nodiscard]] Result<compat::json::Value> boost_json(const models::Boost& boost, const models::Message& message,
                                                       std::string_view base_url);
  // `users/_user.json.jbuilder`
  [[nodiscard]] compat::json::Value user_json(const models::User& user, std::string_view base_url) const;

 private:
  db::Connection* conn_;
  Arena* arena_;
  const App* app_;
  std::string request_host_;
  DbRecordLookup lookup_;
  richtext::CompatResolver resolver_;
  // `message.content_type` with what each presentation needs.
  [[nodiscard]] Result<views::messages::MessageContent> content_of(const models::Message& message,
                                                                   const std::string& plain_text);

  std::unordered_map<std::int64_t, models::User> users_;
  // `room_display_name(message.room, for_user: nil)` of the rooms that this presenter has seen.
  std::unordered_map<std::int64_t, std::string> room_names_;
};

}  // namespace campfire::app
