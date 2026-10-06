// Users::AvatarsController. Rails: app/controllers/users/avatars_controller.rb. Rust: crates/campfire/src/controllers/
// users/avatars.rs.
//
// `show` is a benchmark route. A hit of the per-worker cache skips the signature check, the user query, the
// attachment query and the rendering of the SVG. An entry holds the number of `users` commits that the change hub
// counted when it was made, and it is dropped when the number is not the same any more: any change of a user row
// (the name, the avatar, which touches the user) makes new entries.
#include <memory>
#include <unordered_map>

#include "app/active_storage.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/concerns.hpp"
#include "app/dispatch.hpp"
#include "assets/assets.hpp"
#include "compat/ruby.hpp"
#include "compat/signed_id.hpp"
#include "core/time_format.hpp"
#include "models/attachments.hpp"
#include "models/user.hpp"
#include "routes/routes.hpp"
#include "views/helpers/accounts.hpp"
#include "views/helpers/users.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `ActionView::Digestor.digest(name: "users/avatars/show", ...)`: the digest of `show.svg.erb`. `EtagWithTemplateDigest`
// adds it when the template can be found for the formats of the request.
constexpr std::string_view kTemplateDigest = "d500db55e2a67222018ef0156839c3c9";

// `expires_in 30.minutes, public: true, stale_while_revalidate: 1.week`
constexpr std::uint64_t kMaxAge = 30 * 60;
constexpr std::uint64_t kStaleWhileRevalidate = 7 * 24 * 60 * 60;

// What a response of this user is made of, until a row of `users` changes.
struct AvatarEntry {
  std::shared_ptr<ChangeHub> hub;  // keeps the hub alive, so that its address is not used again
  std::uint64_t generation = 0;
  std::int64_t user_id = 0;
  std::string cache_key;  // `user.cache_key_with_version`
  bool bot = false;
  bool has_variant = false;  // an avatar that can be transformed: the response comes from the variant file
  std::string svg;           // the initials (not for a bot, nor for a variant)
};

constexpr std::size_t kMaxEntries = 4096;

struct TokenHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
using EntryMap = std::unordered_map<std::string, std::shared_ptr<const AvatarEntry>, TokenHash, std::equal_to<>>;

EntryMap& local_cache() {
  thread_local EntryMap cache;
  return cache;
}

// `lookup_context.find_all("show", ["users/avatars", ...])` finds `show.svg.erb` for the formats of the request: `*/*`
// or svg, or no registered format at all (`Accept: image/*` parses to none).
bool template_found(Rq& rq) {
  auto formats = rq.formats();
  if (!formats) return false;
  if (formats->empty()) return true;
  for (const req::Format format : *formats) {
    if (format == &req::mime::ALL || format == &req::mime::SVG) return true;
  }
  return false;
}

