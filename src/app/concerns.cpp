// ApplicationController concerns. Rails: app/controllers/concerns/authentication.rb, block_banned_requests.rb,
// version_headers.rb. Rust: crates/campfire/src/concerns.rs.
#include "app/concerns.hpp"

#include <atomic>

#include "models/ban.hpp"
#include "models/hooks.hpp"
#include "models/session.hpp"
#include "models/user.hpp"
#include "req/bcrypt.hpp"
#include "req/forgery.hpp"
#include "routes/routes.hpp"

namespace campfire::app::concerns {

namespace {

std::atomic<AllowBrowserFn> g_allow_browser{&allow_browser};

std::unexpected<Failure> db_error(const Error& error) {
  return fail_internal(error.message);
}

// `Session.find_by(token:)` and its user, through the cache of the worker. A user that is gone means no session.
Flow<std::shared_ptr<const AuthEntry>> lookup_session(Rq& rq) {
  const auto raw = rq.cookies().get("session_token");
  if (!raw) return std::shared_ptr<const AuthEntry>{};
  SessionCache& cache = rq.worker.sessions();
  if (auto hit = cache.find(*raw)) return hit;
  const auto token = rq.cookies().signed_value("session_token");
  if (!token) return std::shared_ptr<const AuthEntry>{};
  auto session = models::sessions::find_by_token(rq.db(), rq.arena(), *token);
  if (!session) return db_error(session.error());
  if (!*session) return std::shared_ptr<const AuthEntry>{};
  auto user = models::users::find_by_id(rq.db(), rq.arena(), (*session)->user_id);
  if (!user) return db_error(user.error());
  if (!*user) return std::shared_ptr<const AuthEntry>{};
  auto entry = std::make_shared<const AuthEntry>(AuthEntry{std::move(**session), std::move(**user)});
  cache.put(std::string(*raw), entry);
  return entry;
}

std::optional<std::string_view> user_agent_of(Rq& rq) {
  return rq.request.has_header("user-agent") ? std::optional<std::string_view>(rq.request.header("user-agent"))
                                             : std::nullopt;
}

// `cookies.signed.permanent[:session_token] = { value: session.token, httponly: true, same_site: :lax }`
Flow<void> set_authentication_cookie(Rq& rq, const models::Session& session) {
  req::Cookie cookie;
  cookie.value = session.token;
  cookie.permanent = true;
  cookie.httponly = true;
  cookie.same_site = req::SameSite::Lax;
  if (auto saved = rq.cookies().set_signed("session_token", std::move(cookie)); !saved) {
    return fail_with(ErrorKind::CookieOverflow, saved.error().message);
  }
  return {};
}

Flow<void> request_authentication(Rq& rq) {
  rq.session().insert("return_to_after_authenticating", compat::json::Value(rq.info.url()));
  auto redirect = rq.redirect_to(rq.url_for(campfire::routes::new_session()));
  if (!redirect) return std::unexpected(std::move(redirect.error()));
  return halt(std::move(*redirect));
}

Flow<void> redirect_signed_in_user_to_root(Rq& rq) {
  if (!rq.signed_in()) return {};
  auto redirect = rq.redirect_to(rq.url_for(campfire::routes::root()));
  if (!redirect) return std::unexpected(std::move(redirect.error()));
  return halt(std::move(*redirect));
}

// `bot_authentication`: `params[:bot_key].present?` and a matching active bot.
Flow<bool> bot_authentication(Rq& rq) {
  const req::Param* param = rq.params().get("bot_key");
  if (param == nullptr || param->is_blank()) return false;
  const auto key = param->as_str();
  // `params[:bot_key].strip` raises NoMethodError for a hash or an array.
  if (!key) return fail_internal("undefined method 'strip' for bot_key");
  std::string_view text = *key;
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  auto bot = models::users::authenticate_bot(rq.db(), rq.arena(), text);
  if (!bot) return db_error(bot.error());
  if (!*bot) return false;
  rq.auth = std::make_shared<const AuthEntry>(AuthEntry{models::Session{}, std::move(**bot)});
  rq.authenticated_by = AuthenticatedBy::BotKey;
  return true;
}

}  // namespace

void set_allow_browser(AllowBrowserFn fn) noexcept {
  g_allow_browser.store(fn);
}

void set_version_headers(Rq& rq) {
  rq.set_header("x-version", rq.app.config.app_version);
  if (rq.app.config.git_revision) rq.set_header("x-rev", *rq.app.config.git_revision);
}

net::Response head_in_before_action(Rq& rq, int status) {
  net::Response response = rq.ctx.response(status);
  if (!((status >= 100 && status < 200) || status == 204 || status == 205 || status == 304)) {
    response.add("content-type", "text/html");
  }
  return response;
}

Task<Flow<void>> reject_banned_ip(Rq& rq) {
  if (rq.is_get() || rq.is_head()) co_return Flow<void>{};
  const auto ip = rq.info.remote_ip();
  if (!ip) co_return fail_with(ErrorKind::IpSpoofAttack, "IP spoofing attack");
  auto banned = models::bans::banned(rq.db(), rq.arena(), *ip);
  if (!banned) co_return db_error(banned.error());
  if (*banned) co_return halt(head_in_before_action(rq, 429));
  co_return Flow<void>{};
}

Task<Flow<bool>> restore_authentication(Rq& rq) {
  auto found = lookup_session(rq);
  if (!found) co_return std::unexpected(std::move(found.error()));
  std::shared_ptr<const AuthEntry> entry = std::move(*found);
  if (!entry) co_return false;
  // Only a session due for its refresh goes to the writer (Rust: resume_session). The cookie is signed
  // again on the same schedule, which keeps its 20 year expiry rolling.
  const bool refresh = entry->session.needs_resume(rq.now());
  if (refresh) {
    const auto ip = rq.info.remote_ip();
    if (!ip) co_return fail_with(ErrorKind::IpSpoofAttack, "IP spoofing attack");
    const auto agent = user_agent_of(rq);
    const std::string_view ip_view = *ip;
    models::Session before = entry->session;
    auto wrote = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Result<models::Session> {
      models::Session s = before;
      if (auto resumed = models::sessions::resume(tx, s, agent, ip_view); !resumed)
        return std::unexpected(resumed.error());
      return s;
    });
    if (!wrote) co_return db_error(wrote.error());
    const auto raw = rq.cookies().get("session_token");
    entry = std::make_shared<const AuthEntry>(AuthEntry{std::move(*wrote), entry->user});
    // The writer posted the change to this worker: `sessions()` applies it, then the new row goes in.
    SessionCache& cache = rq.worker.sessions();
    if (raw) cache.put(std::string(*raw), entry);
    if (auto cookie = set_authentication_cookie(rq, entry->session); !cookie) {
      co_return std::unexpected(std::move(cookie.error()));
    }
  }
  rq.auth = std::move(entry);
  rq.authenticated_by = AuthenticatedBy::Session;
  co_return true;
}

