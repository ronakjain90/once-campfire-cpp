// Process entry. Rails: bin/boot (Thruster + Puma); Rust: crates/campfire/src/main.rs.
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "app/app.hpp"
#include "app/not_found.hpp"
#include "app/rails.hpp"
#include "app/routes.hpp"
#include "core/config.hpp"
#include "core/log.hpp"
#include "net/server.hpp"

int main(int argc, char** argv) {
  using namespace campfire;
  const std::string_view command = argc > 1 ? argv[1] : "server";
  if (command != "server") {
    std::fprintf(stderr, "campfire: command '%s' is not available yet\n", argv[1]);
    return 2;
  }
  // The signals go to sigwait in this thread. The mask comes before the threads start.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  if (const char* level = std::getenv("RAILS_LOG_LEVEL")) Logger::instance().set_level_from(level);
  const FrontConfig front = FrontConfig::from_env();
  auto config = Config::from_env();
  if (!config) {
    log_error("configuration: {}", config.error().message);
    return 1;
  }
  auto clock = clock_from_env();
  if (!clock) {
    log_error("clock: {}", clock.error().message);
    return 1;
  }
  app::AppOptions options;
  options.job_threads = std::max<std::size_t>(config->job_concurrency, 2);
  if (const char* mb = std::getenv("CAMPFIRE_PAGE_CACHE_MB")) {
    options.page_cache_bytes = static_cast<std::size_t>(std::strtoull(mb, nullptr, 10)) << 20;
  }
  if (const char* every = std::getenv("CAMPFIRE_PAGE_AUDIT_EVERY")) {
    options.audit_every = static_cast<unsigned>(std::strtoul(every, nullptr, 10));
  }
  auto state = app::App::create(std::move(*config), *clock, options);
  if (!state) {
    log_error("cannot start the app: {}", state.error().message);
    return 1;
  }
  app::set_app(state->get());
  net::App routes_app{&app::routes(), &app::not_found};
  net::ServerOptions server_options = net::ServerOptions::from_config(front);
  server_options.after_static = [](net::Ctx& ctx, net::Response& response) { app::add_hsts(ctx.request(), response); };
  net::Server server(server_options, routes_app);
  if (auto started = server.start(); !started) {
    log_error("cannot start the server: {}", started.error().message);
    return 1;
  }
  if (server.https_port() != 0) {
    log_info("Server started http=:{} https=:{} target={}:{} workers={}", server.http_port(), server.https_port(),
             front.target_bind, server.target_port(), server.worker_count());
  } else {
    log_info("Server started http=:{} target={}:{} workers={}", server.http_port(), front.target_bind,
             server.target_port(), server.worker_count());
  }
  int signal_number = 0;
  sigwait(&signals, &signal_number);
  log_info("Stopping the server");
  server.stop();
  log_info("Server stopped");
  return 0;
}
