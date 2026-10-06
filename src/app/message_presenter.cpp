// Rails: app/views/messages/*, app/helpers/messages_helper.rb. Rust: crates/campfire/src/controllers/presenters.rs.
#include "app/message_presenter.hpp"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <memory>

#include "assets/assets.hpp"
#include "compat/global_id.hpp"
#include "compat/signed_id.hpp"
#include "core/time_format.hpp"
#include "models/sound.hpp"
#include "models/storage_records.hpp"
#include "richtext/content.hpp"
#include "richtext/richtext.hpp"
#include "richtext/text_util.hpp"
#include "routes/routes.hpp"
#include "storage/paths.hpp"
#include "storage/storage.hpp"

namespace campfire::app {

namespace {

using views::messages::AttachmentPreview;
using views::messages::AttachmentView;
using views::messages::BoostView;
using views::messages::MessageContent;
using views::messages::MessageView;
using views::messages::UserView;

// `Message::THUMBNAIL_MAX_WIDTH` and `THUMBNAIL_MAX_HEIGHT`.
constexpr std::int64_t kThumbnailMaxWidth = 1200;
constexpr std::int64_t kThumbnailMaxHeight = 800;

struct CodeDeleter {
  void operator()(pcre2_code* code) const noexcept { pcre2_code_free(code); }
};

const pcre2_code* all_emoji_code() {
  static const std::unique_ptr<pcre2_code, CodeDeleter> code = [] {
    constexpr std::string_view pattern = R"(\A(\p{Emoji_Presentation}|\p{Extended_Pictographic}|\x{FE0F})+\z)";
    int error = 0;
    PCRE2_SIZE offset = 0;
    pcre2_code* compiled = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), PCRE2_UTF,
                                         &error, &offset, nullptr);
    return std::unique_ptr<pcre2_code, CodeDeleter>(compiled);
  }();
  return code.get();
}

std::string to_fs_number(const std::string& db_text) {
  const auto t = parse_db(db_text);
  return t ? format_to_fs_number(*t) : std::string{};
}

compat::Timestamp compat_now(const App& app) {
  return models::to_compat(app.now());
}

richtext::MentionUser mention_user(const App& app, const models::User& user) {
  richtext::MentionUser out;
  out.id = user.id;
  out.name = user.name;
  out.title = user_title(user);
  out.attachable_sgid = compat::global_id::attachable_sgid(
      app.secrets, compat::global_id::GlobalId::make("User", std::to_string(user.id)));
  out.user_path = campfire::routes::user(user.id);
  out.avatar_path = avatar_path(app, user);
  return out;
}

}  // namespace

bool all_emoji(std::string_view text) {
  const pcre2_code* code = all_emoji_code();
  if (code == nullptr || text.empty()) return false;
  pcre2_match_data* data = pcre2_match_data_create_from_pattern(code, nullptr);
  const int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(), 0, 0, data, nullptr);
  pcre2_match_data_free(data);
  return rc >= 0;
}

std::string user_title(const models::User& user) {
  const auto blank = [](std::string_view s) { return s.find_first_not_of(" \t\n\v\f\r") == std::string_view::npos; };
  std::string title;
  if (!blank(user.name)) title = user.name;
  if (user.bio && !blank(*user.bio)) {
    if (!title.empty()) title += " \xE2\x80\x93 ";
    title += *user.bio;
  }
  return title;
}

std::string to_sentence(const std::vector<std::string>& items, std::string_view two_words_connector) {
  if (items.empty()) return {};
  if (items.size() == 1) return items[0];
  if (items.size() == 2) return items[0] + std::string(two_words_connector) + items[1];
  std::string out;
  for (std::size_t i = 0; i + 1 < items.size(); ++i) out += (i == 0 ? "" : ", ") + items[i];
  return out + ", and " + items.back();
}

std::string avatar_token(const App& app, std::int64_t user_id) {
  return compat::signed_id::generate(app.secrets, "User", user_id, "avatar", std::nullopt);
}

std::string avatar_path(const App& app, const models::User& user) {
  return campfire::routes::fresh_user_avatar(avatar_token(app, user.id), to_fs_number(user.updated_at));
}

