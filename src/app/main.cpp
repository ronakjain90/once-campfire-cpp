// Process entry. Rails: bin/boot (Thruster + Puma); Rust: crates/campfire/src/main.rs.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "app/not_found.hpp"
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
  net::App app{&app::routes(), &app::not_found};
  net::Server server(net::ServerOptions::from_config(front), app);
  if (auto started = server.start(); !started) {
    log_error("cannot start the server: {}", started.error().message);
    return 1;
  }
  log_info("Server started http=:{} target={}:{} workers={}", server.http_port(), front.target_bind,
           server.target_port(), server.worker_count());
  int signal_number = 0;
  sigwait(&signals, &signal_number);
  log_info("Stopping the server");
  server.stop();
  log_info("Server stopped");
  return 0;
}