Task<Flow<void>> require_authentication(Rq& rq) {
  auto restored = co_await restore_authentication(rq);
  if (!restored) co_return std::unexpected(std::move(restored.error()));
  if (*restored) co_return Flow<void>{};
  auto bot = bot_authentication(rq);
  if (!bot) co_return std::unexpected(std::move(bot.error()));
  if (*bot) co_return Flow<void>{};
  co_return request_authentication(rq);
}

Flow<void> deny_bots(Rq& rq) {
  if (rq.authenticated_by == AuthenticatedBy::BotKey) return halt(head_in_before_action(rq, 403));
  return {};
}

Flow<void> ensure_can_administer(Rq& rq) {
  const models::User* user = rq.current_user();
  if (user == nullptr || !user->can_administer()) return halt(head_in_before_action(rq, 403));
  return {};
}

Flow<void> verify_authenticity_token(Rq& rq) {
  rq.marked_for_same_origin_verification = rq.is_get();
  const std::string base_url = rq.info.base_url();
  req::ForgeryInput input;
  input.method = rq.request.method_text;
  if (rq.request.has_header("origin")) input.origin = rq.request.header("origin");
  if (rq.request.has_header("sec-fetch-site")) input.sec_fetch_site = rq.request.header("sec-fetch-site");
  input.base_url = base_url;
  input.ssl = rq.info.ssl();
  input.force_ssl = rq.app.proxy.force_ssl;
  input.exempt = rq.authenticated_by == AuthenticatedBy::BotKey;
  if (auto invalid = req::verify_authenticity_token(input)) {
    return fail_with(ErrorKind::InvalidAuthenticityToken, invalid->message);
  }
  return {};
}

