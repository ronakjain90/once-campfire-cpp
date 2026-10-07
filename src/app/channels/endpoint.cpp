// `GET /cable`: the WebSocket upgrade, the session of a socket and `ApplicationCable::Connection`.
// Rails: ActionCable::Server::Base#call, reference/app/channels/application_cable/connection.rb,
// Authentication::SessionLookup. Rust: crates/cable/src/server.rs (`call`), crates/campfire/src/channels/connection.rs.
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "app/channels/server.hpp"
#include "app/dispatch.hpp"
#include "app/rails.hpp"
#include "app/session_cache.hpp"
#include "app/worker_state.hpp"
#include "cable/connection.hpp"
#include "cable/websocket.hpp"
#include "core/log.hpp"
#include "models/session.hpp"
#include "models/user.hpp"
#include "net/ctx.hpp"
#include "net/ws.hpp"
#include "req/cookie.hpp"

namespace campfire::app::channels {

namespace {

// `find_session_by_cookie` and its user: `cookies.signed[:session_token]`, `Session.find_by(token:)`. With `cached`,
// the cache of the worker answers first, as for a request. Without it, the database does (the re-check of a socket must
// see a ban that the cache did not hear yet).
std::shared_ptr<const CableUser> find_user(const App& app, const std::vector<std::string>& cookie_headers,
                                           bool cached) {
  std::vector<std::string_view> headers(cookie_headers.begin(), cookie_headers.end());
  const req::CookieJar cookies(headers, app.secrets, *app.clock);
  const auto raw = cookies.get("session_token");
  if (!raw) return nullptr;
  WorkerState& state = worker_state();
  if (cached) {
    if (auto hit = state.sessions().find(*raw))
      return std::make_shared<const CableUser>(CableUser{hit->user.id, hit->user.name});
  }
  const auto token = cookies.signed_value("session_token");
  if (!token) return nullptr;
  Arena arena(1024);
  auto session = models::sessions::find_by_token(state.reader(), arena, *token);
  if (!session) {
    log_error("Could not look up the cable session: {}", session.error().message);
    return nullptr;
  }
  if (!*session) return nullptr;
  auto user = models::users::find_by_id(state.reader(), arena, (*session)->user_id);
  if (!user) {
    log_error("Could not look up the cable session: {}", user.error().message);
    return nullptr;
  }
  if (!*user) return nullptr;
  return std::make_shared<const CableUser>(CableUser{(*user)->id, (*user)->name});
}

// One socket: the Action Cable connection on top of the transport of the worker.
class CableSession final : public net::WsSession, private cable::Transport {
 public:
  CableSession(net::WsTransport& transport, CableServer& server, unsigned worker, bool deflate)
      : transport_(transport), server_(server), connection_(server.hub(), worker, *this, server.registry(), deflate) {}

  // `ApplicationCable::Connection#connect`, then `handle_open`.
  void open(std::vector<std::string> cookie_headers) {
    const App& app = server_.app();
    auto user = find_user(app, cookie_headers, true);
    if (!user) {
      log_error("An unauthorized connection attempt was rejected");
      connection_.reject_unauthorized();
      return;
    }
    const std::string identifier = connection_identifier(user->id);
    // A ban or a sign-out that disconnected this user between the check above and the subscription to the internal
    // channel went unheard: check again now that it would be heard.
    connection_.open(user, identifier,
                     [&app, cookies = std::move(cookie_headers)] { return find_user(app, cookies, false) != nullptr; });
  }

  void on_data(std::string_view bytes) override { connection_.on_data(bytes); }
  void on_write_stall() override { connection_.on_write_stall(); }
  void on_lagged() override { connection_.on_lagged(); }
  void on_closed() override { connection_.on_closed(); }

 private:
  // cable::Transport
  void send(std::span<const cable::ws::Bytes> buffers) override { transport_.send(buffers); }
  void close(std::chrono::milliseconds grace) override { transport_.close(grace); }

