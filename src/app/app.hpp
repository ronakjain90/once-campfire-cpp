// The app: one object built at boot and shared read only (design: plans/architecture.md section 3).
// It holds the config, the secrets, the database, the shared caches and the job pool. Per-worker
// state is in worker_state.hpp. Rails: Rails.application. Rust: crates/campfire/src/app.rs.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/fragment_cache.hpp"
#include "app/page_cache.hpp"
#include "app/proxy.hpp"
#include "app/rate_limit.hpp"
#include "app/session_cache.hpp"
#include "assets/assets.hpp"
#include "compat/secrets.hpp"
#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/error.hpp"
#include "db/database.hpp"
#include "net/thread_pool.hpp"

namespace campfire::app {

// Settings that only this layer reads.
struct AppOptions {
  std::size_t page_cache_bytes = std::size_t{32} << 20;  // CAMPFIRE_PAGE_CACHE_MB
  unsigned audit_every = default_audit_every();           // CAMPFIRE_PAGE_AUDIT_EVERY
  std::size_t job_threads = 2;                            // threads of the blocking pool (bcrypt)
  bool prepare_database = true;
};

struct App {
  Config config;
  compat::Secrets secrets;
  SharedClock clock;
  ProxyConfig proxy;
  std::unique_ptr<db::Database> db;
  std::shared_ptr<SharedFragmentCache> fragments;
  mutable PageCache pages;  // shared, synchronized inside
  std::shared_ptr<ChangeHub> changes;
  mutable RateLimiter rate_limits;  // `rate_limit`: the Rails cache counters, one process like one cache store
  mutable net::ThreadPool jobs;  // blocking work: bcrypt
  assets::StylesheetTags stylesheets;  // `stylesheet_link_tag :all`, fixed at build time
  std::string preload_link_header;     // the `link` header of a page in the application layout

  App(Config c, SharedClock k, std::size_t job_threads, PageCache::Options page_options);
  App(const App&) = delete;
  App& operator=(const App&) = delete;
  ~App();

  // Opens the database (and loads the schema into an empty one), starts the writer, and wires the change hub.
  [[nodiscard]] static Result<std::unique_ptr<App>> create(Config config, SharedClock clock,
                                                            const AppOptions& options = {});

  [[nodiscard]] Timestamp now() const { return clock->now(); }

 private:
  std::uint64_t subscription_ = 0;
};

// The process app. `set_app` is called once, before the server starts. A handler reads it with `app()`.
void set_app(const App* app) noexcept;
[[nodiscard]] const App& app();

// `bin/rails db:prepare`: loads the schema into an empty database, then adds the indexes that this app adds.
[[nodiscard]] Status prepare_database(const std::string& path, std::string_view environment, const Clock& clock);

}  // namespace campfire::app