// `User.from_avatar_token(params[:user_id])` and the user. A bad signature is `head :not_found`
// (`rescue_from ActiveSupport::MessageVerifier::InvalidSignature`). A user that is gone is `RecordNotFound`.
Flow<std::shared_ptr<const AvatarEntry>> make_entry(Rq& rq, std::string_view token, std::uint64_t generation) {
  const auto id = compat::signed_id::verify(
      rq.app.secrets, "User", token, "avatar",
      compat::Timestamp{std::chrono::nanoseconds(rq.now().seconds * 1'000'000'000LL + rq.now().nanos)});
  if (!id) return halt(rq.head(404));
  auto user = models::users::find_by_id(rq.db(), rq.arena(), *id);
  if (!user) return fail_internal(user.error().message);
  if (!*user) return fail_with(ErrorKind::NotFound, "Couldn't find User");
  auto entry = std::make_shared<AvatarEntry>();
  entry->hub = rq.app.changes;
  entry->generation = generation;
  entry->user_id = (*user)->id;
  const auto updated = parse_db((*user)->updated_at);
  entry->cache_key = "users/" + std::to_string((*user)->id) + "-" + (updated ? format_cache_version(*updated) : "");
  entry->bot = (*user)->is_bot();
  models::attachments::AttachmentRecords records(rq.db());
  auto attached = records.attached("User", (*user)->id, "avatar");
  if (!attached) return fail_internal(attached.error().message);
  entry->has_variant = attached->has_value() && (*attached)->is_variable();
  if (!entry->has_variant && !entry->bot) {
    const std::string initials = views::helpers::initials((*user)->name);
    Out out;
    views::users::avatars::show_svg(out, views::helpers::avatar_background_color((*user)->id), initials);
    entry->svg = out.to_string();
  }
  return std::shared_ptr<const AvatarEntry>(std::move(entry));
}

Task<Flow<net::Response>> avatars_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  rq.live_response = true;  // `include ActiveStorage::Streaming`
  const std::string_view token = rq.param_str("user_id").value_or("");
  const std::uint64_t generation = rq.app.changes->users_generation();
  auto& cache = local_cache();
  std::shared_ptr<const AvatarEntry> entry;
  if (const auto it = cache.find(token); it != cache.end()) {
    const auto& cached = it->second;
    if (cached->hub == rq.app.changes && cached->generation == generation) entry = cached;
  }
  if (!entry) {
    auto made = make_entry(rq, token, generation);
    if (!made) co_return std::unexpected(std::move(made.error()));
    entry = std::move(*made);
    if (cache.size() >= kMaxEntries) cache.clear();
    cache[std::string(token)] = entry;
  }

  // `stale?(etag: @user)`
  Freshness freshness;
  freshness.etag = entry->cache_key;
  if (template_found(rq)) freshness.template_digest = std::string(kTemplateDigest);
  if (auto not_modified = rq.fresh_when(freshness)) co_return std::move(*not_modified);
  expires_in(rq, kMaxAge, true, kStaleWhileRevalidate);

  if (entry->has_variant) {
    // `send_file ..., content_type: "image/webp", disposition: :inline`
    auto variant = co_await active_storage::processed_variant(
        rq, models::attachments::Record::user(entry->user_id), "avatar",
        storage::Variation::resize_to_limit(512, 512, "webp"));
    if (!variant) co_return std::unexpected(std::move(variant.error()));
    if (*variant) {
      const std::string path = rq.app.storage->path_for(**variant).string();
      co_return rq.send_file(path, "image/webp", "inline", (**variant).key);
    }
  }
  if (entry->bot) {
    // `send_file Rails.root.join("app/assets/images/default-bot-avatar.svg"), content_type: "image/svg+xml"`
    const auto logical = assets::asset_path("default-bot-avatar.svg");
    const auto file = logical ? assets::find_file(*logical) : std::nullopt;
    if (!file) co_return fail_internal("missing asset default-bot-avatar.svg");
    co_return rq.send_data(file->identity, "image/svg+xml", "inline", "default-bot-avatar.svg");
  }
  // `render formats: :svg`
  Out out(rq.ctx.resource());
  if (entry->svg.empty()) {
    const auto user = models::users::find_by_id(rq.db(), rq.arena(), entry->user_id);
    if (!user) co_return fail_internal(user.error().message);
    if (!*user) co_return fail_with(ErrorKind::NotFound, "Couldn't find User");
    views::users::avatars::show_svg(out, views::helpers::avatar_background_color((*user)->id),
                                    views::helpers::initials((*user)->name));
  } else {
    out.append_raw(entry->svg);
  }
  co_return rq.render_as(200, "image/svg+xml; charset=utf-8", std::move(out));
}

// `Current.user.avatar.destroy`, then back to the profile.
Task<Flow<net::Response>> avatars_destroy(Rq& rq) {
  rq.live_response = true;  // `include ActiveStorage::Streaming`
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const std::int64_t user_id = rq.current_user()->id;
  active_storage::Applied applied;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    applied.purge.clear();
    auto removed = models::attachments::destroy(tx, models::attachments::Record::user(user_id), "avatar", applied.purge);
    if (!removed) return std::unexpected(removed.error());
    return {};
  });
  if (!written) co_return fail_internal(written.error().message);
  active_storage::Assignment none;
  co_await active_storage::after_write(rq, models::attachments::Record::user(user_id), none, applied);
  co_return redirect_to_path(rq, campfire::routes::user_profile());
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::users_avatars {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::avatars_show);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::avatars_destroy);
}

}  // namespace campfire::routes::users_avatars
