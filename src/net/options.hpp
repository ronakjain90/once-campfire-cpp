// Settings of the server. Rust: crates/kit/src/front/config.rs; core/config.hpp (FrontConfig).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/config.hpp"
#include "net/ctx.hpp"
#include "net/front/front.hpp"
#include "net/front/tls.hpp"
#include "net/parser.hpp"
#include "net/router.hpp"

namespace campfire::net {

struct ServerOptions {
  std::uint16_t http_port = 80;      // the front: HTTP_PORT (0: choose a free port)
  std::uint16_t https_port = 443;    // with TLS: HTTPS_PORT (0: choose a free port)
  std::uint16_t target_port = 3000;  // the app: TARGET_PORT (0: choose a free port)
  std::string target_bind = "127.0.0.1";
  bool listen_http = true;
  bool listen_target = true;
  bool listen_https = false;  // set with `tls`
  bool h2c = false;           // H2C_ENABLED: HTTP/2 with prior knowledge on the HTTP port
  // Timeouts in milliseconds. 0 means no timeout (as in Go).
  std::int64_t idle_timeout_ms = 60'000;
  std::int64_t read_timeout_ms = 30'000;
  std::int64_t write_timeout_ms = 30'000;
  // MAX_REQUEST_BODY: a larger body gets 413. 0 means no limit.
  std::uint64_t max_request_body = 0;
  // The most of a request body that the server keeps in memory. A larger body gets 413 (Rust:
  // body.rs MAX_BUFFERED_BODY, "the 16 MiB rules").
  std::uint64_t max_buffered_body = std::uint64_t{16} << 20;
  std::size_t workers = 0;  // 0: one for each CPU in the cpuset
  ParserLimits parser;
  // Add the headers of the front (vary, x-cache, date) to the responses on the HTTP_PORT.
  bool front_headers = true;
  // The front pipeline (cache, compression, X-Forwarded-*). `Server` makes one with the default
  // settings if `front_headers` is on and this is null. `from_config` makes one from the config.
  std::shared_ptr<front::Front> front;
  // TLS_DOMAIN: the certificates and the TLS contexts. With them, the HTTP port only redirects.
  std::shared_ptr<front::TlsServer> tls;
  // ActionDispatch::Static: answer from the asset table before the routes (A8, front/static_files.cpp).
  bool serve_static = false;
  // Runs on each response that the asset table gave (the app adds what its middleware adds to all
  // responses, e.g. HSTS). Null: nothing.
  void (*after_static)(Ctx&, Response&) = nullptr;

  // Called on the worker thread when something asked it to wake (`Server::wake`). The hub of Action Cable drains the
  // queue of the worker here. Null: nothing.
  std::function<void(unsigned worker)> on_wake;
  // Called on each worker thread every 3 seconds (the Action Cable heartbeat). Null: no timer.
  std::function<void(unsigned worker)> on_beat;

  [[nodiscard]] static ServerOptions from_config(const FrontConfig& config);
};

// What the server serves.
struct App {
  const RouteTable* routes = nullptr;
  HandlerFn not_found = nullptr;  // called when no route matches
  // Rack::MethodOverride: for a POST, the method that the request asks for ("PATCH"), or nothing. The worker
  // calls it before the router. Null: no override.
  std::optional<std::string_view> (*method_override)(const Request&) = nullptr;
};

// Applies `app.method_override` to a POST: the router and the action then see the new method.
void apply_method_override(const App& app, Request& request);

}  // namespace campfire::net