views::messages::UserView user_view(const App& app, const models::User& user) {
  return UserView{user.id, user.name, user_title(user), avatar_path(app, user)};
}

std::optional<richtext::MentionUser> DbRecordLookup::user(std::int64_t id) const {
  auto found = models::users::find_by_id(*conn_, *arena_, id);
  if (!found || !*found) return std::nullopt;
  return mention_user(*app_, **found);
}

bool DbRecordLookup::record_exists(std::string_view model, std::int64_t id) const {
  if (model == "Message") {
    auto found = models::messages::find_by_id(*conn_, *arena_, id);
    return found && *found;
  }
  if (model == "Room" || model.starts_with("Rooms::")) {
    auto found = models::room_refs::find(*conn_, *arena_, id);
    return found && *found;
  }
  return false;
}

MessagePresenter::MessagePresenter(db::Connection& conn, Arena& arena, const App& app, std::string request_host)
    : conn_(&conn),
      arena_(&arena),
      app_(&app),
      request_host_(std::move(request_host)),
      lookup_(conn, arena, app),
      resolver_(app.secrets, compat_now(app), lookup_) {}

Result<models::User> MessagePresenter::user(std::int64_t id) {
  if (const auto it = users_.find(id); it != users_.end()) return it->second;
  auto found = models::users::find_by_id(*conn_, *arena_, id);
  if (!found) return std::unexpected(found.error());
  if (!*found) return fail(Errc::NotFound, "Couldn't find User with 'id'=" + std::to_string(id));
  return users_.emplace(id, std::move(**found)).first->second;
}

Result<views::messages::UserView> MessagePresenter::user_view(std::int64_t id) {
  auto found = user(id);
  if (!found) return std::unexpected(found.error());
  return app::user_view(*app_, *found);
}

Result<std::string> MessagePresenter::room_display_name(const models::RoomRef& room) {
  if (!room.direct()) return room.name.value_or("");
  auto members = models::room_refs::users(*conn_, *arena_, room.id);
  if (!members) return std::unexpected(members.error());
  std::vector<std::string> names;
  for (const models::User& member : *members) names.push_back(member.name);
  return to_sentence(names, " and ");
}

Result<std::string> MessagePresenter::body_html(const models::Message& message) {
  auto body = models::messages::body_html(*conn_, *arena_, message.id);
  if (!body) return std::unexpected(body.error());
  return body->value_or("");
}

Result<std::optional<views::messages::AttachmentView>> MessagePresenter::attachment(const models::Message& message) {
  models::DbRecords records(*conn_);
  auto blob = records.attached("Message", message.id, "attachment");
  if (!blob) return std::unexpected(blob.error());
  if (!*blob) return std::optional<AttachmentView>{};
  const storage::Blob& b = **blob;
  const compat::MessageVerifier& verifier = app_->storage->verifier();
  AttachmentView view;
  view.filename = b.filename.sanitized();
  view.blob_path = storage::paths::blob_redirect_path(verifier, b);
  view.download_path = storage::paths::blob_redirect_path(verifier, b, "attachment");
  if (b.is_previewable() || b.is_variable()) {
    const compat::Variation thumb =
        compat::Variation::resize_to_limit(kThumbnailMaxWidth, kThumbnailMaxHeight, std::nullopt);
    if (b.is_video()) {
      // `attachment.preview(format: :webp, resize_to_limit: [...])`
      view.preview.kind = AttachmentPreview::Kind::Video;
      using compat::marshal::Value;
      const compat::Variation poster({{"format", Value::symbol("webp")},
                                      {"resize_to_limit", Value::array({Value::integer(kThumbnailMaxWidth),
                                                                        Value::integer(kThumbnailMaxHeight)})}});
      view.preview.url = storage::paths::representation_redirect_path(verifier, b, poster);
    } else {
      view.preview.kind = AttachmentPreview::Kind::Image;
      compat::Variation variation = thumb;
      if (!b.is_previewable()) {
        auto varied = app_->storage->variation_for(b, thumb);
        if (!varied) return std::unexpected(varied.error());
        variation = std::move(*varied);
      }
      view.preview.url = storage::paths::representation_redirect_path(verifier, b, variation);
    }
  }
  const auto dimension = [&](std::string_view name) -> std::optional<views::messages::RubyNumber> {
    const compat::json::Value* value = b.metadata.find(name);
    if (value == nullptr) return std::nullopt;
    if (const auto n = value->to_int64()) return views::messages::RubyNumber{false, static_cast<double>(*n)};
    if (value->is_double()) return views::messages::RubyNumber{true, value->as_double()};
    return std::nullopt;
  };
  view.width = dimension("width");
  view.height = dimension("height");
  return std::optional<AttachmentView>(std::move(view));
}

