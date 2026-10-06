// SessionsController. Rails: app/controllers/sessions_controller.rb. Rust: crates/campfire/src/controllers/sessions.rs.
#include "app/controllers/sessions.hpp"

#include "app/concerns.hpp"
#include "app/page.hpp"
#include "models/user.hpp"
#include "routes/routes.hpp"
#include "views/helpers/forms.hpp"
#include "views/helpers/turbo.hpp"
#include "views/sessions/helpers.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `rate_limit to: 10, within: 3.minutes, only: :create`
constexpr std::uint64_t kRateLimitTo = 10;
constexpr std::int64_t kRateLimitWithinSeconds = 180;
constexpr std::string_view kRejection = "Too many requests or unauthorized.";

const db::Query<void(std::string_view, std::int64_t)> kDestroySubscription{
    "DELETE FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"endpoint\" = ? AND "
    "\"push_subscriptions\".\"user_id\" = ?"};

// `render :new, status:`
Flow<net::Response> render_new(Rq& rq, int status) {
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) return std::unexpected(std::move(format.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) return std::unexpected(std::move(layout.error()));
  auto owner = models::users::first_administrator(rq.db(), rq.arena());
  if (!owner) return fail_internal(owner.error().message);
  const std::optional<std::string_view> email = rq.param_str("email_address");
  // Everything that is not SQL and that the page prints.
  deps.facet("page", "sessions#new");
  deps.facet("base_url", rq.info.base_url());
  deps.facet("frame", static_cast<std::uint64_t>(rq.is_turbo_frame_request()));
  deps.facet("email", email.value_or(""));
  deps.facet("email_given", static_cast<std::uint64_t>(email.has_value()));
  const auto notice = rq.flash().notice();
  const auto alert = rq.flash().alert();
  deps.facet("notice", notice.value_or(""));
  deps.facet("alert", alert.value_or(""));
  deps.facet("alert_given", static_cast<std::uint64_t>(alert.has_value()));
  std::optional<views::HelpContact> help;
  if (*owner) help = views::HelpContact{(*owner)->name, (*owner)->email_address};
  const views::helpers::FormWith form =
      views::helpers::form_with_url(rq.url_for(campfire::routes::session())).cls("flex flex-column gap");
  const LayoutData& data = *layout;
  const auto render = [&](Out& out) {
    views::LayoutParts parts;
    parts.page_title = "Sign in";
    // The head part is `turbo_page_requires_reload`; the content is the template.
    parts.head = [](Out& o) { views::helpers::turbo_page_requires_reload_tag(o); };
    const views::ViewContext ctx = make_view_context(rq, data);
    parts.content = [&](Out& o) { views::sessions::new_(o, ctx, form, email, help); };
    if (rq.is_turbo_frame_request()) {
      views::layouts::turbo_rails::frame(out, parts);
    } else {
      views::layouts::application(out, ctx, parts);
    }
  };
  return cached_page(rq, status, deps, render);
}

// `flash.now[:alert] = "Too many requests or unauthorized."; render :new, status:`
Flow<net::Response> render_rejection(Rq& rq, int status) {
  rq.flash().now("alert", compat::json::Value(std::string(kRejection)));
  return render_new(rq, status);
}

}  // namespace

// `allow_unauthenticated_access only: %i[ new create ]`, `before_action :ensure_user_exists, only: :new`
Task<Flow<net::Response>> sessions_new(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.allow_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  // `redirect_to first_run_url if User.none?`
  auto none = models::users::none(rq.db(), rq.arena());
  if (!none) co_return fail_internal(none.error().message);
  if (*none) {
    auto redirect = rq.redirect_to(rq.url_for(campfire::routes::first_run()));
    if (!redirect) co_return std::unexpected(std::move(redirect.error()));
    co_return std::move(*redirect);
  }
  co_return render_new(rq, 200);
}

Task<Flow<net::Response>> sessions_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.allow_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  // `rate_limit`: "rate-limit:sessions:#{request.remote_ip}"
  const auto ip = rq.info.remote_ip();
  if (!ip) co_return fail_with(ErrorKind::IpSpoofAttack, "IP spoofing attack");
  if (rq.app.rate_limits.increment("rate-limit:sessions:" + *ip, rq.now(), kRateLimitWithinSeconds) > kRateLimitTo) {
    co_return render_rejection(rq, 429);
  }
  const auto email = rq.param_str("email_address");
  const auto password = rq.param_str("password");
  std::optional<models::User> user;
  if (email && password) {
    auto found = co_await concerns::authenticate_by(rq, std::string(*email), std::string(*password));
    if (!found) co_return std::unexpected(std::move(found.error()));
    user = std::move(*found);
  }
  if (!user) co_return render_rejection(rq, 401);
  auto started = co_await concerns::start_new_session_for(rq, std::move(*user));
  if (!started) co_return std::unexpected(std::move(started.error()));
  co_return rq.redirect_to(concerns::post_authenticating_url(rq));
}

Task<Flow<net::Response>> sessions_destroy(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  // `Push::Subscription.destroy_by(endpoint: params[:push_subscription_endpoint], user_id: Current.user.id)`
  if (const auto endpoint = rq.param_str("push_subscription_endpoint"); endpoint && rq.current_user() != nullptr) {
    const std::string text(*endpoint);
    const std::int64_t user_id = rq.current_user()->id;
    auto removed = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
      auto done = tx.conn().exec(kDestroySubscription, text, user_id);
      if (!done) return std::unexpected(done.error());
      return {};
    });
    if (!removed) co_return fail_internal(removed.error().message);
  }
  auto ended = co_await concerns::terminate_current_session(rq);
  if (!ended) co_return std::unexpected(std::move(ended.error()));
  co_return rq.redirect_to(rq.url_for(campfire::routes::root()));
}

}  // namespace campfire::app::controllers

// The route handlers (src/app/routes/sessions.inc).
namespace campfire::routes::sessions {

Task<net::Response> new_(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::sessions_new);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::sessions_create);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::sessions_destroy);
}

}  // namespace campfire::routes::sessions