Task<Flow<void>> before_actions(Rq& rq, Before before) {
  set_version_headers(rq);
  // set_current_request: `default_url_options` carry the host and protocol of the request; url_for does that.
  if (auto banned = co_await reject_banned_ip(rq); !banned) co_return std::unexpected(std::move(banned.error()));
  if (before.authentication == Authentication::Required) {
    if (auto authed = co_await require_authentication(rq); !authed)
      co_return std::unexpected(std::move(authed.error()));
  }
  if (before.deny_bots) {
    if (auto denied = deny_bots(rq); !denied) co_return std::unexpected(std::move(denied.error()));
  }
  if (before.forgery_protection && rq.authenticated_by != AuthenticatedBy::BotKey) {
    if (auto verified = verify_authenticity_token(rq); !verified)
      co_return std::unexpected(std::move(verified.error()));
  }
  if (const AllowBrowserFn allow = g_allow_browser.load()) {
    if (auto allowed = co_await allow(rq); !allowed) co_return std::unexpected(std::move(allowed.error()));
  }
  if (before.authentication == Authentication::RequireUnauthenticated) {
    auto restored = co_await restore_authentication(rq);
    if (!restored) co_return std::unexpected(std::move(restored.error()));
    if (auto redirected = redirect_signed_in_user_to_root(rq); !redirected) {
      co_return std::unexpected(std::move(redirected.error()));
    }
  }
  co_return Flow<void>{};
}

Task<Flow<std::optional<models::User>>> authenticate_by(Rq& rq, std::string email_address, std::string password) {
  // `authenticate_by` returns nil for a blank password before it looks anything up.
  if (password.empty()) co_return std::optional<models::User>{};
  auto candidate = models::users::find_active_by_email_address(rq.db(), rq.arena(), email_address);
  if (!candidate) co_return db_error(candidate.error());
  auto user = co_await rq.ctx.offload(rq.app.jobs,
                                      [candidate = std::move(*candidate), password = std::move(password)]() mutable {
                                        return models::users::authenticated(std::move(candidate), password);
                                      });
  co_return user;
}

Task<Flow<std::optional<std::string>>> password_digest(Rq& rq, std::optional<std::string> password) {
  if (!password || password->empty()) co_return std::optional<std::string>{};
  // The test environment of Rails uses the lowest cost (`ActiveModel::SecurePassword.min_cost`).
  const int cost = rq.app.config.environment == "test" ? req::bcrypt::kMinCost : req::bcrypt::kDefaultCost;
  std::string digest = co_await rq.ctx.offload(
      rq.app.jobs, [password = std::move(*password), cost] { return req::bcrypt::hash_password(password, cost); });
  if (digest.empty()) co_return fail_internal("bcrypt failed");
  co_return std::optional<std::string>(std::move(digest));
}

Task<Flow<void>> start_new_session_for(Rq& rq, models::User user) {
  const auto ip = rq.info.remote_ip();
  if (!ip) co_return fail_with(ErrorKind::IpSpoofAttack, "IP spoofing attack");
  const auto agent = user_agent_of(rq);
  const std::string_view ip_view = *ip;
  const std::int64_t user_id = user.id;
  auto started = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Result<models::Session> {
    return models::sessions::start(tx, user_id, agent, ip_view);
  });
  if (!started) co_return db_error(started.error());
  if (auto cookie = set_authentication_cookie(rq, *started); !cookie)
    co_return std::unexpected(std::move(cookie.error()));
  rq.auth = std::make_shared<const AuthEntry>(AuthEntry{std::move(*started), std::move(user)});
  rq.authenticated_by = AuthenticatedBy::Session;
  co_return Flow<void>{};
}

Task<Flow<void>> terminate_current_session(Rq& rq) {
  const models::User* signed_in = rq.current_user();
  const std::int64_t user_id = signed_in != nullptr ? signed_in->id : 0;
  if (const models::Session* current = rq.current_session(); current != nullptr) {
    const models::Session session = *current;
    auto destroyed = co_await rq.app.db->write(
        rq.ctx.scheduler(), [&](db::Tx& tx) -> Status { return models::sessions::destroy(tx, session); });
    if (!destroyed) co_return db_error(destroyed.error());
  }
  rq.reset_session();
  rq.cookies().remove("session_token");
  // Rails also closes the user's Action Cable sockets (`reset_remote_connections`).
  if (user_id != 0) models::hooks::disconnect_user(user_id, true);
  co_return Flow<void>{};
}

std::string post_authenticating_url(Rq& rq) {
  std::string stored;
  bool is_string = false;
  if (const auto* value = rq.session().get("return_to_after_authenticating")) {
    if (value->is_string()) {
      stored = value->as_string();
      is_string = true;
    } else {
      stored = compat::json::generate(*value);
      is_string = true;
    }
  }
  rq.session().remove("return_to_after_authenticating");
  return is_string ? stored : rq.url_for(campfire::routes::root());
}

}  // namespace campfire::app::concerns