Result<std::string> MessagePresenter::plain_text_body(const models::Message& message) {
  auto body = body_html(message);
  if (!body) return std::unexpected(body.error());
  {
    auto text = richtext::to_plain_text(*body, render_context());
    if (!text) return fail(Errc::Parse, text.error().message);
    if (!richtext::is_blank(*text)) return std::move(*text);
  }
  auto attached = attachment(message);
  if (!attached) return std::unexpected(attached.error());
  return *attached ? (*attached)->filename : std::string();
}

Result<views::messages::BoostView> MessagePresenter::boost(const models::Boost& boost) {
  auto booster = user_view(boost.booster_id);
  if (!booster) return std::unexpected(booster.error());
  BoostView view;
  view.id = boost.id;
  view.updated_at = boost.updated_at;
  view.message_id = boost.message_id;
  view.content = boost.content;
  view.all_emoji = all_emoji(boost.content);
  view.booster = std::move(*booster);
  return view;
}

Result<views::messages::MessageView> MessagePresenter::message(const models::Message& message) {
  // The room of a page is the same for each message: read it and its display name once.
  auto cached_name = room_names_.find(message.room_id);
  if (cached_name == room_names_.end()) {
    auto room = models::room_refs::find(*conn_, *arena_, message.room_id);
    if (!room) return std::unexpected(room.error());
    if (!*room) return fail(Errc::NotFound, "Couldn't find Room with 'id'=" + std::to_string(message.room_id));
    auto room_name = room_display_name(**room);
    if (!room_name) return std::unexpected(room_name.error());
    cached_name = room_names_.emplace(message.room_id, std::move(*room_name)).first;
  }

  MessageView view;
  view.id = message.id;
  view.client_message_id = message.client_message_id;
  view.room_id = message.room_id;
  view.room_name = cached_name->second;
  view.created_at = message.created_at;
  view.updated_at = message.updated_at;

  auto creator = user_view(message.creator_id);
  if (!creator) {
    if (creator.error().code != Errc::NotFound) return std::unexpected(creator.error());
    // `message_tag` rescues what its block raises (`avatar_tag message.creator` for a creator that is gone) and renders
    // `messages/_unrenderable` in place of the whole message.
    view.creator = UserView{message.creator_id, {}, {}, {}};
    view.content = views::messages::UnrenderableContent{};
    return view;
  }
  view.creator = std::move(*creator);

  auto plain_text = plain_text_body(message);
  if (!plain_text) return std::unexpected(plain_text.error());
  view.all_emoji = all_emoji(*plain_text);
  auto content = content_of(message, *plain_text);
  if (!content) return std::unexpected(content.error());
  view.content = std::move(*content);

  auto boosts = models::boosts::for_message_ordered(*conn_, *arena_, message.id);
  if (!boosts) return std::unexpected(boosts.error());
  for (const models::Boost& b : *boosts) {
    auto boost_view = boost(b);
    if (!boost_view) return std::unexpected(boost_view.error());
    view.boosts.push_back(std::move(*boost_view));
  }
  return view;
}