  net::WsTransport& transport_;
  CableServer& server_;
  cable::Connection connection_;
};

std::vector<std::string_view> header_values(const net::Request& request, std::string_view lower_name) {
  std::vector<std::string_view> values;
  for (const net::Header& h : request.headers) {
    if (net::iequals(h.name, lower_name)) values.push_back(h.value);
  }
  return values;
}

// `Rack::Request#ssl?` for a request that did not arrive over TLS itself.
bool forwarded_ssl(const net::Request& request) {
  const auto first = [&](std::string_view name) {
    std::string_view value = request.header(name);
    value = value.substr(0, value.find(','));
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
    std::string lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    return lower;
  };
  return first("x-forwarded-ssl") == "on" || first("x-forwarded-scheme") == "https" ||
         first("x-forwarded-proto") == "https";
}

// `allow_request_origin?`: the same origin as the host, or one of the allowed origins.
bool allow_request_origin(const CableConfig& config, const net::Request& request) {
  if (config.disable_request_forgery_protection) return true;
  const std::string_view origin = request.header("origin");
  const bool has_origin = request.has_header("origin");
  const std::string proto = config.assume_ssl || forwarded_ssl(request) ? "https" : "http";
  const std::string same_origin = proto + "://" + std::string(request.header("host"));
  if (config.allow_same_origin_as_host && has_origin && origin == same_origin) return true;
  if (has_origin && std::find(config.allowed_request_origins.begin(), config.allowed_request_origins.end(),
                              std::string(origin)) != config.allowed_request_origins.end()) {
    return true;
  }
  log_error("Request origin not allowed: {}", has_origin ? origin : std::string_view("(none)"));
  return false;
}

// `Connection::Base#respond_to_invalid_request` for anything that is not an upgrade from an allowed origin.
net::Response page_not_found(net::Ctx& ctx) {
  net::Response response = ctx.response(404);
  response.add("content-type", "text/plain; charset=utf-8");
  response.add("content-length", "14");
  response.body_view("Page not found");
  response.add("vary", "Accept-Encoding");
  add_rails_tail(ctx, response);
  return response;
}

}  // namespace

Task<net::Response> cable_show(net::Ctx& ctx) {
  CableServer* server = cable_server();
  const net::Request& request = ctx.request();
  if (server == nullptr) co_return page_not_found(ctx);
  const auto connection = header_values(request, "connection");
  const auto extensions = header_values(request, "sec-websocket-extensions");
  const auto protocols = header_values(request, "sec-websocket-protocol");
  if (!cable::ws::is_upgrade_request(request.method_text, connection, request.header("upgrade")) ||
      !allow_request_origin(server->config(), request)) {
    co_return page_not_found(ctx);
  }
  cable::ws::HandshakeRequest handshake;
  handshake.version = request.header("sec-websocket-version");
  handshake.key = request.header("sec-websocket-key");
  handshake.extensions = extensions;
  handshake.protocols = protocols;
  const auto accepted = cable::ws::accept_handshake(handshake);
  if (!accepted) co_return page_not_found(ctx);

  net::Response response = ctx.response(101);
  response.add("upgrade", "websocket");
  response.add("connection", "upgrade");
  response.add_copy("sec-websocket-accept", accepted->accept);
  if (accepted->deflate) response.add("sec-websocket-extensions", cable::ws::kDeflateResponse);
  if (!accepted->protocol.empty()) response.add_copy("sec-websocket-protocol", accepted->protocol);
  add_rails_tail(ctx, response);

  // The request is gone when the worker calls this: keep what the connection needs.
  std::vector<std::string> cookies;
  for (const std::string_view value : header_values(request, "cookie")) cookies.emplace_back(value);
  response.ws_accept = [server, cookies = std::move(cookies), deflate = accepted->deflate](
                           net::WsTransport& transport, unsigned worker) mutable -> std::unique_ptr<net::WsSession> {
    WorkerContext& context = worker_context();
    context.scheduler = &transport.scheduler();
    context.index = worker;
    auto session = std::make_unique<CableSession>(transport, *server, worker, deflate);
    session->open(std::move(cookies));
    return session;
  };
  co_return response;
}

}  // namespace campfire::app::channels

namespace campfire::routes::cable_endpoint {

Task<net::Response> show(net::Ctx& c) {
  return app::channels::cable_show(c);
}

}  // namespace campfire::routes::cable_endpoint
