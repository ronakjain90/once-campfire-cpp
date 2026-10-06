// The request context of an action. Rails: ActionController::Base (request, params, cookies, session, flash,
// response). Rust: crates/kit/src/ctx.rs (Ctx). One Rq lives in the frame of the handler coroutine.
//
//   Task<net::Response> show(net::Ctx& c) { return app::dispatch(c, &action); }
//   Task<Flow<>> action(Rq& rq) { ...; co_return rq.html(200, std::move(out)); }
//
// A before action or an action returns `Flow<T>`: a value, or a `Failure` (a halt with a response, or an error).
// See src/app/README.md for the full list of calls.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.hpp"
#include "app/flow.hpp"
#include "app/page_cache.hpp"
#include "app/proxy.hpp"
#include "app/worker_state.hpp"
#include "compat/json.hpp"
#include "core/out.hpp"
#include "db/scope.hpp"
#include "net/ctx.hpp"
#include "req/cookie.hpp"
#include "req/format.hpp"
#include "req/param.hpp"
#include "req/session.hpp"

namespace campfire::app {

// `authenticated_by`
enum class AuthenticatedBy : std::uint8_t { Nothing, Session, BotKey };

// `response.cache_control` (ActionDispatch::Http::Cache). Rust: crates/kit/src/response.rs.
struct CacheControl {
  std::optional<std::uint64_t> max_age;
  bool is_public = false;
  bool is_private = false;
  bool must_revalidate = false;
  bool no_cache = false;
  bool no_store = false;
  std::optional<std::uint64_t> stale_while_revalidate;
  bool immutable = false;
  [[nodiscard]] bool empty() const noexcept;
  // The header value after `merge_and_normalize_cache_control!`.
  [[nodiscard]] std::string to_header() const;
};

// Validators for `fresh_when` and `stale?`.
struct Freshness {
  std::optional<std::string> etag;  // the weak validator, already a cache key
  std::optional<std::string> strong_etag;
  std::optional<Timestamp> last_modified;
  bool is_public = false;
  std::optional<std::string> template_digest;
};

struct RedirectOptions {
  int status = 302;
  std::optional<std::string> notice;
  std::optional<std::string> alert;
  bool allow_other_host = false;
};

class Rq {
 public:
  explicit Rq(net::Ctx& ctx);
  Rq(const Rq&) = delete;
  Rq& operator=(const Rq&) = delete;
  ~Rq();

  net::Ctx& ctx;
  const App& app;
  WorkerState& worker;
  const net::Request& request;
  RequestInfo info;

  [[nodiscard]] Arena& arena() noexcept { return ctx.arena(); }
  [[nodiscard]] db::Connection& db() noexcept { return worker.reader(); }
  [[nodiscard]] Timestamp now() const { return app.now(); }
  [[nodiscard]] bool is_get() const noexcept { return request.method == net::Method::Get; }
  [[nodiscard]] bool is_head() const noexcept { return request.method == net::Method::Head; }
  [[nodiscard]] std::string_view user_agent() const noexcept { return request.header("user-agent"); }

  // Parses the body, the query and the path parameters, and builds the cookie jar. The dispatcher calls it
  // before the action. A failure is the error that Rails would raise.
  [[nodiscard]] Flow<void> init();

  // `params`: body params, then query params, then path params, merged as Rails does.
  [[nodiscard]] const req::ParamMap& params() const noexcept { return *params_; }
  [[nodiscard]] std::optional<std::string_view> param_str(std::string_view key) const { return params_->str(key); }
  // The wire method after nothing overrides it (`_method` override is not wired: see README).
  [[nodiscard]] std::string_view method_name() const noexcept { return request.method_text; }

  // Cookies, session and flash.
  [[nodiscard]] req::CookieJar& cookies() { return *cookies_; }
  [[nodiscard]] req::Session& session();
  [[nodiscard]] req::Flash& flash();
  void reset_session();

  // Formats. `request.formats`; an invalid Accept header gives UnknownFormat (406).
  [[nodiscard]] Flow<std::span<const req::Format>> formats();
  // `respond_to`: the first acceptable format among `offered`, or UnknownFormat.
  [[nodiscard]] Flow<req::Format> respond_to(std::span<const req::Format> offered);
  [[nodiscard]] req::Format rendered_format();
  [[nodiscard]] bool is_turbo_frame_request() const;

