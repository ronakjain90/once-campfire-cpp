// The app object and the boot of the database. Rust: crates/campfire/src/app.rs, crates/db/src/schema.rs (prepare).
#include "app/app.hpp"

#include <atomic>
#include <cstdlib>

#include "app/data.hpp"
#include "models/job_sink.hpp"
#include "storage/storage.hpp"
#include "core/log.hpp"
#include "core/time_format.hpp"

namespace campfire::app {

namespace {

std::atomic<const App*> g_app{nullptr};

// Every migration in reference/db/migrate, oldest first.
constexpr std::string_view kMigrationVersions[] = {
    "20231215043540", "20231220143106", "20240110071740", "20240115124901", "20240130003150",
    "20240130213001", "20240131105830", "20240209110503", "20250825100957", "20250825100958",
    "20250825100959", "20251126092013", "20251126115722", "20251126130131", "20251212154340"};
// SHA1 of reference/db/schema.rb, which `db:schema:load` records in `ar_internal_metadata`.
constexpr std::string_view kSchemaSha1 = "f75da8dad38bfb179ffd757bd7a7c2b3f818bc29";

// An index that this app adds to the Rails schema: a room's messages are paged by `created_at`.
constexpr std::string_view kAdditions =
    "CREATE INDEX IF NOT EXISTS \"index_messages_on_room_id_and_created_at\" ON \"messages\" (\"room_id\", "
    "\"created_at\");";

}  // namespace

App::App(Config c, SharedClock k, std::size_t job_threads, PageCache::Options page_options)
    : config(std::move(c)),
      secrets(config.secret_key_base),
      clock(std::move(k)),
      proxy(ProxyConfig::production(config.disable_ssl)),
      fragments(std::make_shared<SharedFragmentCache>(config.fragment_cache_bytes)),
      pages(page_options),
      changes(std::make_shared<ChangeHub>()),
      jobs(job_threads == 0 ? 1 : job_threads),
      storage(std::make_unique<storage::Storage>(storage::DiskService(config.storage.files, "local"),
                                                 secrets.active_storage_verifier())),
      job_sink(std::make_shared<models::NullJobSink>()) {}

App::~App() {
  if (db && subscription_ != 0) db->unsubscribe(subscription_);
}

Result<std::unique_ptr<App>> App::create(Config config, SharedClock clock, const AppOptions& options) {
  if (!clock) clock = std::make_shared<SystemClock>();
  if (auto dirs = config.storage.create_dirs(); !dirs) return std::unexpected(dirs.error());
  const std::string path = config.storage.database.string();
  if (options.prepare_database) {
    if (auto prepared = prepare_database(path, config.environment, *clock); !prepared) {
      return std::unexpected(prepared.error());
    }
  }
  PageCache::Options page_options;
  page_options.max_bytes = options.page_cache_bytes;
  page_options.audit_every = options.audit_every;
  auto app = std::make_unique<App>(std::move(config), clock, options.job_threads, page_options);
  db::DatabaseOptions db_options;
  db_options.clock = clock;
  auto database = db::Database::open(path, db_options);
  if (!database) return std::unexpected(database.error());
  app->db = std::move(*database);
  const std::shared_ptr<ChangeHub> hub = app->changes;
  app->subscription_ = app->db->subscribe([hub](std::span<const db::Change> changes) { hub->publish(changes); });
  static constexpr std::pair<std::string_view, std::string_view> kSheetOptions[] = {{"data-turbo-track", "reload"}};
  auto sheets = assets::stylesheet_link_tag_all(kSheetOptions);
  if (!sheets) return std::unexpected(sheets.error());
  app->stylesheets = std::move(*sheets);
  app->preload_link_header = assets::append_preload_links("", app->stylesheets.preload_links);
  return app;
}

void set_app(const App* app) noexcept {
  g_app.store(app, std::memory_order_release);
}

const App& app() {
  const App* a = g_app.load(std::memory_order_acquire);
  if (a == nullptr) {
    log_error("app() called before set_app()");
    std::abort();
  }
  return *a;
}

Status prepare_database(const std::string& path, std::string_view environment, const Clock& clock) {
  auto conn = db::Connection::open(path, db::Role::Writer);
  if (!conn) return std::unexpected(conn.error());
  Arena arena(1024);
  static const db::Query<std::int64_t()> has_migrations{
      "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'schema_migrations'"};
  auto exists = conn->first(has_migrations, arena);
  if (!exists) return std::unexpected(exists.error());
  if (!*exists) {
    // `db:schema:load`: the schema, the versions (the newest first, after the current one) and the metadata.
    std::string sql = "BEGIN IMMEDIATE;\n";
    sql += data::f_schema_sql;
    sql += "\n";
    for (auto it = std::rbegin(kMigrationVersions); it != std::rend(kMigrationVersions); ++it) {
      sql += "INSERT INTO \"schema_migrations\" (\"version\") VALUES ('" + std::string(*it) + "');\n";
    }
    const std::string now = format_db(clock.now());
    for (const auto& [key, value] : {std::pair<std::string_view, std::string_view>{"environment", environment},
                                     std::pair<std::string_view, std::string_view>{"schema_sha1", kSchemaSha1}}) {
      sql += "INSERT INTO \"ar_internal_metadata\" (\"key\", \"value\", \"created_at\", \"updated_at\") VALUES ('" +
             std::string(key) + "', '" + std::string(value) + "', '" + now + "', '" + now + "');\n";
    }
    sql += "COMMIT;";
    if (auto loaded = conn->exec_sql(sql); !loaded) {
      (void)conn->exec_sql("ROLLBACK");
      return loaded;
    }
  }
  return conn->exec_sql(std::string(kAdditions));
}

}  // namespace campfire::app
