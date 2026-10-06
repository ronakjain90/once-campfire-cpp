// Settings of the server. Rust: crates/kit/src/front/config.rs; core/config.hpp (FrontConfig).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/config.hpp"
#include "net/ctx.hpp"
#include "net/parser.hpp"
#include "net/router.hpp"

namespace campfire::net {

struct ServerOptions {
  std::uint16_t http_port = 80;     // the front: HTTP_PORT (0: choose a free port)
  std::uint16_t target_port = 3000; // the app: TARGET_PORT (0: choose a free port)
  std::string target_bind = "127.0.0.1";
  bool listen_http = true;
  bool listen_target = true;
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

  [[nodiscard]] static ServerOptions from_config(const FrontConfig& config);
};

// What the server serves.
struct App {
  const RouteTable* routes = nullptr;
  HandlerFn not_found = nullptr;  // called when no route matches
};

}  // namespace campfire::net