Result<views::messages::MessageContent> MessagePresenter::content_of(const models::Message& message,
                                                                     const std::string& plain_text) {
  auto body = body_html(message);
  if (!body) return std::unexpected(body.error());
  const richtext::RenderContext ctx = render_context();
  // `message_tag` evaluates `message.plain_text_body` first. Where that raises, it rescues and renders
  // `messages/_unrenderable`, unless logging the exception raises again (a message that is not UTF-8): then the page
  // fails (verified against the reference).
  if (auto text = richtext::to_plain_text(*body, ctx); !text) {
    if (text.error().kind == richtext::Error::Kind::Unrenderable) {
      return fail(Errc::Internal, "message_tag's rescue raised logging " + text.error().message);
    }
    return MessageContent(views::messages::UnrenderableContent{});
  }
  auto attached = attachment(message);
  if (!attached) return std::unexpected(attached.error());
  if (*attached) return MessageContent(std::move(**attached));
  if (const models::Sound* sound = models::sounds::sound_in(plain_text)) {
    views::messages::SoundView view;
    view.url = assets::asset_path(std::string(sound->name) + ".mp3").value_or("");
    if (sound->image) {
      view.image =
          views::messages::SoundImage{assets::asset_path("sounds/" + std::string(sound->image->file)).value_or(""),
                                      sound->image->width, sound->image->height};
    }
    if (sound->text) view.text = std::string(*sound->text);
    return MessageContent(std::move(view));
  }
  const richtext::Presentation presentation = richtext::present_message(*body, ctx);
  if (presentation.kind == richtext::Presentation::Kind::Html) {
    return MessageContent(views::messages::TextContent{presentation.html});
  }
  return MessageContent(views::messages::UnrenderableContent{});
}

Result<std::string> MessagePresenter::body_to_s(const models::Message& message) {
  auto body = body_html(message);
  if (!body) return std::unexpected(body.error());
  if (body->empty()) return std::string();
  const richtext::RenderContext ctx = render_context();
  auto content = richtext::Content::load(*body, ctx);
  if (!content) return std::string();
  auto html = content->to_rendered_html_with_layout(ctx);
  return html ? std::move(*html) : std::string();
}

Result<std::string> MessagePresenter::editable_body(const models::Message& message) {
  auto body = body_html(message);
  if (!body) return std::unexpected(body.error());
  // An error is where the edit page raises in Rails (a missing attachment, for example).
  auto value = richtext::editable_value(*body, render_context());
  if (!value) return fail(Errc::Internal, "editable_body raised: " + value.error().message);
  return value->value_or("");
}

compat::json::Value MessagePresenter::user_json(const models::User& user, std::string_view base_url) const {
  const std::string_view role = user.role == 1 ? "administrator" : user.role == 2 ? "bot" : "member";
  return compat::json::Value(compat::json::Value::Object{
      {"id", compat::json::Value(user.id)},
      {"name", compat::json::Value(user.name)},
      {"role", compat::json::Value(role)},
      {"avatar_url", compat::json::Value(std::string(base_url) + avatar_path(*app_, user))}});
}

Result<compat::json::Value> MessagePresenter::message_json(const models::Message& message, std::string_view base_url) {
  auto plain_text = plain_text_body(message);
  if (!plain_text) return std::unexpected(plain_text.error());
  auto html = body_to_s(message);
  if (!html) return std::unexpected(html.error());
  auto creator = user(message.creator_id);
  if (!creator) return std::unexpected(creator.error());
  using compat::json::Value;
  const auto created = parse_db(message.created_at);
  return Value(Value::Object{
      {"id", Value(message.id)},
      {"created_at", Value(created ? format_iso8601_millis(*created) : std::string())},
      {"body", Value(Value::Object{{"plain_text", Value(std::move(*plain_text))}, {"html", Value(std::move(*html))}})},
      {"creator", user_json(*creator, base_url)},
      {"room", Value(Value::Object{{"id", Value(message.room_id)}})},
      {"url", Value(std::string(base_url) + campfire::routes::room_message(message.room_id, message.id))}});
}

Result<compat::json::Value> MessagePresenter::boost_json(const models::Boost& boost, const models::Message& message,
                                                         std::string_view base_url) {
  auto booster = user(boost.booster_id);
  if (!booster) return std::unexpected(booster.error());
  using compat::json::Value;
  const auto created = parse_db(boost.created_at);
  return Value(Value::Object{
      {"id", Value(boost.id)},
      {"content", Value(boost.content)},
      {"created_at", Value(created ? format_iso8601_millis(*created) : std::string())},
      {"booster", user_json(*booster, base_url)},
      {"message", Value(Value::Object{{"id", Value(boost.message_id)},
                                      {"url", Value(std::string(base_url) +
                                                    campfire::routes::room_message(message.room_id, message.id))}})}});
}

}  // namespace campfire::app
