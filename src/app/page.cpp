// Pages in the layout and the page cache path. Rust: crates/campfire/src/controllers/presenters/view_context.rs.
#include "app/page.hpp"

#include <stdexcept>

#include "app/controllers/rooms.hpp"
#include "app/message_presenter.hpp"
#include "app/platform.hpp"
#include "app/splice.hpp"
#include "assets/assets.hpp"
#include "compat/signed_id.hpp"
#include "core/time_format.hpp"
#include "models/account.hpp"
#include "models/room.hpp"
#include "routes/routes.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app {

namespace {

std::string to_fs_number(const std::string& db_text) {
  const auto t = parse_db(db_text);
  return t ? format_to_fs_number(*t) : std::string{};
}

}  // namespace

std::string user_avatar_path(const Rq& rq, const models::User& user) {
  // The token has no expiry: the path is a pure function of the user id and `updated_at`, so the worker keeps it.
  std::string key = "avatar:" + std::to_string(user.id) + ':';
  key += user.updated_at;
  return rq.worker.memo(key, [&] {
    const std::string token = compat::signed_id::generate(rq.app.secrets, "User", user.id, "avatar", std::nullopt);
    return campfire::routes::fresh_user_avatar(token, to_fs_number(user.updated_at));
  });
}

views::messages::UserView current_user_view(const Rq& rq) {
  const models::User& user = *rq.current_user();
  return views::messages::UserView{user.id, user.name, user_title(user), user_avatar_path(rq, user)};
}

Flow<LayoutData> load_layout(Rq& rq) {
  LayoutData data;
  auto account = models::accounts::first(rq.db(), rq.arena());
  if (!account) return fail_internal(account.error().message);
  if (*account) {
    const models::Account& a = **account;
    data.account.name = a.name;
    data.account.logo_url = campfire::routes::fresh_account_logo(to_fs_number(a.updated_at));
    data.account.has_logo = a.has_logo;
    data.custom_styles = a.custom_styles;
    data.restrict_room_creation_to_administrators = a.restrict_room_creation_to_administrators;
  } else {
    data.account.logo_url = campfire::routes::fresh_account_logo();
  }
  if (const models::User* user = rq.current_user()) {
    views::CurrentUser current;
    current.id = user->id;
    current.name = user->name;
    current.administrator = user->can_administer();
    current.bot = user->is_bot();
    current.avatar_url = user_avatar_path(rq, *user);
    data.current_user = std::move(current);
    // `last_room_visited`: `Current.user.rooms.find_by(id: cookies[:last_room]) || Current.user.rooms.original`
    std::optional<models::Room> visited;
    if (const auto cookie = rq.cookies().get("last_room")) {
      if (const auto id = controllers::cast_id(*cookie)) {
        auto found = models::rooms::find_for_user(rq.db(), rq.arena(), user->id, models::RoomScope::All, *id);
        if (!found) return fail_internal(found.error().message);
        visited = std::move(*found);
      }
    }
    if (!visited) {
      auto original = models::rooms::original_of_user(rq.db(), rq.arena(), user->id);
      if (!original) return fail_internal(original.error().message);
      visited = std::move(*original);
    }
    if (visited) data.last_room_visited_id = visited->id;
  }
  return data;
}

views::ViewContext make_view_context(Rq& rq, const LayoutData& data) {
  views::ViewContext ctx;
  ctx.current_user = data.current_user;
  ctx.account = data.account;
  if (const auto notice = rq.flash().notice()) ctx.flash_notice = std::string(*notice);
  if (const auto alert = rq.flash().alert()) ctx.flash_alert = std::string(*alert);
  ctx.vapid_public_key = rq.app.config.vapid_public_key;
  ctx.asset_path = [](std::string_view source) {
    auto path = assets::asset_path(source);
    if (!path) throw std::runtime_error(path.error().message);  // Propshaft::MissingAssetError: a 500
    return std::move(*path);
  };
  ctx.importmap_tags = std::string(assets::javascript_importmap_tags());
  ctx.stylesheet_tags = rq.app.stylesheets.html;
  ctx.custom_styles = data.custom_styles;
  ctx.base_url = rq.info.base_url();
  ctx.request_url = rq.info.url();
  if (rq.request.has_header("referer")) ctx.referrer = std::string(rq.request.header("referer"));
  ctx.last_room_visited_id = data.last_room_visited_id;
  ctx.app_version = rq.app.config.app_version;
  // `platform`: a page that prints it must add a facet for the User-Agent to its cache key.
  ctx.platform = ApplicationPlatform(rq.user_agent()).to_view();
  return ctx;
}