  // Current attributes (`Current.user`, `Current.session`).
  std::shared_ptr<const AuthEntry> auth;
  AuthenticatedBy authenticated_by = AuthenticatedBy::Nothing;
  [[nodiscard]] const models::User* current_user() const noexcept { return auth ? &auth->user : nullptr; }
  [[nodiscard]] const models::Session* current_session() const noexcept { return auth ? &auth->session : nullptr; }
  [[nodiscard]] bool signed_in() const noexcept { return auth != nullptr; }

  // A header for the response, set before the response exists (`response.headers[...] =`). It does not
  // replace a header that the action sets.
  void set_header(std::string_view name, std::string_view value);
  [[nodiscard]] std::string_view staged_header(std::string_view name) const noexcept;
  CacheControl cache_control;
  // The page dependency scope of this request (architecture 6.1). Statements of the read connection fold into it.
  [[nodiscard]] db::DependencyScope& track();

  // Builders. A body is an `Out` in the arena.
  [[nodiscard]] net::Response render_as(int status, std::string_view content_type, Out&& body);
  [[nodiscard]] net::Response html(int status, Out&& body);  // `text/html; charset=utf-8`
  [[nodiscard]] net::Response turbo_stream(Out&& body);      // `text/vnd.turbo-stream.html; charset=utf-8`
  [[nodiscard]] net::Response json(int status, const compat::json::Value& value);  // ActiveSupport::JSON
  [[nodiscard]] net::Response head(int status);                                    // `head status`
  [[nodiscard]] Flow<net::Response> redirect_to(std::string_view location, RedirectOptions options = {});
  // `send_data` and `send_file`. The file is read into memory: streaming is a later task (A6).
  [[nodiscard]] net::Response send_data(std::string_view bytes, std::string_view content_type,
                                        std::optional<std::string_view> disposition = "attachment",
                                        std::optional<std::string_view> filename = std::nullopt, int status = 200);
  [[nodiscard]] Flow<net::Response> send_file(const std::string& path, std::string_view content_type,
                                              std::optional<std::string_view> disposition = "attachment",
                                              std::optional<std::string_view> filename = std::nullopt);
  // A page from the page cache: the identity body, the gzip body and the ETag come from the entry. The
  // response has the headers of a render (`content-type`, `vary`), and no ETag: `finish` adds it in place.
  [[nodiscard]] net::Response respond_page(std::shared_ptr<const PageEntry> entry, int status = 200);
  // `url_for`: an absolute URL on the host of the request.
  [[nodiscard]] std::string url_for(std::string_view path) const { return info.base_url() + std::string(path); }
  // `fresh_when`: sets the validators. Returns a 304 response if the request is fresh.
  [[nodiscard]] std::optional<net::Response> fresh_when(const Freshness& freshness);

  // Turns the result of the action into the response that Rails sends: halts and errors, cookies, cache
  // headers, ETag, conditional GET, gzip and the tail headers. The dispatcher calls it.
  [[nodiscard]] net::Response finish(Flow<net::Response> result);
  // Marks a GET for the cross-origin JavaScript check (verify_same_origin_request).
  bool marked_for_same_origin_verification = false;

 private:
  friend class Finisher;
  [[nodiscard]] std::string_view copy(std::string_view text) { return arena().copy(text); }
  [[nodiscard]] req::NegotiationInput negotiation_input() const;
  [[nodiscard]] bool is_fresh(std::string_view etag, std::string_view last_modified) const;

  std::optional<req::ParamMap> params_storage_;
  const req::ParamMap* params_ = nullptr;
  std::optional<req::CookieJar> cookies_;
  req::Session session_;
  std::optional<req::Flash> flash_;
  std::vector<net::Header> staged_;
  std::optional<std::expected<std::vector<req::Format>, req::InvalidMimeType>> formats_;
  req::Format rendered_format_ = nullptr;
  std::optional<db::DependencyScope> scope_;
  std::string raw_post_;
  std::shared_ptr<const PageEntry> page_entry_;
  bool head_by_wire_ = false;
};

}  // namespace campfire::app
