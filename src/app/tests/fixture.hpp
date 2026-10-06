// A running app on a temporary database for the app tests.
#pragma once

#include <doctest.h>
#include <libdeflate.h>

#include <filesystem>
#include <memory>
#include <string>

#include "app/app.hpp"
#include "app/not_found.hpp"
#include "app/routes.hpp"
#include "app/tests/http_client.hpp"
#include "db/tests/test_util.hpp"
#include "net/server.hpp"
#include "req/bcrypt.hpp"

namespace campfire::app::testing {

inline constexpr const char* kPassword = "secret123456";

inline const db::Query<void(std::string_view, std::string_view, std::string_view, std::int64_t)> kInsertUser{
    "INSERT INTO users (name, email_address, password_digest, role, status, created_at, updated_at) VALUES "
    "(?, ?, ?, ?, 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00')"};
inline const db::Query<void()> kInsertAccount{
    "INSERT INTO accounts (name, join_code, singleton_guard, created_at, updated_at) VALUES "
    "('37signals', 'abcd-efgh-ijkl', 0, '2026-01-01 16:00:00', '2026-01-01 16:00:00')"};

// The environment of a test app: a storage directory and the secret.
inline Config test_config(const std::filesystem::path& dir) {
  auto config = Config::from_lookup([&](std::string_view key) -> std::optional<std::string> {
    if (key == "SECRET_KEY_BASE") return std::string(64, 'k');
    if (key == "CAMPFIRE_STORAGE_PATH") return dir.string();
    if (key == "DISABLE_SSL") return "true";
    if (key == "APP_VERSION") return "test";
    if (key == "GIT_REVISION") return "rev";
    return std::nullopt;
  });
  REQUIRE(config.has_value());
  return std::move(*config);
}

struct Fixture {
  explicit Fixture(AppOptions options = {}) {
    options.job_threads = 2;
    clock = TestClock::frozen_at(*from_civil(2026, 3, 2, 16, 0, 0));
    auto created = App::create(test_config(dir.file("storage")), clock, options);
    REQUIRE(created.has_value());
    state = std::move(*created);
    seed();
    set_app(state.get());
    net::ServerOptions server_options;
    server_options.http_port = 0;
    server_options.target_port = 0;
    server_options.workers = 2;
    server = std::make_unique<net::Server>(server_options, net::App{&routes(), &not_found});
    REQUIRE(server->start().has_value());
  }
  ~Fixture() {
    server->stop();
    server.reset();
  }

  void seed() {
    QueueScheduler scheduler;
    const std::string digest = req::bcrypt::hash_password(kPassword, req::bcrypt::kMinCost);
    auto wrote = db::testing::run_task(scheduler, state->db->write(scheduler, [&](db::Tx& tx) -> Status {
                                         if (auto r = tx.conn().exec(kInsertAccount); !r) return std::unexpected(r.error());
                                         auto r = tx.conn().exec(kInsertUser, "David", "david@example.com", digest, 1);
                                         if (!r) return std::unexpected(r.error());
                                         return {};
                                       }));
    REQUIRE(wrote.has_value());
  }

  [[nodiscard]] std::uint16_t port() const { return server->http_port(); }

  db::testing::TempDir dir;
  std::shared_ptr<TestClock> clock;
  std::unique_ptr<App> state;
  std::unique_ptr<net::Server> server;
};

// The decoded body of a gzip reply.
inline std::string gunzip(const std::string& gz) {
  libdeflate_decompressor* d = libdeflate_alloc_decompressor();
  std::string out(1 << 20, '\0');
  std::size_t n = 0;
  REQUIRE(libdeflate_gzip_decompress(d, gz.data(), gz.size(), out.data(), out.size(), &n) == LIBDEFLATE_SUCCESS);
  libdeflate_free_decompressor(d);
  out.resize(n);
  return out;
}

inline const std::string kSameOrigin = "Sec-Fetch-Site: same-origin\r\n";
inline const std::string kForm = "Content-Type: application/x-www-form-urlencoded\r\n";

}  // namespace campfire::app::testing