void render_in_layout(Rq& rq, const LayoutData& data, const views::LayoutParts& parts, Out& out) {
  if (rq.is_turbo_frame_request()) {
    views::layouts::turbo_rails::frame(out, parts);
    return;
  }
  const views::ViewContext ctx = make_view_context(rq, data);
  views::layouts::application(out, ctx, parts);
}

void add_page_facets(Rq& rq, db::DependencyScope& deps, std::string_view page) {
  deps.facet("page", page);
  deps.facet("base_url", rq.info.base_url());
  deps.facet("frame", static_cast<std::uint64_t>(rq.is_turbo_frame_request()));
  if (const models::User* user = rq.current_user()) {
    deps.facet("user", static_cast<std::uint64_t>(user->id));
    deps.facet("user_updated_at", user->updated_at);
    deps.facet("user_role", static_cast<std::uint64_t>(user->role));
  }
  const auto notice = rq.flash().notice();
  const auto alert = rq.flash().alert();
  deps.facet("notice", notice.value_or(""));
  deps.facet("alert", alert.value_or(""));
}

net::Response layout_response(Rq& rq, int status, Out&& body) {
  if (!rq.is_turbo_frame_request()) {
    rq.set_header("link", assets::append_preload_links(rq.staged_header("link"), rq.app.stylesheets.preload_links));
  }
  return rq.html(status, std::move(body));
}

namespace {

// Notes where the templates put the cached fragments in the body of a page.
class SpanRecorder final : public views::FragmentRecorder {
 public:
  void record(std::size_t offset, std::size_t size) override { spans.push_back({offset, size}); }
  std::vector<FragmentSpan> spans;
};

}  // namespace

Flow<net::Response> cached_page_checked(Rq& rq, int status, db::DependencyScope& deps,
                                        const std::function<Flow<void>(Out&)>& render, bool preload_link,
                                        bool parts_etag_wanted) {
  SpanRecorder recorder;
  const auto make_body = [&]() -> Flow<Out> {
    Out out(rq.ctx.resource());
    recorder.spans.clear();
    if (parts_etag_wanted) views::set_fragment_recorder(&recorder);
    auto done = render(out);
    views::set_fragment_recorder(nullptr);
    if (!done) return std::unexpected(std::move(done.error()));
    return out;
  };
  if (preload_link && !rq.is_turbo_frame_request()) {
    rq.set_header("link", assets::append_preload_links(rq.staged_header("link"), rq.app.stylesheets.preload_links));
  }
  if (!deps.cacheable()) {
    auto body = make_body();
    if (!body) return std::unexpected(std::move(body.error()));
    return rq.html(status, std::move(*body));
  }
  PageCache& cache = rq.app.pages;
  const Hash128 key = deps.key();
  if (auto hit = cache.get(key)) {
    if (cache.audit_due()) {
      auto fresh = make_body();
      if (!fresh) return std::unexpected(std::move(fresh.error()));
      cache.audit_compare(key, *hit, fresh->to_string());
    }
    return rq.respond_page(std::move(hit), status);
  }
  auto body = make_body();
  if (!body) return std::unexpected(std::move(body.error()));
  std::string text = body->to_string();
  if (parts_etag_wanted) {
    // Kept pieces and digests of the parts: no compression or hash of the whole body (src/app/splice.hpp).
    if (auto spliced = splice::build(text, recorder.spans)) {
      auto entry = cache.put_built(key, std::move(text), std::move(spliced->gzip), "text/html; charset=utf-8",
                                   std::move(spliced->etag));
      return rq.respond_page(std::move(entry), status);
    }
  }
  std::string etag;
  if (parts_etag_wanted) etag = parts_etag(text, recorder.spans);
  auto entry = cache.put(key, std::move(text), "text/html; charset=utf-8", std::move(etag));
  return rq.respond_page(std::move(entry), status);
}

net::Response cached_page(Rq& rq, int status, db::DependencyScope& deps, const std::function<void(Out&)>& render,
                          bool preload_link) {
  auto response = cached_page_checked(
      rq, status, deps,
      [&](Out& out) -> Flow<void> {
        render(out);
        return {};
      },
      preload_link);
  return std::move(*response);  // `render` cannot fail
}

}  // namespace campfire::app
